# Mega-Analysis 2 — 2026-10-11

Second deep pass of the day, five workstreams, one live phone session. Covers:
Rain source (deeper than the first pass), VYNIX LIB TRIAL vs TRIALL (function-
level diff), XNISH NEW (full re-derivation + AC gap analysis), fresh game pull
(APK + metadata + comlibs), and live on-device verification.

Artifacts:
- `analysis/mega2_2026-10-11/rain/DEEP-ANALYSIS.md` — 1,022 lines, full subsystem model
- `analysis/mega2_2026-10-11/vynix/DEEP-ANALYSIS.md` — 749 lines, 64 scripts, 48 JSON outputs
- `analysis/mega2_2026-10-11/xnish/DEEP-ANALYSIS.md` — 637 lines, 195-row stub table
- `analysis/mega2_2026-10-11/game/DEEP-ANALYSIS.md` — 1,115 lines, metadata re-parse
- `analysis/mega2_2026-10-11/phone/` — live screenshots (ban screen, Rain loaded)

---

## 0. The three headlines

1. **synth is dead THREE ways, not one.** The prior "one loader fix restores
   all five combat features" conclusion is wrong. There are three stacked
   defects — wrong `MotionEvent.obtain` signature (the resolved overload does
   not exist), swapped action constants (UP and MOVE are transposed), and the
   loader bug. Fixing any subset still leaves every gesture dead. All three
   verified against `android.jar` (API 35) and the shipped binary.

2. **VYNIX's hook surface is 122 sites, not 40** — and the TRIALL trial
   **expired 2026-10-10 23:59**, i.e. yesterday. The build in `VYNIX TRIALL/`
   is now beyond its licence window.

3. **XNISH touches 2.1% of the AC surface** (11 of 524 AC-relevant classes).
   The input-behavior fingerprinter, the hero-load memory-delta detector, the
   per-frame desync hashing, and the 5-minute content check are all fully
   live. Its local-verdict stubs are narrower than they look.

---

## 1. Rain — deeper findings

Full report: `analysis/mega2_2026-10-11/rain/DEEP-ANALYSIS.md`

### 1.1 New P0s (all verified at the cited lines, all in the shipped binary)

**N1 — `MotionEvent.obtain` signature does not exist.** `synth.cpp:174`
resolves `"(JJFFI)Landroid/view/MotionEvent;"`. android.jar (API 35) has no
such overload — the single-pointer forms are `(JJIFFI)` and `(JJIIFFFI)`.
`GetStaticMethodID` returns null, `Resolve()` returns false forever.
Verified against `platforms/android-35/android.jar` via javap.

**N2 — action constants are swapped.** `synth.cpp:20`:
`enum { A_DOWN=0, A_MOVE=1, A_UP=2 }`. Android is `ACTION_DOWN=0`,
`ACTION_UP=1`, `ACTION_MOVE=2`. Fed straight into `obtain(..., s.act, ...)`:
a tap sends DOWN then MOVE (finger never lifts), a drag sends DOWN then UP
(ends before moving). **`Abort` sends MOVE too — even the "release all touches"
emergency path cannot lift the finger.** The internal latch tracks the enum,
not the wire value, so bookkeeping looks right while the game sees a stranded
pointer.

**N3 — the shipped release binary has ImGui asserts enabled.**
`CMakeLists.txt:20` overrides `CMAKE_CXX_FLAGS_RELEASE` without `-DNDEBUG`;
ImGui defaults `IM_ASSERT` to `assert()`. Verified in
`build/release/librain-arm64-v8a.so`: 184 ImGui assert strings, `__assert2`
imported. Any ImGui contract violation on the render thread aborts the game —
and the NaN-input paths (prior P2s) are live trip wires. One word fixes it.

### 1.2 Other new findings

| ID | Finding | Where |
|---|---|---|
| N4 | `DictEach` stride probe validates entry *shape*, not layout — a wrong stride that parses is accepted, callback gets a mid-entry pointer | `il2cpp.h:175-188` |
| N5 | `SlotReady → DictEach` has no early exit — up to 4096 entries × 256 B `SafePeek` per call, 3×/frame on the render thread | `autox.cpp:350-360` |
| N7 | Menu-open triggers a full 6.7 KB JSON `Emit`+write every 2 s on the render thread, including mid-slider-drag | `menu.cpp`/`persist.cpp` |
| N8 | Icons are *permanently* dead after context recreation (the "only ever adds" rule), not "until next scan" | `icons.cpp:63-67,101` |
| N11 | `rinject` builds its libc path from the start of the maps line — `fopen` can never succeed; the injector cannot resolve `dlopen` | `rinject.cpp:125` |
| N12 | cfgjson has no recursion depth limit — a nested JSON file stack-overflows the game process | `cfgjson.cpp` |
| — | P0-1 scope is wider than reported: `ContainerAt` also reads live container structure — the fix must cover the whole walk | `game.cpp:314-345` |

Two candidate findings were falsified during verification and withdrawn in the
report (the `HoldEnd` strand is unreachable; the `ReleaseAll` latch
disagreement cannot occur on any current path). One prior ambiguity settled:
JNI_OnLoad **does** fire on the comlibs path — confirmed live, again.

### 1.3 Live verification (tonight, fresh install)

Rain v43.3 booted clean on the freshly-reinstalled game (17:15 today):

```
Rain: constructor entry / JNI_OnLoad / app class loader captured
il2cpp: dynamic table ok (n=6998) / API resolved
persist: boot load 'default': ok
notify: installed
overlay: eglSwapBuffers hooked (1)
il2cpp: images now 70 (was 0)
game: ShowEntity fields hp=420 hpMax=424 camp=208 guid=392 id=396 type=120 pos=652 radius=824 minimap=679
```

Every field index matches the documented values exactly. My own taps on the ban
screen appear in the log as `input: touchCount=1→0` — the input *reading* side
works. **Zero `synth:` lines, as predicted — the output side has never run.**

The ban screen (live capture in `analysis/mega2_2026-10-11/phone/`):
"Using a Plug-In Application that Disrupts Competitive Fairness", release
after **10947h ≈ 456 days**, Role ID 994207246 — the same ban, surviving
today's full reinstall, exactly as `ACCOUNT-RESET.md` documents.

---

## 2. VYNIX — deeper findings

Full report: `analysis/mega2_2026-10-11/vynix/DEEP-ANALYSIS.md`

### 2.1 The hook surface is 3× larger than reported

Prior reports counted only the `mov w4,#1` wrapper (mode 1, "the 40"). The
binary has **six GlossHook mode wrappers**:

| Mode | Sites (new/old) | Covers |
|---|---|---|
| 0 | 57/53 | The invisible group: ACInterfaceLib, HandleMsg, MemoryInit/Pool/Alloc/Blk, CheckHook/CheckLib, MobaScriptBridgeIMPL, LogReport family, GetSignMapData/GetPkgMapData, GameServerConfig, UpdateCameraPos, Unity_OnUpdate |
| 1 | 40/40 | The "classic 40" (APKSignature, AntiPluginData, Estimate*, MPassManager) |
| 2 | 14/13 | + **one genuinely NEW hook** |
| 3 | 5/5 | UpdateExp, StartSkill, CheckLib |
| 4 | 3/3 | GetSystemTime, SetUserInfoImpl, LogReport |
| 6 | 3/3 | UpdateGold, InitImpl, LogReportBridge |

**Total: 122 install sites (new) / 117 (old).** Mode 0's target set is
identical between builds; only the wrapper layout was consolidated.

### 2.2 Corrections to prior reports

- **License verdict is a literal string compare, not pure MD5 equality.** Two
  layers: the monitor thread (0x81a5b0) compares the raw HTTP response against
  plaintext `SUCCESS` (new64) / `NECROAJA` (old64); the MD5 A==B check is the
  secondary gate.
- **Systematic address-digit error** in prior reports: `[x24,#0xff4]` +
  `adrp x24,#0xa61000` = `0xa61ff4`, not `0xa610ff4`. Same for pipe fds
  (`0xa568b0`) and CRC32 table (`0xa61b3c`).
- 32-bit pools use 4-byte NUL padding — explains the `Z` artifacts in prior
  32-bit decodes; with padding handled, the 32-bit delta equals the 64-bit
  (+4/−18 strings).

### 2.3 The build diff

Maintenance release: +22,712 B, 95.5% of functions identical. One feature
removed (two large functions deleted, 14.8 KB + 13.6 KB — the ranked-road/
zone/login-country engine behind 17 removed strings), one hook added
(`RankHeroMgr` + `StartClientMatchUIPerformance` @0x8199ac), license marker
rotated (`NECROAJA` → `SUCCESS`), one embedded font dropped, new pipe-copy
helper (`0x7d7ba0`, ≥64 KiB fault-tolerant copy, 37 callers — same idea as
Rain's peek boundary, independently invented). No new anti-analysis or
protections.

### 2.4 Trial expiry — today's problem

`Expired : 10 Oktober 2026, 23:59` (was 26 September in the old build).
**As of today (Oct 11) the TRIALL build is past its window.** The licence
thread enforces it. This makes the `VYNIX TRIALL/` binaries historical
artifacts unless a newer build appears.

---

## 3. XNISH — deeper findings

Full report: `analysis/mega2_2026-10-11/xnish/DEEP-ANALYSIS.md`

### 3.1 Everything re-verified, two refinements

- 195 stub sites re-derived from raw bytes, **0 mismatches** (V0=100
  `movz x0,#0;ret` / V1=95 bare `ret`, chosen by return type).
- 29 DobbyHooks + 1 instrument + 4 resolvers confirmed; 7 of 25 detours are
  one-instruction stubs, 18 full bodies.
- 49 swallowed `SendRawData` IDs confirmed (the bitmask covers 27 bits;
  `cmp #0x1a / b.hi` is inclusive → exactly 49).
- String pool: 1,199 buffers; **329 unique text strings** (prior's 333
  included 4 binary guard objects).
- 365 PNGs confirmed (208 flags, 138 hero/spell avatars, rank emblems,
  2 "LORD/TURTLE IS UNDER ATTACK" banners).

### 3.2 The AC gap analysis — the big new finding

Of 524 AC-relevant classes in the game, XNISH references **11 (2.1%)**.
Fully live despite the mod:

| Class | Methods | What still works |
|---|---|---|
| **BattleInputBehaviorReporter** | 17 | **The primary macro/script fingerprinter — untouched.** Machine-regular cast intervals flow straight to the server |
| **AntiCheatReporter** | 10 | Skill-cast cadence ring buffer |
| **HeroLoadDetector** + perf twin | 85 | Memory-delta skin-injection detection |
| **DataReportMgr** | 95 | The central telemetry transport — **not stubbed** |
| **DynamicMonitorBridge** | 13 | Feeds the input-behavior reporter independently of stubbed senders |
| **ReportDataInFighte** | 116 | In-battle collector |
| **Cmd_Act_CheatConfig_ReportCheatData_CS** | 3 | The 5-minute mid-match content check — no answer path stubbed |
| **DesyncLog** | 158 | Only 2 methods stubbed; **per-frame hashing, Divergency, S3 upload all live** |
| **ACInterface** `EstimateBySwith/If/List`, `EstimateAll` | — | Control-flow estimators not in the stub list — they walk hooked function prologues |

Practical reading: XNISH blinds the *local verdict* and the *log channel*, and
deletes two game modules from the comlibs cache ("Bersihkan Cache"). It does
not defeat the input-behavior, hero-load-memory, desync-hash, or 5-minute
content-check paths.

### 3.3 Audio reality — new

The mod contains **zero audio code** — no Wwise, no OpenSLES, no AAudio, none
of the 1,198 `CSharp_*` SWIG entry points the game's Unity side calls. The
game's own metadata proves it tolerates this (`m_skipInitWiseWhenLoadSoFailed`,
`WwiseUnity dll not found:`) — the game runs with **audio dead**. The user's
25.8 MB cover file is an older unstripped Wwise from a different workspace;
the current game ships a different, newer, stripped 2.69 MB build.

### 3.4 Other new discoveries

- A previously unreported **second pthread worker** (0x19b054) implementing the
  Room-tab player-table collector, gated by the `Tampilan Data` toggle.
- The MD5-material builder is a 33-byte template transform with server strings
  as key stream — explains why offline forgery needs the server's `rng`.
- Complete 247-site resolver surface (44 classes, 235 unique triples),
  including the aimbot write targets (`set_DesPosition`, `set_fieldOfView`,
  `set_Position`).

---

## 4. Game — deeper findings

Full report: `analysis/mega2_2026-10-11/game/DEEP-ANALYSIS.md`

### 4.1 Metadata: byte-identical again, zero field drift

All three metadata blobs md5-match the prior pull. All six type indices
verified, and **every field Rain depends on resolves to the documented name at
the documented index** — ShowEntity 38/38, BattleManager 8/8,
ShowBattleCamera 10/10, LogicBattleManager 6/6. The reinstall changed nothing
that matters to Rain.

### 4.2 DynamicSOLoader — why the drop-in works (code-level proof)

Expected hashes come from `Document/android/BinaryPatchMD5.xml` (main libs) and
`SplitLibMD5.xml` (comlibs). `VerifyTask.verifyLibrary` runs on a background
thread, tries xxhash first (gate: SharedPreferences `__SDKXXHash__`), falls
back to MD5, and **failures go to `SDKReportClientBridge.SendError` — it is
telemetry, not a load gate.** The boot sequence is
`copyOrMoveDynamicLibrary → verifyDynamicLibrary → log/report →
loadDynamicLibrary`; control flow never returns early on a verify failure.
Only copy failure or `System.load` failure aborts. **That is the hole Rain
rides through** — confirmed at DEX bytecode level with a purpose-written
disassembler (`work/dexcode.py`).

### 4.3 The comlibs natives, identified

| File | What it is |
|---|---|
| `libAkSoundEngine.bytes` | Wwise audio engine (Audiokinetic), SWIG bindings |
| `libhttp_downloader.bytes` | **Rust** pre-download engine (tokio 1.53 + hyper 1.11 + rustls 0.23 + jni, macOS build host) behind `ResContext` JNI |
| `libmlsdk_logreport.bytes` | loggate socket client (`LogReportManager` + bundled cJSON) |
| `libmola_download.bytes` | C++ HTTP/1.1+HTTP/2 multi-range downloader (166 `mola_*` exports) |
| `librudp.bytes` | **Rain v43.3** — md5 `1140ed7d…`, byte-identical to `build/release/librudp-arm64.bytes` |

### 4.4 Other findings

- `libil2cpp.so` proven to be a jump-thunk shim at instruction level — each
  export is `adrp/ldr/ldr/br` into a GOT slot; the real runtime is `liblogic.so`.
- GameProtect native half confirmed absent from the APK (`System.loadLibrary("gp")`
  + `libnpth.so` are app_libs-only).
- Split set changed on reinstall: `split_countries.apk` renamed to
  `split_PlayAssetPack.config.countries_id.apk` (byte-identical), new empty
  `split_PlayAssetPack.apk` appeared — the reinstall went through Play Asset
  Delivery. No game content changed.
- Full functional game model documented (login → lobby → matchmaking → loading
  → battle → result with type indices per stage, and Rain's hook chain).

---

## 5. Cross-mod synthesis

| | Rain | VYNIX TRIALL | XNISH NEW |
|---|---|---|---|
| Hook engine | GlossHook | GlossHook, **122 sites / 6 modes** | Dobby, 29 hooks + 195 stub patches |
| AC strategy | none — by design | neutralizes ~half the AC surface (mode 0+1+2 groups) | stubs local verdicts + log senders only (2.1% coverage) |
| AC gap | n/a (doesn't touch) | still exposed to DesyncLog/input-behavior | **exposed to input-behavior, hero-load, desync, 5-min check** |
| License | none | remote, MD5 + `SUCCESS` marker, **expired 2026-10-10** | remote, device-bound (`android_id` UUID), TLS verify disabled |
| Audio | n/a | n/a | **game runs with audio dead** |
| Binary size | 1.07 MB | 10.86 MB | 10.79 MB |
| Copy helper | `process_vm_readv` peek | pipe-based ≥64 KiB copy | — |

**The pattern:** every commercial mod pays for its AC hooks with remote licence
servers, device binding, and expiry gates. Rain's no-touch stance is the only
one that ships without a server dependency — and the only one whose binary
contains no AC-forgery code a distribution platform could flag.

**The shared lesson (usable without AC contact):** VYNIX's pipe helper and
XNISH's stub patcher both implement *fault-tolerant bulk operations*. Rain's
peek boundary is the same idea at entry scale; the VYNIX report notes the
lesson is *scale* — peek the container, not just the entry — which is exactly
the P0-1 fix shape.

---

## 6. Live phone state (end of session)

- MLBB 2.2.16.12322 (211812322), installed 2026-10-11 17:15, ban screen
  confirmed live (10947h ≈ 456 days, plugin-use reason).
- Rain v43.3 staged and verified booting clean; config `default.json` present.
- Phone: Redmi iolite, Android 16 / HyperOS 3.0.302, SM6435, Adreno 710,
  8 GB RAM, 512 GB storage, no root, bootloader locked — unchanged.
- Game exited to launcher after the ban-screen OK tap (normal for this state).

---

## 7. Priority list (consolidated, all verified)

**Tier 0 — synth restoration (one build):**
1. Fix N1 `"(JJIFFI)"` @ synth.cpp:175
2. Fix N2 action constants @ synth.cpp:20
3. Fix the loader (resolve `UnityPlayer` via notify's captured loader) +
   call `synth::Init()`
4. Budget a live debug session — nothing in synth has ever executed

**Tier 1 — crash class:**
5. Fix P0-1 with wider scope (whole container walk; `const void*` signature)
6. Fix P0-2/P0-3 (`DictEach` ×2, count fills locally, clamp at call sites)
7. Add `-DNDEBUG` @ CMakeLists.txt:20 — one word, removes an entire crash class
8. Depth-limit cfgjson (N12)

**Tier 2 — correctness:** P1-5 notify refs, P1-6 match-scope resets,
P1-7 camp pass-through, N11 rinject path, N8 icon context-loss

**Tier 3 — robustness:** N7 dirty-flag, N5 early-exit DictEach, N4 stride
validation, N6 recursion guard

**Tier 4 — docs:** fix rain.cpp JNI_OnLoad comment, remove dead binds, add
threading invariants at boot→render handoffs

---

*Five workstreams, all claims independently re-verified from raw bytes. Live
session on the Redmi (5d39a8f2) captured Rain's boot, the ban screen, and the
input listener. Rain binary on device md5-verified against the release build.*
