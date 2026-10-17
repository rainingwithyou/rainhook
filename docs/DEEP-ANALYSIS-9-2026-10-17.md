# DEEP-ANALYSIS-9 — 2026-10-17

Full-stack round: live device session on MLBB **2.2.16.12322**, Rain v45.2 verified running
on it, VYNIX old/new license logic resolved by disassembly, XNISH re-verified, the
librudp integrity-coverage question closed with the current manifest, and a fresh pull of
the entire current game (APKs, comlibs, metadata, Document, Lua layer) archived under
`analysis/pull_2026-10-17/`.

---

## 0. Headline results

* **Rain v45.2 is live in the shipping game.** Boot chain completes in
  `com.mobile.legends:UnityKillsMe`, overlay renders (watermark + launcher), menu opens via
  injected touch, no crashes across the session.
* **librudp coverage: RESOLVED.** `BinaryPatchMD5.xml` (realversion 2.2.16.1232.1) lists
  `comlibs/arm64-v8a/librudp.bytes` with stock md5 `51bac6f7…` (1,285,576 B). The live file
  is Rain (`c095e1df…`), matches no manifest entry. Survival remains download-window based.
* **VYNIX license verdict: RESOLVED.** No MD5 exists in either build. Accept is a raw
  string compare of the server response against `"SUCCESS"` (new) / `"NECROAJA"` (old+new
  fallback). Both trials' expiry banners are now in the past (Sep 26 / **Oct 10, 2026**).
* **XNISH trojan: empirically reconfirmed.** Zero Wwise/AK::/CSharp_ markers in the mod;
  the cover carries thousands.
* **New game-side levers found:** `hb_Assembly-CSharp.bytes` is an unencrypted PE
  (HybridCLR hot-update DLL, 10.6 MB) — directly decompilable; `logic.unity3d` (85.7 MB Lua
  bundle) pulled and archived; `hb_*_libdynegcmode.bytes` hot-update ELF libs identified.

---

## 1. Live device session

Device: Redmi Note 17 Pro 5G (`iolite`), Android 16, bootloader locked, no root.
Game: `com.mobile.legends` **2.2.16.12322** (firstInstall 2026-10-11 23:23).
Rain: comlibs `librudp.bytes` md5 `c095e1dfe109929ffebe539242bcf59e` ==
`build/release/librudp-arm64.bytes` (v45.2). Installed since 2026-10-14 20:19.

### 1.1 Boot chain (logcat tag `Rain`, pid 1269, class `com.mobile.legends:UnityKillsMe`)

```
Rain: boot thread started (pid=1269)
il2cpp: dynamic table ok (symtab=0x7188852320 strtab=0x7188898820 n=6998)
Rain: JNI_OnLoad
notify: app class loader captured
il2cpp: API resolved, domain=0x72e419efc0
persist: boot load 'default': ok
account: --- local account state ---   (2 device-id keys, 84 chars, login cache present)
notify: installed — notification button toggles the overlay
Rain: overlay installed, waiting for battle scene
il2cpp: images now 70 (was 0)          (was 0 in older builds — image walk binds post-Unity)
il2cpp: bound class UnityEngine::Input
overlay: ImGui ready, surface 1968x908
```

Per-frame loop confirmed live: `game: monsters field ok=1 list=… count=0` (lobby, empty
list), `game: monster dict=… -> 0 entities`, `drone: h=0.0 fov=+40.0 touched=0 sf=0 hook=0`
(fov +40 is the saved default.json).

### 1.2 Interaction control (adb → Unity → Rain)

* `adb shell input tap` produces `input: touchCount=1 (hdr=1)` → `touchCount=0 (hdr=0)` in
  Rain's own log — the pump sees injected touches.
* Tapping the launcher button (168, 634) opened Rain's ImGui menu; screenshot
  `analysis/pull_2026-10-17/re/rain_menu_open.png` shows the full menu (title
  `@rainhook | Hooking with You <3`, Visual tab, ESP/Box/Health bar/Name tag/Tracer rows).
* **Live confirmation of `menu.cpp:1199`:** while the menu was open,
  `cfgjson: saved 'default' (7623 bytes)` fires every ~2 s — the per-frame `MarkDirty()`
  rewriting default.json continuously, exactly as the code review predicted.
* Title-bar X tap (1445, 148) closed the menu; game returned to lobby; no
  FATAL/AndroidRuntime/SIGSEGV in the window.

Screenshots: `re/game_launch_1.png` (invite popup, watermark + launcher visible),
`re/game_state_now.png` (lobby), `re/rain_menu_open.png`, `re/rain_menu_closed.png`.

### 1.3 Account state

Guest login works (lobby live, friend invites arriving, level 49/50 account Bedug_Mania).
`account::Diagnose` output on boot: `com.mobile.legends.v2.playerprefs.xml` has 2
device-ID keys (84-char `1c0fc764…`), `ug_install_settings_pref.xml` + `snssdk_openudid.xml`
present, `login_data` present. Note: the full device ID is printed to logcat by
`account.cpp:493-498` (documented leak, unchanged).

---

## 2. Integrity manifests — the librudp question closed

All under `analysis/pull_2026-10-17/document/` (pulled from
`/sdcard/Android/data/com.mobile.legends/files/dragon2017/assets/Document/android/`):

| Manifest | Items | librudp | Notes |
|---|---|---|---|
| `BinaryPatchMD5.xml` | 55 | **listed** | `comlibs/arm64-v8a/librudp.bytes` md5 `51bac6f79e3e13c5823e44b352686fb4`, xxhash `ded282`; armv7 + x86 entries too; realversion `2.2.16.1232.1` |
| `SplitLibMD5.xml` | 14 | — | 3 metadata dats + 11 SO libs (liblogic/libfirst/libcsharp/lib_burst_generated ×2 ABIs) |
| `BBAI1_Version_1278.xml` | — | — | arm64 zip md5 CEDC9158 (49,377,527 B); per-file md5s of feature/map/master/model/table + libmobile_bbai all match the pull |
| `BBAI2_Version_1278.xml` | — | — | libbytenn/libcpp_shared/libtbb md5s match |

Stock-file map:

* `metadata/librudp.stock.bytes` — 1,285,576 B, md5 `51bac6f7…` = **stock for the installed
  2.2.16 build** (also identical to `refs/game/librudp_device.so`).
* `metadata/comlibs_current/librudp.bytes` — 1,065,704 B, md5 `1140ed7d…` = **previous
  game version's stock** (byte-identical to the Oct-11 mega2 pull). Matches no current
  manifest entry.
* Live comlibs file — 1,004,896 B, md5 `c095e1df…` = Rain v45.2.

Consequence unchanged from DEEP-ANALYSIS-6: the slot is hash-covered, the download/repair
path can re-provision it, runtime `loadSplit()` does not hash-check. Deployment lives in
the window between server accesses.

---

## 3. Fresh pull inventory (`analysis/pull_2026-10-17/`)

### 3.1 APKs (`apks/`, 10 files)

base.apk 129,482,973 B md5 `7d9324fa…` (identical to the device copy and the
`/sdcard/Download` copy). Splits: GNASDK, GSDKRTC, PlayAssetPack(+countries_id), SDKVK,
WifiDetective, config.arm64_v8a (37,800,482 B). Base APK: **4 dex files**, **zero
`lib/` entries** (all natives are hot-update .bytes), biggest assets
`Resources4-1..4.dat` (20–29 MB each — these are the Moonton "MLBB" containers).

### 3.2 comlibs (`comlibs/arm64-v8a/`, 16 files)

Only **librudp.bytes changed** between the Oct-02, Oct-11 and Oct-17 pulls — everything
else is byte-identical, including all BBAI files:

| File | Size | md5 | Status vs prior pulls |
|---|---|---|---|
| libmobile_bbai.bytes | 90,995,760 | `110210c9…` | SAME |
| model.bin | 24,585,512 | `7fb781ab…` | SAME |
| libcpp_shared.bytes | 6,238,016 | `9b58a5b1…` | SAME |
| map.bin | 4,687,848 | `a53d5484…` | SAME |
| libAkSoundEngine.bytes | 2,686,088 | `1f029d8e…` | SAME (genuine, stripped) |
| libbytenn.bytes | 2,640,192 | `1980bca7…` | SAME |
| libhttp_downloader.bytes | 1,874,144 | `2633872a…` | SAME |
| feature.bin | 1,597,544 | `4148ab5c…` | SAME |
| master.bin | 1,502,968 | `cf4a0572…` | SAME |
| libtbb.bytes | 784,760 | `c43ca519…` | SAME |
| table.bin | 50,696 | `5088a16a…` | SAME |
| BBAI*_Version_1278.xml | 954/696 | — | SAME |

### 3.3 Metadata (`metadata/`)

`global-csharp-metadata.dat` (20,090,228 B, `a9be9ac2…`), `global-first-metadata.dat`
(1,456,952 B, `97c9aadc…`), `global-metadata.dat` (4,754,024 B, `09287419…`) — all
byte-identical to `refs/game/md_pull/` and recorded in SplitLibMD5.xml. The `refs/game/`
root copies hold an older (larger) generation.

### 3.4 Document (`document/`, 430 files)

New / notable artifacts:

* `hb_Assembly-CSharp.bytes` — 10,589,696 B, header `4D 5A` (**PE**): the HybridCLR
  hot-update Assembly-CSharp DLL. Unencrypted → directly decompilable (ILSpy/dnSpy).
  `hb_Assembly-CSharp-firstpass.bytes` 914,944 B same format.
* `hb_arm64-v8a_libdynegcmode.bytes` 16,861,992 B / armv7 13,896,028 B — **ELF64 shared
  objects** (hot-update native libs, "DyneGC mode"). Role not yet identified.
* `BattleConfig.unity3d` 10,589,574 B, `Document.unity3d` 35,579,365 B, plus 378
  `.unity3d` total, 23 language XMLs, `mode_versions_build`, `ResCheckConf`.
* `android_lua/logic.unity3d` — **85,745,545 B**, pulled this round (was missing);
  `server_gameres_in_pack.unity3d` 1,204 B. Referenced by BinaryPatchMD5.xml (md5
  `0da2d46a…`).

### 3.5 Live profiles (`metadata/rain_configs/`, `metadata/cfg*.json`)

`default.json`: drone ON (fov +40), maphack ON, ESP = enemy hero only, ui_scale 1.28.
`profile_1..5`: turtle/lord/buff added, alerts ON, drone fov 32, minimap moved
(92,0 292×292, world_half 52–57). All automation (retri/aim/combo/kimmy/skills) OFF in
every profile. `profile_4` == `cfg.json`; `cfg_new` == `cfg_now`; skin changer off
everywhere.

---

## 4. VYNIX — contradictions resolved by disassembly

Roster (md5s re-verified): old64 `a35b2671…`, old32 `ca6d6e17…`, new64 `bcc16ce4…`,
new32 `65768dcc…`.

### 4.1 License accept condition (settles the doc conflict)

* **There is no MD5 in either build** — no MD5_Init/Update/Final dynsyms, no K-table, no
  IV constants in .text/.rodata.
* NEW (TRIALL) monitor thread `0x81a5b0`: fetches a JNI string, builds std::string, then
  `0x81a6c0 bl 0x7e33cc` (std::string == char*) against `"SUCCESS"` (ref built at
  `0x81a654`, foff 0x2b5886); on failure the same helper against `"NECROAJA"`
  (`0x81a66c`, foff 0x1eb2f5). Accept stub `0x81a6d4`: reads gate `0xa61ff4`; writes
  license flag **`0xa568c4`** = verdict recompute (`0x831f0c` → `0x7fed4c`
  string==string of `0xa710a0` vs `0xa710b8`); success also sets `0xa62008=1`.
* Obfuscated verdict fn `~0x7fca54–0x7fd640`: `0x7fd594 bl 0x7fed4c` compares the two
  std::strings; `0x7fd5a4 strb w8,[x24,#0xff4]` → **`0xa61ff4` = verdict**,
  `0x7fd5b4` → **`0xa61ff8` = 1**. Early fail clears both.
* OLD (LIB TRIAL) thread `0x809818`: compares against `"NECROAJA"` only (`"SUCCESS"` string
  absent from old build entirely); gate `0xa5cb34`, license flag **`0xa5cb48`**.

**Verdict:** doc (b) was right about the gate (raw string equality on the server
response; both literals are load-bearing operands, not log tags). Doc (a) was wrong about
MD5; its "value A" (`Vm8Lk7Uj2JmsjCPVPVjrLa7zgfx3uz9E`, XOR-0x5A, foff 0x6d7917) is a
string compared against another string at the second-stage gate. What fills `0xa710a0`
upstream of `0x7fd570` remains the one open thread.

### 4.2 Other verified deltas (new64 vs old64)

* 4 new strings confirmed **XOR-0x5A** only: `GetRoomPlayerInfo` @0x6d750f,
  `Expired : 10 Oktober 2026, 23:59` @0x6d7f47, `RankHeroMgr` @0x6d85e7,
  `StartClientMatchUIPerformance` @0x6d85f3. OLD had `Expired : 26 September 2026, 23:59`
  @0x6c5228 — the banner was **changed, not removed**.
* Exactly 20 only-OLD XOR-0x5A identifiers: the 17 road/season/zone/login names
  (PlayerRealRoadInfo, RecentRoadInfo, RecentRoadInfoSdp, gvRecentRoadInfo, iRealRoad,
  sThisLoginCountry, uiRankLevel, uiZoneId, vCurSeasonRealRoadInfo, RoomData, _steamName,
  iBattleNum, iMythPoint, iTotalNum, summonSkillId, uiHeroIDChoose, vHeroIds) + NECROAJA
  (pool copy) + September + kbZZZZZZZZ.
* Pipe helper `0x7d7ba0`: confirmed function (`stp x29,x30` prologue, requires
  len ≥ 0x10000, lazy `pipe()`, `fcntl(F_SETFL|O_NONBLOCK)`, `__read_chk`); **exactly 37
  `bl` callers** in new64; OLD has no `pipe`/`__read_chk` imports at all.
* Sections: +22,712 B total = .text −8,200, .rodata +77,024, .data −43,896, .bss −4,240,
  new `.comment` (+966 B, clang 18.0.3 / LLD 18.0.3, ".Android (8481493…)").
* Expiry string is **display-only** — referenced once at `0x80a8ac`, decoded and printed;
  no date arithmetic exists for it. Enforcement, if any, is server-side. Both trial dates
  are past as of today (2026-10-17).

---

## 5. XNISH — empirical re-verification

| Check | mod `libAkSoundEngine.bytes` | cover `libAkSoundEngine#.bytes` |
|---|---|---|
| Size | 10,785,120 | 25,881,856 |
| md5 | `6015317b422016a46bc923c5ca0c2c09` ✓ | `27dba2918dae40e421c46f0bd2f25d6c` ✓ |
| `Wwise` marker | **0** | 251 |
| `AK::` | **0** | 91 |
| `AkSoundEngine` | **0** | 37 |
| `CSharp_` | **0** | 3,594 |

* `movz x0,#0; ret` byte pattern (`00 00 80 D2 C0 03 5F D6`): **97 exact occurrences** —
  table-driven stubs (docs' 195-site table = 100 movz0+ret / 95 bare-ret; bare-ret counting
  is ambiguous in raw bytes since it overlaps ordinary returns). The empirical count is
  consistent with ~100 movz0 sites minus a few applied elsewhere.
* `divarvian` plaintext at foff `0x955414` (brand string; the license strings themselves
  are XOR-0x7E).

---

## 6. Rain source-level findings (consolidated, code-verified)

New in this round (core layer, from a full read of game.cpp/esp.cpp/il2cpp.cpp/config.h):

1. **Raw container/dict walks outside the peek boundary** — `ClassifyContainer`
   `object_get_class` (game.cpp:324), `ArrayLength` (335), `ContainerAt` (345-351),
   `DictEach` (il2cpp.h:168-204 used at game.cpp:1054/2052/2104) all deref live game
   memory. This is the one read path the peek model doesn't cover; match teardown frees
   here are SIGSEGV in the render thread.
2. **Live-self camp override bypasses the 5-frame hysteresis** (game.cpp:1134-1143 vs
   515-521) — one stale-but-valid read can invert colors/maphack camp instantly.
3. **Stale Unity camera pointer never cleared** (game.cpp:1162-1191) — null `get_main`
   keeps the old pointer and `runtime_invoke` keeps running on it.
4. **Peek-then-write TOCTOU** (maphack 1259→1280-1287) and **no-peek writes**
   (TickSkin 1755-1763).
5. **Drone restore uses a stale instance's stock offsets** after scene re-resolve
   (1592-1596, off_base not re-captured while `engaged`).
6. **kMonsters blue/red contradiction**: note derives 1015/1016=blue, 1013/1014=red;
   table lists 1013 Blue / 1014 Red / 1015 Blue / 1016 Red (1804-1805).
7. **Live string deref in the monster debug log** (game.cpp:1084) — `ReadIl2CppString`
   contract requires peeked bytes; `EntityName` does it right, this path doesn't.
8. **Minimap ignores `pos_is_unity`** (esp.cpp:213-227) — logic-space fallback entities get
   the 45° mapping wrongly; single `mm_half` vs default 268×245 box.

Still open from earlier rounds (unchanged at HEAD): synth `(JJFFI)` + swapped
ACTION constants; Capture-last-tap self-click; automation not gated on menu_open;
cfgjson recursion/size/unchecked-narrowing; persist no-fsync + per-frame MarkDirty (now
**live-confirmed**, see §1.2); account logcat ID leak + backup-failure proceeds; rinject
`waitpid` no timeout.

---

## 7. Open items

* **Rain fixes** (priority order): PAT rotation in `.git/config`; synth repair; capture-tap;
  autox menu gating; cfgjson hardening; persist fsync + MarkDirty; account log hygiene;
  then the §6 core-layer items.
* **Game-side levers**: decompile `hb_Assembly-CSharp.bytes` (PE) for authoritative C#
  logic; parse `logic.unity3d` (85.7 MB, MLBB container) next round; identify
  `libdynegcmode` role; the +372 metadata type-count question can now be re-checked against
  the split libs.
* **VYNIX**: what fills `0xa710a0` upstream of the verdict compare; mode-2 hook claim
  (RankHeroMgr/StartClientMatchUIPerformance); 32-bit census re-verification.
* **XNISH**: stub-table ↔ byte-pattern reconciliation (97 vs 100); confirm the cover file's
  JNI_OnLoad offset discrepancy is resolved (mod @0x1ed7c0 recorded).
* The game is still running on the phone, Rain menu closed, lobby state, no crash.

## 8. Artifact index (this round)

```
analysis/pull_2026-10-17/
  apks/            10 APKs (md5 table in §3.1; base = 7d9324fa…)
  comlibs/arm64-v8a/  16 files (§3.2; live librudp = Rain c095e1df…)
  document/        430 files + android_lua/ (logic.unity3d 85.7 MB)
  metadata/        dats + librudp.stock.bytes + comlibs_current/ + cfg*.json + rain_configs/
  re/              game_launch_1.png, game_state_now.png, rain_menu_open.png,
                   rain_menu_closed.png, vynix_old64.strings, vynix_new64.strings
```
