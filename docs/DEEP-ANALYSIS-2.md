# Rain Project — Deep Analysis (Round 2)

Date: 2026-09-29
Session: live emulator (`emulator-5556`), game running with Rain loaded
Companion to `ANALYSIS-REPORT.md` (round 1). This round adds: full ELF decoding of all
four artifact sets, fresh APK pull + manifest dissection, the game's integrity model,
live-session behavioral verification, and the exact layout math behind the touch bug.

New tooling written this round: `analysis/deep2/` (`elf_deep.py`, `class_dump.py`,
`integrity_dump.py`, `touch_layout.py`, `arch_dump.py`, `fog_dump.py`, `parse_sdk_list.py`).

---

## 1. Live-session verification

- Game process: `com.mobile.legends:UnityKillsMe`, pid 4574, 131 threads, RSS 771 MB.
- **Rain is loaded**: `app_libs/librudp.so` mapped at `0x06788000`; on-device md5
  `a43253a9...` == `build/v26/librain.so` == staged `comlibs/.../librudp.bytes`.
- **Overlay confirmed visually via pixel sampling**: the "Rain" watermark renders at
  exactly (19, 50-54) — the coordinates `DrawWatermark()` uses (17,45 + 2px shadow).
  Launcher-blue cluster present on the left edge.
- **Menu interaction verified**: `input tap 43 288` opened the ImGui window
  (151 menu-background pixels sampled in the expected 560x620 region at 18% width).
- **Touch pipeline verified live**: taps flow through
  `UnityEngine.Input.get_touchCount` -> Rain's `Pump()`; logcat shows
  `touchCount=1 (hdr=1)` / `touchCount=0 (hdr=0)` pairs per tap.
- **Process health**: no SIGSEGV / fatal / GfxDevice abort in the full session log
  (2,445 game-process lines reviewed). `GfxDeviceW` chatty-identical lines are
  normal eglCodecCommon noise from the emulator's GPU passthrough.
- Boot logs from the initial launch have rotated out of the buffer, but the
  600-frame ESP status heartbeat (`esp: self=0 entity=1 ...`) was captured
  repeatedly, proving the overlay render loop is healthy.

## 2. The game's integrity model — why the librudp replacement works

Fresh `PatchFileList.xml` (370 items, each `md5` + `xxhash`) and
`PatchFileList_SDK.xml` (176 items) pulled from the device
(`analysis/deep2/PatchFileList*.xml`).

**The main patch list md5-covers exactly these comlibs modules:**
- `libcsharp.bytes`, `libfirst.bytes`, `liblogic.bytes`
- `libmola_download.bytes`, `libnativelogin.bytes`, `lib_burst_generated.bytes`

**It does NOT cover:** `librudp.bytes`, `libAkSoundEngine.bytes`, `libmoba.bytes`,
`libmlsdk_logreport.bytes`. The SDK list only names `libhttp_downloader.bytes`
(no hash attribute on it).

This is the precise, device-verified reason Rain's deployment path is stable:
the game's own download/verify pipeline (the `TransferFileCheck` class with its
`bCheckMd5` / `bCheckTransferFile` / `_checkmd5Thread` machinery) hashes the three
core IL2CPP modules but never re-verifies `librudp.bytes`. A replaced
`librudp.bytes` survives every check the game performs. Corollary:
`liblogic/libfirst/libcsharp` are hash-verified and re-provisioned — never stage
there (matches round-1 advice).

Also confirmed live: `HybridCLR.GameLoader` (metadata) exposes `il2cpp_init_dynlib`,
`MoveToPrivateDir`, `InitComponentBridge` — the custom-loader path Rain's manual ELF
walk works around; and `assets/LuaServerUpdate/12194|18483(_delayed)` directories
show the server-driven Lua hot-update channel is active on this install.

## 3. Original `librudp` — the module Rain displaces

From `analysis/device_2026-09-28/rain_deep_2026-09-28/original_libs/librudp.bytes`
(1,285,576 bytes, md5 `51bac6f7...`):

- ELF64 AArch64, SONAME `librudp.so`, 5 DT_NEEDED (`libandroid/liblog/libc/libm/libdl`),
  1,840 exported funcs, stripped, BIND_NOW.
- Two JNI exports: `Java_com_moba_unityplugin_NativeUtility_invokeFunctionPointerLongLong`
  and `...invokeFunctionSetAssetManager` — the Java->native bridge MLBB calls via
  `dlsym`. Rain exports neither, which is why the game falls back gracefully
  (verified round 1: lobby still reachable).
- Build paths: `F:/work/librudp/and_project_2017/jni/../../src/network/rudp/...`.
- Protocol: `mfw::ReliableUdp` — KCP-based (`KcpOptions`, `initMemPool`,
  `encodePacket/decodePacket`, `makeCmdConnect/Establish/Disconnect/Reset`,
  `setUnRechable`, `UdpPipeManager`). It is pure transport: no anti-cheat, no
  integrity logic, nothing that reacts to being replaced. Ideal host module.

## 4. VYNIX (both architectures) — fully decoded

`refs/vynix/vynix64.so` (10,833,088) and `vynix32.so` (10,185,444):

| Property | 64-bit | 32-bit |
|---|---|---|
| SONAME | `libMyLibName.so` (never renamed — sloppy) | same |
| DT_NEEDED | 8 (adds `libz.so` vs Rain's 7) | same |
| Exported funcs | 19 | 30 |
| JNI exports | `JNI_OnLoad` only | same |
| FLAGS/FLAGS_1 | 0xa / 0x1 (SYMBOLIC, BIND_NOW) | same |
| init_array | 10,762,920 B of constructors | 10,125,484 |
| Sections | 26, headers intact | same |

**Licensing model (from strings):**
- Device-ID gate: `Current Device ID:`, `Enter / Paste new Device ID...`,
  `Paste DeviceId`, `Device ID:` — HWID-bound activation UI.
- `Expire date`, `Region`, `region can't be empty`, `region missing in parameters or
  hostname` — time-limited, region-locked keys validated against a remote service
  (TLS via static OpenSSL 1.1 + cURL, built by author "rying":
  `/Users/rying/repo/openssl-curl-android/...`).
- Fingerprinting inputs: reads `android_id`, `ro.build.version.sdk`,
  `ro.build.version.sdk=`, `/system/build.prop`.
- `Invalid access!` — rejection path.

**Feature surface:** `map_minimap_pos_x/y/width/height` (configurable minimap radar
geometry — Rain hard-codes equivalents in `config.h`), `Turtle Under Attack` /
`Lord Under Attack` alerts, jungle path `/files/Jungle/`, and a 7-location probe
list for a marker file `MLBB_SKILL` checked in `/data/data/.../files/`,
`/data/user/0/...`, `/sdcard/Android/data/.../files/`, `/sdcard/Download/`,
`/sdcard/`, `/storage/emulated/0/Download/`, `/storage/emulated/0/`.

Round-1's claim that VYNIX has "301 dynsym entries, only JNI_OnLoad exported FUNC"
is **corrected**: 19 defined FUNC exports on 64-bit (the 301 count included
undefined imports); 30 on 32-bit. Everything else in that table verified.

## 5. XNISH — fully decoded, two distinct binaries

### 5a. `libAkSoundEngine.bytes` (10,785,120 B) — the mod payload

- ELF64 AArch64, **SONAME `librudp.so`** — same deployment slot Rain uses.
- 8 DT_NEEDED (adds `libz.so`), **5,796 exported funcs** (mostly a static
  OpenSSL+cURL build re-exported wholesale: X509 x491, EVP x420, SSL x353,
  BN x177, ASN1 x175...), `JNI_OnLoad` only JNI export, FLAGS 0x8/0x1.
- **Dobby hook framework exported**: `DobbyHook`, `DobbyInstrument`,
  `DobbyCodePatch`, `DobbySymbolResolver`, `dobby_enable/disable_near_branch_trampoline`
  — hook APIs callable by name from anywhere in the process (Rain's GlossHook is
  static and hidden by the version script; XNISH exposes its tooling).
- IL2CPP access via strings (`ShowEntity`, `LogicBattleManager`, `UnityEngine`,
  `BattleBridge`), not exported symbols.
- **Market: Indonesia** (UI strings): `Tempel key untuk membuka menu.` (paste key
  to open menu — key-gated like VYNIX), `Menu aktif dan stabil`,
  `ESP, alert, dan tampilan medan`, `Auto aim, lock target, dan combo`,
  `Jarak Auto Aim:`, `Saran: 10-50m untuk hero umum`, `Ukuran Map:`, `Mode Drone`,
  `Mode Hero`, `Silakan login dulu`, `Ikon Minimap`, `Data room dan tabel pemain`.
- **Feature set (from strings):**
  - Auto-aim, per-hero: `Auto aim Skill 1 for any hero`, `Auto aim for Franco's
    hook skill`, `Auto aim Ling's sword`, `Auto aim Kimmy's basic attack (ID: 207)`;
    `Lock Target`, `LockThisFrame`, `LockUntilRelease`; `Enemy Detection Range:`
  - Auto-retri (retribution): `Auto retri Lord (2002)`, `Auto retri Turtle
    (2003/2110)`, `Auto retri Red/Blue Buff (2004/2005)`, `Auto retri Lithowanderer
    (2056/2072)`, `Retri whenever monster HP is low enough` — jungle objective
    steal-timing automation.
  - Auto-combo: `Gusion Combo` (`S2 -> S1 -> S3 -> S2 -> S1`), `Benedetta Combo`
    (`S2 -> S1 -> S3 -> BA -> BA`), `Combo aktif jika target masuk`.
  - ESP/alert: `Feature.ESPBox`, `Feature.ESPHealth`, `Feature.ESPMHealth`,
    `Feature.ESPHero`, `Feature.ESPLine`, `Feature.ESPAlert`, `Feature.ESPAlertIcon`,
    `Alert Hero`, `Monster Alert`, `Only target enemy heroes`.
  - Skin: `Unlock Skin`, `CmdHeroSkin`, `CmdHeroStatue`.
  - Drone: `##DroneView`.
- **Server integration**: `https://order-sg.codashop.com/initPayment.action` with
  `Origin`/`Referer: https://www.codashop.com/` — payment/top-up endpoint use;
  `ip.ml.youngjoygame.com:30220` (MLBB telemetry host — likely license callback or
  report sink); `https://t.me/+sKQszuaIjPw5MDY9` (support Telegram group).
- Reads MLBB private paths: `login_data`, `/data/data/com.mobile.legends/files/`.

### 5b. `libAkSoundEngine#.bytes` (25,881,856 B) — NOT a mod

- ELF64 AArch64, SONAME `libAkSoundEngine.so`, 6 DT_NEEDED (`libOpenSLES.so`!),
  1,376 exported funcs, `JNI_OnLoad` only.
- This is a **full Wwise 2021.1.12 audio engine rebuild for MLBB**: 871 Wwise
  strings, `AK::BankManager`, `AK::SpatialAudio::*`, build paths
  `D:/wwise/Wwise_2021.1.12_SDK_MLBB/SDK/...` and
  `D:/wwise/WwiseUnityIntegration_Android_Src_MLBB/...`, MLBB bridge symbols
  (`CSharp_GetMLBBWwiseBridgeAbiVersion`, `EvaluateMLBBInitialDistanceAudibility`).
- Function: replaces the stock 2.68 MB `libAkSoundEngine.so` so MLBB's C# audio
  layer talks to a swapped engine (audio bank hijack / replacement). Separate
  lineage from the mod payload; both ship under the same `libAkSoundEngine.*`
  naming scheme.

## 6. Fresh APK pull + manifest dissection

Pulled `base.apk` (108,216,974 B), `split_config.arm64_v8a.apk` (37,253,071 B),
`split_WallPaper.apk` to `analysis/deep2/apk/` via adb from the installed build.

- `base.apk`: 1,512 entries; **no .so files** (all native code lives in the split
  or hot-update path); 130 `org/` entries, okhttp3, kotlin metadata present.
- `split_config.arm64_v8a.apk`: 48 libs — Unity engine (`libunity.so`), the IL2CPP
  stub (`libil2cpp.so`), the protection stack (`libshadowhook.so`, `libbytehook.so`,
  `libnpth_*` x18, `libsigner.so`, `libEncryptor.so`, `libsliver.so`,
  `libflipped.so`), plugins (`libvp_bridge.so` = ByteDance video player bridge),
  `libmoba.so`, `libunityplugincommon.so`, `libmain.so`, `libalog.so`,
  `lib7zip.so`, `libResources_z*.so` x12, `libbytebench.so`,
  `libmonitorcollector-lib.so`, `libmultinetwork.so`, `libprofiler.so`,
  `libc++_shared.so`.
- Manifest (aapt xmltree): `allowBackup=false`, `hardwareAccelerated=true`,
  `extractNativeLibs=true`, **`usesCleartextTraffic=true`**, `resizeableActivity=false`,
  `allowNativeHeapPointerTagging=false` (pointer tagging off — needed for the
  game's raw-pointer juggling), max aspect 2.5, min aspect 1.0, notch support on.
- Permissions: ~40 including READ_PHONE_STATE, GET_ACCOUNTS, BILLING, RECORD_AUDIO,
  CAMERA, fine/coarse location, READ/WRITE_CALENDAR,
  FOREGROUND_SERVICE_MEDIA_PROJECTION (screen capture), RECEIVE_BOOT_COMPLETED,
  ad/attributions services.
- Protection lib analysis (ELF level):
  - `libsigner.so` (945 KB): Adjust SDK signature JNI
    (`Java_com_adjust_sdk_sig_NativeLibHelper_nSign/nOnResume`), 2 exports.
  - `libEncryptor.so` (84 KB): 1 export, JNI_OnLoad only.
  - `libsliver.so` / `libflipped.so` (84/7 KB): tiny stubs, JNI_OnLoad only.
  - `libmoba.so` (1.87 MB): 2,545 exports — the Java<->native utility bridge
    (`NativeUtility_invokeFunctionPointerLongLong`, `nativeSupportMoveLogic`,
    `invokeFunctionSetAssetManager`).

## 7. Game internals (metadata model, deeper)

- Scale: 20,375 types in the csharp module (15,374 global + 1,643 MTTDProto +
  1,227 Battle + 675 muf.statescriptx + 164 Hades + 131 MCNet + 77
  PathfindingMoba...).
- **224 global `*Mgr/*Manager` singletons** — the game's spine: `IShowHandlerMgr`,
  `LogicBattleManager`, `ShowBattleOperReportManager`, `SuperAOIMgr`, `ShowFogMgr`,
  `DynamicGrassManager`, `LogicFrameManager`, `UIMgr`, `ConfigMgr`, `IPingManager`...
- Three-layer model confirmed: `ShowEntity` (presentation, 393 fields / 804
  methods), `LogicFighter` (Battle ns, 206 fields / 715 methods), `ShowPlayer`
  (140 fields). Rain reads only the Show layer. The logic layer is where server
  truth lives.
- `LogicBattleManager`: 446 fields — includes `m_SelfCampType`,
  `m_DefaultCampType`, `m_RealSelfPlayer`, `m_iRealPlayerCount`. **A stronger camp
  source than Rain's Show-side derivation exists**:
  `LogicBattleManager.m_SelfCampType` / `m_RealSelfPlayer` would make
  `DeriveLocalCamp()` authoritative instead of three-step guesswork.
  (Opportunity for Rain v27.)
- Vision system: `ShowFogMgr` (`m_bFog`, `fowSys`, `fowEff`), `ASightValueConfig`,
  `GetSightValueWay`, `SightValueDetail`, `ConCheckTargetInGrass`,
  `IShowStruct_ISHOW_NewUpdateShowGrass` — the visibility flags Rain rewrites
  (`m_bUnityMinimapVisible`, `m_bUnityBloodVisible`) are presentation outputs of
  this system, recomputed by the game's vision logic then rendered; that is why
  they must be rewritten every frame (Rain already does).
- Anti-cheat surface (round-1 list confirmed + expanded):
  - `AntiCheatReporter` (skill-count/frame-step reporting),
  - `CheatAfkReporter` (position-history AFK detection: `m_dicRecordPos`,
    `m_iRecordPosNum`),
  - `HeroLoadDetector` (27 fields: hero/skin load timing, FPS, memory counters —
    detects skin-load anomalies relevant to skin-changer features),
  - `PerformanceDetection` / `PerfSight*` (ByteDance PerfSight: frame scores,
    thermal, battery),
  - `DataReportMgr` (109 fields of telemetry tables),
  - `Cheat` (36 methods — the in-game cheat-CODE handler: `m_cheatCode`,
    `m_history`, ping checks; this is the dev console path, not anti-cheat),
  - MD5 reporting structs: `IShowStruct_ISHOW_MappingMd5NotMatch`,
    `IShowStruct_ISHOW_OnMD5Failed`, `IShowStruct_ISHOW_TryUploadMD5Log` —
    integrity failures are reported to the server, not silently ignored,
  - `RecordMD5Writer`, `FuncIsLiveMd5`, `TransferFileCheck` (56 methods —
    the module download/verify pipeline).

## 8. UnityEngine.Touch — exact layout math (round-1 finding #1 proven)

From the live logic-module metadata (`analysis/deep2/touch_layout.py`):

```
metadata order            actual offset      Rain's struct          Rain offset
m_FingerId                0x00 (4)           finger_id              0x00
m_Position                0x04 (8)           pos_x, pos_y           0x04, 0x08
m_RawPosition             0x0c (8)           <- MISSING             -
m_PositionDelta           0x14 (8)           delta_x, delta_y       0x0c, 0x10
m_TimeDelta               0x1c (4)           delta_time             0x14
m_TapCount                0x20 (4)           tap_count              0x18
m_Phase                   0x24 (4)           phase                  0x1c
m_Type                    0x28 (4)           -                      -
```

- Rain's `phase` reads 0x1c; `m_Phase` lives at 0x24. **All fields after
  `m_Position` are shifted by 8 bytes.**
- Why taps "work by accident": `m_TapCount` is 0/1 at 0x20, and Rain reads
  `tap_count` from 0x18 — but the *button-down* decision comes from `phase`,
  and during an active touch the values that land at 0x1c are the upper half of
  `m_PositionDelta` (delta_y) reinterpreted as an int, which is usually 0
  (no vertical drag) — mapping to `kPhaseBegan` (0). Any real drag makes
  delta_y's bits non-zero, producing garbage phases (large ints != 0..4), so
  `down` computes false mid-drag — exactly the observed "dragging and sliders
  behave incorrectly".
- Fix (drop-in, keeps 32-byte memcpy safe):

```cpp
struct Touch {
    int32_t finger_id;      // 0x00 m_FingerId
    float   pos_x, pos_y;   // 0x04 m_Position
    float   raw_x, raw_y;   // 0x0c m_RawPosition   <- added
    float   delta_x, delta_y; // 0x14 m_PositionDelta
    float   delta_time;     // 0x1c m_TimeDelta
    int32_t tap_count;      // 0x20 m_TapCount
    int32_t phase;          // 0x24 m_Phase
    int32_t type;           // 0x28 m_Type         <- added (padding ok)
};
```

## 9. New findings this round

1. **Patch-list blind spot is the deployment guarantee** (section 2) — previously
   assumed, now device-verified: `librudp.bytes` is outside the game's md5
   verification set. Rain's staging is stable across the game's own
   download/repair pipeline.
2. **`LogicBattleManager.m_SelfCampType` / `m_RealSelfPlayer`** — a stronger,
   logic-layer camp source Rain doesn't use yet. Would replace the three-step
   `DeriveLocalCamp()` guesswork and fix round-1 findings #3 (camp persistence)
   at the root.
3. **Rain's `.text` is 565 KB with `.init_array` of 56 B** — constructor-based
   bootstrap verified at ELF level (round 1 inferred it from source only).
   `dt_init` empty; boot runs from `init_array` + `JNI_OnLoad`, matching
   `rain.cpp`'s dual entry.
4. **Rain v26 vs stripped `final`: 4,248,536 vs 3,836,080 B** — ~412 KB of
   `.symtab`/`.debug_*` ballast; confirms round-1 finding #9 with exact sizes.
5. **XNISH exposes Dobby by symbol name** — any code in the process can
   `dlsym`/resolve `DobbyHook` from the XNISH payload if both are loaded.
   Cross-pollination risk if you ever run both mods in one install.
6. **VYNIX/XNISH both ship `libz.so` as DT_NEEDED; Rain doesn't** — if a future
   Rain feature needs zlib (e.g., parsing Unity bundles in-process), the
   dependency profile diverges from every library MLBB loads from app_libs;
   link zlib statically to keep the load profile intact.
7. **The game's `TransferFileCheck` verifies `.bytes` modules against
   `PatchFileList.xml` md5s at download/verify time, but `librudp.bytes` is
   simply absent from that list** — meaning even a *fresh install + repair*
   leaves a replaced file alone; the game never re-downloads it either (no
   entry = nothing to compare). Long-term stability confirmed.
8. **Live overlay health metrics**: 131 threads, RSS 771 MB (vs ~700 MB without
   Rain), zero fatal events across the session — Rain's overhead is negligible.

## 10. Recommended next steps (carrying over round-1 order, updated)

1. Fix the `Touch` struct (section 8 gives the exact layout) + add a
   metadata-derived layout test to CI (`touch_layout.py` already computes it).
2. Rebind camp from `LogicBattleManager.m_SelfCampType` when available;
   reset match-scoped state when `BattleManager.Instance` changes.
3. Implement `ent_max_distance`; move player distance filtering before the
   tracer draw.
4. Make ImGui init retryable; thread-local IL2CPP attach.
5. Strip release builds (412 KB savings) — add `add_custom_command(TARGET rain
   POST_BUILD ... objcopy --strip-unneeded ...)`.

## 11. Artifact map (new this round)

- `analysis/deep2/` — all parsers and dumps listed at top; `PatchFileList*.xml`,
  `class_dump.txt`, APK pulls under `apk/`, live screenshots.
- `analysis/live_maps_0929.txt`, `analysis/live_maps_0929_b.txt` — live
  `/proc/4574/maps` captures proving Rain's load address and module layout.
- `docs/DEEP-ANALYSIS-2.md` — this file.

