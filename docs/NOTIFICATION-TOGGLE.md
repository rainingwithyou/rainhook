# Rain — Notification Toggle (v43)

**Date:** 2026-10-10
**Build:** v43 (`build/v43/librain.so`, md5 `d514570bf41923c0c9e5e5eefb41abcb`)
**Status:** implemented, deployed, verified live on the phone.

---

## What it does

Rain posts an ongoing notification under **MLBB's own identity**:

```
Rain
Overlay: visible            [ Hide UI ]
```

Tapping **Hide UI** makes every Rain visual disappear from the game — watermark,
launcher button, ESP, minimap markers, alerts, the whole overlay. The engine-side
features (maphack writes, drone offset, skin re-apply, combat automation, config
auto-save) keep running, because the toggle is about what is drawn, not about
turning the mod off. Tap **Show UI** to bring it all back.

The notification text and button label track the state, so the shade always
shows which mode you are in.

## Why it is posted as MLBB

The dex is loaded into the game's process with the game's Context, so every
call runs with MLBB's identity: `pkg=com.mobile.legends`, and the notification
header reads "Mobile Legends: Bang Bang". No separate app, no extra icon, no
extra permission — MLBB already holds `POST_NOTIFICATIONS` (verified:
`granted=true`).

## Architecture

```
src/notify/java/com/rain/mod/RainNotify.java   the Java source
        │  javac + d8 (build.sh)
        ▼
src/src/notify_dex.h                           3.7 KB dex, embedded as a byte array
        │  linked into
        ▼
librain.so
        │  at runtime, in JNI_OnLoad:
        │    FindClass("com/unity3d/player/UnityPlayer")   <- app class, visible here
        │    -> getClassLoader() -> global ref (the app loader)
        ▼
        │  on the boot thread, once the activity exists:
        │    new InMemoryDexClassLoader(ByteBuffer(dex), app_loader)
        │    -> loadClass("com.rain.mod.RainNotify")
        │    -> new RainNotify(activity) -> install()
        ▼
        │  install(): registerReceiver(TOGGLE_UI) + post notification
        ▼
  button -> broadcast -> receiver flips a static -> native reads it per frame
```

Key points:

* **Nothing is written to disk.** The dex lives in the library's rodata and is
  handed to `InMemoryDexClassLoader` as a direct ByteBuffer. No file extraction,
  no APK modification.
* **The classloader is captured in `JNI_OnLoad`.** That is the one place the
  app's loader is on the stack; `FindClass` there sees application classes.
  This is the F1 finding applied — the previous JNI bridges failed precisely
  because they called `FindClass` from bare native threads. There is also a
  fallback that resolves the loader through `ActivityThread.currentApplication()`
  (a boot class, visible from any thread) if `JNI_OnLoad` was not on the stack.
* **The receiver is internal.** The action targets the package
  (`setPackage`), the receiver is registered `RECEIVER_NOT_EXPORTED` on API 33+,
  and the PendingIntent is `FLAG_IMMUTABLE`. No other app can drive the overlay.
* **Fail-open.** If the notification never installs (dex rejected, loader
  missing), `UiVisible()` returns true and the overlay behaves exactly as
  before. The feature can fail without taking the mod with it.
* **Re-post on a timer.** The game cancels notifications on some scene
  transitions; the render tick re-posts once a minute if needed
  (`ensurePosted`).

## What stays running while hidden

| Runs while hidden | Why |
|---|---|
| `maphack::Tick` | engine flag writes, gameplay state |
| `game::ApplyDrone` | camera offset, gameplay state |
| `game::TickSkin` | re-applies the skin, gameplay state |
| `autox::Tick` | combat automation |
| `synth::Tick` | touch queue |
| `persist::Tick` | config auto-save |
| entity snapshot / objective tracking | feeds the automation |

Only the draw calls are gated: menu, launcher, ESP, watermark, floating
buttons. That split is deliberate — hiding the UI should not disable your
auto-retribution.

## Rebuilding the dex

```bash
bash src/notify/build.sh          # regenerates src/src/notify_dex.h
# then rebuild librain.so as usual
```

The script needs a JDK and the Android SDK's `d8` (build-tools 35). The
generated header is checked in, so a normal build does not need Java.

## Verification (live, 2026-10-10)

| Step | Evidence |
|---|---|
| Loader captured | `notify: app class loader captured` |
| Dex loaded, receiver registered, posted | `notify: installed — notification button toggles the overlay` |
| Notification posted as MLBB | `pkg=com.mobile.legends id=31249 channel=rain_overlay actions=1` |
| Shade shows it | "Rain · Overlay: visible" + "Hide UI" button |
| **Tap Hide UI** | notification flips to "Overlay: hidden" / "Show UI"; **watermark and launcher gone from the game** (screenshot diff) |
| **Tap Show UI** | notification flips back; **watermark returns** |
| Build determinism | regenerating the dex header yields the same `librain.so` md5 |
| Hygiene | 7 deps, BIND_NOW, 2 exports, stripped |

## Files

| File | Change |
|---|---|
| `src/notify/java/com/rain/mod/RainNotify.java` | new — receiver + notification |
| `src/notify/build.sh` | new — javac + d8 + header generation |
| `src/src/notify_dex.h` | new — generated, embedded dex |
| `src/src/notify.{h,cpp}` | new — JNI bridge (loader cache, dex load, install, poll) |
| `src/src/rain.cpp` | capture loader in `JNI_OnLoad`; install on boot |
| `src/src/overlay.cpp` | gate the draw calls on `notify::UiVisible()` |
| `src/CMakeLists.txt` | `notify.cpp` added |
