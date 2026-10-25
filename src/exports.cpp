#include "guest.h"

#include <mutex>

extern "C" const uint8_t guest_elf_start[];
extern "C" const uint8_t guest_elf_end[];

static uint32_t A(jint v) { return (uint32_t)v; }
static uint32_t A(jboolean v) { return v; }
static uint32_t A(jfloat v) { uint32_t u; memcpy(&u, &v, 4); return u; }
static uint32_t A(jobject v) { return jref_local(v); }

template <typename... Ts>
static uint32_t fwd(JNIEnv* env, uint32_t fn, jobject thiz, Ts... args) {
    JniFrame frame(env);
    return (uint32_t)call_guest(fn, {jni_guest_env(), jref_local(thiz), A(args)...});
}

template <typename... Ts>
static jobject fwd_obj(JNIEnv* env, uint32_t fn, jobject thiz, Ts... args) {
    JniFrame frame(env);
    uint32_t h = (uint32_t)call_guest(fn, {jni_guest_env(), jref_local(thiz), A(args)...});
    jobject o = jref_host(h);
    return o ? env->NewLocalRef(o) : nullptr;
}

static uint32_t resolve(const char* name) {
    uint32_t a = guest_sym(name);
    if (!a) LOGE("guest is missing %s", name);
    return a;
}

#define FN(cls, name) []() { static const uint32_t a = resolve("Java_com_gzl_stardunk_glue_" #cls "_" #name); return a; }()
#define NATIVE(ret, cls, name, ...) extern "C" JNIEXPORT ret JNICALL Java_com_gzl_stardunk_glue_##cls##_##name(JNIEnv* env, jobject thiz, ##__VA_ARGS__)

// ---- Loader
NATIVE(jint, Loader, createNativeInstance) { return (jint)fwd(env, FN(Loader, createNativeInstance), thiz); }
NATIVE(void, Loader, freeNativeInstance, jint p) { fwd(env, FN(Loader, freeNativeInstance), thiz, p); }
NATIVE(void, Loader, initNative) { fwd(env, FN(Loader, initNative), thiz); }

// ---- Renderer
NATIVE(void, Renderer, back0) { fwd(env, FN(Renderer, back0), thiz); }
NATIVE(void, Renderer, disposeView) { fwd(env, FN(Renderer, disposeView), thiz); }
NATIVE(jint, Renderer, getBestScore0) { return (jint)fwd(env, FN(Renderer, getBestScore0), thiz); }
NATIVE(jstring, Renderer, getFacebookFeedBody0) { return (jstring)fwd_obj(env, FN(Renderer, getFacebookFeedBody0), thiz); }
NATIVE(jstring, Renderer, getStarCode0) { return (jstring)fwd_obj(env, FN(Renderer, getStarCode0), thiz); }
NATIVE(jstring, Renderer, getTwitterStatusBody0) { return (jstring)fwd_obj(env, FN(Renderer, getTwitterStatusBody0), thiz); }
NATIVE(void, Renderer, initAndLoadSounds, jobject se) { fwd(env, FN(Renderer, initAndLoadSounds), thiz, se); }
NATIVE(void, Renderer, initNative) { fwd(env, FN(Renderer, initNative), thiz); }
NATIVE(void, Renderer, initialize, jint loader, jint w, jint h, jstring version, jboolean details) {
    LOGI("Renderer.initialize %dx%d", w, h);
    fwd(env, FN(Renderer, initialize), thiz, loader, w, h, (jobject)version, details);
}
NATIVE(jboolean, Renderer, isFriendCodeEntered0) { return (jboolean)fwd(env, FN(Renderer, isFriendCodeEntered0), thiz); }
NATIVE(jboolean, Renderer, isInMainMenu) { return (jboolean)fwd(env, FN(Renderer, isInMainMenu), thiz); }
NATIVE(void, Renderer, loadGameResources, jint loader) { fwd(env, FN(Renderer, loadGameResources), thiz, loader); }
NATIVE(void, Renderer, loadingFinished, jint loader, jint online, jstring s, jfloat lon, jfloat lat) {
    fwd(env, FN(Renderer, loadingFinished), thiz, loader, online, (jobject)s, lon, lat);
}
NATIVE(void, Renderer, onAlertBoxResult0, jint a, jint b, jint p) { fwd(env, FN(Renderer, onAlertBoxResult0), thiz, a, b, p); }
NATIVE(void, Renderer, openStarShop0) { fwd(env, FN(Renderer, openStarShop0), thiz); }
NATIVE(void, Renderer, renderView) { fwd(env, FN(Renderer, renderView), thiz); }
NATIVE(void, Renderer, restoreContext, jint loader) { fwd(env, FN(Renderer, restoreContext), thiz, loader); }
NATIVE(void, Renderer, saveProfileData, jint loader) { fwd(env, FN(Renderer, saveProfileData), thiz, loader); }
NATIVE(void, Renderer, sendStarCode0, jstring s) { fwd(env, FN(Renderer, sendStarCode0), thiz, (jobject)s); }
NATIVE(void, Renderer, setRatioMainMenuAds0, jint r) { fwd(env, FN(Renderer, setRatioMainMenuAds0), thiz, r); }
NATIVE(void, Renderer, showMainMenu0) { fwd(env, FN(Renderer, showMainMenu0), thiz); }
NATIVE(void, Renderer, syncProfileAfterPurchase0, jint loader, jint sp, jint premium, jstring a, jstring b, jstring d) {
    fwd(env, FN(Renderer, syncProfileAfterPurchase0), thiz, loader, sp, premium, (jobject)a, (jobject)b, (jobject)d);
}
NATIVE(void, Renderer, touchesBegan, jfloat x, jfloat y) { fwd(env, FN(Renderer, touchesBegan), thiz, x, y); }
NATIVE(void, Renderer, touchesEnded, jfloat x, jfloat y) { fwd(env, FN(Renderer, touchesEnded), thiz, x, y); }
NATIVE(void, Renderer, touchesMoved, jfloat x, jfloat y) { fwd(env, FN(Renderer, touchesMoved), thiz, x, y); }
NATIVE(void, Renderer, unlockItemsForAchievement0, jstring s) { fwd(env, FN(Renderer, unlockItemsForAchievement0), thiz, (jobject)s); }
NATIVE(void, Renderer, updateFeatures0) { fwd(env, FN(Renderer, updateFeatures0), thiz); }
NATIVE(void, Renderer, updateTapPoints0, jint t) { fwd(env, FN(Renderer, updateTapPoints0), thiz, t); }

// ---- SoundEngine
NATIVE(void, SoundEngine, init) { fwd(env, FN(SoundEngine, init), thiz); }
NATIVE(jboolean, SoundEngine, isEnabled) { return (jboolean)fwd(env, FN(SoundEngine, isEnabled), thiz); }
NATIVE(void, SoundEngine, runLoop) { fwd(env, FN(SoundEngine, runLoop), thiz); }
NATIVE(void, SoundEngine, setupRunLoop) { fwd(env, FN(SoundEngine, setupRunLoop), thiz); }
NATIVE(void, SoundEngine, stopRunLoop) { fwd(env, FN(SoundEngine, stopRunLoop), thiz); }

// ---- OnlineManager
NATIVE(jint, OnlineManager, createNativeInstance, jint view) { return (jint)fwd(env, FN(OnlineManager, createNativeInstance), thiz, view); }
NATIVE(jint, OnlineManager, freeNativeInstance, jint p) { return (jint)fwd(env, FN(OnlineManager, freeNativeInstance), thiz, p); }
NATIVE(void, OnlineManager, initNative) { fwd(env, FN(OnlineManager, initNative), thiz); }
NATIVE(void, OnlineManager, logIn, jint p) { fwd(env, FN(OnlineManager, logIn), thiz, p); }
NATIVE(void, OnlineManager, requestFailed, jint p, jint id, jint code, jstring msg) {
    fwd(env, FN(OnlineManager, requestFailed), thiz, p, id, code, (jobject)msg);
}
NATIVE(void, OnlineManager, requestSucceeded, jint p, jint id, jbyteArray data) {
    fwd(env, FN(OnlineManager, requestSucceeded), thiz, p, id, (jobject)data);
}

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    LOGI("Stardunk ARM64 bridge starting");
    if (!mem_init() || !cpu_init()) abort();
    hle_libc_register();
    hle_gl_register();
    jni_guest_init(vm);
    JNIEnv* env = jni_host_env();
    JniFrame frame(env);
    if (!load_guest(guest_elf_start, (size_t)(guest_elf_end - guest_elf_start))) {
        LOGE("failed to load guest libGame.so");
        abort();
    }
    uint32_t onload = guest_sym("JNI_OnLoad");
    uint32_t ver = onload ? (uint32_t)call_guest(onload, {jni_guest_vm(), 0}) : 0;
    LOGI("guest JNI_OnLoad -> 0x%x", ver);
    return JNI_VERSION_1_6;
}
