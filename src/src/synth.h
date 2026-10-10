#pragma once
// Touch synthesis through the app's own input pipeline.
//
// Gestures are built as MotionEvents and handed to the Activity's
// dispatchTouchEvent — the same path real touches travel, so the game's UI and
// Unity's input see them as genuine touches. Used by the automation features
// (retribution, aim assist, skill aim, combos, the Kimmy press-and-hold);
// nothing is sent while the features are off.
//
// One pointer (finger 0) at a time. Discrete gestures (Tap/Drag) expand into a
// step queue dispatched across frames; a sustained press (HoldBegin/HoldMove/
// HoldEnd) is a separate latch so it can outlive the queue. Abort() is the hard
// release: it drops every queued step and lifts a finger that is down, so no
// caller can leave the screen stuck held.

namespace synth {

// Resolve UnityPlayer.currentActivity and MotionEvent.obtain. Safe to call
// repeatedly; retries until Unity is up.
bool Init();
bool Ready();

// Tell synth the size of the coordinate space its callers work in. Callers
// pass positions that came from Unity's own touch list (surface pixels), but
// dispatchTouchEvent expects physical display pixels, and the game renders at
// its own resolution and upscales. Without this the synthesized touch lands at
// the wrong point on screen. Call once per frame with the EGL surface size.
void SetUnitySize(int w, int h);

// ---- discrete gestures ----
// A tap is DOWN, `hold_frames` frames of nothing, UP. A drag is DOWN, `frames`
// interpolated MOVEs, UP. Both return false when not ready, when a sustained
// hold owns the pointer, or when the step queue cannot take the gesture.
bool Tap(float x, float y, int hold_frames = 4);
bool Drag(float x0, float y0, float x1, float y1, int frames = 6);

// ---- sustained press ----
// HoldBegin presses and leaves the finger down; HoldMove updates the position
// (coalesced — the last call in a frame wins); HoldEnd releases. HoldBegin
// returns false if a hold is already active or a discrete gesture is still in
// flight (one pointer).
bool HoldBegin(float x, float y);
void HoldMove(float x, float y);
void HoldEnd();

bool Holding();
bool Busy();

// Hard release: drop every queued step and lift the finger if it is down.
// Safe to call at any time, a no-op when idle. Wired to feature toggles, match
// teardown and lifecycle events so a press can never survive its owner.
void Abort();

// Per-frame dispatch of queued events. Called from the render hook.
void Tick();

}  // namespace synth
