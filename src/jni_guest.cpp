#include "guest.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

// Guest-visible JNIEnv/JavaVM whose function tables point at SVC thunks. Object
// references are 32-bit handles: ((index + 1) << 2) | tag, tag 1 = local, 2 = global, 3 = weak.

static JavaVM* s_vm;
static uint32_t s_genv, s_gvm;
static thread_local JNIEnv* t_env;
static thread_local std::vector<jobject> t_locals;
static thread_local std::vector<uint32_t> t_free_locals;
static thread_local std::vector<size_t> t_frames;

static std::mutex s_global_lock;
static std::vector<jobject> s_globals;
static std::vector<uint32_t> s_free_globals;

struct MethodInfo {
    jmethodID id;
    std::string params;  // one char per parameter: ZBCSIJFDL
    char ret;
};
struct FieldInfo {
    jfieldID id;
};
constexpr uint32_t MAX_IDS = 8192;
static MethodInfo s_methods[MAX_IDS];
static std::atomic<uint32_t> s_nmethods{0};
static FieldInfo s_fields[MAX_IDS];
static std::atomic<uint32_t> s_nfields{0};
static std::mutex s_id_lock;

static jclass s_array_classes[8];  // [Z [B [C [S [I [J [F [D
static const int kElemSize[8] = {1, 1, 2, 2, 4, 8, 4, 8};

uint32_t jni_guest_env() { return s_genv; }
uint32_t jni_guest_vm() { return s_gvm; }

JNIEnv* jni_host_env() {
    if (t_env) return t_env;
    JNIEnv* env = nullptr;
    if (s_vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) s_vm->AttachCurrentThread(&env, nullptr);
    t_env = env;
    return env;
}

JniFrame::JniFrame(JNIEnv* env) : prev_env(t_env), mark(t_locals.size()) { t_env = env; }

JniFrame::~JniFrame() {
    t_locals.resize(mark);
    std::erase_if(t_free_locals, [this](uint32_t i) { return i >= mark; });
    t_env = prev_env;
}

uint32_t jref_local(jobject o) {
    if (!o) return 0;
    uint32_t idx;
    size_t floor = t_frames.empty() ? 0 : t_frames.back();
    if (!t_free_locals.empty() && t_free_locals.back() >= floor) {
        idx = t_free_locals.back();
        t_free_locals.pop_back();
        t_locals[idx] = o;
    } else {
        idx = (uint32_t)t_locals.size();
        t_locals.push_back(o);
    }
    return ((idx + 1) << 2) | 1;
}

static uint32_t jref_global(jobject g, uint32_t tag) {
    std::lock_guard<std::mutex> l(s_global_lock);
    uint32_t idx;
    if (!s_free_globals.empty()) { idx = s_free_globals.back(); s_free_globals.pop_back(); s_globals[idx] = g; }
    else { idx = (uint32_t)s_globals.size(); s_globals.push_back(g); }
    return ((idx + 1) << 2) | tag;
}

jobject jref_host(uint32_t h) {
    if (!h) return nullptr;
    uint32_t idx = (h >> 2) - 1, tag = h & 3;
    if (tag == 1) {
        if (idx < t_locals.size()) return t_locals[idx];
        LOGE("stale local ref 0x%x", h);
        return nullptr;
    }
    if (tag == 2 || tag == 3) {
        std::lock_guard<std::mutex> l(s_global_lock);
        if (idx < s_globals.size()) return s_globals[idx];
    }
    LOGE("invalid ref 0x%x", h);
    return nullptr;
}

static void drop_global(uint32_t h, bool weak) {
    if (!h || (h & 3) == 1) return;
    std::lock_guard<std::mutex> l(s_global_lock);
    uint32_t idx = (h >> 2) - 1;
    if (idx >= s_globals.size() || !s_globals[idx]) return;
    if (weak) jni_host_env()->DeleteWeakGlobalRef((jweak)s_globals[idx]);
    else jni_host_env()->DeleteGlobalRef(s_globals[idx]);
    s_globals[idx] = nullptr;
    s_free_globals.push_back(idx);
}

static uint32_t add_method(jmethodID id, const char* sig) {
    if (!id) return 0;
    MethodInfo m{id, {}, 'V'};
    const char* p = sig + 1;
    while (*p && *p != ')') {
        char t = *p;
        if (t == '[') {
            while (*p == '[') p++;
            if (*p == 'L') while (*p && *p != ';') p++;
            p++;
            m.params += 'L';
            continue;
        }
        if (t == 'L') { while (*p && *p != ';') p++; }
        p++;
        m.params += t;
    }
    if (*p == ')') m.ret = p[1] == '[' ? 'L' : p[1];
    std::lock_guard<std::mutex> l(s_id_lock);
    uint32_t n = s_nmethods.load();
    for (uint32_t i = 0; i < n; i++)
        if (s_methods[i].id == id) return i + 1;
    if (n >= MAX_IDS) abort();
    s_methods[n] = m;
    s_nmethods.store(n + 1);
    return n + 1;
}

static const MethodInfo* method(uint32_t h) {
    if (h == 0 || h > s_nmethods.load()) { LOGE("bad method id %u", h); return nullptr; }
    return &s_methods[h - 1];
}

static uint32_t add_field(jfieldID id) {
    if (!id) return 0;
    std::lock_guard<std::mutex> l(s_id_lock);
    uint32_t n = s_nfields.load();
    for (uint32_t i = 0; i < n; i++)
        if (s_fields[i].id == id) return i + 1;
    if (n >= MAX_IDS) abort();
    s_fields[n] = {id};
    s_nfields.store(n + 1);
    return n + 1;
}

static jfieldID field(uint32_t h) { return (h && h <= s_nfields.load()) ? s_fields[h - 1].id : nullptr; }

#define ENV jni_host_env()
#define OBJ(i) jref_host(c.arg(i))

// Builds a jvalue array for a call. variant: 0 = varargs, 1 = va_list, 2 = jvalue*.
static std::vector<jvalue> build_args(Cpu& c, const MethodInfo& m, int first, int variant) {
    std::vector<jvalue> out(m.params.size());
    if (variant == 2) {
        uint32_t p = c.arg(first);
        for (size_t i = 0; i < m.params.size(); i++, p += 8) {
            jvalue& v = out[i];
            switch (m.params[i]) {
                case 'J': case 'D': memcpy(&v.j, g2h(p), 8); break;
                case 'L': v.l = jref_host(rd32(p)); break;
                case 'F': memcpy(&v.f, g2h(p), 4); break;
                default: v.i = (jint)rd32(p);
            }
        }
        return out;
    }
    VarArgs va = variant == 0 ? VarArgs::regs(c, first) : VarArgs::valist(c.arg(first));
    for (size_t i = 0; i < m.params.size(); i++) {
        jvalue& v = out[i];
        switch (m.params[i]) {
            case 'J': v.j = (jlong)va.next64(); break;
            case 'D': v.d = va.nextd(); break;
            case 'F': v.f = (float)va.nextd(); break;  // promoted to double
            case 'L': v.l = jref_host(va.next32()); break;
            case 'Z': v.z = (jboolean)va.next32(); break;
            case 'B': v.b = (jbyte)va.next32(); break;
            case 'C': v.c = (jchar)va.next32(); break;
            case 'S': v.s = (jshort)va.next32(); break;
            default: v.i = (jint)va.next32();
        }
    }
    return out;
}

// kind: 0 virtual (env,obj,mid,...), 1 nonvirtual (env,obj,cls,mid,...), 2 static (env,cls,mid,...)
static void do_call(Cpu& c, int kind, int rtype, int variant) {
    JNIEnv* e = ENV;
    int mid_arg = kind == 1 ? 3 : 2;
    const MethodInfo* m = method(c.arg(mid_arg));
    if (!m) { c.ret64(0); return; }
    jobject obj = OBJ(1);
    jclass cls = kind == 1 ? (jclass)OBJ(2) : (jclass)obj;
    std::vector<jvalue> a = build_args(c, *m, mid_arg + 1, variant);
    const jvalue* av = a.data();
    jmethodID id = m->id;
#define CALL3(T) (kind == 0 ? e->Call##T##MethodA(obj, id, av) : kind == 1 ? e->CallNonvirtual##T##MethodA(obj, cls, id, av) : e->CallStatic##T##MethodA(cls, id, av))
    switch (rtype) {
        case 0: c.ret(jref_local(CALL3(Object))); break;
        case 1: c.ret(CALL3(Boolean)); break;
        case 2: c.ret((uint32_t)(int32_t)CALL3(Byte)); break;
        case 3: c.ret(CALL3(Char)); break;
        case 4: c.ret((uint32_t)(int32_t)CALL3(Short)); break;
        case 5: c.ret((uint32_t)CALL3(Int)); break;
        case 6: c.ret64((uint64_t)CALL3(Long)); break;
        case 7: c.retf(CALL3(Float)); break;
        case 8: c.retd(CALL3(Double)); break;
        case 9:
            if (kind == 0) e->CallVoidMethodA(obj, id, av);
            else if (kind == 1) e->CallNonvirtualVoidMethodA(obj, cls, id, av);
            else e->CallStaticVoidMethodA(cls, id, av);
            break;
    }
#undef CALL3
}

static void get_field(Cpu& c, bool is_static, int t) {
    JNIEnv* e = ENV;
    jobject o = OBJ(1);
    jfieldID f = field(c.arg(2));
    jclass k = (jclass)o;
#define GF(T) (is_static ? e->GetStatic##T##Field(k, f) : e->Get##T##Field(o, f))
    switch (t) {
        case 0: c.ret(jref_local(GF(Object))); break;
        case 1: c.ret(GF(Boolean)); break;
        case 2: c.ret((uint32_t)(int32_t)GF(Byte)); break;
        case 3: c.ret(GF(Char)); break;
        case 4: c.ret((uint32_t)(int32_t)GF(Short)); break;
        case 5: c.ret((uint32_t)GF(Int)); break;
        case 6: c.ret64((uint64_t)GF(Long)); break;
        case 7: c.retf(GF(Float)); break;
        case 8: c.retd(GF(Double)); break;
    }
#undef GF
}

static void set_field(Cpu& c, bool is_static, int t) {
    JNIEnv* e = ENV;
    jobject o = OBJ(1);
    jfieldID f = field(c.arg(2));
    jclass k = (jclass)o;
#define SF(T, v) (is_static ? e->SetStatic##T##Field(k, f, v) : e->Set##T##Field(o, f, v))
    switch (t) {
        case 0: SF(Object, OBJ(3)); break;
        case 1: SF(Boolean, (jboolean)c.arg(3)); break;
        case 2: SF(Byte, (jbyte)c.arg(3)); break;
        case 3: SF(Char, (jchar)c.arg(3)); break;
        case 4: SF(Short, (jshort)c.arg(3)); break;
        case 5: SF(Int, (jint)c.arg(3)); break;
        case 6: SF(Long, (jlong)c.arg64(4)); break;
        case 7: SF(Float, c.argf(3)); break;
        case 8: SF(Double, c.argd(4)); break;
    }
#undef SF
}

static void array_region(JNIEnv* e, int t, jarray a, jsize start, jsize len, void* buf, bool set) {
#define AR(T, J) (set ? e->Set##T##ArrayRegion((J##Array)a, start, len, (const J*)buf) : e->Get##T##ArrayRegion((J##Array)a, start, len, (J*)buf))
    switch (t) {
        case 0: AR(Boolean, jboolean); break;
        case 1: AR(Byte, jbyte); break;
        case 2: AR(Char, jchar); break;
        case 3: AR(Short, jshort); break;
        case 4: AR(Int, jint); break;
        case 5: AR(Long, jlong); break;
        case 6: AR(Float, jfloat); break;
        case 7: AR(Double, jdouble); break;
    }
#undef AR
}

static uint32_t array_elements(JNIEnv* e, int t, jarray a, uint32_t is_copy) {
    if (!a) return 0;
    jsize n = e->GetArrayLength(a);
    uint32_t buf = gmalloc((uint32_t)std::max(1, n * kElemSize[t]));
    array_region(e, t, a, 0, n, g2h(buf), false);
    if (is_copy) *(uint8_t*)g2h(is_copy) = 1;
    return buf;
}

static void release_elements(JNIEnv* e, int t, jarray a, uint32_t buf, int mode) {
    if (!buf) return;
    if (mode != JNI_ABORT && a) array_region(e, t, a, 0, e->GetArrayLength(a), g2h(buf), true);
    if (mode != JNI_COMMIT) gfree(buf);
}

static int array_type(JNIEnv* e, jobject a) {
    for (int i = 0; i < 8; i++)
        if (e->IsInstanceOf(a, s_array_classes[i])) return i;
    return 1;
}

static void jni_dispatch(Cpu& c, uint32_t idx) {
    JNIEnv* e = ENV;
    if (idx >= 34 && idx <= 63) { do_call(c, 0, (idx - 34) / 3, (idx - 34) % 3); return; }
    if (idx >= 64 && idx <= 93) { do_call(c, 1, (idx - 64) / 3, (idx - 64) % 3); return; }
    if (idx >= 114 && idx <= 143) { do_call(c, 2, (idx - 114) / 3, (idx - 114) % 3); return; }
    if (idx >= 95 && idx <= 103) { get_field(c, false, idx - 95); return; }
    if (idx >= 104 && idx <= 112) { set_field(c, false, idx - 104); return; }
    if (idx >= 145 && idx <= 153) { get_field(c, true, idx - 145); return; }
    if (idx >= 154 && idx <= 162) { set_field(c, true, idx - 154); return; }
    if (idx >= 175 && idx <= 182) {
        jsize n = (jsize)c.arg(1);
        jarray a = nullptr;
        switch (idx - 175) {
            case 0: a = e->NewBooleanArray(n); break;
            case 1: a = e->NewByteArray(n); break;
            case 2: a = e->NewCharArray(n); break;
            case 3: a = e->NewShortArray(n); break;
            case 4: a = e->NewIntArray(n); break;
            case 5: a = e->NewLongArray(n); break;
            case 6: a = e->NewFloatArray(n); break;
            case 7: a = e->NewDoubleArray(n); break;
        }
        c.ret(jref_local(a));
        return;
    }
    if (idx >= 183 && idx <= 190) { c.ret(array_elements(e, idx - 183, (jarray)OBJ(1), c.arg(2))); return; }
    if (idx >= 191 && idx <= 198) { release_elements(e, idx - 191, (jarray)OBJ(1), c.arg(2), (int)c.arg(3)); return; }
    if (idx >= 199 && idx <= 206) { array_region(e, idx - 199, (jarray)OBJ(1), c.arg(2), c.arg(3), g2h(c.arg(4)), false); return; }
    if (idx >= 207 && idx <= 214) { array_region(e, idx - 207, (jarray)OBJ(1), c.arg(2), c.arg(3), g2h(c.arg(4)), true); return; }

    switch (idx) {
        case 4: c.ret(0x00010006); return;  // GetVersion
        case 6: {  // FindClass
            const char* name = gstr(c.arg(1));
            jclass k = e->FindClass(name);
            if (!k) LOGW("FindClass(%s) failed", name);
            c.ret(jref_local(k));
            return;
        }
        case 10: c.ret(jref_local(e->GetSuperclass((jclass)OBJ(1)))); return;
        case 11: c.ret(e->IsAssignableFrom((jclass)OBJ(1), (jclass)OBJ(2))); return;
        case 13: c.ret((uint32_t)e->Throw((jthrowable)OBJ(1))); return;
        case 14: c.ret((uint32_t)e->ThrowNew((jclass)OBJ(1), gstr(c.arg(2)))); return;
        case 15: c.ret(jref_local(e->ExceptionOccurred())); return;
        case 16: e->ExceptionDescribe(); return;
        case 17: e->ExceptionClear(); return;
        case 18: LOGE("guest FatalError: %s", gstr(c.arg(1))); guest_backtrace("FatalError"); abort();
        case 19:  // PushLocalFrame
            t_frames.push_back(t_locals.size());
            c.ret((uint32_t)e->PushLocalFrame((jint)c.arg(1)));
            return;
        case 20: {  // PopLocalFrame
            jobject res = OBJ(1);
            if (!t_frames.empty()) {
                size_t mark = t_frames.back();
                t_frames.pop_back();
                t_locals.resize(mark);
                std::erase_if(t_free_locals, [mark](uint32_t i) { return i >= mark; });
            }
            c.ret(jref_local(e->PopLocalFrame(res)));
            return;
        }
        case 21: { jobject o = OBJ(1); c.ret(o ? jref_global(e->NewGlobalRef(o), 2) : 0); return; }
        case 22: drop_global(c.arg(1), false); return;
        case 23: {  // DeleteLocalRef
            uint32_t h = c.arg(1);
            if ((h & 3) != 1) return;
            uint32_t i = (h >> 2) - 1;
            if (i < t_locals.size() && t_locals[i]) {
                e->DeleteLocalRef(t_locals[i]);
                t_locals[i] = nullptr;
                t_free_locals.push_back(i);
            }
            return;
        }
        case 24: c.ret(e->IsSameObject(OBJ(1), OBJ(2))); return;
        case 25: c.ret(jref_local(e->NewLocalRef(OBJ(1)))); return;
        case 26: c.ret((uint32_t)e->EnsureLocalCapacity((jint)c.arg(1))); return;
        case 27: c.ret(jref_local(e->AllocObject((jclass)OBJ(1)))); return;
        case 28: case 29: case 30: {  // NewObject / V / A
            const MethodInfo* m = method(c.arg(2));
            if (!m) { c.ret(0); return; }
            std::vector<jvalue> a = build_args(c, *m, 3, idx - 28);
            c.ret(jref_local(e->NewObjectA((jclass)OBJ(1), m->id, a.data())));
            return;
        }
        case 31: c.ret(jref_local(e->GetObjectClass(OBJ(1)))); return;
        case 32: c.ret(e->IsInstanceOf(OBJ(1), (jclass)OBJ(2))); return;
        case 33: case 113: {  // GetMethodID / GetStaticMethodID
            const char* name = gstr(c.arg(2));
            const char* sig = gstr(c.arg(3));
            jclass k = (jclass)OBJ(1);
            jmethodID id = idx == 33 ? e->GetMethodID(k, name, sig) : e->GetStaticMethodID(k, name, sig);
            if (!id) LOGW("Get%sMethodID(%s %s) failed", idx == 33 ? "" : "Static", name, sig);
            c.ret(add_method(id, sig));
            return;
        }
        case 94: case 144: {  // GetFieldID / GetStaticFieldID
            const char* name = gstr(c.arg(2));
            const char* sig = gstr(c.arg(3));
            jclass k = (jclass)OBJ(1);
            jfieldID id = idx == 94 ? e->GetFieldID(k, name, sig) : e->GetStaticFieldID(k, name, sig);
            if (!id) LOGW("Get%sFieldID(%s %s) failed", idx == 94 ? "" : "Static", name, sig);
            c.ret(add_field(id));
            return;
        }
        case 163: c.ret(jref_local(e->NewString((const jchar*)g2h(c.arg(1)), (jsize)c.arg(2)))); return;
        case 164: c.ret((uint32_t)e->GetStringLength((jstring)OBJ(1))); return;
        case 165: case 224: {  // GetStringChars / GetStringCritical
            jstring s = (jstring)OBJ(1);
            if (!s) { c.ret(0); return; }
            jsize n = e->GetStringLength(s);
            uint32_t buf = gmalloc((uint32_t)(n + 1) * 2);
            e->GetStringRegion(s, 0, n, (jchar*)g2h(buf));
            ((jchar*)g2h(buf))[n] = 0;
            if (c.arg(2)) *(uint8_t*)g2h(c.arg(2)) = 1;
            c.ret(buf);
            return;
        }
        case 166: case 225: gfree(c.arg(2)); return;
        case 167: {
            uint32_t s = c.arg(1);
            c.ret(s ? jref_local(e->NewStringUTF(gstr(s))) : 0);
            return;
        }
        case 168: c.ret((uint32_t)e->GetStringUTFLength((jstring)OBJ(1))); return;
        case 169: {  // GetStringUTFChars
            jstring s = (jstring)OBJ(1);
            if (!s) { c.ret(0); return; }
            const char* u = e->GetStringUTFChars(s, nullptr);
            uint32_t g = gstrdup(u ? u : "");
            if (u) e->ReleaseStringUTFChars(s, u);
            if (c.arg(2)) *(uint8_t*)g2h(c.arg(2)) = 1;
            c.ret(g);
            return;
        }
        case 170: gfree(c.arg(2)); return;
        case 171: c.ret((uint32_t)e->GetArrayLength((jarray)OBJ(1))); return;
        case 172: c.ret(jref_local(e->NewObjectArray((jsize)c.arg(1), (jclass)OBJ(2), OBJ(3)))); return;
        case 173: c.ret(jref_local(e->GetObjectArrayElement((jobjectArray)OBJ(1), (jsize)c.arg(2)))); return;
        case 174: e->SetObjectArrayElement((jobjectArray)OBJ(1), (jsize)c.arg(2), OBJ(3)); return;
        case 215: LOGW("guest RegisterNatives ignored (%u methods)", c.arg(3)); c.ret(0); return;
        case 216: c.ret(0); return;
        case 217: c.ret((uint32_t)e->MonitorEnter(OBJ(1))); return;
        case 218: c.ret((uint32_t)e->MonitorExit(OBJ(1))); return;
        case 219: wr32(c.arg(1), s_gvm); c.ret(0); return;
        case 220: e->GetStringRegion((jstring)OBJ(1), c.arg(2), c.arg(3), (jchar*)g2h(c.arg(4))); return;
        case 221: e->GetStringUTFRegion((jstring)OBJ(1), c.arg(2), c.arg(3), (char*)g2h(c.arg(4))); return;
        case 222: {  // GetPrimitiveArrayCritical
            jarray a = (jarray)OBJ(1);
            c.ret(a ? array_elements(e, array_type(e, a), a, c.arg(2)) : 0);
            return;
        }
        case 223: {
            jarray a = (jarray)OBJ(1);
            if (a) release_elements(e, array_type(e, a), a, c.arg(2), (int)c.arg(3));
            return;
        }
        case 226: { jobject o = OBJ(1); c.ret(o ? jref_global(e->NewWeakGlobalRef(o), 3) : 0); return; }
        case 227: drop_global(c.arg(1), true); return;
        case 228: c.ret(e->ExceptionCheck()); return;
        case 229: c.ret(jref_local(e->NewDirectByteBuffer(g2h(c.arg(1)), (jlong)c.arg64(2)))); return;
        case 230: {
            void* p = e->GetDirectBufferAddress(OBJ(1));
            uint8_t* b = (uint8_t*)p;
            c.ret(b >= g_mem && b < g_mem + (1ull << 32) ? h2g(p) : 0);
            return;
        }
        case 231: c.ret64((uint64_t)e->GetDirectBufferCapacity(OBJ(1))); return;
        case 232: c.ret((uint32_t)e->GetObjectRefType(OBJ(1))); return;
    }
    LOGE("unimplemented JNI function #%u", idx);
    guest_backtrace("unimplemented JNI");
    abort();
}

template <uint32_t I>
static void jni_thunk(Cpu& c) { jni_dispatch(c, I); }

template <uint32_t... Is>
static void register_jni(std::integer_sequence<uint32_t, Is...>) {
    (svc_register(SVC_JNI_BASE + Is, jni_thunk<Is>, "JNI"), ...);
}

static void vm_dispatch(Cpu& c, uint32_t idx) {
    switch (idx) {
        case 3: c.ret(0); return;  // DestroyJavaVM
        case 4: case 7:            // AttachCurrentThread(AsDaemon)
            jni_host_env();
            if (c.arg(1)) wr32(c.arg(1), s_genv);
            c.ret(0);
            return;
        case 5: c.ret(0); return;  // DetachCurrentThread: Java threads stay attached
        case 6:                     // GetEnv
            jni_host_env();
            if (c.arg(1)) wr32(c.arg(1), s_genv);
            c.ret(0);
            return;
    }
    LOGE("unimplemented JavaVM function #%u", idx);
    abort();
}

template <uint32_t I>
static void vm_thunk(Cpu& c) { vm_dispatch(c, I); }

void jni_guest_init(JavaVM* vm) {
    s_vm = vm;
    constexpr uint32_t N = 233;
    uint32_t table = data_alloc(N * 4, 4);
    for (uint32_t i = 0; i < N; i++) wr32(table + 4 * i, THUNK_BASE + (SVC_JNI_BASE + i) * 8);
    s_genv = data_alloc(4, 4);
    wr32(s_genv, table);
    register_jni(std::make_integer_sequence<uint32_t, N>{});

    uint32_t vtable = data_alloc(8 * 4, 4);
    for (uint32_t i = 0; i < 8; i++) wr32(vtable + 4 * i, THUNK_BASE + (SVC_VM_BASE + i) * 8);
    s_gvm = data_alloc(4, 4);
    wr32(s_gvm, vtable);
    svc_register(SVC_VM_BASE + 3, vm_thunk<3>, "JavaVM");
    svc_register(SVC_VM_BASE + 4, vm_thunk<4>, "JavaVM");
    svc_register(SVC_VM_BASE + 5, vm_thunk<5>, "JavaVM");
    svc_register(SVC_VM_BASE + 6, vm_thunk<6>, "JavaVM");
    svc_register(SVC_VM_BASE + 7, vm_thunk<7>, "JavaVM");

    JNIEnv* e = jni_host_env();
    const char* names[8] = {"[Z", "[B", "[C", "[S", "[I", "[J", "[F", "[D"};
    for (int i = 0; i < 8; i++) {
        jclass k = e->FindClass(names[i]);
        s_array_classes[i] = (jclass)e->NewGlobalRef(k);
        e->DeleteLocalRef(k);
    }
}
