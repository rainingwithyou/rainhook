# Rain — build, deploy, use

MLBB client modification, built to mirror the VYNIX reference implementation.

## Layout

```
src/                     Rain source
  CMakeLists.txt         NDK build (arm64-v8a)
  rain.map               version script: exports only JNI_OnLoad/JNI_OnUnload
  src/rain.cpp           entry point (constructor + JNI_OnLoad)
  src/il2cpp.{h,cpp}     IL2CPP runtime resolution + reflection helpers
  src/game.{h,cpp}       MLBB battle model (ShowEntity/ BattleManager / camera)
  src/input.{h,cpp}      touch input via UnityEngine.Input
  src/overlay.{h,cpp}    eglSwapBuffers hook + ImGui/GLES rendering
  src/esp.{h,cpp}        ESP renderer
  src/maphack.{h,cpp}    visibility flags
  src/menu.{h,cpp}       ImGui menu
  src/config.h           shared feature config
  src/cfgjson.{h,cpp}    JSON config profiles (write/parse/list/save/load/delete)
  src/persist.{h,cpp}    auto-save profile + boot load
third_party/imgui        v1.91.8 (pinned)
third_party/GlossHook    v1.9.5 by XMDS (AArch64 static lib)
docs/                    reverse-engineering notes
analysis/                metadata parser, dumped class model, extracted assets
refs/                    pulled game libs + metadata
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

Two build constraints matter and are easy to break:

* **libc++ must be static** (`-static-libstdc++`). Every library MLBB loads from
  `app_libs` links only `liblog/libm/libdl/libc/libandroid`; `libc++_shared.so`
  is not resolvable in that load context and the `dlopen` silently fails.
  `ANDROID_STL` alone does not take effect — the flag is forced in
  `CMAKE_CXX_FLAGS_RELEASE` and `CMAKE_SHARED_LINKER_FLAGS`.
* **Exports must stay minimal.** The version script keeps GlossHook's symbols
  from leaking, matching the reference build.

## Deploy

MLBB copies every file in

```
/sdcard/Android/data/com.mobile.legends/files/dragon2017/assets/comlibs/arm64-v8a/*.bytes
```

into `/data/data/com.mobile.legends/app_libs/*.so` and `dlopen`s it, **with no
signature check on those files**. Replacing `librudp.bytes` therefore loads Rain
into the game's Unity process:

```bash
D=emulator-5556
C=/data/media/0/Android/data/com.mobile.legends/files/dragon2017/assets/comlibs/arm64-v8a
adb -s $D push build/r/librain.so /data/local/tmp/rain.so
adb -s $D shell "su -c 'am force-stop com.mobile.legends'"
adb -s $D shell "su -c 'cp /data/local/tmp/rain.so $C/librudp.bytes'"
adb -s $D shell "monkey -p com.mobile.legends -c android.intent.category.LAUNCHER 1"
```

Rain exports only `JNI_OnLoad`, so MLBB's `dlsym` of the original library's
symbols fails and it continues on its fallback path (the game reaches the lobby
normally). If `librudp` turns out to be needed for matchmaking, use
`libmlsdk_logreport.bytes` (telemetry) as the host instead — the load path is
identical and every `app_libs` library lands in the same `:UnityKillsMe` process.

`liblogic.so` / `libfirst.so` / `libcsharp.so` are **not** staged this way; they
come from `app_libs` and are re-provisioned if tampered with, so don't try to
swap those.

### Restore the original

```bash
adb -s $D shell "su -c 'cp /data/local/tmp/orig/librudp.bytes $C/librudp.bytes'"
```

### Removing Rain

Delete `app_libs/librudp.so` and restore `comlibs/.../librudp.bytes` as above.

## Use

Launch the game; a circular **Rain** button appears on the left edge with a
`Rain` watermark top-left. Tap it to open the menu.

The menu is a left sidebar of sections with a scrolling content pane:

* **Visual** — ESP master toggles and shared geometry (box style/scale/thickness,
  health-bar placement, nametag placement, tracer origin, distance caps), then
  the **category matrix**: one row per entity class (Self, Ally Hero, Enemy Hero,
  Ally Minion, Enemy Minion, Turtle, Lord, Buff, Creep) with its own enable
  toggle, colour picker, and per-element switches (box, health bar, HP value,
  display name, level tag, distance, tracer, portrait icon, spell cooldowns).
  Also objective alerts, drone view, and the skin changer.
* **Combat** — shared targeting policy, then basic-attack assist, skill auto
  aim, the Kimmy press-and-hold, auto combo, auto retribution, and calibration.
* **Map** — minimap markers (portraits, health ring, name labels, ally/monster/
  minion filters), minimap calibration, and the engine health-bar flag.
* **Status** — binding state, live entity table, retribution damage estimate,
  objective HP, detection surface, icon directory help, battle records.
* **Settings** — UI scale, watermark, the **config profile manager** (below),
  emergency touch release.

### Config profiles

Every setting lives in a plain JSON file. The auto-save profile is
`default.json`; named profiles sit beside it and are managed from **Settings**:

```
/sdcard/Android/data/com.mobile.legends/files/rain_configs/
  default.json      <- auto-saved ~2 s after any change, loaded at boot
  profile_1.json    <- snapshots you create
  gusion.json       <- a profile_1.json you renamed
```

The files are the interface. Browse, back up, rename or copy them with any
file manager, or over ADB:

```bash
adb pull /sdcard/Android/data/com.mobile.legends/files/rain_configs/
adb push my_aggressive.json \
  /sdcard/Android/data/com.mobile.legends/files/rain_configs/aggressive.json
```

Tap **Refresh** in the Settings tab to pick up files pushed while the game is
running. **Load** applies a profile to the live config and makes it the new
default; **Overwrite** snapshots the current settings over the selected file;
**Delete** removes it; **New profile** snapshots as `profile_N`. Renaming the
file renames the profile.

The schema is one object per feature area, with each module's enable flag and
its settings in the same block:

```json
{
  "version": 1,
  "esp":       { "enabled": true, "box_style": 0, "categories": { "enemy_hero": { "color": [1, 0.25, 0.25, 1] } } },
  "retribution": { "enabled": false, "hp_pct": 20, "stacks": 0 },
  "kimmy":     { "enabled": false, "aim_mode": 0 },
  "skills":    [ { "enabled": false, "range": 12 } ]
}
```

A partial file loads cleanly — fields that are absent keep their current
value, so a hand-written file with three keys is a valid override. A malformed
file is rejected whole; the live config is never half-applied.

### Targeting

Every combat feature selects through one policy, configured once in **Combat →
Targeting**:

* **Class priority** — an ordered list (Minion / Monster / Hero, reorderable).
  The first class with a valid target wins; the priority only breaks ties inside
  that class. This is what makes "minions first, then jungle, then heroes" a
  setting instead of a code change.
* **Mode** — Players Only / Monsters Only / Both (Adaptive).
* **Priority** — Closest Distance / Lowest HP / Lowest HP %.

### Drone view

Two independent controls, both restored to stock when switched off:

* **Zoom Out (FOV)** widens `Camera.fieldOfView`. This has no interaction with
  the camera controller at all and is what makes the view read as a drone.
* **Camera High** writes `SmoothFollow.offSetPos` — the *parameter* the camera
  controller reads to place the camera. Writing the input rather than the output
  is the point: the controller carries the change itself instead of overwriting
  it, which is what made the old transform-writing version flicker.

If `SmoothFollow` cannot be resolved on a build, Camera High falls back to
lifting the live camera transform.

### Kimmy press-and-hold

Holds the basic-attack button while a target is in range, so the game's own
held-attack targeting keeps firing. Active only while your hero is Kimmy
(`m_ID` 71). A floating **KM** button toggles it in-match without opening the
menu. The press is released on target loss (with hysteresis), death,
switch-off, or while the menu is open.

### Skill auto aim

Five slots (Skill 1–3, Basic, Spell), each with its own enable, range, targeting
mode and priority. Two cast modes:

* **Smart-cast** — taps the button and lets the game pick the direction.
* **Drag-aim** — presses the button and drags toward the target's projected
  screen position, which is true directional aiming. The drag length is fixed
  and only the direction is taken from the target, so the same magnitude works
  at any range.

`Skip while on cooldown` gates on the entity's own `m_ShowCoolDownComp`.

### Auto retribution

Per-objective toggles (Lord, Turtle, Buffs, Litho — all four now actually
read), a trigger mode (enemy-nearby-only or always), and a predicted-damage
model: fire when objective HP is at or below `base + per-level·(level−1) +
per-stack·jungleStacks`, scaled by a safety factor. Hero level is read live from
the local entity; jungle stacks are manual, because that field is not readable
on this build. The Status tab shows the live estimate so the coefficients can be
calibrated in practice mode.

### Minimap

Markers are drawn by Rain onto the minimap rectangle, touching no engine state —
so they work through fog and cannot crash the client. Per-marker content:
portrait icon (`rain_icons/<hero_id>.png`, falling back to a dot), health-tinted
ring, name label, and independent filters for allies, monsters and minions.

To supply portraits, see `analysis/vynix_icon_map.py` (extracts VYNIX's embedded
icon set in registration order and builds a `rain_icons/` directory), or just
drop `<hero_id>.png` files into

```
/sdcard/Android/data/com.mobile.legends/files/rain_icons/
```

Rain rescans that directory every 10 s, so newly dropped icons appear without
restarting the game.

## Design notes

* **No hardcoded offsets.** Classes and fields are resolved by name through the
  IL2CPP reflection API, so a game update that moves fields around does not break
  Rain. `game: ShowEntity fields …` in logcat prints the resolved offsets.
* **No input hooks.** Touch comes from polling `UnityEngine.Input.get_touchCount`
  / `GetTouch`, which avoids hooking the Java MotionEvent path.
* **Rendering** hooks `eglSwapBuffers` in `libEGL.so` via GlossHook and draws
  ImGui on the game's own GL context before presentation.
* **One entity snapshot per frame.** `game::Snapshot()` walks each entity list
  once and every consumer in the frame reads that copy — ESP, minimap,
  targeting, the Status table. Objective HP tracking rides the same walk, so
  auto-retribution keeps working with Entity ESP switched off. Previously each
  consumer did its own walk, paying the same per-entity `process_vm_readv` six
  times a frame.
* **Touch synthesis** (`synth`) is a step queue plus a sustained-press latch, not
  a fire-and-forget tap. `Tap`, `Drag`, `HoldBegin/HoldMove/HoldEnd`, and
  `Abort` (the hard release wired to every teardown path so a press can never be
  left stuck). `downTime` is held constant across a gesture — the previous
  version stamped it fresh on every event, which made a drag read as a series of
  new gestures.

Four runtime details are load-bearing and non-obvious:

1. `il2cpp_runtime_invoke` **boxes value-type returns**. The returned pointer is
   an object whose 16-byte header precedes the payload; reading at offset 0 gives
   the class pointer. All value-type reads add `0x10`.
2. The render thread must be **attached** to the IL2CPP runtime
   (`il2cpp_thread_attach`) before any `runtime_invoke`. Invoking from an
   unattached thread faults on a null thread state and kills Unity's GfxDevice
   worker, which blacks out the screen.
3. MLBB loads the game assembly **after** startup (HybridCLR), so the image list
   and class lookups must be re-tried, and negative lookups must never be cached.
4. **Touches are not consumed by the overlay.** Rain reads the touch stream to
   drive ImGui but does not swallow it, so tapping a menu control also sends that
   tap to the game underneath. This is the cost of the no-input-hook design; put
   the launcher and menu over dead screen areas.

## Hero names

`game.cpp` carries the full roster keyed by `m_ID`, derived from VYNIX's
embedded name list (125 heroes in exact release order) with the mapping
`heroId = releaseIndex + 6`. Two entries are live-verified: Zilong sits at
release index 10 and reports `m_ID` 16, Kimmy at 65 and reports 71. The nametag
prefers the game's own `m_RoleName` string at runtime; the table is the fallback
when that string is empty.

## Status

Verified working: library load, IL2CPP API resolution, class/field binding
(including `Assembly-CSharp.dll` classes), Unity camera binding, ImGui overlay
rendering, touch input, the full sidebar menu, ESP categories, minimap markers,
targeting, and the automation features on the lobby side.

Config persistence: **file-based and verified live** — the auto-save profile
writes within ~2 s of a change, loads at boot, and the Settings-tab profile
manager's save/load/delete all work end-to-end, including loading a profile
pushed over ADB while the game is running. (This replaced a JNI
SharedPreferences bridge that could never bind on this load model and had a
latent abort in its colour-save path.)

Not yet verified live in a match: ESP geometry, the drone offset path, the
Kimmy press-and-hold, drag-aim, and the retribution damage model — all of which
need a battle scene, since battle entities do not exist in the lobby.

