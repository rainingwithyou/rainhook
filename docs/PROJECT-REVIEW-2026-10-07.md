# Rain Project — Full Workspace Review (2026-10-07)

Reviewer pass over the entire `D:\rain` workspace: source, docs, builds, analysis
artifacts, reference binaries, and the live device state. This is a *current-state*
document — it supersedes the "recommended order" lists in rounds 1–5 where those
items have since landed.

---

## 1. Executive summary

Rain is an AArch64 native overlay + runtime-inspection module for Mobile Legends:
Bang Bang. It loads inside the game's Unity process by replacing
`librudp.bytes` in the game's unsigned `comlibs/` hot-update directory — a
deployment path the game never md5-verifies. Current source is **v38** (built
2026-10-05 00:54, 978,384 bytes) and **is staged live on the physical phone
right now** (`librudp.bytes`, 978,384 bytes, dated 2026-10-04 23:54 on device).

Since the round-1 audit, the source has been substantially rewritten: the touch
ABI bug is fixed, a process_vm_readv lifetime guard protects every entity read,
match-scoped state resets on manager change, the overlay init is retryable, the
drone view uses VYNIX's parameter-write model, and a full combat-automation
suite (retribution, aim, skill aim, combo, Kimmy hold) plus a nine-category ESP
matrix and minimap upgrade landed.

The three-lineage reverse-engineering body of work (VYNIX trial, XNISH payload,
MLBB internals) remains the deepest part of the repository and is what the
feature set is calibrated against.

---

## 2. Workspace inventory

| Area | Contents | Notes |
|---|---|---|
| `src/` | 21 files, ~9,200 LOC C++17 | CMake + NDK 26.1, arm64-v8a, android-26 |
| `docs/` | 8 analysis documents (rounds 1–5 + RAIN.md + MLBB-INTERNALS) | This file is the current-state layer |
| `build/` | v2–v38 + check/final/probe/verify | v38 = current; ~200 MB stale history |
| `analysis/` | 1,342 files (~400 MB) | Parsers, dumps, screenshots, logcat, phone pulls |
| `refs/game/` | MLBB libs + metadata + APKs | il2cpp/csharp/logic/first blobs |
| `VYNIX LIB TRIAL/` | 32-bit + 64-bit `librudp.bytes` | 10.2 MB / 10.8 MB |
| `XNISH NEW/` | mod payload + Wwise engine | 10.8 MB + 25.9 MB |
| `tools/` | frida-server 16.7.19 (x86_64 + arm64 xz) | arm64 build cannot daemonise under Houdini |
| `third_party/` | imgui 1.91.8 (pinned), GlossHook 1.9.5 | submodules with own .git |

No root `.gitignore`, **no git repository**, no CI, no test suite for the
authored code. Credential previously flagged in `opencode.jsonc` — **file is
gone; nothing to rotate from this tree** (if the key was ever real, rotate it at
the provider anyway).

---

## 3. Architecture (current)

### 3.1 Load path
```
/sdcard/Android/data/com.mobile.legends/files/dragon2017/assets/comlibs/arm64-v8a/librudp.bytes
    → copied by the game to /data/data/com.mobile.legends/app_libs/librudp.so
    → dlopen by MLBB's own loader (not the dynamic linker)
    → Rain constructor + JNI_OnLoad both fire (pthread_once gates double-entry)
```
- No signature check on `comlibs/*.bytes` (rounds 1–5, re-verified).
- `librudp.bytes` is absent from every patch list the game checks (device-verified
  on 2.1.67; observationally true on 2.2.16 where the list is server-delivered).
- `liblogic/libfirst/libcsharp.bytes` **are** hash-covered — never stage there.
- Second load model available: `rinject` (ptrace remote-dlopen) for hot-attach
  without restart; built as a separate target.

### 3.2 Runtime stack
1. **Boot thread** — polls up to 900×200 ms for IL2CPP in `liblogic.so`.
2. **IL2CPP resolution** — parses `/proc/self/maps` for the low-address
   `liblogic.so` mapping, walks program headers + DT_* tables (section headers
   are unmapped on this loader), resolves ~30 runtime functions by hand.
3. **Class binding** — by name across all loaded images, refreshed (HybridCLR
   loads the game assembly late), positive-cache only.
4. **Overlay** — GlossHook inline hook on `eglSwapBuffers` in `libEGL.so`;
   ImGui 1.91.8 GLES3 drawn on the game's own context.
5. **Input** — read-only polling of `UnityEngine.Input.get_touchCount/GetTouch`
   (no Java input hooks); output synthesized through the activity's own
   `dispatchTouchEvent` with `MotionEvent.obtain`.
6. **Game access** — reflection field reads/writes on the Show layer
   (`ShowEntity`, `BattleManager`, `ShowBattleCamera`, `LogicBattleManager`),
   plus a small invoke surface (`Camera.get_main`, transform get/set position,
   `fieldOfView`, `FindObjectOfType`).

### 3.3 Safety model (the round-1 gap, now closed)
Every entity read goes through `process_vm_readv` into a stack copy
(`SafePeek` → `PeekEntity` → `Fill`). EFAULT on freed/unmapped pages instead of
SIGSEGV; field-sanity validation (camp ∈ [0,16], HP in range) rejects
freed-but-mapped garbage. Writes only ever touch the live object after the
peeked copy validates. Match-scoped state (camp, manager pointer) resets when
`BattleManager.Instance` changes.

---

## 4. Feature inventory (v38, source-verified)

### ESP
- Nine-category matrix: Self, Ally Hero, Enemy Hero, Ally Minion, Enemy Minion,
  Turtle, Lord, Buff, Creep — per-category enable, colour, and sub-elements
  (box, health bar, HP text, level tag, name, distance, tracer, portrait icon,
  spell cooldowns).
- Global geometry: 3 box styles (2D corner / outline / glow), 4 health-bar
  positions, name above/below, scale, thickness, glow layers, tracer origin
  (camera or player), per-list distance caps (players + entities, both applied
  before draw).
- Status markers (DEAD / SIGHT / HIDDEN), dead-entity handling with dedicated
  colour, clone tag.
- Objective alerts (Turtle/Lord under attack, HP-drop triggered, fog-proof).

### Minimap
- Own-drawn markers on the minimap rectangle — touches no engine state.
- Portrait icons (drop-in `<hero_id>.png` directory, 10 s rescan), health-tinted
  ring, name labels, independent ally/monster/minion filters, 45° map rotation
  and flip calibration.

### Maphack
- Minimap-visibility + health-bar flags only, rewritten every frame (the game
  recomputes them from its vision logic). Bush-reveal and renderer flags were
  deliberately removed — they crash the client.

### Drone view (VYNIX model)
- `Camera.fieldOfView` zoom-out (no controller interaction) + `SmoothFollow.
  offSetPos` parameter write (VYNIX's per-axis math) with stock-value capture
  and restore; `m_iYMax` raised while active and restored on release; legacy
  transform-lift fallback with read-back guard.

### Combat automation (XNISH-pattern)
- **Shared targeting**: Mode (Players/Monsters/Both) × Priority (Closest /
  Lowest HP / Lowest HP %) × ordered class preference (Minion/Monster/Hero).
- **Basic-attack assist** — tap on valid target, calibrated button.
- **Skill auto aim** — 5 slots (S1–S3, Basic, Spell), smart-cast or drag-aim
  (direction from target screen projection, fixed drag length), CD-gated.
- **Auto combo** — 5-step sequences with Gusion/Benedetta presets, re-arm on
  target exit.
- **Auto retribution** — per-objective toggles (Lord/Turtle/Buff/Litho), trigger
  modes (enemy-nearby / always), predicted-damage model (level + manual jungle
  stacks, tunable coefficients), live estimate in Status.
- **Kimmy press-and-hold** — floating KM button, auto-anchored M1, minion-first
  mode, hysteresis release, own-finger masking from ImGui.

### Infrastructure
- `synth` action queue: Tap / Drag / HoldBegin / HoldMove / HoldEnd / Abort,
  constant `downTime` per gesture, surface→physical pixel conversion, hard
  release wired to every teardown path.
- `persist`: SharedPreferences save/load of the whole config (generated
  per-category keys), dirty-flag writes.
- Skin changer (local player `m_ShowSkinId`, re-applied every ~1 s).
- Status tab: binding state, live entity table, retribution estimate, detection
  surface, hero-name directory help.

---

## 5. Reference-lineage work (unchanged this pass)

| | Rain v38 | VYNIX trial | XNISH mod |
|---|---|---|---|
| Size | 978 KB | 10.8 MB | 10.8 MB (+25.9 MB Wwise cover) |
| Hook | GlossHook 1.9.5 | GlossHook 1.9.2 | Dobby |
| Exports | 2 (JNI_OnLoad/Unload) | 1 | 1 + ~6,700 leaked |
| Game access | Reflection by name (Show + Logic + camera) | Reflection + ACInterface mirroring | XOR-0x7E table + MobaScript bridge |
| Network | none | static curl/OpenSSL, key+device-id license (trial expired 2026-09-26) | Codashop/Telegram key sales |
| Anti-forensics | stripped, minimal exports | XOR-0x5A pool, PAC/BTI, CRC32 self-check | split tables, runtime-decrypted UI |

MLBB internals knowledge base (docs/MLBB-INTERNALS.md + rounds): custom metadata
format (sanity 0xFAB11BAF, version 1024, cumulative-end sections, module-tagged
indices), three modules (logic/first/csharp), `ShowEntity` (393 fields),
`LogicBattleManager.m_SelfCampType` (authoritative camp), MTTDProto protocol
(857 `Cmd_*`), reporter/telemetry surface, and the server-side watch items
(`Cmd_ClientBattleMapInfoReport`, `BattleInputBehaviorReporter`).

---

## 6. Findings audit — round 1 → now

| # | Round-1/2 finding | Status |
|---|---|---|
| 1 | `Touch` struct missing `m_RawPosition` (8-byte shift) | **Fixed** — `input.cpp` carries raw_x/raw_y + type, verified offsets |
| 2 | Managed-object lifetime unprotected | **Fixed** — process_vm_readv peek boundary + field sanity validation |
| 3 | Local camp persists across matches | **Fixed** — reset on `BattleManager.Instance` change; Logic-layer source validated before latch |
| 4 | `ent_max_distance` not implemented | **Fixed** — applied in `DrawEntities` (and before tracer) |
| 5 | Overlay init one-shot, not recoverable | **Fixed** — retryable init, context destroyed on failed retry |
| 6 | EGL context recreation unhandled | **Fixed** — surface-change detection → backend shutdown + re-init |
| 7 | Render-thread attach process-global | **Fixed** — `thread_local` attach |
| 8 | IL2CPP API validation partial | **Partially fixed** — core subset validated at Init; non-core functions still called unguarded (each has a null check at call site in practice) |
| 9 | Release binaries contain debug sections | **Fixed** — POST_BUILD strip; v38 readelf shows no .debug/.symtab |
| 10 | Stale docs / build-path drift | **Partially fixed** — RAIN.md documents v38 features but build examples still say `build/r` |
| 11 | Credential in `opencode.jsonc` | **Resolved** — file removed from tree |
| 12 | No git / .gitignore | **Still open** — no repo, no ignore rules |

Round-2/3 open items also resolved: camp garbage latching (logic camp now
cross-validated against live entities before acceptance), drone flicker
(parameter-write model), ImGui ID conflicts (unique suffixes), menu position
clamp, tracer projection (raw Unity matrices, ~13 px accuracy).

---

## 7. Open items and risks (current)

1. **Icon mapping not deployed.** `analysis/vynix_icon_map.py` exists and the
   VYNIX registration-order artifacts (`vynix_reg_order.txt`,
   `vynix_names_seq.txt`, `vynix_heroes.txt`, `contact_sheet.jpg`) are produced,
   but no `rain_icons/` directory exists on the phone. The ESP/minimap icon
   features are code-complete and fall back to dots — the mapping needs a run +
   push to be live.
2. **IL2CPP API capability state** — remaining from round 1: validate every
   resolved function (or degrade per-feature) rather than the core subset.
3. **Server-side watch item** — patch-list coverage of `librudp` is
   server-delivered on 2.2.16; a server change could add the md5. Nothing in the
   client can prevent this; it's a deployment-lifetime question.
4. **Input-pattern telemetry** — `BattleInputBehaviorReporter` watches tap
   cadence; every automation feature generates synthetic input. They ship
   default-off with individual toggles, which is the right posture, but this is
   the one detection surface Rain's features genuinely touch.
5. **`git init` + `.gitignore`** — still absent. ~200 MB of stale build dirs and
   the 400 MB analysis tree should be ignored, not committed.
6. **Doc drift** — `docs/RAIN.md` build section still references `build/r`.
7. **v38 build provenance** — `build/v38/` has no CMakeCache; the binary was
   copied in. The staged device file (978,384 bytes) matches v38's size, so the
   phone is running current code, but the exact rebuild path for reproducing
   v38 from `src/` should be confirmed (likely `build/rain` or `build/verify`).

---

## 8. Recommendations (ordered)

1. Run `vynix_icon_map.py` → push `rain_icons/` → verify portraits on phone
   (Zilong id 16 / Kimmy id 71 are the calibration anchors).
2. `git init`, root `.gitignore` (build/, analysis/, refs/, *.zip, *.apk,
   device pulls), first commit of `src/` + `docs/` + `third_party/` pins.
3. Finish IL2CPP API validation — a capability bitmask checked once, features
   degrade individually.
4. Update `docs/RAIN.md` build/deploy examples to the current versioned path
   and `rinject` flow.
5. Reproduce v38 from clean `src/` in a fresh build dir; record the exact
   command line in RAIN.md.
6. Optional, in value order: battle-record reader (VYNIX parity, offline, zero
   in-game risk), spell-CD calibration against a live match, box-height
   eyeball pass on jungle units.

---

*Review produced 2026-10-07 against workspace state at v38, phone staged, MLBB
2.2.16.12322 installed, ADB device `5d39a8f2` (Redmi 2607DRA18G, Android 16).*
