// Rain — MLBB client modification.
//
// Loaded by MLBB as one of its own hot-updated native modules. The game copies
// every file in
//     <sdcard>/Android/data/com.mobile.legends/files/dragon2017/assets/comlibs/arm64-v8a/*.bytes
// into /data/data/com.mobile.legends/app_libs/*.so and dlopens it, with no
// signature check on those files. Shipping Rain as `librudp.bytes` therefore
// loads it into the game's Unity process before the first frame is presented.
//
// IMPORTANT: that load is a native dlopen, not System.loadLibrary, so
// JNI_OnLoad is never called — the entry point is a static initializer.
//
// Rain exports only JNI_OnLoad / JNI_OnUnload, matching how the reference
// implementation is built, so MLBB's dlsym of the original library's symbols
// simply fails and it continues on its fallback path.
//
// Rain links only liblog/libandroid/libm/libdl/libc (+ the GL libs the overlay
// needs) and links libc++ statically: every library MLBB loads from app_libs
// has that same dependency profile, and anything else fails to resolve in the
// loader's context.

#include <jni.h>
#include <pthread.h>
#include <unistd.h>
#include <android/log.h>
#include <cstdio>

#include "il2cpp.h"
#include "config.h"
#include "game.h"
#include "notify.h"
#include "overlay.h"
#include "persist.h"

#define LOG_TAG "Rain"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

JavaVM* g_vm = nullptr;
pthread_once_t g_once = PTHREAD_ONCE_INIT;

// Rain is loaded as an app_lib alongside liblogic.so, so il2cpp may not be up
// yet and the battle classes certainly are not. Poll until both are usable.
void* Boot(void*) {
    LOGI("Rain: boot thread started (pid=%d)", getpid());

    char err[192] = {0};
    for (int attempt = 1; attempt <= 900; attempt++) {
        if (il2cpp::Init(err, sizeof(err))) break;
        if (attempt % 25 == 0) LOGI("Rain: waiting for il2cpp (%s)", err);
        usleep(200 * 1000);
    }

    if (!il2cpp::api.domain_get) {
        LOGE("Rain: il2cpp never became available: %s", err);
        return nullptr;
    }

    // Category defaults first, then the saved config over the top — the colour
    // defaults are per-index and cannot live in a member initialiser.
    ConfigDefaults(g_cfg);

    // Restore the saved config once the JVM bridge can resolve; the load is
    // best-effort and retries are unnecessary (Load marks itself done).
    persist::Load(g_cfg);

    // Install the notification toggle. The class loader was captured in
    // JNI_OnLoad; this needs the game's activity, so it polls briefly.
    for (int i = 0; i < 150 && !notify::Install(); i++) {
        usleep(200 * 1000);
    }

    // Install the render hook immediately; the battle scene comes later and the
    // hook retries class binding on each frame until ShowEntity exists.
    if (!overlay::Install()) {
        LOGE("Rain: overlay install failed: %s", overlay::LastError());
        return nullptr;
    }
    LOGI("Rain: overlay installed, waiting for battle scene");

    return nullptr;
}

void LaunchBoot() {
    pthread_t t;
    if (pthread_create(&t, nullptr, Boot, nullptr) != 0) {
        LOGE("Rain: failed to start boot thread");
        return;
    }
    pthread_detach(t);
}

}  // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    g_vm = vm;
    il2cpp::set_vm(vm);
    LOGI("Rain: JNI_OnLoad");
    // This is the one place the app's class loader is on the stack — capture
    // it before the boot thread needs it (the F1 pattern).
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) {
        notify::CaptureClassLoader(env, vm);
    }
    pthread_once(&g_once, LaunchBoot);
    return JNI_VERSION_1_6;
}

__attribute__((constructor)) static void RainOnLoad() {
    LOGI("Rain: constructor entry");
    pthread_once(&g_once, LaunchBoot);
}

extern "C" JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* vm, void* reserved) {
    LOGI("Rain: JNI_OnUnload");
}
