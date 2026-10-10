#pragma once
// Automation features: auto retribution, basic-attack assist, skill auto aim,
// the Kimmy press-and-hold, and skill combos. All ride the touch-synthesis core
// — the game sees genuine touches through its own dispatchTouchEvent. Every
// feature is off by default and needs calibrated button positions before it can
// act; the menu carries the calibration UI and the positions are persisted.
//
// Targeting is shared: every feature selects through target::Select, so the
// mode / priority / class-order controls mean the same thing everywhere.

namespace autox {

// Per-frame logic. `menu_open` suppresses the sustained press so the player
// cannot be holding the attack button while dragging menu widgets around.
void Tick(bool menu_open);

// Release every synthesized gesture. Safe to call at any time; wired to match
// teardown and lifecycle events so a press can never be orphaned.
void ReleaseAll();

// Floating in-match toggle button (the reference mod's "KM"), so the Kimmy
// hold can be switched without opening the whole menu.
bool KimmyButtonOpen();
void SetKimmyButtonOpen(bool open);

}  // namespace autox
