// Stardunk ARM64 bridge: runs the original armeabi-v7a libGame.so under Dynarmic.
#pragma once

#include <android/log.h>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>

#define TAG "StardunkBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#ifdef BRIDGE_TRACE
#define LOGT(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#else
#define LOGT(...) ((void)0)
#endif

// Guest address space layout (32-bit).
constexpr uint32_t THUNK_BASE = 0x00010000;  // svc #n; bx lr  (8 bytes each)
constexpr uint32_t THUNK_COUNT = 0x2000;
constexpr uint32_t DATA_BASE = 0x00020000;  // bridge-owned static guest data
constexpr uint32_t DATA_END = 0x00100000;
constexpr uint32_t IMAGE_BASE = 0x00100000;  // libGame.so load address
constexpr uint32_t IMAGE_END = 0x01000000;
constexpr uint32_t STACK_BASE = 0x10000000;  // per-Jit guest stacks
constexpr uint32_t STACK_SIZE = 0x00200000;
constexpr uint32_t STACK_SLOTS = 64;
constexpr uint32_t HEAP_BASE = 0x20000000;
constexpr uint32_t HEAP_END = 0x80000000;
constexpr uint32_t COMMPAGE_BASE = 0xffff0000;

// SVC numbers.
constexpr uint32_t SVC_JNI_BASE = 0x1000;  // + JNINativeInterface index
constexpr uint32_t SVC_VM_BASE = 0x1200;   // + JNIInvokeInterface index
constexpr uint32_t SVC_RETURN = 0x1fff;    // host->guest call finished

extern uint8_t* g_mem;  // host base of the 4 GiB guest window

inline void* g2h(uint32_t a) { return g_mem + a; }
template <typename T>
inline T* gp(uint32_t a) { return a ? reinterpret_cast<T*>(g_mem + a) : nullptr; }
inline uint32_t h2g(const void* p) { return p ? (uint32_t)((const uint8_t*)p - g_mem) : 0; }
inline uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, g_mem + a, 4); return v; }
inline void wr32(uint32_t a, uint32_t v) { memcpy(g_mem + a, &v, 4); }
inline const char* gstr(uint32_t a) { return a ? (const char*)(g_mem + a) : nullptr; }

// memory.cpp
bool mem_init();
bool mem_mapped(uint32_t addr);
uint32_t data_alloc(uint32_t size, uint32_t align = 8);
uint32_t data_strdup(const char* s);
uint32_t gmalloc(uint32_t size);
uint32_t gcalloc(uint32_t n, uint32_t size);
uint32_t grealloc(uint32_t p, uint32_t size);
uint32_t gmemalign(uint32_t align, uint32_t size);
void gfree(uint32_t p);
uint32_t gstrdup(const char* s);

// cpu.cpp
struct Cpu;
using Handler = void (*)(Cpu&);

struct Cpu {
    void* jit;  // Dynarmic::A32::Jit*
    uint32_t& reg(int i);
    uint32_t arg(int i);  // AAPCS i-th 32-bit word (r0-r3 then stack)
    uint64_t arg64(int i) { return (uint64_t)arg(i) | ((uint64_t)arg(i + 1) << 32); }
    float argf(int i) { uint32_t v = arg(i); float f; memcpy(&f, &v, 4); return f; }
    double argd(int i) { uint64_t v = arg64(i); double d; memcpy(&d, &v, 8); return d; }
    void ret(uint32_t v) { reg(0) = v; }
    void ret64(uint64_t v) { reg(0) = (uint32_t)v; reg(1) = (uint32_t)(v >> 32); }
    void retf(float f) { uint32_t v; memcpy(&v, &f, 4); reg(0) = v; }
    void retd(double d) { uint64_t v; memcpy(&v, &d, 8); ret64(v); }
};

// Reads variadic arguments following AAPCS (softfp) from registers/stack or a va_list.
struct VarArgs {
    Cpu* cpu = nullptr;
    int ncrn = 0;
    uint32_t nsaa = 0;
    uint32_t va = 0;  // when non-zero, read from this va_list pointer instead
    static VarArgs regs(Cpu& c, int first);
    static VarArgs valist(uint32_t p) { VarArgs v; v.va = p; return v; }
    uint32_t next32();
    uint64_t next64();
    double nextd() { uint64_t v = next64(); double d; memcpy(&d, &v, 8); return d; }
};

bool cpu_init();
void svc_register(uint32_t num, Handler h, const char* name);
uint64_t call_guest(uint32_t fn, std::initializer_list<uint32_t> args);
void guest_backtrace(const char* why);

// loader.cpp
struct GuestImage {
    uint32_t base, size;
    uint32_t exidx, exidx_count;
};
extern GuestImage g_image;
bool load_guest(const uint8_t* elf, size_t len);
uint32_t guest_sym(const char* name);

// hle_*.cpp
void hle_libc_register();
void hle_gl_register();
Handler hle_lookup(const char* name);
uint32_t hle_data_symbol(const char* name);  // data imports (__sF, __stack_chk_guard)
void hle_register(const char* name, Handler h);

// printf.cpp
std::string guest_format(const char* fmt, VarArgs& va);
int guest_scan(const char* input, const char* fmt, VarArgs& va, void* file);

// jni_guest.cpp
#include <jni.h>
void jni_guest_init(JavaVM* vm);
uint32_t jni_guest_env();
uint32_t jni_guest_vm();
JNIEnv* jni_host_env();
uint32_t jref_local(jobject o);   // host -> guest handle (local)
jobject jref_host(uint32_t h);    // guest handle -> host
struct JniFrame {                 // scope of one host->guest native call
    JniFrame(JNIEnv* env);
    ~JniFrame();
    JNIEnv* prev_env;
    size_t mark;
};
