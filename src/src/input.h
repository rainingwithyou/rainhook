#pragma once
// Touch input.
//
// Rather than hooking the Android input stack (fragile: MLBB's touch path runs
// through Java MotionEvents into Unity, and the relevant natives are registered
// dynamically), we poll Unity's own input state through IL2CPP reflection:
//   UnityEngine.Input.get_touchCount()
//   UnityEngine.Input.GetTouch(int) -> Touch
// This is version-tolerant and needs no hooks.

// One live touch, converted to the overlay's coordinate space.
struct TouchPoint {
    int finger_id;
    float x, y;    // top-left origin, matching ImGui and screen pixels
    int phase;     // 0 began, 1 moved, 2 stationary, 3 ended, 4 canceled
};

namespace input {

bool Init();

// Feed the current touch state into ImGui for this frame. Coordinates are
// converted from Unity's bottom-left origin to ImGui's top-left origin.
void Pump();

// Every live touch this frame, not just the first. The menu only ever needed
// one finger, but the in-match toggles have to tell the player's finger apart
// from a synthesized hold — and while the Kimmy aura is pressing M1, the
// player's own tap arrives as a second touch.
int Touches(TouchPoint* out, int max);

// Finger id to keep away from ImGui. The aura's synthesized hold occupies a
// touch slot; if that slot happened to be the one ImGui reads, the floating
// buttons would stop responding the moment the aura armed. -1 = none.
void SetIgnoredFinger(int finger_id);

// True while a finger is down, used to detect the menu gesture.
bool Touching();

// The last completed tap's screen position (top-left origin). Returns false
// when no tap has been seen yet — used by the menu's calibration capture.
bool LastTapPos(float* x, float* y);

}  // namespace input
