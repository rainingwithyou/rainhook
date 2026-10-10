#pragma once
// Overlay visibility + the notification toggle.
//
// A tiny dex (built from RainNotify.java, embedded in this library) is loaded
// through an InMemoryDexClassLoader with the game's own class loader as
// parent. It registers a broadcast receiver on the game's process and posts a
// notification with a "Hide UI" / "Show UI" action, so the overlay can be
// switched without opening the menu (useful when the menu itself is what you
// want out of the way).
//
// The class loader is captured in JNI_OnLoad — an app class like this one is
// invisible to the system loader that FindClass would use from a native
// thread (the F1 finding). Everything here runs with the game's identity.

namespace notify {

// Called from JNI_OnLoad, where the app's class loader is available. Stores
// the loader and loads the embedded dex. Safe to call once; later calls are
// ignored.
void CaptureClassLoader(void* jni_env, void* java_vm);

// Called from the boot thread once the game's Context can be resolved.
// Constructs RainNotify, registers the receiver and posts the notification.
// Retries internally; returns true once installed.
bool Install();

// Called once per frame from the render thread. Cheap: a cached static-method
// call, and only while installed.
void Tick();

// The overlay visibility the notification controls. The renderer asks this
// before drawing anything.
bool UiVisible();

}  // namespace notify
