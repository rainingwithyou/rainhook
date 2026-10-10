#include "synth.h"

#include <android/log.h>
#include <jni.h>
#include <chrono>
#include <cstring>
#include <mutex>

#include "il2cpp.h"

#define LOG_TAG "Rain"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace synth {

namespace {

enum : uint8_t { A_DOWN = 0, A_MOVE = 1, A_UP = 2 };

// One dispatch step. `after` is how many frames to idle once it has been sent,
// which is where a tap's hold length and a drag's move spacing live — that way
// every gesture is one flat step list and the tick stays trivial.
struct Step {
    uint8_t act;
    float x, y;
    int after;
};

constexpr int kQueue = 48;

Step g_q[kQueue];
int g_qn = 0;
int g_delay = 0;

bool g_is_down = false;   // the pointer is down right now
jlong g_down_ms = 0;      // downTime — constant for the whole gesture
float g_last_x = 0, g_last_y = 0;

bool g_hold = false;        // sustained press active
float g_hold_x = 0, g_hold_y = 0;
bool g_hold_dirty = false;  // position changed since the last MOVE

std::mutex g_mtx;

// JNI environment for the calling thread. The render thread is attached to
// the JVM here — Unity's render worker is not a Java thread, so every use
// attaches on demand and keeps the env for the thread's lifetime.
JNIEnv* Env() {
    JavaVM* vm = il2cpp::get_vm();
    if (!vm) return nullptr;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK)
        return env;
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
    return env;
}

jclass g_unity_player = nullptr;
jfieldID g_fid_current_activity = nullptr;
jclass g_motion_event = nullptr;
jmethodID g_mid_obtain = nullptr;
jmethodID g_mid_recycle = nullptr;

// dispatchTouchEvent is resolved once against the concrete activity class and
// kept as a global ref. It used to be re-resolved with GetObjectClass +
// GetMethodID on every single dispatch, which is a JNI lookup per event.
jmethodID g_mid_dispatch = nullptr;

// ---- coordinate space ----
//
// Callers work in Unity's surface pixels (what GetTouch reports), but
// dispatchTouchEvent expects physical display pixels. The game renders at its
// own resolution and the compositor upscales, so the two differ — on the test
// device the surface is 1968x908 while the panel is 2772x1280. Synthesizing at
// the raw Unity coordinate therefore presses the wrong spot: the aura held
// "somewhere" instead of the attack button, so nothing ever fired.
int g_uni_w = 0, g_uni_h = 0;      // surface size, set by the overlay each frame
int g_phys_w = 0, g_phys_h = 0;    // physical panel size, queried once

// Physical display size, from the activity's own Display. Cached: it cannot
// change while the process lives.
void QueryPhysicalSize(JNIEnv* env, jobject activity) {
    if (g_phys_w > 0 && g_phys_h > 0) return;

    jclass act_cls = env->GetObjectClass(activity);
    if (!act_cls) { if (env->ExceptionCheck()) env->ExceptionClear(); return; }

    jmethodID get_wm = env->GetMethodID(act_cls, "getWindowManager",
                                        "()Landroid/view/WindowManager;");
    jobject wm = get_wm ? env->CallObjectMethod(activity, get_wm) : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); wm = nullptr; }

    jclass wm_cls = wm ? env->GetObjectClass(wm) : nullptr;
    jmethodID get_disp = wm_cls ? env->GetMethodID(wm_cls, "getDefaultDisplay",
                                                   "()Landroid/view/Display;")
                                : nullptr;
    jobject disp = (get_disp && wm) ? env->CallObjectMethod(wm, get_disp) : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); disp = nullptr; }

    jclass disp_cls = disp ? env->GetObjectClass(disp) : nullptr;
    if (disp_cls) {
        // getRealSize(Point) is the API that ignores any compatibility scaling.
        jclass point_cls = env->FindClass("android/graphics/Point");
        jmethodID point_ctor = point_cls
            ? env->GetMethodID(point_cls, "<init>", "()V") : nullptr;
        jobject point = point_ctor ? env->NewObject(point_cls, point_ctor) : nullptr;
        jmethodID get_real = env->GetMethodID(disp_cls, "getRealSize",
                                              "(Landroid/graphics/Point;)V");
        if (get_real && point) {
            env->CallVoidMethod(disp, get_real, point);
            if (env->ExceptionCheck()) env->ExceptionClear();
            jfieldID fx = env->GetFieldID(point_cls, "x", "I");
            jfieldID fy = env->GetFieldID(point_cls, "y", "I");
            if (fx && fy) {
                g_phys_w = env->GetIntField(point, fx);
                g_phys_h = env->GetIntField(point, fy);
            }
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (point) env->DeleteLocalRef(point);
        if (point_cls) env->DeleteLocalRef(point_cls);
        env->DeleteLocalRef(disp_cls);
    }
    if (disp) env->DeleteLocalRef(disp);
    if (wm_cls) env->DeleteLocalRef(wm_cls);
    if (wm) env->DeleteLocalRef(wm);
    env->DeleteLocalRef(act_cls);

    if (g_phys_w > 0 && g_phys_h > 0)
        LOGI("synth: physical display %dx%d", g_phys_w, g_phys_h);
    else
        LOGW("synth: physical display size unavailable, using raw coords");
}

// Surface pixel -> physical pixel. Falls back to identity when either size is
// unknown, which is the previous behaviour.
void ToPhysical(float* x, float* y) {
    if (g_uni_w <= 0 || g_uni_h <= 0 || g_phys_w <= 0 || g_phys_h <= 0) return;
    *x *= static_cast<float>(g_phys_w) / static_cast<float>(g_uni_w);
    *y *= static_cast<float>(g_phys_h) / static_cast<float>(g_uni_h);
}

bool Resolve() {
    JNIEnv* env = Env();
    if (!env) return false;

    if (!g_unity_player) {
        jclass c = env->FindClass("com/unity3d/player/UnityPlayer");
        if (!c) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            return false;
        }
        g_unity_player = static_cast<jclass>(env->NewGlobalRef(c));
        env->DeleteLocalRef(c);
    }
    if (!g_fid_current_activity) {
        g_fid_current_activity = env->GetStaticFieldID(
            g_unity_player, "currentActivity", "Landroid/app/Activity;");
        if (!g_fid_current_activity) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            return false;
        }
    }
    if (!g_motion_event) {
        jclass c = env->FindClass("android/view/MotionEvent");
        if (!c) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            return false;
        }
        g_motion_event = static_cast<jclass>(env->NewGlobalRef(c));
        env->DeleteLocalRef(c);
        g_mid_obtain = env->GetStaticMethodID(
            g_motion_event, "obtain", "(JJFFI)Landroid/view/MotionEvent;");
        g_mid_recycle = env->GetMethodID(g_motion_event, "recycle", "()V");
        if (!g_mid_obtain || !g_mid_recycle) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            return false;
        }
    }
    return true;
}

jobject CurrentActivity(JNIEnv* env) {
    if (!g_unity_player || !g_fid_current_activity) return nullptr;
    jobject a = env->GetStaticObjectField(g_unity_player, g_fid_current_activity);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return nullptr;
    }
    return a;  // local ref; caller deletes
}

// Resolve dispatchTouchEvent from the live activity the first time one is
// available. The activity class is stable across a session, so this caches.
bool ResolveDispatch(JNIEnv* env, jobject activity) {
    if (g_mid_dispatch) return true;
    jclass cls = env->GetObjectClass(activity);
    if (!cls) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return false;
    }
    g_mid_dispatch = env->GetMethodID(cls, "dispatchTouchEvent",
                                      "(Landroid/view/MotionEvent;)Z");
    env->DeleteLocalRef(cls);
    if (!g_mid_dispatch) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return false;
    }
    LOGI("synth: dispatchTouchEvent bound");
    return true;
}

jlong NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Send one MotionEvent. Lock is held by the caller.
//
// downTime is held constant for the whole gesture: the original code stamped
// both downTime and eventTime with "now" on every event, so the game saw each
// MOVE/UP as the start of a fresh gesture. Unity derives its touch deltas from
// that pair, so a stable downTime is what makes a drag read as a drag.
void DispatchStep(JNIEnv* env, const Step& s) {
    jobject activity = CurrentActivity(env);
    if (!activity) return;
    if (!ResolveDispatch(env, activity)) {
        env->DeleteLocalRef(activity);
        return;
    }
    QueryPhysicalSize(env, activity);

    const jlong now = NowMs();
    if (s.act == A_DOWN) {
        g_down_ms = now;
    } else if (g_down_ms == 0) {
        g_down_ms = now;  // orphan MOVE/UP — shouldn't happen, stay well-formed
    }

    float px = s.x, py = s.y;
    ToPhysical(&px, &py);

    jobject ev = env->CallStaticObjectMethod(g_motion_event, g_mid_obtain,
                                             g_down_ms, now, s.act, px, py,
                                             /*metaState=*/0);
    if (env->ExceptionCheck() || !ev) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(activity);
        return;
    }
    env->CallBooleanMethod(activity, g_mid_dispatch, ev);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->CallVoidMethod(ev, g_mid_recycle);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(ev);
    env->DeleteLocalRef(activity);

    g_is_down = (s.act != A_UP);
    g_last_x = s.x;
    g_last_y = s.y;
    if (s.act == A_UP) g_down_ms = 0;
}

// Append a step. Lock is held by the caller.
bool Queue(uint8_t act, float x, float y, int after) {
    if (g_qn >= kQueue) return false;
    g_q[g_qn].act = act;
    g_q[g_qn].x = x;
    g_q[g_qn].y = y;
    g_q[g_qn].after = (after < 0) ? 0 : after;
    g_qn++;
    return true;
}

void ClearQueue() {
    g_qn = 0;
    g_delay = 0;
}

}  // namespace

bool Init() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return Resolve();
}

bool Ready() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_unity_player && g_fid_current_activity && g_motion_event &&
           g_mid_obtain && g_mid_recycle;
}

void SetUnitySize(int w, int h) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_uni_w = w;
    g_uni_h = h;
}

bool Tap(float x, float y, int hold_frames) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_hold) return false;          // sustained press owns the pointer
    if (g_qn > 2) return false;        // a gesture is already in flight
    if (hold_frames < 1) hold_frames = 1;
    if (!Queue(A_DOWN, x, y, hold_frames)) return false;
    if (!Queue(A_UP, x, y, 0)) { ClearQueue(); return false; }
    return true;
}

bool Drag(float x0, float y0, float x1, float y1, int frames) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_hold) return false;
    if (frames < 2) frames = 2;
    if (frames > 12) frames = 12;
    if (g_qn + frames + 2 > kQueue) return false;

    // Down on the button, hold a beat so the game registers the press before
    // the direction starts moving — a zero-length press reads as a tap and
    // smart-casts instead of aiming.
    if (!Queue(A_DOWN, x0, y0, 1)) return false;
    for (int i = 1; i <= frames; i++) {
        const float t = static_cast<float>(i) / static_cast<float>(frames);
        const float x = x0 + (x1 - x0) * t;
        const float y = y0 + (y1 - y0) * t;
        if (!Queue(A_MOVE, x, y, 1)) { ClearQueue(); return false; }
    }
    if (!Queue(A_UP, x1, y1, 0)) { ClearQueue(); return false; }
    return true;
}

bool HoldBegin(float x, float y) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_hold) return false;
    if (g_qn > 0) return false;        // let an in-flight gesture finish
    if (!Queue(A_DOWN, x, y, 0)) return false;
    g_hold = true;
    g_hold_x = x;
    g_hold_y = y;
    g_hold_dirty = false;
    return true;
}

void HoldMove(float x, float y) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_hold) return;
    g_hold_x = x;
    g_hold_y = y;
    g_hold_dirty = true;   // coalesced: only the last position this frame goes out
}

void HoldEnd() {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_hold) return;
    g_hold = false;
    g_hold_dirty = false;
    Queue(A_UP, g_hold_x, g_hold_y, 0);
}

bool Holding() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_hold;
}

bool Busy() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_hold || g_qn > 0 || g_delay > 0;
}

void Abort() {
    std::lock_guard<std::mutex> lk(g_mtx);
    ClearQueue();
    g_hold = false;
    g_hold_dirty = false;
    if (!g_is_down) return;

    // Lift immediately rather than queueing: this runs on teardown paths where
    // there may be no next frame to drain a queue.
    JNIEnv* env = Env();
    if (!env) {
        g_is_down = false;
        g_down_ms = 0;
        return;
    }
    DispatchStep(env, Step{A_UP, g_last_x, g_last_y, 0});
    LOGI("synth: abort released the pointer");
}

void Tick() {
    JNIEnv* env = Env();
    if (!env) return;
    if (!Resolve()) return;

    std::lock_guard<std::mutex> lk(g_mtx);

    if (g_qn > 0) {
        if (g_delay > 0) {
            g_delay--;
            return;
        }
        const Step s = g_q[0];
        for (int i = 0; i < g_qn - 1; i++) g_q[i] = g_q[i + 1];
        g_qn--;
        DispatchStep(env, s);
        g_delay = s.after;
        return;
    }

    // Nothing queued: keep a sustained press alive, tracking its position.
    if (g_hold && g_hold_dirty) {
        g_hold_dirty = false;
        DispatchStep(env, Step{A_MOVE, g_hold_x, g_hold_y, 0});
    }
}

}  // namespace synth
