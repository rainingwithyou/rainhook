#include "input.h"

#include <android/log.h>
#include <cstring>

#include "game.h"
#include "il2cpp.h"
#include "imgui.h"

#define LOG_TAG "Rain"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace input {

using namespace il2cpp;

namespace {

Il2CppClass* g_input = nullptr;
const MethodInfo* g_m_touch_count = nullptr;
const MethodInfo* g_m_get_touch = nullptr;

// UnityEngine.Touch layout verified against live metadata (v2.1.67.11733):
//   0x00 int32 m_FingerId
//   0x04 float m_Position.x/y
//   0x0c float m_RawPosition.x/y
//   0x14 float m_PositionDelta.x/y
//   0x1c float m_TimeDelta
//   0x20 int32 m_TapCount
//   0x24 int32 m_Phase
//   0x28 int32 m_Type
struct Touch {
    int32_t finger_id;      // 0x00 m_FingerId
    float   pos_x, pos_y;   // 0x04 m_Position
    float   raw_x, raw_y;   // 0x0c m_RawPosition
    float   delta_x, delta_y; // 0x14 m_PositionDelta
    float   delta_time;     // 0x1c m_TimeDelta
    int32_t tap_count;      // 0x20 m_TapCount
    int32_t phase;          // 0x24 m_Phase
    int32_t type;           // 0x28 m_Type
};

enum { kPhaseBegan = 0, kPhaseMoved = 1, kPhaseStationary = 2, kPhaseEnded = 3, kPhaseCanceled = 4 };

int g_touch_count = 0;
bool g_began_this_frame = false;
float g_last_x = 0, g_last_y = 0;
bool g_have_tap = false;      // a completed tap was seen
float g_tap_x = 0, g_tap_y = 0;

// Every finger this frame, in the overlay's coordinate space.
constexpr int kMaxTouches = 10;
TouchPoint g_touch_pts[kMaxTouches];
int g_touch_n = 0;
int g_ignored_finger = -1;   // the aura's synthesized hold, hidden from ImGui

// il2cpp_runtime_invoke boxes value-type results: the returned pointer is an
// Il2CppObject whose header precedes the payload. The header is two pointers
// ([klass][monitor]), so the payload offset is 16 on 64-bit and 8 on 32-bit.
#if defined(__LP64__)
static constexpr size_t kBoxedPayload = 0x10;
#else
static constexpr size_t kBoxedPayload = 0x08;
#endif

int InvokeInt(const MethodInfo* m, void* obj, void** params = nullptr) {
    Il2CppObject* exc = nullptr;
    void* r = api.runtime_invoke(m, obj, params, &exc);
    if (!r || exc) return 0;
    return *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(r) + kBoxedPayload);
}

// Diagnostic: report both the header reading and the payload reading.
void InvokeIntDiag(const MethodInfo* m, void* obj, int* raw_val, int* hdr_val, void** raw_ptr) {
    Il2CppObject* exc = nullptr;
    void* r = api.runtime_invoke(m, obj, nullptr, &exc);
    *raw_ptr = r;
    if (!r || exc) { *raw_val = *hdr_val = -999; return; }
    *raw_val = *reinterpret_cast<int32_t*>(r);
    *hdr_val = *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(r) + kBoxedPayload);
}

bool InvokeTouch(const MethodInfo* m, int index, Touch* out) {
    Il2CppObject* exc = nullptr;
    int32_t idx = index;
    void* params[1] = { &idx };
    void* r = api.runtime_invoke(m, nullptr, params, &exc);
    if (!r || exc) return false;
    memcpy(out, reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(r) + kBoxedPayload),
           sizeof(Touch));
    return true;
}

}  // namespace

bool Init() {
    g_input = FindClass("UnityEngine", "Input");
    if (!g_input) {
        LOGI("input: UnityEngine.Input not found");
        return false;
    }
    g_m_touch_count = api.class_get_method_from_name(g_input, "get_touchCount", 0);
    g_m_get_touch = api.class_get_method_from_name(g_input, "GetTouch", 1);
    if (!g_m_touch_count || !g_m_get_touch) {
        LOGI("input: touch API missing (touchCount=%p GetTouch=%p)",
             (void*)g_m_touch_count, (void*)g_m_get_touch);
        return false;
    }
    LOGI("input: UnityEngine.Input bound");
    return true;
}

bool Touching() { return g_touch_count > 0; }

void Pump() {
    if (!g_m_touch_count || !g_m_get_touch || !api.runtime_invoke) return;

    ImGuiIO& io = ImGui::GetIO();
    g_began_this_frame = false;

    const int count = InvokeInt(g_m_touch_count, nullptr);
    g_touch_count = count;

    static int dbg_last = -2;
    if (count != dbg_last) {
        int raw_val = 0, hdr_val = 0;
        void* raw_ptr = nullptr;
        InvokeIntDiag(g_m_touch_count, nullptr, &raw_val, &hdr_val, &raw_ptr);
        LOGI("input: touchCount=%d (hdr=%d)", count, hdr_val);
        dbg_last = count;
    }

    if (count <= 0) {
        g_touch_n = 0;
        io.AddMouseButtonEvent(0, false);
        return;
    }

    // Read every finger. The menu only drives ImGui from the first, but the
    // toggles need to see a second finger tapping M1 while the aura holds it.
    const int n = (count > kMaxTouches) ? kMaxTouches : count;
    for (int i = 0; i < n; i++) {
        Touch t{};
        if (!InvokeTouch(g_m_get_touch, i, &t)) continue;
        TouchPoint& p = g_touch_pts[i];
        p.finger_id = t.finger_id;
        p.x = t.pos_x;
        p.y = static_cast<float>(game::ScreenHeight()) - t.pos_y;  // Unity is bottom-left
        p.phase = t.phase;
    }
    g_touch_n = n;

    // ImGui gets the first finger that is not the aura's synthesized hold.
    // Feeding a fixed slot would break the floating buttons the moment the
    // aura armed: its held touch can occupy the slot ImGui reads, and then
    // every tap on a floating button looks like a drag of that hold.
    const TouchPoint* ui = nullptr;
    for (int i = 0; i < n; i++) {
        if (g_touch_pts[i].finger_id == g_ignored_finger) continue;
        ui = &g_touch_pts[i];
        break;
    }
    if (!ui) {
        // Nothing but our own hold: keep ImGui's pointer where it is and let go
        // of the button, so a floating toggle cannot be left stuck down.
        io.AddMouseButtonEvent(0, false);
        return;
    }

    io.AddMousePosEvent(ui->x, ui->y);
    const bool down = (ui->phase == kPhaseBegan || ui->phase == kPhaseMoved ||
                       ui->phase == kPhaseStationary);
    io.AddMouseButtonEvent(0, down);

    if (ui->phase == kPhaseBegan) {
        g_began_this_frame = true;
        g_last_x = ui->x;
        g_last_y = ui->y;
    }
    // A completed tap: Began followed by Ended. The menu's calibration
    // capture reads this position.
    if (ui->phase == kPhaseEnded) {
        g_tap_x = ui->x;
        g_tap_y = ui->y;
        g_have_tap = true;
    }
}

void SetIgnoredFinger(int finger_id) { g_ignored_finger = finger_id; }

int Touches(TouchPoint* out, int max) {
    if (!out || max <= 0) return 0;
    const int n = (g_touch_n < max) ? g_touch_n : max;
    for (int i = 0; i < n; i++) out[i] = g_touch_pts[i];
    return n;
}

bool LastTapPos(float* x, float* y) {
    if (!g_have_tap || !x || !y) return false;
    *x = g_tap_x;
    *y = g_tap_y;
    return true;
}

}  // namespace input
