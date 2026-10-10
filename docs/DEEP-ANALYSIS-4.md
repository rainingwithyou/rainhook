# Rain Project — Deep Analysis (Round 4): VYNIX, XNISH, MLBB internals, live session

Date: 2026-09-30 (evening session)
Targets: `D:\rain` full project, `VYNIX LIB TRIAL/`, `XNISH NEW/`, MLBB v2.1.67.11733
(live on `emulator-5556`, Rain v35 staged as `librudp`)
Companion to rounds 1–3 (`ANALYSIS-REPORT.md`, `DEEP-ANALYSIS-2.md`, `DEEP-ANALYSIS-3.md`).
New artefacts: `analysis/device_2026-09-30/`, `analysis/xnish_*.txt`,
`analysis/final_metadata_dive*.txt`, `analysis/final_meta_fields.txt`,
`analysis/live_0930_*.png`.

---

## 1. MLBB APK + delivery pipeline (re-verified on the live device)

### 1.1 Package facts
- `com.mobile.legends`, versionName `2.1.67.11733`, versionCode `211811733`, installed 2026-07-08.
- The 09-28 APK snapshot (`analysis/device_2026-09-28/base_pull.apk`,
  md5 `5d678fc8db3cdd3110b767b312f820c2`; split arm64
  `b9afcf396cab854f82b4a13e48314f61`) is **byte-identical to what is installed
  today** — no Play-store update; all change arrives through the in-app hot-update
  channel, exactly as documented in round 3.
- `realversion.xml` (inside the APK): `realversion=2.1.67.1173.1`,
  `main_assets=2327625`, `client_code=254465`, `plugin=36251`,
  engine `Unity 2019.4.33` (`engineInnerVer 2.01.0054.07`), `isSOSplit=1`.
  Note: `version.xml` still carries the older `2.1.60.1173.1` plus login/report
  endpoints (`login.ml.youngjoygame.com:30021`,
  `report.ml.youngjoygame.com:30071`, `ip.ml.youngjoygame.com:30220`).
- Fresh pulls today: `analysis/device_2026-09-30/` —
  `libAkSoundEngine.so`, `libhttp_downloader.so`, `libmola_download.so`,
  `libmlsdk_logreport.so` (the libs the game hot-updated at 16:28 on 09-30),
  plus the full `comlibs/arm64-v8a` staging set.

### 1.2 APK composition
- base.apk: 1,512 entries — 3 dex files (8.9 + 10.4 + 8.5 MB ≈ 27,398 classes),
  `assets/bin/Data` (Unity data), `Resources4-*.dat`, `mobile.v2.27.2.html`
  (in-game webview shell), `assets/cid` (a small 32-bit ELF, armeabi, built with
  NDK r17b, links only liblog/libstdc++/libm/libdl — a device-fingerprint helper
  dropped at runtime).
- split_config.arm64_v8a: 43 native libs. Security/monitor stack:
  `libsigner.so` (Adjust signature SDK, sole JNI entry
  `Java_com_adjust_sdk_sig_NativeLibHelper_nSign` — install-referrer signing,
  **not** APK verification), `libsliver.so` (libxhook 1.2.0 PLT hooking + thread
  suspension, used by the ByteDance APM stack), `libmonitorcollector-lib.so`
  (qiyi xhook + bytehook, binder/proc monitoring: `enableBinderHook`,
  `getProcInfo`, `hookLoadedLibs`), `libEncryptor.so` (`ttEncrypt` only),
  the npth family (ByteDance NPTH crash/ANR), `libbytebench`, `libprofiler`,
  `libshadowhook`.
- Engine lib is `libunity.so` (24.4 MB) + 384 KB `libil2cpp.so` stub — the real
  IL2CPP runtime lives in `liblogic.so`, loaded from `app_libs` (round 1 finding,
  still true on this build).
- Play Asset Pack splits are **empty stubs** (manifest + SourceStamp only).

### 1.3 Java layer — full component map
From the decoded manifest (27,000+ classes across 3 dex):
- Launch chain: `MobaGameMainActivityWithExtractor` (activity-alias, LAUNCHER)
  → `MobaGameUnityActivity` in process `:UnityKillsMe`; transparent and restart
  activity variants; `NativeVideoPlayerActivity`, `CaptchaActivity`
  (`com.moba.sdk.captcha`), AIHelp support (`net.aihelp.ui.HostActivity`),
  UniWebView (`com.onevcat.uniwebview`), VK/Facebook/TikTok share entries.
- Hot-update loader: `DynamicSOLoader` with **`DynamicSOLoader$LibraryVerifyManager`**
  + `VerifyTask`/`VerifyResult`/`LibraryInfo$eVerifyType` — the Java-side verify
  pass over `comlibs/*.bytes`. It runs on the *downloaded* file set (the same
  files Rain displaces); Rain's staged `librudp.bytes` md5 was stable through
  it in rounds 2–3, and today's launch confirmed it again (Rain booted).
- Plugin surface: `Z7Extractor` (7zip asset extraction), `UnityBridge`,
  `SystemProperties`, `TracRoute` (traceroute diagnostics), `MultiNetworkUtility`
  (dual-network), `FTPUploader`, `ScreenshotManager`/`ScreenCaptureService`
  (`DETECT_SCREEN_CAPTURE` permission declared), push stack (Firebase + own
  `PushNotifacationManager`), `MemoryReduer`, widgets, video wallpaper service.
- Protection: **`com.bytedance.gameprotect`** — `GameProtect`, `GPPacketChannel`,
  `GameChannel`, `UserConfig$Builder`, `IEmulatorCallback`, `Region`,
  `AppLifecycleListener`. This is ByteDance's GameProtect SDK: a Java-side
  emulator-detection callback plus its own packet channel (independent of MTTD).
  The emulator callback interface confirms emulator detection exists in the
  Java layer; the game still runs on LDPlayer, so it is evidently
  report-only (telemetry), not a hard block, on this build.
- Permissions: standard game set incl. `READ_PHONE_STATE`, location, calendar,
  camera, mic, `FOREGROUND_SERVICE_MEDIA_PROJECTION`, `ACCESS_ADSERVICES_*`,
  `DETECT_SCREEN_CAPTURE`.

### 1.4 Key takeaway for Rain
No signature check exists on the `comlibs/*.bytes` files themselves (round 1,
re-confirmed twice). The only integrity machinery is `LibraryVerifyManager`
(Java, over the same files) and the `gameprotect` telemetry SDK. The native
`libsigner.so` is unrelated to game integrity (Adjust install-referrer signing).

---

## 2. VYNIX 64-bit — full decode (`VYNIX LIB TRIAL/VYNIX LIB 64 BIT/librudp.bytes`)

10,833,088 bytes, ELF64 AArch64 DYN, SONAME `libMyLibName.so`, stripped,
full RELRO (`SYMBOLIC | BIND_NOW`, FLAGS_1 NOW), statically-linked libc++
(`_ZTVNSt6__ndk1*`), 6.9 MB `.rodata`. Dynsym: **301 entries, only `JNI_OnLoad`
exported FUNC** (plus 4 libc++ RTTI objects) — strictly minimal exports, same
shape Rain matches via `rain.map`.

### 2.1 Stack
- Hooking: **GlossHook v1.9.2** (`GlossLinkerFuncProxy`, `Custimized trampoline
  range`, `Hook failed, Linker proxy function (GlossLinkerFuncProxy) is invalid.`)
  — the exact framework vendored in `third_party/GlossHook` (Rain carries v1.9.5).
  GlossHook strings confirm inline hooking with trampoline allocation and
  `__loader_dlopen` linker-namespace resolution, plus ELF parsing
  (`Elf dynamicInfo: j: %d, dyn: 0x%lx, tag: %lld`, `Size of program header`,
  `FindSymbolFromDynamicTable`, `FindSymbolFromImportTable`).
- Rendering: ImGui (GLES3 backend; shader sources present verbatim;
  `##MainMenuBar`, `##EnemyTable`, `##TeamTable`, `##Menu_%02d`), fonts include
  Cousine + a custom font, **60+ hero icons embedded as base64 PNGs** (decoded
  list below), country-name table (flag art lookups).
- Networking: statically-linked **libcurl + OpenSSL 1.1.1** (TLS 1.3, OCSP,
  DoH, SRP, GOST; built from `openssl-curl-android`, OPENSSLDIR
  `/Users/rying/repo/openssl-curl-android/openssl/build/arm64-v8a/ssl` —
  macOS build host, username `rying`).
- IL2CPP: 27 `il2cpp_*` imports incl. `il2cpp_domain_assembly_open`,
  `il2cpp_class_get_static_field_data`, `il2cpp_runtime_invoke`,
  `il2cpp_object_new`, `il2cpp_array_new`, `il2cpp_string_new` —
  **VYNIX constructs objects, arrays and invokes methods**, a deeper runtime
  surface than Rain's field read/write layer.

### 2.2 Feature surface (from config keys + UI strings)
Persisted config keys (SharedPreferences, `putInt`/`putString` JNI):
`map_active`, `map_minimap_pos_x/y`, `map_minimap_width/height`,
`map_circle_size`, `esp_player_health/line/icon/skill`,
`esp_monster_health/alert`. UI feature labels: ESP box/health/name/level/
distance/tracer, monster ESP with alerts, **Turtle Under Attack / Lord Under
Attack** banners (Rain's objective-alert feature matches VYNIX exactly),
minimap maphack with position/size calibration, `Hero List` table,
lane labels (`Exp Lane`, `Mid Lane`, `Gold Lane`, `Roam`, `Jungle`).

### 2.3 Hero icons
67 MLBB hero names present as embedded icon assets (Aamon → Zilong incl.
late heroes Kalea, Lukas, Suyou, Zetian, Sora, Cici, Chip, Nolan). The ESP
icon feature draws per-hero art; the icons ship inside the 6.9 MB `.rodata`
as base64 PNG blobs.

### 2.4 Licensing / distribution
- Key-auth UI: `Key loaded. Press Login to continue.`, `Key pasted. Press Login
  to continue.`, `Paste DeviceId`, `Enter / Paste new Device ID...`,
  `Current Device ID:`, `Device ID:`, `Expire date`, `Save Current Account`,
  `Load Account`, `Send Owner` / `sendOwner` (encrypted owner-report path),
  `Invalid access!`, `Login denied`, `Authentication failed: %d`.
- **`Cheat Mati, Silahkan Beli !!!`** (Indonesian: "cheat off, please buy") —
  the trial gate; the trial build disables features when unlicensed and points
  at purchase. No hardcoded license endpoint was found in plaintext (the
  auth flow rides the static libcurl/TLS stack; endpoint likely encrypted).
- Multi-variant support: package ids `com.mobile.legends.usa`,
  `com.mobile.legends.lite`, `com.vng.mlbbvn`, `com.hhgame.mlbbvn`,
  `com.mobiin.gp`, `com.mobilechess.us` — one binary serves global/VN/lite/
  chess builds; also reads `BattlePlayerInfoCache`/`BattleRecord`/
  `FightHistory`/`OfflineReport` battle-record files and
  `/files/dragon2017/BattleRecord`, `/files/Jungle/` paths.
- IP literals (CIDR blocks, e.g. `102.131.64.0`, `103.16.158.0`,
  `131.0.172.48`, `100.12.0.0`) — embedded route/allow lists.

### 2.5 VYNIX vs Rain (gap analysis)
| Aspect | VYNIX | Rain |
|---|---|---|
| Hook framework | GlossHook 1.9.2 | GlossHook 1.9.5 (newer) |
| Load slot | `librudp.bytes` | same |
| Exports | JNI_OnLoad only | JNI_OnLoad/OnUnload only |
| IL2CPP depth | invoke + object/array construction + static fields | field/offset read-write + a few invokes |
| Features | ESP+icons(67 heroes), maphack+circle, objective alerts, hero list, battle-record reading | ESP (no icons), maphack, drone view, skin changer, alerts |
| Licensing | key/device-id gate w/ expiry, purchase CTA, multi-package | none (private) |
| Size | 10.8 MB (curl+OpenSSL+icons) | 850 KB |

The only feature Rain lacks that VYNIX has and the metadata supports:
**per-hero ESP icons** (`esp_player_icon` — needs `m_ID`→hero art mapping, all
icons would have to be embedded or pulled from game assets) and
**battle-record reading** (offline replay/inspection, reads
`/files/dragon2017/BattleRecord`).

---

## 3. XNISH — `XNISH NEW/libAkSoundEngine.bytes` (10,785,120 bytes) full decode

### 3.1 Identity
- ELF64 AArch64 DYN, stripped, 64 sections. Exports **6,683 dynamic symbols**
  (mostly static OpenSSL/libc++ leakage — sloppier than VYNIX/Rain's minimal
  exports) plus the single real entry **`JNI_OnLoad`**.
- **Not a real sound engine.** The genuine `libAkSoundEngine.so` is 2.68 MB
  with `Ak*` (Wwise) symbols; XNISH's is 4× larger, an ImGui overlay cheat
  that merely borrows the filename to ride the `comlibs` hot-update path
  (same vector as Rain/VYNIX).
- The 25.8 MB `libAkSoundEngine#.bytes` in the same folder **is** a genuine
  Wwise sound engine from a newer build (dense `Ak*` symbols, 82k strings) —
  i.e. the folder holds one cheat payload + one stock lib for reference/diff.
- Hooking: **Dobby** (`DobbyHook`, `DobbyInstrument`, `DobbySymbolResolver`,
  `DobbyCodePatch`, near-branch trampoline controls) + manual linker internals
  (`__dl__Z8__dlopenPKciPKv`, `__dl__Z9do_dlopenPKciPK17android_dlextinfoPv`,
  `__loader_dlopen`, `fake_dlopen_with_path`), `eglSwapBuffers` in `libEGL.so`,
  targets `liblogic.so`/`librudp.so`. Different hook engine than VYNIX/Rain
  (Dobby vs GlossHook).
- IL2CPP API usage similar to VYNIX minus static-field helpers.

### 3.2 Feature surface (by far the deepest of the three)
Indonesian-English mixed UI ("Tempel key untuk membuka menu" = paste key to
open menu; "Menu aktif dan stabil"; "Jarak Auto Aim"; "Percentage HP Terendah"
= lowest HP %):

- **ESP**: box/name/health/hero/round markers for players and monsters
  (`Feature.ESPBox`, `Feature.ESPName`, `Feature.ESPHero`, `Feature.ESPHealth`,
  `Feature.ESPLine`, `Feature.ESPRound`, `Feature.ESPAlert`, `Feature.ESPAlertIcon`,
  `Feature.ESPMHealth`, `Feature.ESPMRound`, `Feature.ESPMinion`,
  `Feature.ESPSpellCD` — incl. **spell cooldown ESP**, which Rain lacks),
  minion dots, HP-bar thickness, alert icons.
- **Maphack**: `Atur Mini Map` (position/size/icon sliders), minimap icons.
- **Drone view**: `Mode Drone`, `##DroneView`.
- **Auto-retribution subsystem** (model-driven, matches round-3 finding):
  `Enable Auto Retri`, `Retri whenever monster HP is low enough`,
  `Only retri when enemy players are close`, `Percentage HP Terendah`,
  per-target toggles `Retri Lord (2002)`, `Retri Turtle (2003/2110)`,
  `Retri Red/Blue Buff (2004/2005)`, `Retri Litho (2056/2072)` —
  **hardcoded monster ID table**, `##RetriEnemyDist` enemy-distance guard.
- **Auto-aim / skill assist** (Rain has nothing in this class):
  `Skill Targeting Configuration`, targeting modes `Players Only` /
  `Monsters Only` / `Both (Adaptive)` ("pilih target terdekat" = nearest),
  target priority (minion>monster>player option), `Aim Basic`,
  `Aim Skill 1..4`, `Aim Spell`, per-hero specials: `Kimmy Auto Aim` (basic
  attack, hero ID 207), `Auto aim Ling's sword`, `Auto aim for Franco's hook`,
  `Auto aim Skill 1 for any hero`, `Generic Skill Auto Aim`, range sliders
  (`##RangeFOV`, `##SkillRange`, `##TargetDistance`), `Lock Target`,
  `Trigger Mode`, `CD Skill`/`CD Spell` cooldown awareness.
- **Auto-combo**: `Gusion Combo` (S2→S1→S3→S2→S1 burst),
  `Benedetta Combo` (S2→S1→S3→BA→BA), `Auto Combo`, `Combo aktif jika target
  masuk [range]` (combo triggers when target enters range), `Jarak Combo`,
  in-game combo button (`Tombol combo muncul di game`).
- **Kimmy farm subsystem**: `Kimmy Quick-Clear Mode`, `Kimmy Minion Clear Mode`,
  `##KimmyRange`, `##KimmyMinionRange`, `Minion Clear Range`.
- **Licensing**: key gate (`Key kosong` = empty key, `Tempel key untuk membuka
  menu`, `KEY OWNERS` panel, `##owners`, `##routes`, `Expire date`), plus
  **`https://t.me/+sKQszuaIjPw5MDY9`** (Telegram distribution channel) and
  **codashop integration**: `Origin: https://www.codashop.com`,
  `Referer: https://www.codashop.com/`,
  `https://order-sg.codashop.com/initPayment.action` — the payment flow for
  buying keys goes through Codashop's order API. `openTelegram` JNI bridge.
- Paths: reads/writes `/sdcard/Android/data/com.mobile.legends/files/
  dragon2017/assets/comlibs/arm64-v8a/libbattlelogic.bytes`,
  `libmlsdk_logreport.bytes`, `libagoraSdkCWrapper.bytes`,
  `.../dragon2017/login_data` — it stages/reads sibling comlibs and the
  login-data file (deeper account-file access than Rain).
- Packages: `com.mobile.legends`, `com.mobilelegends.hwag`, `com.vng.mlbbvn`.

### 3.3 XNISH vs Rain (gap analysis)
XNISH's differentiators Rain could borrow, in order of value:
1. **Auto-retri with HP-percentage model + enemy-distance guard** — metadata
   support confirmed (`Battle.SpellCastData`, `BattleManager` spell fields);
   the monster-ID table (2002 lord / 2003+2110 turtle / 2004-2005 buffs /
   2056+2072 litho) is reusable as-is.
2. **Spell-cooldown ESP** (`ESPSpellCD`) — `ShowSkillData` (29 fields) +
   `ShowSkillCoolDownCalcData` exist in metadata.
3. **Targeting/aim assist** — largest gap; requires invoke-level IL2CPP
   (VYNIX-style `il2cpp_runtime_invoke` of skill-cast methods) and input
   synthesis; biggest detection surface of any feature class.
4. **Auto-combo** — same invoke+input requirements.

---

## 4. MLBB internals — metadata deep dive (round-4 additions)

All from `refs/game/global-csharp-metadata.dat` (20,375 types) via
`analysis/meta_extract.py` + `types_all.json`.

### 4.1 Battle core (confirmed sizes)
`ShowEntity` 393 fields/804 methods, `LogicBattleManager` 446/187,
`BattleManager` 45/121, `ShowBattleCamera` 31/51, `ShowMonster` 12/55,
`Battle.ShowSkill` 17/51. Every field Rain binds was re-verified present
(round 3 offsets still logged at bind time: hp=420 hpMax=424 camp=208).

### 4.2 Network protocol: `MTTDProto` — 1,716 classes, **857 `Cmd_*` messages**
The game's server protocol namespace (MTT = Moonton). Security-relevant commands:
- `Cmd_Battle_Check_CS/SC`, `Cmd_Battle_CheckCanEnterPvpBattle_CS/SC` —
  pre-battle server gates.
- `Cmd_Battle_Monitor_Report_CS` (17 fields) — battle-behavior telemetry;
  sibling `MTTDProto.MTTD` report types include `BattlePlayerInfo` with
  `sPlayerIP`, `sPlayerSSID`, `iBanHeroId`, `iPing`, `iFPS`, `iBattery`,
  `iMoveLag` — device/network fingerprinting per battle.
- `Cmd_Battle_SpecDataReport_CS`, `Cmd_Battle_MtrReport_CS`,
  `Cmd_Battle_EndMiscDataReport_CS`, `Cmd_Battle_ReportLogicError_CS`,
  `Cmd_Battle_ReportPingStamp_CS`/`Cmd_Battle_ReportPing_CS`/
  `Cmd_Battle_ReportPlayerPing_CS` (three separate ping reports).
- `Cmd_Act_CheatConfig_ReportCheatData_CS` (5 fields) — the client-side cheat
  report channel; server-driven via `Cmd_Act_CheatConfig` config commands.
- `Cmd_Battle_VoteKick_UploadBattleData_CS/SC`, `Cmd_ClientBattleMapInfoReport_CS/SC`
  (map-info consistency report — relevant to maphack detection),
  `Cmd_Battle_Result_CS/SC` settlement.
- Battle-record round-trip: `Cmd_Battle_ReportPlayer_BattleInfo`,
  `BattlePlayerInfoCache`, `FightHistory` types — what VYNIX reads offline.

### 4.3 Anti-cheat / reporter classes (client side, all present)
- `AntiCheatReporter` (8/10), `CheatAfkReporter` (7/8) — AFK + cheat flags.
- `BattleInputBehaviorReporter` (20 fields/17 methods) — input-pattern
  telemetry (tap cadence etc.). Relevant to any input-synthesis feature.
- `HeroLoadDetector` (27/43) + `HeroLoadDetectorPerformanceTool` (27/42) +
  `HeroLoadFailReport` — hero-load anomaly detection with memory/FPS/PSS
  probes (`_pssLoadBefore`, `_monoUsedMemoryBefore`, `_unityLoadBefore`).
- `PerformanceDetection` (9/8), `DynamicMonitorBridge` (12/13),
  `ShowBattleOperReportManager` (4/8) + `ShowBattleOperReportData` (16/7).
- The deobfuscated battle classes (round-3 table) map to these reporters via
  `Hook_Online_CmdId_*` style wrappers seen in the dex deobfuscation sample.
- **Net assessment**: client-side detection is telemetry-shaped (reporters
  aggregating behavior/network/device data to the server); the enforcement
  (bans) is server-side. Nothing client-side blocks Rain's current feature
  set on this build; maphack draws from local state (`Cmd_ClientBattleMapInfoReport`
  implies server cross-checks what the client claims to see — the one
  feature with a plausible server-correlation signal).

### 4.4 Skill/spell surface (for future auto-retri work)
`Battle.SpellCastData` (43 fields) + `Battle.SpellCastDataStru` (33),
`IShowStruct_ISHOW_SkillCast` + `SkillCastRecalibration`,
`ShowSkillData` (29/30), `JoystickEvent` (21), `UISpecialEffectAction_
AddSkillButtonEffect`, `ConCheckCastSkill` — the cast path is joystick-event
driven; a Rain auto-retri would model `SpellCastData` + monster HP read-off
`ShowMonster`/`ShowEntity` (`m_Hp`/`m_HpMax` already bound) and invoke cast
via `il2cpp_runtime_invoke` (needs the VYNIX-style invoke plumbing Rain
already has for drone/camera methods).

### 4.5 Camera (drone) — round-3 conclusion holds
92 camera-related types (`SCamera`, `SCameraCamp`, `VCameraDeathFollow`,
`IShowStruct_ISHOW_CameraFollow`, `MainCamera_SetCameraPos` family). The
render-hook camera-lift approach (v34/v35) remains the minimal-surface design;
hooking `SetCameraPosLerp` stays the fallback if a build change makes writes
unconditional.

### 4.6 Skin system (for the skin-changer feature)
`ShowSkinIDToReplace`, `CData_GameResAlternativeExSkinId` (+Element),
`CData_MatchLiveCNHeroSkinReplace`, `HeroSkinShaderChange1/2`,
`LogicHeroSkinData`, `SkinData` (15 fields) — the `m_ShowSkinId` write Rain
does is presentation-side only; server checks exist (`Cmd_Battle_CheckSkin_
Available_CS/SC`) but apply to selection, not to local render.

---

## 5. Live session (emulator, 2026-09-30 evening)

- Game launched via `MobaGameMainActivityWithExtractor`; process
  `com.mobile.legends:UnityKillsMe` pid 5376.
- **Rain v35 confirmed live**: logcat shows the ESP status heartbeat every
  ~6 s (`esp: self=0 entity=1 box_style=0 ...`, render-thread tid 5673) and
  touch pump events (`input: touchCount=1 (hdr=1)`).
- Screenshot `analysis/live_0930_menu.png`: Rain menu open over the lobby —
  title bar (▼ Rain / ✕), Visual+Status tabs, Player ESP section (Enable,
  Self ESP, Allies, Minions, Box 2D/Outline/Glow, Box scale, Thickness,
  Health bar L/R/Up/Down, Health value, Nametag w/ hero name+level+distance+
  status markers, Tracer/Line camera/player) all rendering correctly over
  the Unity surface at 1440×900.
- Game continued its own hot-update downloads during the session
  (`dragon2017/assets/...unity3d` CDN queue) — coexistence with Rain's load
  path re-confirmed.

---

## 6. Open items for Rain (updated)

1. **Lifetime guard** (rounds 1–3, still open) — highest priority; the
   reporters above confirm the game mutates entities aggressively
   (`m_bSynDeadStatus`, `m_bResetAfterSync`, `SyncData` round-trips).
2. **Box-height eyeball pass** (round 3) — pending jungle screenshot.
3. **Maphack marker visual re-check** — pending; note the new
   `Cmd_ClientBattleMapInfoReport_CS/SC` finding: minimap-consistency
   reporting exists, so the feature carries nonzero server-correlation risk.
4. **Touch `m_RawPosition` layout fix** (round 1) — still latent.
5. `git init` + `.gitignore` — still pending.
6. New candidates from this round, by risk/value:
   - per-hero ESP icons (VYNIX parity; static data, low risk)
   - spell-cooldown ESP (XNISH parity; read-only, low risk)
   - auto-retri w/ HP model + enemy guard (XNISH parity; input+invoke, higher
     risk — `BattleInputBehaviorReporter` watches input patterns)
   - battle-record reader (VYNIX parity; offline analysis, zero in-game risk)

## 7. Cheat-ecosystem lineage (summary)

Three independent cheat families ride the same unsigned `comlibs/*.bytes`
hot-update vector, with different stacks:

| | VYNIX | XNISH | Rain |
|---|---|---|---|
| Hook | GlossHook 1.9.2 | Dobby | GlossHook 1.9.5 |
| Auth | key+device-id, expiry, purchase CTA | key gate + Telegram + Codashop payments | none |
| Language of UI | English + Indonesian purchase line | Indonesian/English | English |
| Distinctive | 67-hero icons, curl/OpenSSL, multi-package | auto-aim/auto-combo/retri model, spell-CD ESP | drone view, skin changer, cleanest build |
| Exports | 1 FUNC | 1 FUNC + 6,683 leaked | 2 FUNC |
| Size | 10.8 MB | 10.8 MB | 850 KB |

Rain is the most engineering-disciplined of the three (minimal exports,
smallest footprint, no network, no licensing) and now matches VYNIX's visual
feature set; the remaining parity gaps are icons, spell-CD ESP, and the
XNISH-only automation features.
