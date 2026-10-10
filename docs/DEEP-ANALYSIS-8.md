# Rain Project — Deep Analysis Round 8 (2026-10-10)

Full-depth pass: Rain v41 source, both VYNIX builds, XNISH payload, the live
phone, and the game itself — including a **new artifact class no earlier round
touched: the game's own BattleRecord replay files**, pulled fresh from the
device this session. Everything below is measured this session unless a
citation says otherwise.

**Session artifacts:** `analysis/deep10/pull/` (battle records, BBAI model set,
verified against the game's own MD5 manifest), `docs/DEEP-ANALYSIS-8.md` (this
file).

---

## 0. Executive summary

| # | Finding | Impact |
|---|---------|--------|
| 1 | **The phone is clean.** Staged `librudp.bytes` = stock original (md5 `51bac6f7…`), not Rain, not VYNIX. Restore point intact. | Nothing to undo; deploy from scratch when ready |
| 2 | **VYNIX TRIALL (new) is a toolchain refresh, not a feature change.** Same 40 hook sites, same license endpoint, same strings that matter. Delta = LLD 18 + clang 18 LTO `.comment`, Cousine font removed, one `SUCCESS` log tag. | The two trial builds are interchangeable behaviorally |
| 3 | **VYNIX's new fd-copy helper decoded**: `write(pipe, src, len)` → `__read_chk(pipe, dst, len)` — a *fault-tolerant memory copy* that turns a bad pointer into `EFAULT` instead of SIGSEGV. Same purpose as Rain's `process_vm_readv` peek, 37 call sites. | Independent confirmation of Rain's safety model |
| 4 | **XNISH's full Indonesian UI recovered** — complete menu tree, its own server `ip.ml.youngjoygame.com:30220` (resolves to `161.202.213.91`, Singapore), Telegram sales `t.me/+sKQszuaIjPw5MDY9`, Codashop payment JSON, key gate `divarvian_sharedpref`. | The commercial picture, end to end |
| 5 | **The game's replay format decoded (first time).** BattleRecord `.bytes` = header + `client_common_config` table (90+ keys, embedded Lua snippets, AI tree names) + **~20,700 delta-compressed frame packets** + version footer. `ReplayData`/`ReplayKillEvent` classes identified in metadata. | A new, zero-risk data source for calibration and match history |
| 6 | **BBAI SDK fully pulled and verified** — 9 files, all md5-matched to the game's own `BBAI*_Version_1278.xml`. `model.bin` (24.6 MB) is the actual NN model. | The AI engine's data side is now in-tree, verified |
| 7 | **Rain v41's two JNI bugs remain live in source** (F1 app-class FindClass from native threads; F2 unresolved `putString`), re-confirmed line-by-line today. | Persistence + automation still never-executed |

---

## 1. Device state — measured today

| Check | Value |
|---|---|
| Device | Redmi 2607DRA18G (`iolite`), Android 16, `5d39a8f2` |
| MLBB | 2.2.16.12322, updated 2026-10-07, process not running (lobby of launcher) |
| Staged `librudp.bytes` | **md5 `51bac6f79e3e13c5823e44b352686fb4` = stock original**, 1,285,576 B, mtime 2026-10-09 14:03 |
| `/data/local/tmp/orig/` | intact, same md5 |
| `/data/local/tmp/` | `rain.so`, `rain39.so` … `rain41.so`, `rain40f.so`, `rain40t.so`, tombstone copies `t00–t08.txt` |
| `rain_icons/` | 145 files still on device |
| Root | none (`run-as` refused — non-debuggable; no `su`) |

The Oct 9 doc described VYNIX as staged; today's measurement shows stock.
Either way, **the current state is clean** — a deploy starts from zero.

---

## 2. VYNIX — the two builds, measured

### 2.1 TRIALL (new, 10,855,800 B) vs LIB TRIAL (old, 10,833,088 B)

| Property | Old | New |
|---|---|---|
| `.text` | 2,694,872 | 2,686,672 (−8,200) |
| `.rodata` | 6,904,308 | 6,981,332 (+77,024) |
| `.data` | 64,864 | 20,968 (−43,896) |
| `.comment` | — | **966 B (new section)** |
| Imports | 295 | 297 (**+`pipe`, +`__read_chk`**) |
| GlossHook | v1.9.2 | v1.9.2 (identical) |
| License endpoint | `https://pinikcitt.my.id/api/connect` (XOR 0x3A, length-delimited) | identical, byte-for-byte |
| Token | `Vm8Lk7Uj2JmsjCPVPVjrLa7zgfx3uz9E` (XOR 0x5A) | identical |
| JNI_OnLoad | 4,284 B @ 0x81aa0c | same size, same role |

**Meaningful string delta** (after filtering font/table noise):
- **Removed:** the entire Cousine font (Ascender/SIL OFL strings, glyph table
  names `onefraction`…`uni00B9`) — that is the −43,896 `.data` and most of the
  "removed strings" earlier rounds counted.
- **Removed:** `Assembly-CSharp.dll`, `RoomData` references (trimmed reflection
  surface).
- **Added:** `.comment` (LLD 18.0.3 + mixed clang 18/12/7 provenance),
  `__read_chk`, `pipe`, `SUCCESS` log tag.

The build is **a toolchain refresh with a hardened license path** — the
previous round's "refresh, not redesign" conclusion holds, now with the
mechanism pinned down.

### 2.2 The new fd-copy helper (decoded)

At `0x7d7ba0`, called from **37 sites** (verified by BL-target scan):

```asm
; rdi=src, rsi=len, rdx=dst
cbz  x20, fail            ; src null
cmp  x21, #0x10000        ; len < 64K → fail
cbz  x19, fail            ; dst null
ldr  w8, [global]         ; lazy pipe init flag
tbz  w8, #31, use_pipe    ; if not init'd → pipe()
; fast path: fd pair already open
ldr  w0, [global+0x8b4]   ; write end
bl   write@plt            ; write(pipe_w, src, len) ← faults EFAULT, not SIGSEGV
cmp  x0, x19              ; short write → fail
ldr  w0, [global+0x8b0]   ; read end
bl   __read_chk@plt       ; read back into dst
mov  w0, #1               ; success
```

`write()` to a pipe is the one syscall that safely validates a source pointer
without dereferencing it in-process. This is VYNIX's equivalent of Rain's
`process_vm_readv` peek boundary — arrived at independently, for the same
reason: managed objects die under you and a raw read is a SIGSEGV.

---

## 3. XNISH — the complete picture

### 3.1 The payload (`libAkSoundEngine.bytes`, 10,785,120 B)

| Property | Value |
|---|---|
| SONAME | `librudp.so` (disguise) |
| Exports | 6,135 FUNC (vs Rain's 2) |
| Hook framework | Dobby (DobbyHook, DobbyInstrument, DobbyCodePatch, DobbySymbolResolver) |
| Entry | `JNI_OnLoad` @ `0x1ed7c0` (5,092 B) + 5 init_array ctors |
| AC neutralization | **195 sites / 153 methods / 30 classes** (V0 `movz x0,#0;ret` ×100, V1 `ret` ×95) |
| Dobby hooks | 29 + 1 instrument + 4 resolvers |

**Neutralization map by class** (top): `ACInterface` 37, `LogReportAOT` 17+17,
`GameMain` 13, `MobaScriptBridgeIMPL` 12, `NLReport` 10, `ACInterfaceLib` 10,
`LuaHelper` 8, `Gumiho` 7, `BattleData/_ISHOW` 6, `SDKReportModel` 6,
`ComFunc` 5, plus `GPSDK`, `LogicPunish`, `NativeLoginBridge`, `CountBattle`.

### 3.2 The commercial layer (new this round — full UI string table recovered)

- **Server:** `ip.ml.youngjoygame.com:30220` → `161.202.213.91` (Singapore).
- **Sales:** `https://t.me/+sKQszuaIjPw5MDY9` (Telegram), plus a full
  Codashop `initPayment.action` JSON body with `voucherTypeName: MOBILE_LEGENDS`,
  `shopLang: id_ID`.
- **Key gate:** prefs file `divarvian_sharedpref`; strings "Silakan login dulu"
  ("Please log in first"), "Tempel key untuk membuka menu" ("Paste key to open
  the menu"), "Key kosong" ("Empty key").
- **Version:** `27.0`, status `Full` (self-reported).
- **Sabotage targets:** `libbattlelogic.bytes`, `libmlsdk_logreport.bytes`,
  `libagoraSdkCWrapper.bytes` — deleted by the "Bersihkan Cache" button (gated,
  not automatic).
- **Menu tree** (Indonesian, full): Beranda/Visual/Bidik/Peta/Room; ESP toggles
  (`Feature.ESP*`), Aim S1–S3/Basic/Spell, `Ling Manual`, `Franco Hook`,
  Retri menu, `Unlock Skin`, `Lock Target` (Closest/Lowest HP/Lowest HP%),
  Auto Combo (Benedetta/Gusion presets), `Kimmy Quick-Clear`, `Kimmy Minion
  Clear`, drone view, minimap config, Room Info (team/enemy tables with
  rank/winrate/flag).

### 3.3 The cover (`libAkSoundEngine#.bytes`, 25,881,856 B)

Genuine **Wwise 2021.1.12 SDK_MLBB** (`libAkSoundEngine.so` SONAME, Wwise
MIDI/pipeline symbols, `libOpenSLES` dep). It is not a mod — it is the audio
engine shipped to make the comlibs slot look legitimate. Device's real
`libAkSoundEngine.bytes` is 2,686,088 B (md5 `1f029d8e…`) — **neither XNISH
file matches it**.

### 3.4 XNISH vs the other two

| | Rain v41 | VYNIX (new) | XNISH v27 |
|---|---|---|---|
| Size | 1,004,424 | 10,855,800 | 10,785,120 (+25.9 MB cover) |
| Hook fw | GlossHook 1.9.5 | GlossHook 1.9.2 | Dobby |
| Hooks | 1 (+1) | 40 | 29 + 195 patches |
| AC touched | **none** | Estimate*/signature | **195 functions stubbed** |
| Input | synthesized touches | (not analyzed) | state writes |
| Network | none | curl + license server | own server + Telegram + Codashop |
| Safe-read method | `process_vm_readv` | pipe write/read | (n/a — patches, not reads) |

---

## 4. Rain v41 — source re-verified

The two live bugs re-confirmed today, line by line:

- **F1** — `persist.cpp:47` and `synth.cpp:150` both
  `FindClass("com/unity3d/player/UnityPlayer")` from native threads with no
  Java frames; the system classloader cannot see app classes → null →
  persistence no-ops, synth queue never drains. Neither file has a
  `JNI_OnLoad`-time resolution path.
- **F2** — `g_mid_put_string` (persist.cpp:33) is called by `SaveColor()`
  (line 245) but never assigned in `Resolve()` (lines 396–442); the validation
  gate at line 434 checks only bool/int/float/apply. Fixing F1 alone turns the
  first config save into an ART `CHECK_NON_NULL_ARGUMENT` abort.
- **F4** — `ClassifyContainer` (game.cpp:305) still raw-derefs a live pointer
  via `object_get_class`, on the per-frame path (called from
  `SnapshotPlayers` → line 877 and `SnapshotMonsters` → line 904).

Everything else in the architecture reads as documented in round 7: the
`process_vm_readv` peek boundary (`game.cpp:454`), the `Fill` sanity gates
(camp ∈ [0,16], HP range), the one-snapshot-per-frame model, the IL2CPP
resolver walking program headers because MLBB's loader maps `liblogic.so`
outside the linker (section headers unmapped), `thread_local` render-thread
attach, retryable overlay init.

---

## 5. The game — new territory this round

### 5.1 BattleRecord replay format (decoded, first time)

`/sdcard/Android/data/com.mobile.legends/files/dragon2017/BattleRecord/*.bytes`
— 11 files pulled (7 real matches + 1 empty + 3 small). Layout:

```
70 70 00 02 80 41 <header>
  → tag stream (0x4f <tag> <len> <ascii> = string field; 0x0f/0x6f/0x7f = ints)
  → client_common_config table: 90–93 keys per match, e.g.
       key 24  = "Summon_Wukong_AIRouge;Summon_MeiLiSha_AI;RankAloneAI2_Warm;…"  (AI trees)
       key 87  = "RankAloneAI2_Stand-1,2,3,4"
       key 399 = "local SystemData= luanet.import_type('SystemData') SystemData.m_bEndPlayOptimize = false"  ← embedded Lua
       key 434 = "switch=1;iCheckRoadTime=180000"
  → player records: tag 0x33 = team name, 0x34 = squad tag
       e.g. "Dawuhan T Santuy"/"DTS", "peminat bokep"/"XNXX", "C.Familiae Onera"/"cF"
  → battle config blob: AShowSkillConfig / Effect_extra_* tables (skill ids, params)
  → ~20,700 frame packets (delta stream, each ~16–24 B)
  → footer: "2.2.16.1232.1.2687730"
```

Match metadata classes confirmed in `global-csharp-metadata.dat`:

| Class | Fields |
|---|---|
| `ReplayData` | `CampType`, `BattleTime`, `OwnerFpsId`, `killEvents`, `highlightEvents` |
| `ReplayKillEvent` | `type`, `time`, `killer`, `victim`, `assistants`, `teamBattleID`, `continueKill`, `multiKill` |
| `ReplayHighlightEvent` | `id`, `sub`, `time`, `teamBattleID`, `priority`, `param` |
| `BattleReplay` | `data`, `state`, `perspectiveState`, `Enable`, `Perspective`, sessions |
| `ReplayBattleSession` | `_data`, `_currentFrame`, `_complete`, `Rate`, `OnMd5Desync`, `_isMd5Error`, `jumpFrameIndex` |

**Why this matters:** this is a *zero-risk* data source — it is the game's own
file, no injection needed. It carries the full `client_common_config` table
(the tuning knobs), the AI tree assignments, per-match skill/effect configs,
and per-kill events. For Rain, this is calibration data for the retribution
model and a match-history reader — both offline, both untouchable by AC.

### 5.2 BBAI SDK — pulled and verified

All nine files pulled and **md5-matched against the game's own
`BBAI1_Version_1278.xml` / `BBAI2_Version_1278.xml`**:

| File | Size | MD5 (verified) |
|---|---|---|
| `libmobile_bbai.bytes` | 90,995,760 | `110210C9…` ✓ |
| `model.bin` | 24,585,512 | `7FB781AB…` ✓ |
| `map.bin` | 4,687,848 | `A53D5484…` ✓ |
| `feature.bin` | 1,597,544 | `4148AB5C…` ✓ |
| `master.bin` | 1,502,968 | `CF4A0572…` ✓ |
| `table.bin` | 50,696 | `5088A16A…` ✓ |
| `libbytenn.bytes` | 2,640,192 | `1980BCA7…` ✓ |
| `libcpp_shared.bytes` | 6,238,016 | `9B58A5B1…` ✓ |
| `libtbb.bytes` | 784,760 | `C43CA519…` ✓ |

The XML also pins the **SDK zip md5** (`CEDC9158…`, length 49,377,527) — a
complete integrity chain for the AI subsystem. `libmobile_bbai.bytes` md5
differs between arm64 and arm32, while `feature/map/master/model/table.bin`
are shared — so the NN data is architecture-independent, the runtime is not.

### 5.3 Live game facts (device, this session)

- Version 2.2.16.12322 (`2.2.16.1232.1` real string), targetSdk 36,
  `lastUpdateTime 2026-10-07 20:15`.
- 13 APKs installed incl. `split_WifiDetective.apk` (49,618 B — new split,
  previously uncaptured, now in `analysis/deep9_pull_1008/`).
- `comlibs/arm64-v8a/` holds 16 files; the game's own `libAkSoundEngine.bytes`
  is 2.68 MB, `librudp.bytes` is the stock 1,285,576 B networking module.
- BattleRecord dir has fresh entries from **today 12:11** (a 3.59 MB ranked
  match), proving replays write continuously and are pullable any time.

---

## 6. What each project *is*, in one line

- **Rain** — a surgical, reflection-based overlay that patches nothing in the
  AC surface; the smallest and cleanest of the three, with two JNI bugs
  between it and its full feature set.
- **VYNIX** — the commercial reference: 40 hooks, license-gated against
  `pinikcitt.my.id`, GeoIP region gating, signature forgery, ~10.8 MB; the new
  trial is a toolchain refresh of the same machine.
- **XNISH** — the heavyweight: 195 AC functions neutralized, own backend
  (`youngjoygame.com`), Telegram/Codashop sales, key-gated UI, Wwise cover
  binary; the maximal-intervention bet.
- **MLBB** — the target: three IL2CPP modules behind a custom hot-update
  loader, a 91 MB on-device AI (BBAI, now fully in-tree), a replay system that
  mirrors its whole battle config, and an unsigned `comlibs` slot all three
  mods ride.

---

## 7. Priority order (updated)

1. **Fix F1 + F2 together** (JNI_OnLoad-time class caching + `putString` in
   the Resolve gate). Until then persistence and all input-synthesis
   automation remain dead. F4 next.
2. **Mine the BattleRecord corpus** — the config table is a free calibration
   feed for the retribution model; a reader is offline and AC-invisible.
3. **`git init` + `.gitignore`** — still absent; the tree now exceeds 4 GB of
   which ~3.4 GB is generated churn.
4. **Deploy when ready** — the phone is clean, `rinject` exists as the
   hot-attach fallback, and the `librudp` slot remains download-time-checked
   only (server-controlled, per round 6).
5. **Keep the BBAI set pinned** — it is md5-verified and versioned; re-pull
   only when `BBAI*_Version_*.xml` changes.

---

## 8. Session artifacts

| Path | Contents |
|---|---|
| `analysis/deep10/pull/battlerecords/BattleRecord/` | 11 replay files (7 real matches) |
| `analysis/deep10/pull/bbai/arm64-v8a/` | 9 BBAI files, md5-verified + 2 version XMLs |
| `docs/DEEP-ANALYSIS-8.md` | This document |

---

*Round 8, 2026-10-10. Phone: 5d39a8f2 (Redmi 2607DRA18G, Android 16),
MLBB 2.2.16.12322. Device clean (stock `librudp`); Rain v41 in `build/v41`,
F1/F2/F4 open in source. VYNIX TRIALL decoded to the toolchain level; XNISH
decoded to its sales channel; the game's replay format opened for the first
time.*
