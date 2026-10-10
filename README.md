# Rain

An AArch64 native overlay and runtime-inspection module for Mobile Legends:
Bang Bang, loaded through the game's own hot-update native-module path.

Rain draws an ImGui overlay on the game's OpenGL ES context, reads game state
through the IL2CPP reflection API by name (no hardcoded offsets), and drives a
full feature set — ESP, minimap markers, drone view, maphack flags, and a
combat-automation suite — while deliberately patching **nothing** in the
anti-cheat surface.

```
librudp.bytes (the game's unsigned comlibs slot)
  -> app_libs/librudp.so  (copied by the game's own loader)
  -> dlopen by MLBB's loader (not the dynamic linker)
  -> constructor + JNI_OnLoad
  -> boot thread: resolve liblogic.so via /proc/self/maps
       walk PHDR -> PT_DYNAMIC -> DT_SYMTAB / DT_GNU_HASH by hand
  -> eglSwapBuffers hook (GlossHook) -> ImGui GLES3 overlay
  -> one entity snapshot per frame, every read through a process_vm_readv
     peek boundary
```

## Features

* **ESP** — nine entity categories (self, ally/enemy hero, ally/enemy minion,
  turtle, lord, buff, creep), per-category enable/colour/sub-elements (box,
  health bar, HP text, name, level tag, distance, tracer, portrait icon,
  spell cooldowns), three box styles, tracer origins, distance caps.
* **Minimap** — own-drawn markers with hero portraits, health ring, name
  labels, independent ally/monster/minion filters, 45-degree map rotation and
  flip calibration. Touches no engine state.
* **Maphack** — minimap-visibility and health-bar flags only (the flags the
  engine tolerates being rewritten; the crash-prone ones were removed).
* **Drone view** — FOV zoom-out plus the `SmoothFollow.offSetPos` parameter
  hook, restored to stock on release.
* **Combat automation** — shared targeting policy, basic-attack assist, skill
  auto aim (5 slots, smart-cast or drag-aim), auto combo, auto retribution
  with a predicted-damage model, and the Kimmy press-and-hold.
* **Notification toggle** — an ongoing notification posted under the game's own
  identity with a **Hide UI / Show UI** button, so the overlay can be switched
  without opening the menu.
* **Config profiles** — plain JSON files under
  `/sdcard/Android/data/com.mobile.legends/files/rain_configs/`, managed from
  the Settings tab, editable by hand, pullable over ADB.

## Layout

```
src/
  CMakeLists.txt           NDK build (arm64-v8a, android-26)
  rain.map                 version script: exports only JNI_OnLoad/JNI_OnUnload
  src/
    rain.cpp               entry point (constructor + JNI_OnLoad)
    il2cpp.{h,cpp}         IL2CPP runtime resolution + reflection helpers
    game.{h,cpp}           MLBB battle model (ShowEntity / BattleManager / camera)
    input.{h,cpp}          touch input via UnityEngine.Input
    overlay.{h,cpp}        eglSwapBuffers hook + ImGui/GLES rendering
    esp.{h,cpp}            ESP renderer
    maphack.{h,cpp}        visibility flags
    menu.{h,cpp}           ImGui menu
    synth.{h,cpp}          touch synthesis (dispatchTouchEvent)
    persist.{h,cpp}        auto-save profile + boot load
    cfgjson.{h,cpp}        JSON config profiles
    notify.{h,cpp}         notification toggle bridge
    notify_dex.h           generated: embedded dex for the notification
    icons.{h,cpp}          hero portrait loading
    autox.{h,cpp}          combat automation
    target.{h,cpp}         shared targeting policy
    config.h               shared feature config
  notify/
    java/                  RainNotify.java (notification receiver + poster)
    build.sh               javac + d8 -> notify_dex.h
  injector/rinject.cpp     standalone hot-attach injector (ptrace + dlopen)
  probe/raintest.cpp       loader probe
docs/                      reverse-engineering notes and design docs
third_party/               imgui (pinned), GlossHook (vendored), stb
```

## Build

```bash
SDK="C:/Users/RYZEN/AppData/Local/Android/Sdk"
"$SDK/cmake/3.22.1/bin/cmake.exe" -S src -B build/r -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$SDK/ndk/26.1.10909125/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 \
  -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_MAKE_PROGRAM="$SDK/cmake/3.22.1/bin/ninja.exe"
"$SDK/cmake/3.22.1/bin/cmake.exe" --build build/r --target rain
```

Two build constraints matter:

* **libc++ must be static.** Every library MLBB loads from `app_libs` links
  only `liblog/libm/libdl/libc/libandroid`; `libc++_shared.so` is not
  resolvable in that load context and the `dlopen` silently fails.
* **Exports must stay minimal.** The version script keeps GlossHook's symbols
  from leaking.

The notification dex is checked in as `src/src/notify_dex.h`. After editing
`src/notify/java/...`, regenerate it with `bash src/notify/build.sh` (needs a
JDK and the SDK's `d8`).

## Design notes

* **No hardcoded offsets.** Classes and fields resolve by name through the
  IL2CPP reflection API, so a game update that moves fields does not break it.
* **The peek boundary.** Every entity read goes `process_vm_readv` into a stack
  copy, so a freed page returns `EFAULT` instead of taking a SIGSEGV, and
  field-sanity checks reject freed-but-mapped garbage.
* **One snapshot per frame.** `game::Snapshot()` walks each list once and
  every consumer reads that copy.
* **No anti-cheat patching.** Rain reads the game and writes only presentation
  state. It touches none of the anti-cheat surface — no signature forgery, no
  reporter neutralisation. That is a deliberate architectural choice.
* **No input hooks.** Touch *in* polls `UnityEngine.Input`; touch *out*
  synthesizes `MotionEvent`s through the activity's own `dispatchTouchEvent`.

## Docs

| Document | Contents |
|---|---|
| [`TUTORIAL.md`](TUTORIAL.md) | **deployment with a computer (ADB)** |
| [`TUTORIAL-NO-PC.md`](TUTORIAL-NO-PC.md) | **deployment with only the phone** (file manager) |
| `docs/RAIN.md` | build, deploy, feature reference |
| `docs/MLBB-INTERNALS.md` | game module layout, metadata format, entity model |
| `docs/CONFIG-SYSTEM.md` | JSON config profiles |
| `docs/NOTIFICATION-TOGGLE.md` | notification bridge architecture |
| `docs/DEEP-ANALYSIS-*.md` | reverse-engineering rounds |
| `docs/VYNIX-ANALYSIS.md` | reference implementation analysis |
| `docs/TOUCH-FIX-MIUI-GESTURES.md` | MIUI gesture-zone interference fix |

## Quick start

**With a computer:** see [`TUTORIAL.md`](TUTORIAL.md).

```bash
GAMEDIR=/sdcard/Android/data/com.mobile.legends/files/dragon2017/assets/comlibs/arm64-v8a
adb shell "cp $GAMEDIR/librudp.bytes /sdcard/Download/librudp.original.bytes"  # back up
adb push librain-arm64-v8a.so /sdcard/Download/rain.so
adb shell "am force-stop com.mobile.legends"
adb shell "cp /sdcard/Download/rain.so $GAMEDIR/librudp.bytes"
```

**Phone only:** see [`TUTORIAL-NO-PC.md`](TUTORIAL-NO-PC.md). Install
ZArchiver, download `librudp-arm64.bytes` from the releases page, and copy it
into the game's `comlibs/arm64-v8a/` folder (replacing the existing
`librudp.bytes` after backing it up).

Launch the game. Restore with the backup file whenever you want.

## Requirements

* arm64-v8a Android device, Android 8.0+ (tested on Android 16)
* Mobile Legends: Bang Bang installed
* A JDK + Android SDK/NDK to build

## License

Source is provided for research and educational use. Vendored components keep
their own licenses (`third_party/imgui` MIT, `third_party/GlossHook` MIT).
