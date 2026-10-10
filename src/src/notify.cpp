#include "notify.h"

#include <android/log.h>
#include <jni.h>

#include <cstring>

#include "notify_dex.h"

#define LOG_TAG "Rain"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace notify {

namespace {

// Captured in JNI_OnLoad. The dex class is an app class: resolving it from a
// native thread with no Java frames through FindClass would hit the system
// class loader, which cannot see it (project finding F1). JNI_OnLoad runs
// with the loader that loaded this library on the stack, so both the
// UnityPlayer class and the app's class loader are grabbed there and kept as
// global refs.
jclass g_unity_player = nullptr;   // com/unity3d/player/UnityPlayer
jobject g_loader = nullptr;        // its ClassLoader (the app loader)
JavaVM* g_vm = nullptr;
bool g_capture_done = false;

// The RainNotify class (global ref) and the pieces we call on it.
jclass g_cls = nullptr;
jmethodID g_ctor = nullptr;       // (Context)V
jmethodID g_install = nullptr;    // ()V
jmethodID g_ensure = nullptr;     // ()V  — re-post if missing
jmethodID g_is_visible = nullptr; // ()Z  (static)

jobject g_instance = nullptr;
bool g_installed = false;
bool g_gave_up = false;

JNIEnv* Env() {
    if (!g_vm) return nullptr;
    JNIEnv* env = nullptr;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK)
        return env;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
    return env;
}

// The activity the game is running. Resolved through the cached UnityPlayer
// class, so no FindClass from a native thread is needed.
jobject CurrentActivity(JNIEnv* env) {
    if (!g_unity_player && g_loader) {
        // Loader-only capture path: resolve UnityPlayer through the app loader.
        jclass loader_cls = env->GetObjectClass(g_loader);
        jmethodID load = loader_cls
            ? env->GetMethodID(loader_cls, "loadClass",
                               "(Ljava/lang/String;)Ljava/lang/Class;")
            : nullptr;
        if (load) {
            jstring name = env->NewStringUTF("com.unity3d.player.UnityPlayer");
            if (name) {
                jclass up = static_cast<jclass>(
                    env->CallObjectMethod(g_loader, load, name));
                if (env->ExceptionCheck()) { env->ExceptionClear(); up = nullptr; }
                if (up) {
                    g_unity_player = static_cast<jclass>(env->NewGlobalRef(up));
                    env->DeleteLocalRef(up);
                }
                env->DeleteLocalRef(name);
            }
        }
        if (loader_cls) env->DeleteLocalRef(loader_cls);
    }
    if (!g_unity_player) return nullptr;
    jfieldID fid = env->GetStaticFieldID(g_unity_player, "currentActivity",
                                         "Landroid/app/Activity;");
    if (!fid) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return nullptr;
    }
    jobject a = env->GetStaticObjectField(g_unity_player, fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    return a;  // local ref; caller deletes
}

// Define com.rain.mod.RainNotify against the app loader using the embedded
// dex bytes. InMemoryDexClassLoader is a boot class, visible from anywhere.
jclass LoadClass(JNIEnv* env) {
    if (!g_loader) return nullptr;

    jclass dex_loader_cls = env->FindClass("dalvik/system/InMemoryDexClassLoader");
    if (!dex_loader_cls) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        LOGE("notify: InMemoryDexClassLoader not found");
        return nullptr;
    }
    jmethodID ctor = env->GetMethodID(
        dex_loader_cls, "<init>",
        "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
    if (!ctor) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(dex_loader_cls);
        return nullptr;
    }

    jobject buf = env->NewDirectByteBuffer(
        const_cast<unsigned char*>(kRainNotifyDex),
        static_cast<jlong>(kRainNotifyDexLen));
    if (!buf) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(dex_loader_cls);
        return nullptr;
    }

    jobject dex_loader = env->NewObject(dex_loader_cls, ctor, buf, g_loader);
    if (env->ExceptionCheck()) { env->ExceptionClear(); dex_loader = nullptr; }
    env->DeleteLocalRef(buf);
    env->DeleteLocalRef(dex_loader_cls);
    if (!dex_loader) {
        LOGE("notify: InMemoryDexClassLoader construction failed");
        return nullptr;
    }

    jclass loader_cls = env->GetObjectClass(dex_loader);
    jmethodID load = env->GetMethodID(loader_cls, "loadClass",
                                      "(Ljava/lang/String;)Ljava/lang/Class;");
    jclass out = nullptr;
    if (load) {
        jstring name = env->NewStringUTF("com.rain.mod.RainNotify");
        if (name) {
            out = static_cast<jclass>(env->CallObjectMethod(dex_loader, load, name));
            if (env->ExceptionCheck()) { env->ExceptionClear(); out = nullptr; }
            env->DeleteLocalRef(name);
        }
    }
    env->DeleteLocalRef(loader_cls);
    env->DeleteLocalRef(dex_loader);
    return out;
}

// Fallback loader resolution for the case JNI_OnLoad was not on the stack.
// ActivityThread is a boot class, so it is visible from any thread, and the
// Application it returns carries the app's own ClassLoader.
jobject LoaderViaActivityThread(JNIEnv* env) {
    jclass at = env->FindClass("android/app/ActivityThread");
    if (!at) { if (env->ExceptionCheck()) env->ExceptionClear(); return nullptr; }
    jmethodID current_app = env->GetStaticMethodID(
        at, "currentApplication", "()Landroid/app/Application;");
    jobject app = current_app
        ? env->CallStaticObjectMethod(at, current_app)
        : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); app = nullptr; }
    env->DeleteLocalRef(at);
    if (!app) return nullptr;

    jclass app_cls = env->GetObjectClass(app);
    jmethodID get_loader = app_cls
        ? env->GetMethodID(app_cls, "getClassLoader", "()Ljava/lang/ClassLoader;")
        : nullptr;
    jobject loader = get_loader ? env->CallObjectMethod(app, get_loader) : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); loader = nullptr; }
    if (app_cls) env->DeleteLocalRef(app_cls);
    env->DeleteLocalRef(app);
    return loader;
}

}  // namespace

void CaptureClassLoader(void* jni_env, void* java_vm) {
    if (g_capture_done) return;
    JNIEnv* env = static_cast<JNIEnv*>(jni_env);
    g_vm = static_cast<JavaVM*>(java_vm);
    if (!env || !g_vm) return;

    // JNI_OnLoad runs with the app's loader on the stack, so this FindClass
    // resolves the application class the system loader could not see.
    jclass up = env->FindClass("com/unity3d/player/UnityPlayer");
    if (!up) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        // Not on the JNI_OnLoad stack (or the class is not loaded yet):
        // fall back to the Application's own loader.
        jobject loader = LoaderViaActivityThread(env);
        if (loader) {
            g_loader = env->NewGlobalRef(loader);
            env->DeleteLocalRef(loader);
            LOGI("notify: app class loader captured via ActivityThread");
        } else {
            LOGE("notify: could not capture the app class loader");
        }
        g_capture_done = true;
        return;
    }
    g_unity_player = static_cast<jclass>(env->NewGlobalRef(up));

    // UnityPlayer's own ClassLoader is the app loader: the parent the dex
    // loader needs so framework and game classes resolve.
    jclass class_cls = env->GetObjectClass(up);
    jmethodID get_loader = class_cls
        ? env->GetMethodID(class_cls, "getClassLoader",
                           "()Ljava/lang/ClassLoader;")
        : nullptr;
    jobject loader = get_loader
        ? env->CallObjectMethod(up, get_loader)
        : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); loader = nullptr; }
    if (loader) {
        g_loader = env->NewGlobalRef(loader);
        LOGI("notify: app class loader captured");
    } else {
        LOGE("notify: UnityPlayer has no class loader");
    }
    env->DeleteLocalRef(loader);
    if (class_cls) env->DeleteLocalRef(class_cls);
    env->DeleteLocalRef(up);
    g_capture_done = true;
}

bool Install() {
    if (g_installed) return true;
    if (g_gave_up) return false;
    if (!g_loader) return false;

    JNIEnv* env = Env();
    if (!env) return false;

    jobject activity = CurrentActivity(env);
    if (!activity) return false;   // activity not up yet; caller retries

    jclass cls = LoadClass(env);
    if (!cls) {
        // The dex is embedded and the loader is cached, so a failure here is
        // permanent (bad dex, loader mismatch) — do not retry every second.
        g_gave_up = true;
        env->DeleteLocalRef(activity);
        return false;
    }
    g_cls = static_cast<jclass>(env->NewGlobalRef(cls));
    env->DeleteLocalRef(cls);

    g_ctor = env->GetMethodID(g_cls, "<init>", "(Landroid/content/Context;)V");
    g_install = env->GetMethodID(g_cls, "install", "()V");
    g_ensure = env->GetMethodID(g_cls, "ensurePosted", "()V");
    g_is_visible = env->GetStaticMethodID(g_cls, "isUiVisible", "()Z");
    if (!g_ctor || !g_install || !g_ensure || !g_is_visible) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        LOGE("notify: RainNotify methods not found");
        g_gave_up = true;
        env->DeleteLocalRef(activity);
        return false;
    }

    jobject inst = env->NewObject(g_cls, g_ctor, activity);
    if (env->ExceptionCheck()) { env->ExceptionClear(); inst = nullptr; }
    env->DeleteLocalRef(activity);
    if (!inst) {
        LOGE("notify: RainNotify construction failed");
        g_gave_up = true;
        return false;
    }
    g_instance = env->NewGlobalRef(inst);
    env->DeleteLocalRef(inst);

    env->CallVoidMethod(g_instance, g_install);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        LOGE("notify: install() threw");
        return false;   // retryable: the receiver registration can fail early
    }

    g_installed = true;
    LOGI("notify: installed — notification button toggles the overlay");
    return true;
}

void Tick() {
    if (!g_installed) return;
    // The game clears notifications on some transitions; re-post when the
    // user-visible notification has disappeared. Throttled to once a minute.
    static int tick = 0;
    if (++tick < 3600) return;   // ~60 s at 60 fps
    tick = 0;
    JNIEnv* env = Env();
    if (!env) return;
    env->CallVoidMethod(g_instance, g_ensure);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

bool UiVisible() {
    if (!g_installed || !g_cls || !g_is_visible) return true;  // fail-open
    JNIEnv* env = Env();
    if (!env) return true;
    jboolean v = env->CallStaticBooleanMethod(g_cls, g_is_visible);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return true; }
    return v != 0;
}

}  // namespace notify
