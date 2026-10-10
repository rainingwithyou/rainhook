// Deployment probe.
//
// MLBB hot-loads its native modules from /data/data/com.mobile.legends/app_libs/
// using its own loader rather than the dynamic linker. If that loader enumerates
// the directory (rather than loading a fixed, signed list), then dropping a .so
// in there is a load path for Rain that needs no APK patching and no injector.
//
// This library does nothing but announce that it ran. If RAINTEST lines appear in
// logcat while the game starts, the directory is enumerated.

#include <android/log.h>
#include <unistd.h>

#define LOG_TAG "RAINTEST"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

__attribute__((constructor)) static void on_load(void) {
    LOGI("=================================================");
    LOGI("RAINTEST: loaded into pid=%d uid=%d", getpid(), getuid());
    LOGI("RAINTEST: if you can read this, app_libs is enumerated");
    LOGI("=================================================");
}

__attribute__((destructor)) static void on_unload(void) {
    LOGI("RAINTEST: unloaded from pid=%d", getpid());
}
