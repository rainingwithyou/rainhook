#pragma once
// OpenGL ES overlay.
//
// Hooks eglSwapBuffers in libEGL.so and renders an ImGui frame on top of the
// game's back buffer right before it is presented. The hook runs on the game's
// own render thread, so the EGL context is already current and no context
// switching is required.

namespace overlay {

bool Install();     // installs the hook; rendering begins on the next swap
bool IsInstalled();

// Set by Install/Hook when something failed, for reporting on screen.
const char* LastError();

}  // namespace overlay
