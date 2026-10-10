#pragma once
// Per-hero ESP icons.
//
// Hero portraits are loaded from a drop-in directory at startup — files named
// <hero_id>.png (16.png = Zilong, 71.png = Kimmy, ...) — decoded with
// stb_image and uploaded as GL textures. No art is embedded and no id->icon
// mapping is guessed: the Status tab shows live entity ids, and the mapping
// is calibrated by dropping files named for them. The directory lives in the
// game's own external files dir, so adb push reaches it without root.

#include <imgui.h>

namespace icons {

// Scan the icon directory and upload textures. Called from the render hook
// once the GL context exists; retries until the directory is readable.
bool Init();
bool Ready();

// Texture for a hero config id, or null when no icon is calibrated for it.
ImTextureID IconFor(int hero_id);

}  // namespace icons
