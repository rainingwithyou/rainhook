# Rain Project and MLBB Analysis Report

Date: 2026-09-29
Workspace: `D:\rain`
Target package: `com.mobile.legends`
Target version: `2.1.67.11733` (`versionCode 211811733`)

## Executive Summary

Rain is an AArch64 native overlay and runtime-inspection module intended to load inside Mobile Legends: Bang Bang through the game's external hot-update native-module path. It uses:

- A constructor-based bootstrap thread.
- Manual ELF symbol resolution for the IL2CPP runtime in `liblogic.so`.
- Name-based IL2CPP class and field discovery.
- GlossHook-based `eglSwapBuffers` interception.
- ImGui and OpenGL ES rendering.
- Unity input polling through `UnityEngine.Input`.
- Direct reads and writes of managed game objects for visual and camera-related features.

The installed game snapshot matches the checked-in `refs/game` libraries and metadata byte-for-byte. The current Rain binary also matches `build/v26/librain.so` and is the file staged as the live `librudp` replacement during the emulator session.

The project successfully demonstrated library loading, overlay initialization, IL2CPP image discovery, class binding, camera binding, and basic input discovery. The most important confirmed source defect is the incorrect native layout used for `UnityEngine.Touch`; `m_RawPosition` is missing, shifting all later fields and making drag/held input unreliable.

The highest general reliability risk is direct, unsynchronized access to managed objects and collections while Unity and the game logic can destroy or replace them. This can produce stale pointers, invalid reads, invalid writes, or crashes during scene transitions and respawns.

No source files were changed while producing this report. The emulator was used for read-only inspection and artifact acquisition. No installed APK, game library, metadata file, account file, or login data was modified.

## Scope and Evidence

### Local project areas reviewed

- `src/`: authored Rain C++ source and CMake build.
- `docs/`: project design and reverse-engineering notes.
- `analysis/`: parsers, binary analysis, screenshots, logs, and generated reports.
- `refs/game/`: extracted MLBB libraries and metadata.
- `refs/vynix/`: VYNIX reference binaries.
- `VYNIX LIB TRIAL/`: original VYNIX 32-bit and 64-bit samples.
- `XNISH NEW/`: XNISH payload and audio-library samples.
- `third_party/GlossHook/`: hook library and headers.
- `third_party/imgui/`: pinned ImGui source and examples.
- `build/`: historical and current Rain binaries.

### Emulator evidence

The connected device was `emulator-5556`.

```text
Android version: 9
Reported ABI: x86_64,x86,arm64-v8a,armeabi-v7a,armeabi
Game primary ABI: arm64-v8a
Game process: com.mobile.legends:UnityKillsMe
Surface observed: 1440x900
```

The current read-only device snapshot is stored locally at:

`analysis/device_2026-09-28/rain_deep_2026-09-28/`

It contains the installed APK splits, private hot-loaded native libraries, public `.bytes` modules, and three real metadata files. The snapshot is approximately 329 MB.

## Workspace Inventory

Approximate local workspace contents at the time of analysis:

| Area | Files | Approx. size |
|---|---:|---:|
| `src` | 19 | 109 KB |
| `docs` | 3 | 22 KB |
| `analysis` | 617 | 390 MB |
| `refs` | 70 | 408 MB |
| `build` | 894 | 314 MB |
| `third_party/imgui` | 277 | 11 MB |
| `third_party/GlossHook` | 40 | 1.5 MB |
| `tools` | 2 | 66 MB |
| `XNISH NEW` | 2 | 35 MB |
| `VYNIX LIB TRIAL` | 2 | 20 MB |

The complete workspace is roughly 1.34 GB. Most of the size is generated output, screenshots, extracted game binaries, historical builds, APKs, metadata, and vendor sources.

There is no root `.gitignore`, no project test suite, and no CI configuration for the authored Rain code. Historical build directories contain many stale binaries and should be separated from release artifacts.

## Rain Architecture

### Bootstrap and loader behavior

The bootstrap is implemented in [`src/src/rain.cpp`](D:\rain\src\src\rain.cpp).

The module starts from both `JNI_OnLoad` and an ELF constructor. The constructor is important because the game's custom loader may use native `dlopen` behavior without invoking the JNI lifecycle entry point.

The boot thread:

1. Polls for the IL2CPP runtime in `liblogic.so`.
2. Attaches the boot thread to the IL2CPP domain when possible.
3. Installs the EGL swap hook.
4. Leaves battle-class discovery to repeated per-frame initialization.

The poll loop can wait for up to 900 attempts at 200 ms each, approximately three minutes.

### IL2CPP resolution

[`src/src/il2cpp.cpp`](D:\rain\src\src\il2cpp.cpp) reads `/proc/self/maps`, locates the low-address `liblogic.so` mapping, parses the ELF program headers and dynamic table, and resolves exported IL2CPP functions without relying on `dlsym`.

The code handles the game's custom loader model and refreshes the assembly image list because HybridCLR/game assemblies appear after startup. Positive class results are cached; negative class results are intentionally not cached.

The implementation resolves classes and fields by name rather than using fixed offsets. This is a good compatibility strategy for field movement between builds, but it does not eliminate lifetime or thread-safety risks after the managed object has been found.

### Native overlay

[`src/src/overlay.cpp`](D:\rain\src\src\overlay.cpp) hooks `eglSwapBuffers` with GlossHook and renders ImGui on the game's active OpenGL ES context.

The overlay currently:

- Creates an ImGui context.
- Uses the OpenGL ES 3 backend.
- Configures a dark blue visual style.
- Uses a 22 px default font.
- Polls the surface size each frame.
- Attaches the render thread to IL2CPP once.
- Draws the launcher, menu, ESP, maphack, camera modifications, skin writes, and watermark.

The one-shot hook initialization uses a static `tried` flag. If the first surface is invalid or ImGui initialization fails, the code does not retry initialization.

### Game model

[`src/src/game.cpp`](D:\rain\src\src\game.cpp) binds:

- `ShowEntity`
- `BattleManager`
- `ShowBattleCamera`
- `UnityEngine.Camera`

It reads player and monster collections, health, team/camp, positions, visibility flags, camera fields, skin fields, and Unity camera matrices.

The code has two projection paths:

1. Unity camera matrix invocation.
2. A fallback view-projection matrix derived from `ShowBattleCamera` position, rotation, FOV, and screen dimensions.

### Visual and camera features

[`src/src/esp.cpp`](D:\rain\src\src\esp.cpp) includes:

- Player ESP.
- Self ESP.
- Ally filtering.
- Minion filtering.
- Box, outline, and glow rendering.
- Health bars in four positions.
- Hero and level labels.
- Distance labels.
- Status labels.
- Screen-edge clamped tracer lines.
- Monster/entity rendering.
- Minimap marker rendering.

[`src/src/maphack.cpp`](D:\rain\src\src\maphack.cpp) rewrites selected client-side visibility flags each frame when enabled.

Camera and skin writes are implemented in `game.cpp`:

- FOV writes to `ShowBattleCamera.m_iFieldOfView`.
- Drone view writes to the camera position Y coordinate.
- Skin changer writes to `ShowEntity.m_ShowSkinId`.

## Confirmed Rain Findings

### 1. Touch layout is incorrect

Rain defines `Touch` in [`src/src/input.cpp:23`](D:\rain\src\src\input.cpp:23) as:

```cpp
int32_t finger_id;
float pos_x, pos_y;
float delta_x, delta_y;
float delta_time;
int32_t tap_count;
int32_t phase;
```

The exact current metadata defines `UnityEngine.Touch` as:

```text
m_FingerId
m_Position
m_RawPosition
m_PositionDelta
m_TimeDelta
m_TapCount
m_Phase
m_Type
m_Pressure
m_maximumPossiblePressure
m_Radius
m_RadiusVariance
m_AltitudeAngle
m_AzimuthAngle
```

`m_RawPosition` is missing from Rain's struct. Consequently, fields after `m_Position` are read at the wrong offsets. In particular, `phase` does not read `m_Phase`.

Observed consequence:

- A tap can appear to work by accident.
- Dragging and sliders can behave incorrectly.
- Held or stationary touches are not reliably represented.
- The issue is build-specific evidence, not speculation.

### 2. Managed object lifetime is not protected

The generic helpers in [`src/src/il2cpp.h:84`](D:\rain\src\src\il2cpp.h:84) directly dereference `base + offset`.

Collection access in [`src/src/game.cpp:195`](D:\rain\src\src\game.cpp:195) reads managed list/array internals directly. Snapshots, camera writes, map visibility writes, and skin writes all operate on raw pointers without a managed lifetime guard.

The code checks whether pointers look numerically plausible, but a plausible pointer can still refer to an object that is being destroyed or whose collection is being resized.

Highest-risk transitions:

- Lobby to battle.
- Battle to results.
- Respawn.
- Entity removal.
- Camera recreation.
- Hot scene reload.

### 3. Local camp state persists across matches

`g_local_camp` and `g_local_camp_strong` are initialized once in [`src/src/game.cpp:49`](D:\rain\src\src\game.cpp:49) and are not reset when the manager or local player changes.

A later match with a different team/camp can cause:

- Inverted enemy and ally colors.
- Incorrect minimap classification.
- Visibility writes against the wrong set of entities.

### 4. Entity distance slider is not implemented

`ent_max_distance` is defined in [`src/src/config.h:43`](D:\rain\src\src\config.h:43) and exposed in [`src/src/menu.cpp:104`](D:\rain\src\src\menu.cpp:104), but `DrawEntityESP()` does not apply it.

The player distance limit is applied in `esp.cpp`, but only after the tracer line is drawn. Therefore, a configured distance limit can still leave tracer lines visible beyond the intended range.

### 5. Overlay initialization is not recoverable

[`src/src/overlay.cpp:152`](D:\rain\src\src\overlay.cpp:152) uses a one-shot `tried` flag around `ImGuiInit()`.

If the first EGL surface is unusable, the context is recreated, or the first ImGui initialization fails, the overlay cannot recover without process restart.

### 6. EGL context recreation is not handled

The ImGui backend is initialized once and there is no explicit context-loss, surface-loss, or backend shutdown/reinitialization path.

This is risky during:

- Activity pause/resume.
- Video playback.
- Native activity transitions.
- Orientation or surface changes.
- Game restart activity transitions.

### 7. Render-thread attachment is process-global

[`src/src/overlay.cpp:102`](D:\rain\src\src\overlay.cpp:102) uses a function-local static boolean for IL2CPP attachment. If the hook executes on more than one thread, only the first thread is attached.

The attachment state should be thread-local and should distinguish a valid attached thread from a process-wide initialization flag.

### 8. Required IL2CPP API validation is incomplete

[`src/src/il2cpp.cpp:241`](D:\rain\src\src\il2cpp.cpp:241) validates only a subset of resolved function pointers. Other functions are called later without equivalent checks.

Failure should be represented as a capability state, not just a partially populated global API structure.

### 9. Release binaries contain debug sections

`build/v26/librain.so` has `.debug_*` and `.symtab` sections even though its dynamic export surface is minimal.

This increases artifact size and exposes implementation metadata. A release strip step should be explicit and verifiable.

### 10. Documentation and artifacts are stale in places

[`docs/RAIN.md`](D:\rain\docs\RAIN.md) references `build/r`, while the workspace currently uses versioned directories such as `build/v26`.

Older binaries also differ in runtime dependencies. Some historical builds depend on `libc++_shared.so`, while `v26` correctly uses static C++ runtime linking.

## VYNIX Analysis

VYNIX is a 32-bit and 64-bit native module pair with identical architecture-specific functionality.

The 64-bit binary contains:

- ImGui/OpenGL ES rendering.
- EGL and Android native window usage.
- Hooking framework code.
- IL2CPP reflection references.
- MLBB class and feature strings.
- TLS/cURL/OpenSSL support.
- Account, license, region, and device-path indicators.
- Embedded visual assets.

Its dependency profile includes `libz.so`, unlike Rain's current direct dependency set. It also retains a much larger dynamic symbol surface and multiple constructors.

The VYNIX samples are identical to the copies in `refs/vynix`, which confirms the local reference set is consistent.

Notable VYNIX strings include:

```text
map_minimap_pos_x
map_minimap_pos_y
map_minimap_width
map_minimap_height
Turtle Under Attack
Lord Under Attack
Jungle
MLBB_SKILL
```

The VYNIX binary also contains remote service and authentication-related strings. These were not reproduced in this report beyond general identifiers.

## XNISH Analysis

The non-`#` XNISH binary is a large mod payload disguised as `libAkSoundEngine.bytes` and has:

- A `librudp.so` SONAME.
- Approximately 6,690 global exports.
- Dobby hook APIs.
- `eglSwapBuffers` references.
- IL2CPP reflection exports.
- `LogicBattleManager` and `ShowEntity` references.
- ESP, drone, minimap, Lord, Turtle, and menu strings.
- Network, TLS, and external-service strings.

The `#` binary is a separate 25.9 MB Wwise-oriented module. It contains MLBB Wwise source paths, audio bridge symbols, and many exported audio/serialization functions. It is not equivalent to the non-`#` payload.

The installed MLBB `libAkSoundEngine.so` is only 2.68 MB and does not match either XNISH file.

## Game Metadata Model

The game's custom metadata format uses:

- Sanity value `0xFAB11BAF`.
- Metadata version `1024`.
- Cumulative-end section ranges.
- Module-tagged indices.

Module tags:

```text
1 = logic
2 = first
3 = csharp
```

The correct parser is [`analysis/meta_extract.py`](D:\rain\analysis\meta_extract.py), which resolves tagged string and table indices across modules.

Important classes:

| Class | Module | Fields | Methods |
|---|---:|---:|---:|
| `UnityEngine.Touch` | logic | 14 | 12 |
| `UnityEngine.Input` | logic | 4 | 67 |
| `UnityEngine.Camera` | logic | 3 | 238 |
| `ShowEntity` | csharp | 393 | 804 |
| `BattleManager` | csharp | 45 | 121 |
| `ShowBattleCamera` | csharp | 31 | 51 |

`BattleManager` exposes the collections Rain uses:

```text
m_LocalPlayerShow
m_LocalShowPlayers
m_dicPlayerShow
m_dicMonsterShow
m_ShowPlayers
m_ShowMonsters
```

`ShowBattleCamera` exposes:

```text
Instance
m_CamPos
m_CamRotVect
m_iFieldOfView
_uCameraPlayerId
m_bCameraMoving
```

## Game Reporting Surface

The managed model contains classes including:

```text
AntiCheatReporter
CheatAfkReporter
Cheat
HeroLoadDetector
PerformanceDetection
DataReportMgr
GameReportMgr
```

The names alone do not prove a specific Rain detection path. They do show that the game contains reporting and performance-monitoring systems capable of tracking:

- Hero and skin loading.
- Position/history state.
- Performance and frame data.
- Battle reports.
- Client telemetry.

Native and managed strings also include Unity debugger, screen-capture, `/proc` inspection, and IL2CPP debugger APIs. These are consistent with a modern game containing diagnostics, reporting, and platform integration components.

## APK and Integrity Observations

The base APK is signed with v1, v2, and v3 schemes, and SourceStamp verification succeeds.

The manifest indicates:

- `android:allowBackup=false`.
- `android:extractNativeLibs=true` in the decoded manifest representation.
- Cleartext traffic enabled.
- A large permission and SDK integration surface.
- External activity, service, receiver, and provider components.
- Main Unity activity in `:UnityKillsMe`.

The APK signature protects the APK contents, but the external hot-update `.bytes` resources and the external metadata are not covered by the APK signature boundary. That external resource boundary is central to the game's loading model and is also the reason Rain can be staged as a replacement module.

## Current Runtime State

During the active portion of the investigation, Rain logged:

```text
overlay: ImGui ready, surface 1440x900
il2cpp: images now 70
input: UnityEngine.Input bound
game: Unity Camera bound
```

The active Unity process remained stable in the reviewed log window, with no observed `SIGSEGV`, fatal exception, or abort message. Later, the emulator returned to the launcher, so no final live-match measurement was attempted.

The following was confirmed before the process stopped:

- Rain loaded into the Unity process.
- IL2CPP symbols were resolved.
- Unity and game classes were found.
- The overlay initialized.
- Touch count calls returned expected values for basic input events.

The following remains unverified in a live match:

- Correct ESP projection under movement and camera rotation.
- Correct monster classification in every mode.
- Correct behavior through respawn and scene transition.
- Correct behavior of camera writes after camera recreation.
- Correct behavior of skin writes for unavailable assets.

## Security and Secret Handling

The project configuration contains a provider credential in [`opencode.jsonc`](D:\rain\opencode.jsonc:10). The credential is intentionally not copied into this report.

Recommended action:

1. Revoke or rotate the credential immediately.
2. Remove it from the working-tree configuration.
3. Load it through an environment variable or secret store.
4. Add a secret scan to CI.
5. Add a root `.gitignore` covering credentials, generated binaries, APKs, dumps, and emulator snapshots.

## Recommended Engineering Order

1. Correct the `UnityEngine.Touch` struct and add a metadata-derived layout test.
2. Add a safe snapshot boundary for managed collections and objects.
3. Reset match-scoped state when `BattleManager.Instance` or the local player changes.
4. Implement the missing entity distance filter.
5. Apply player distance filtering before tracer rendering.
6. Make EGL initialization retryable and handle context recreation.
7. Use thread-local IL2CPP attachment state.
8. Validate every required IL2CPP API capability before calling it.
9. Bound and validate skin IDs and camera values.
10. Strip release binaries and verify dynamic exports/dependencies in a build check.
11. Replace stale deployment paths in `docs/RAIN.md` with a single versioned release path.
12. Separate generated analysis snapshots and historical builds from the source tree.

## Files of Interest

### Rain source

- [`src/CMakeLists.txt`](D:\rain\src\CMakeLists.txt)
- [`src/src/rain.cpp`](D:\rain\src\src\rain.cpp)
- [`src/src/il2cpp.cpp`](D:\rain\src\src\il2cpp.cpp)
- [`src/src/il2cpp.h`](D:\rain\src\src\il2cpp.h)
- [`src/src/game.cpp`](D:\rain\src\src\game.cpp)
- [`src/src/input.cpp`](D:\rain\src\src\input.cpp)
- [`src/src/overlay.cpp`](D:\rain\src\src\overlay.cpp)
- [`src/src/esp.cpp`](D:\rain\src\src\esp.cpp)
- [`src/src/maphack.cpp`](D:\rain\src\src\maphack.cpp)
- [`src/src/menu.cpp`](D:\rain\src\src\menu.cpp)
- [`src/src/config.h`](D:\rain\src\src\config.h)

### Existing documentation

- [`docs/RAIN.md`](D:\rain\docs\RAIN.md)
- [`docs/VYNIX-ANALYSIS.md`](D:\rain\docs\VYNIX-ANALYSIS.md)
- [`docs/MLBB-INTERNALS.md`](D:\rain\docs\MLBB-INTERNALS.md)

### Analysis tools

- [`analysis/meta_extract.py`](D:\rain\analysis\meta_extract.py)
- [`analysis/il2cpp_meta.py`](D:\rain\analysis\il2cpp_meta.py)
- [`analysis/mine_xnish.py`](D:\rain\analysis\mine_xnish.py)
- [`analysis/resolve_plt.py`](D:\rain\analysis\resolve_plt.py)

### Device snapshot

- [`analysis/device_2026-09-28`](D:\rain\analysis\device_2026-09-28)

## Conclusion

Rain, VYNIX, XNISH, and the installed MLBB build form a clearly related but distinct set of native components:

- The original `librudp` is a networking module.
- Rain is a smaller custom overlay/runtime module currently replacing it.
- VYNIX is a larger licensed and feature-rich overlay implementation.
- XNISH contains separate hook-heavy mod payloads and Wwise-based variants.
- MLBB uses three IL2CPP modules, HybridCLR-style external metadata, a custom hot-update loader, and a separate Unity process.

Rain's fundamental architecture is proven to load and bind on this exact game build. Its main remaining risks are not basic symbol discovery; they are input ABI correctness, managed-object lifetime, scene/match state invalidation, EGL recovery, and release-artifact hygiene.
