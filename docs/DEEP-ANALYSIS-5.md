# DEEP-ANALYSIS-5 — Round 5: Phone-native deep pass (2026-10-02)

First round run against the **physical phone** (Redmi 2607DRA18G, Android 16,
MLBB **2.2.16.12322**) instead of the LDPlayer emulator (2.1.67.11733).
Rain **v37 was deployed on the phone as `librudp.bytes` and live-verified**
(lobby → match → lobby, overlay + menu confirmed in screenshots, zero
errors/crashes across the session). Fresh pull in
`analysis/phone_pull_2026-10-02/`: base.apk (129 MB),
split_config.arm64_v8a.apk (38 MB), all three metadata blobs, full 16-file
comlibs set, live logcat captures. VYNIX + XNISH binaries were re-analyzed
through bounded subagent passes (500 KB read cap, grep-count + excerpt
discipline). One agent inference was verified wrong by direct measurement
and is corrected below (§6).

## 1. The game on 2.2.16 (phone pull)

**APK layout.** `base.apk` = 1498 entries, NO `lib/` dir (natives live in the
split). 4 dex files (classes.dex 9.4 / classes2.dex 10.1 / classes3.dex 7.9 /
classes4.dex 0.8 MB). `split_config.arm64_v8a.apk` = 34 entries: `libunity.so`
(25.7 MB), `libil2cpp.so` (384 KB — only 6 lib strings; the stub), `libsigner`,
`libEncryptorP`, ByteDance stacks (`libbytebench`, `libResources_z*`),
`libc++_shared.so`. All 4 dex grep: `PatchFileList|LibraryVerifyManager|
VerifyManager|comlibs` hits live **only in classes2.dex** — the Java
hot-update/verify layer is classes2-scoped, a useful narrowing for future
dex work.

**PatchFileList is NOT in the APK.** The case-insensitive scan found only a
kotlin-coroutines false positive. Dex strings show where it actually lives:
`Document/android/BinaryPatchMD5.xml`, `(PatchFileList_SDK.xml)`, and
`SDKOverride_AssetsPath{BinaryPatchMD5,PatchListMD5}Missing` — i.e. the
patch list is **server-driven via `overrideAssetsPath`, downloaded post-login**.
Consequence: librudp md5 coverage **cannot be confirmed or denied offline**;
the blind spot previously established on 2.1.67 still holds observationally
(the staged Rain was not touched), but it is a server-side unknown now.
`assets/version/android/version.xml`: `2.2.16.1232.1`,
logip `169.57.143.242:9992`, login `login.ml.youngjoygame.com:30021`;
`realversion.xml`: engine `2019.4.33`, engineInner `2.01.0056.08`.

**The 2.2.16 comlibs set (16 files).** vs the old set, new arrivals:
`libcpp_shared.bytes` (6.2 MB), `libbytenn.bytes` (2.6 MB), `libtbb.bytes`
(785 KB), **`libmobile_bbai.bytes` (91 MB)**, and the `.bin` quintet
(feature 1.6 / map 4.7 / master 1.5 / model 24.6 / table 51 KB). All 5 `.bin`
are length-prefixed blobs (`u64 hdr == size-8`) and **all 5 md5s match the
BBAI1 manifest**. BBAI gating: BBAI1 (`BBAISDK1_1278`, md5-covered) covers
feature/map/master/model/table + `libmobile_bbai`; BBAI2 (`BBAISDK2_1278`)
covers bytenn/cpp_shared/tbb. **`librudp.bytes` is in NEITHER manifest** —
same blind-spot class as `libAkSoundEngine/http_downloader/
mlsdk_logreport/mola_download`. Zero dex string references to
`rudp|librain|bbai`. `libmobile_bbai.bytes` (91 MB, sections read only) shows
a standard NDK `.so` layout, nothing packed-obvious.

**Metadata 2.2.16.** Custom format fully intact (`sanity=0xFAB11BAF`,
`version=1024` on all three). Type counts: logic 6063 / first 1701 /
**csharp 20,375** (+372 vs the 20,003 on 2.1.67). `ShowEntity` = 393 fields /
804 methods; `BattleManager` = 45 / 121. Key field indices stable and
consistent with Rain v37's live offsets (hp=420/hpMax=424 →
`m_Hp=90/m_HpMax=91` consecutive; camp=208 → `m_EntityCampType=54`;
guid=392/id=396 → `m_uGuid=83/m_ID=84` consecutive; type=120 → `m_iType=18`).
**Load-path threat assessment: nothing in 2.2.16 threatens Rain** —
DynamicSOLoader/LibraryInfo shape unchanged, no new verify-type or signature
machinery; the only watch item is csharp typedef growth (+372 types), so any
hardcoded *table-index* lookups (not field offsets) should be re-resolved.

**Asset tree on the phone** (`dragon2017/assets/`): Art ~8.5 GB, Audio ~1 GB,
UI ~1 GB, Document 140 MB, comlibs 137 MB, UnityData_NEW 25 MB.

## 2. Rain v37 — binary profile + live runtime

**Binary** (`analysis/rain_binary_deep.py`, new): 907,312 B, 25 sections,
exactly **2 exports** (`JNI_OnLoad` 80 B / `JNI_OnUnload` 24 B), 176 imports:
49 GL/EGL (complete ImGui GLES pipeline) and **zero `il2cpp_*` imports** —
the manual runtime resolution is confirmed at binary level. 9 constructors in
`.init_array`, `__assert2` present (asserts live in the release build),
clang 17.0.2.

**Correction to prior reporting:** the v26→v27 size break (4.24 MB →
~850 KB) was **stripping debug info** (`.debug_*` 3.1 MB + symtab/strtab
225 KB), not an architecture rewrite — v26 `.text` is 565K vs v37's 635K;
code grew every version. The inventory agent's "architecture rewrite"
characterization is superseded.

**Live runtime** (`phone_pull_2026-10-02/rain_live_logcat.txt`, 164 lines):
heartbeat `esp: self=0 entity=1 …` every ~6.7 s, operator taps arriving as
clean `touchCount=1→0` pairs (timestamps show Down/Up within the same second),
single render thread (tid 20611 throughout), **zero errors/warnings/crashes
in 15+ minutes**. The no-hardcoded-offsets design proved itself on contact:
2.2.16's completely shifted offsets (hp=420 vs the old build's) bound without
a single code change — a hardcoded build would have been dead on arrival.

## 3. VYNIX — boot flow, XOR scheme, license model, anti-cheat mirroring

**Boot flow** (`JNI_OnLoad @0x809c54`): stack canary + PAC/BTI; `GetEnv`
gate; a **7-package detection chain** (`com.hhgame.mlbbvn` → `usa` →
`legends` → `mobilechess.us` → `mobiin.gp` → `legends.lite` → `mobile.v2l`),
selection persisted to a global `std::string @0xa6ca30`; `ActivityThread`
reflection for app context; CRC32-table init (poly 0xEDB88320); `dladdr`
self-path resolve; then a **monitor thread (0x8099dc)** that gates everything
— on 60 s nanosleep cadence it spawns the **hook thread (0x8083ac)**,
which `dlsym`s `eglSwapBuffers` and hooks it, and the **license thread
(0x809818)**, which does `time()`-based expiry math and compares a status
string against **`NECROAJA`** (0x1eb2a6 plain / 0x6c4bfe XOR). JNI_OnLoad's
tail does **`remove()` + `rename()` + obfuscated dlopen** — a drop-and-swap
self-update. Ctors: **Exynos 9810 gating** (`ro.arch` + HWCAP/HWCAP2 →
flag 0xa73c30), ImGui-GL init, base64 PNG icon registration.

**XOR 0x5a scheme (solved):** single-byte XOR on VYNIX's own string pool at
`.rodata 0x6c4803–0x6c5201+` (~460 strings); prior "xor5a decode" XOR'd the
whole rodata so plain lib strings came out Z-heavy garbled — the garble is
the artifact, the 460-string pool is the payload. **Exactly one endpoint
anywhere**: `http://worldtimeapi.org/api/timezone/Etc/UTC` — a time anchor
against clock tampering; the license server host is never in cleartext.
License POST body: `game=MLBB&version=1.0&user_key=%s&serial=%s&resource=%s`
(+32-char key `Vm8Lk7Uj2JmsjCPVPVjrLa7zgfx3uz9E`, hardcoded device-ID fallback
UUID, `VynixCheat` log tag). **Hardcoded trial expiry `26 September 2026` —
already lapsed** on this session's date (2026-10-02); the trial build on disk
cannot license itself. Also `Cheat Mati, Silahkan Beli !!!` nag string and
`IsSignatureSame`/`m_sAPKSignature` gating.

**Feature model** (Indonesian UI): ESP (Minimap/Player/Hero-icon/Hp/Line/
Cooldown/Monster+alert+hp, Gold/Exp/Mid lane labels), `Aim Assist` + Range
FOV, `Hero Combo` + Combo Delay, Drone View + Camera High, TIME SCALE,
Add Gold & Exp, account tools (`Save & Load Akun`, `Reset Guest`, `Yakin
ingin reset guest?`). Persistent keys (`esp_player_health/icon/line/skill`,
`esp_monster_health/alert`, `##new_dev_id`, `MOD_NAME`, …). IL2CPP reflection
targets include `LogicBattleManager`, `GetPlayerRealSelf`, `m_dicMonsterShow`,
`m_EntityCampType`, `set_timeScale`, `UpdateGold/UpdateExp` — VYNIX reaches
the Logic layer that Rain only partially uses.

**Two genuinely new finds:**
(a) **ACInterface mirroring** — the binary carries the game's whole
estimator/reporter API surface (`AntiCheatReporter`, `CheatAfkReporter`,
`EstimateByList/All`, `MemStatistic`, `CheckHook/CheckACHook`,
`MobaScriptBridgeIMPL`, `ACInterfaceLib`, `liblogic.so` as dlopen target) —
i.e. it hooks or spoofs the anti-cheat interface itself, not just the renderer;
(b) **two extra JNI entries** `com.moba.unityplugin.NativeUtility.
invokeFunction{SetAssetManager,PointerLongLong}` — disguise as a Unity moba
plugin. Carrier identity confirmed: complete KCP/rUDP/SDP/LZ4 stack
(`UdpPipeManager`, `mfw::ReliableUdp`, `ikcp_*`), SONAME `libMyLibName.so`
vs shipped `librudp.bytes`. Original 64-bit leaked 2,552 defined symbols vs
32-bit's single `JNI_OnLoad`; the fixup build stripped 64-bit to match.
**Cross-pollination risk for Rain: nil** beyond sharing the librudp slot and
the GlossHook framework — different string pools, no shared payload.

## 4. XNISH — export surface, XOR-7E reflection table, licensing

**Surface** (via readelf, the exports-dump `grep FUNC=0` was a format
artifact — `addr size name` has no type column): MOD = **6135 FUNC / 891
OBJECT**; AUDIO = 1532/214. Statically linked **Dobby** (`DobbyHook/
Instrument/CodePatch/SymbolResolver` + near-branch trampoline toggles,
undef=0) and **OpenSSL (428 `SSL_*`)**. `il2cpp_*` = 0 exported (reflection
via string table). Single `JNI_OnLoad @0x1ed7c0` (5092 B, standard prologue);
undef imports confirm render-hook + injector shape
(`ANativeWindow_getWidth/Height`, `glUniformMatrix4fv`, `mmap`,
`pthread_create`).

**String/obfuscation model:** `str_hash` 63,968 vs `str_small` 21,255;
`only_in_small` head is 3–8 char fragments — a split/garbled table, not C
strings. **XOR 0x7E decodes the Unity reflection/hook table**
(`GetBattlePlayer`, `m_EntityCampType`, `IsHaveSkinForeve`, `WorldToScreen`,
`MobaScriptBridge`/`SendGameReport`, `checkOnlineHook/
checkOfflineHook`, …); XOR 0x5A stays mostly garbled (per-region key or wrong
key) except `…KEY…` fragments. The plain Indonesian key-gate UI was NOT found
in any static set — it lives in a runtime-decrypted ImGui buffer.

**Licensing (mod-only, 7 strings):** Codashop
`https://order-sg.codashop.com/initPayment.action` (+Origin/Referer headers),
Telegram `https://t.me/+sKQszuaIjPw5MDY9` + `openTelegram` handler.
**Auto-retri with exact monster table confirmed in clean strings**:
`Auto retri Lord (2002)`, `Turtle (2003/2110)`, `Red/Blue Buff (2004/2005)`,
`Lithowanderer (2056/2072)` + jungle-emblem warning. `##DroneView`,
`##RetriEnemyDist` ImGui overlays. Crucially, the 0x7E table shows XNISH
touching **`SendGameReport`/`ReportServerWrap` and `HasHooks`-style estimator
strings** — its cheat surface is entangled with the game's own reporting
channel, a risk shape Rain avoids (Rain never writes reports).

## 5. Cross-cutting: three lineages, one slot

| | Rain v37 | VYNIX trial | XNISH mod |
|---|---|---|---|
| Size | 907 KB | 10.8 MB (64) / 10.2 (32) | 10.8 MB + 25.9 MB Wwise cover |
| Exports | JNI_OnLoad/Unload | JNI_OnLoad only | JNI_OnLoad, 6135 FUNC leaked |
| Hook framework | GlossHook 1.9.5 | GlossHook 1.9.2 | Dobby (static) |
| Overlay | ImGui/GLES | ImGui/GLES | ImGui/GLES |
| Game access | IL2CPP reflection (Show layer + camera) | IL2CPP reflection (Show + Logic + ACInterface) | XOR-0x7E reflection table + MobaScript bridge |
| Crypto/net | none (no license) | static curl/OpenSSL, license POST | static OpenSSL, Codashop/Telegram |
| Anti-forensics | stripped, minimal exports | XOR-0x5A pool, PAC/BTI, CRC32 self-check, Exynos gate | split tables, runtime-decrypted UI |
| AC interaction | silent user of Show layer | hooks/spoofs ACInterface estimators | entangled with SendGameReport channel |
| License | none | key+expiry (trial lapsed 2026-09-26) | key gate + payment backend |

All three ride the same **unsigned `comlibs/*.bytes` slot** (`librudp.bytes`
for Rain/VYNIX; `libAkSoundEngine.bytes` masquerade for XNISH) — a device
hosts exactly one at a time in a conflicting slot, and Rain remains the most
engineering-disciplined of the three (no license backend, no report-channel
contact, reflection-by-name).

## 6. Corrections to earlier rounds

1. **v26→v27 was stripping, not a rewrite** (§2) — supersedes the round-4
inventory claim. `.text` grew monotonically 565K → 635K.
2. **The VYNIX agent's §3 claim that `rain_decoded.so` carries VYNIX
fingerprints is wrong.** Direct byte measurement: 0 occurrences of
`NECROAJA`/`Cheat Mati`/`VynixCheat`/`worldtimeapi`/`user_key` in the
decoded blob, but `D:/rain/src` build paths + `il2cpp: bound class` logging
present exactly as in v27/v37.
3. **Resolved: `rain_b64.txt` = base64 of Rain v28, byte-identical**
(md5 `c6395332…`, `cmp` clean). A Rain v28 payload someone staged through
`analysis/tmp_vynix/` on Sep 29 — not a VYNIX second stage. The
`remove()/rename()/dlopen` tail in VYNIX's JNI_OnLoad is the *mechanism* such
a staged payload would ride, which is likely what seeded the confusion — but
this blob is ours, not theirs.
4. **blDSP note**: `exports_*.txt` `grep -c FUNC = 0` is a format artifact
(no type column), not evidence of stripped exports — readelf gives the true
counts (§4).

## 7. Open items (carried + new)

- **Server-side patch-list coverage of librudp is now unknowable offline**
  (§1) — it was device-verified absent on 2.1.67, currently observationally
  absent on 2.2.16, but the list is server-delivered post-login. Watch item.
- Managed-object lifetime guard (round 1→4 carry): still the top engineering
  gap; 2.2.16's aggressive entity mutation evidence unchanged.
- Touch `m_RawPosition` fix: still latent.
- csharp typedef growth 20,003→20,375: re-resolve any table-index (not
  field-offset) lookups before next build.
- New: XNISH's `SendGameReport`-adjacent hook surface deserves a read of
  what Rain's own automation (`synth.cpp` MotionEvents) looks like to
  `BattleInputBehaviorReporter` on 2.2.16.
- Housekeeping (unchanged): no git, credential in `opencode.jsonc` still
  plaintext, ~200 MB stale build dirs, base APK pulled 3× across rounds.

## Artifacts produced this round

- `analysis/phone_pull_2026-10-02/` — base.apk, split_config.arm64_v8a.apk,
  metadata/*.dat (3), comlibs/ (16 incl. 91 MB BBAI module), rain + full
  logcat captures, lobby/match screenshots.
- `analysis/rain_binary_deep.py` — reusable ELF profiler (sections, dynsym
  namespaces, import surface).
- `analysis/tmp_vynix/rain_decode.py` + `rain_decoded.so` — resolves to
  Rain v28 (md5-verified identical to `build/v28/librain.so`).
