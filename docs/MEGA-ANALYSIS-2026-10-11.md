# Mega-Analysis — 2026-10-11

Four parallel workstreams, one live phone session. Covers: Rain source review,
VYNIX TRIALL (new build), XNISH NEW (verified + license correction), fresh game
pull (APK + metadata + dex + native libs), and live on-device verification.

Artifacts:
- `analysis/rain_review_2026-10-11/` — 4 reports (module, bugs, matrix, config)
- `analysis/vynix_triall_2026-10-11/` — 7 reports + scripts (new build, full diff)
- `analysis/xnish_new_2026-10-11/` — 6 reports + 365 extracted assets
- `analysis/game_2026-10-11/` — 6 reports (APK, dex, metadata, AC, libs, endpoints)
- `analysis/pull_2026-10-11/` — fresh APK set + metadata + extracted native libs

---

## 1. The big picture

The game's IL2CPP metadata is **byte-identical between the Sep 27 pull (2.1.67)
and today's fresh pull (2.2.16.12322)**. Moonton is not recompiling the IL2CPP
layer between hotfix builds — only the Android layer (dex + native libs)
changes. Every field index in MLBB-INTERNALS.md remains valid, and Rain's
reflection-by-name design is validated: the game changes underneath it without
breaking it.

All three mods (Rain, VYNIX, XNISH) target the same build, deploy through the
same comlibs vector, and have now been fully reverse-engineered side by side.

---

## 2. Rain — review results

### 2.1 Confirmed prior findings
- **synth dead**: `synth.cpp:150` bare `FindClass` from render thread fails.
  `synth::Init()` has no callers. **Live-verified today**: JNI_OnLoad DOES fire
  (settling the load-model ambiguity), the class loader IS captured — synth
  just never uses it. Zero `synth:` log lines across all captures, ever.
- **F4 ClassifyContainer**: `game.cpp:317` live deref — still open, all four
  container walks funnel through it.
- **target::Query dead filters**: `cc_only`/`use_atk_range` assigned nowhere.

### 2.2 New findings
- **P0-2/P0-3: `DictEach` return-value corruption** (game.cpp:1025, 1964).
  `DictEach` returns *visited*, not *filled*. Monster-dict worst case: ~380 KB
  OOB read past the 256-slot snapshot. Spell-CD case: hero CD dicts carry ~6
  entries against a 4-slot stack buffer → OOB stack read on **every spell-CD
  row drawn**. This is the "random crash" class.
- **P1: notify leaks a JNI global ref per retry** (up to 150).
- **P1: objective tracker survives match boundaries** → stale pit positions.
- **P1: target::Select uses the lagged camp latch** instead of the live camp.

### 2.3 Feature matrix result
Every visual feature (ESP, minimap, maphack flags, drone, skin, alerts,
range circle, calibration, persistence, profiles, account reset) is reachable
and functional. All five combat features (retri, aim, skill aim, combo, Kimmy)
are code-path-reachable but **cannot act** — every dispatch dies at the synth
layer. One fix (VM/loader reuse in synth) restores all five.

---

## 3. VYNIX TRIALL — new build analysis

**Size**: 10,855,800 B (+22,712 vs old). **95.5% of functions identical.**

- New `.comment`: LLD 18.0.3 / clang 18.0.3 — toolchain refresh, not redesign.
- **License verdict rebuilt**: MD5-hex equality, `NECROAJA` → `SUCCESS` tag.
  Endpoint ciphertext byte-identical (`pinikcitt.my.id/api/connect`, key 0x3A).
  Trial expiry → 10 Oktober 2026.
- **NEW pipe helper @0x7d7ba0** (37 callers): fault-tolerant ≥64 KiB copy via
  kernel pipe — same idea as Rain's process_vm_readv boundary, independently
  invented.
- **40 hooks, same targets, same order**: drone (set_offSetPos, fieldOfView),
  ESP (WorldToScreenPoint), time scale, and the full `Estimate*` family +
  APKSignature forgery + AntiPluginData suppression.
- One embedded 43,912-byte TTF deleted from `.data` (43,896 B accounted).

**Assessment**: VYNIX is a polished commercial mod that actively hooks the
game's anti-cheat (~20 of its 40 hooks are AC-surface). It forges APK
signatures, suppresses anti-plugin reports, and neuters the estimate family.

---

## 4. XNISH NEW — verified + major correction

**Binary identical to the prior analysis** (md5 6015317b…) — all findings
re-verified with **zero mismatches**: 195 stub sites, 333 XOR-0x7E strings,
29 DobbyHooks.

### The correction that matters
**The license gate is REMOTE, not local.** Prior passes called it local.
Decoded end to end:
- URL: `https://melzmod.biz.id/connect` (MBA-obfuscated char loop, inverted)
- POST: `game=YL&user_key=%s&serial=%s`, serial =
  `UUID.nameUUIDFromBytes(android_id)` — device binding
- Server JSON: `status/data/token/EXP/rng/reason`; client computes local MD5
  over material including server `rng`, `memcmp`s vs `token`; match → `"OK"` →
  menu gate opens. TLS peer verification explicitly disabled.

### The stub mechanism (fully disassembled)
195 sites → `movz x0,#0; ret` (100×) or bare `ret` (95×) via a private
`sysconf→mprotect(RWX)→memcpy→mprotect(RX)` patcher. Coverage: ACInterface 37,
LogReportAOT 34, GameMain 13 (all TryInject*), MobaScriptBridgeIMPL 12, Gumiho
7, ComFunc 5 (self-MD5), LogicPunish hook checks, every report sender.

### Packet-level cheat
**49 message IDs silently swallowed** in the SendRawData detour. Skin spoof
writes `uiSkinId`/`uiSelHero` into player data at packet level. Maphack via
CanSight hook. **365 embedded PNGs** (208 flags, 138 hero avatars, banners).

**Cover**: `libAkSoundEngine#.bytes` is genuine Wwise 2021.1.12 SDK_MLBB —
unstripped, 23,548 symbols, real build paths.

**Assessment**: XNISH is the most aggressive of the three — it doesn't just
avoid the AC, it blinds it wholesale (195 functions), then cheats at the
packet layer (49 swallowed message IDs, skin packet rewriting).

---

## 5. The game — fresh analysis

### 5.1 What's new in 2.2.16.12322 (vs Sep 27's 2.1.67)
- +1,101 dex classes, +1 dex file (classes4.dex = packaging bucket: apache
  http, checkerframework, perfsight, fmod)
- New video subsystem (mlvideoplayer/videooverlay/gmvideoplayer bridges)
- ~1,200-class obfuscated `ca.da.ca.*` block
- libsigner.so grew 38% (945KB → 1.30MB) with environment attestation
- ByteHook Java wrapper removed; GameProtect unchanged class-for-class
- PerfSight profiler ships its own package

### 5.2 Metadata — unchanged (byte-identical)
ShowEntity 393/804, BattleManager 45/121, ShowBattleCamera 31/51 — all
identical. Method stride corrected: **32 bytes** (not 64 as the old doc said).
All 30+ documented field indices verified present.

### 5.3 Anti-cheat surface (the wall)
| Class | What it reports |
|---|---|
| AntiCheatReporter | skill-cast cadence (input automation) |
| CheatAfkReporter | duplicate-position AFK signal |
| **BattleInputBehaviorReporter** | click/cast timing rings — **the synth detection surface** |
| HeroLoadDetector | memory/load deltas (injected assets) |
| DataReportMgr | central telemetry; warn-battle tables |
| Gumiho | client integrity self-check (hook/inject/repack flags) |
| **DesyncLog** | frame-hash divergence + S3 forensic upload — **the hard wall for state manipulation** |
| ACInterface | native AC bridge |

### 5.4 Endpoints (authoritative)
```
login.ml.youngjoygame.com:30021        (default login)
global-login.ml.youngjoygame.com:30021
login-mlus.mproject.skystone.games:30021 (US)
report.ml.youngjoygame.com:30071       (battle report socket)
169.57.143.242:9992                    (hardcoded log IP)
http://ip.ml.youngjoygame.com:30220/myip  (cleartext IP probe)
databyterangers.com.cn/simulator/mobile/log (emulator telemetry)
```
AC verdict transport = the same report socket; no separate AC host.

---

## 6. Live phone verification (v43.3)

- **Boot**: clean every time — JNI_OnLoad → 6,998 symbols → 70 images → overlay
  → notification → classes bound. Zero errors.
- **Menu**: all five tabs (Visual/Combat/Map/Status/Settings) captured live,
  fully interactive.
- **Persistence**: `rain_configs/default.json` (6,720 B) written and readable.
- **Notification**: `rain_overlay` channel registered under the game's uid.
- **Account**: banned (~456 days) for "plugin use" — consistent with the
  Account Reset experiment's honest limit (server-side ban, Android ID).
- **No new tombstones** during today's sessions; tombstone_13 (15:00) shows
  Rain frames on a *sibling* thread, not the faulting thread.

---

## 7. Three-way comparison (updated)

| | Rain v43.3 | VYNIX TRIALL | XNISH NEW |
|---|---|---|---|
| Size | 1.06 MB | 10.86 MB | 10.79 MB (+25.9 MB cover) |
| Hook fw | GlossHook 1.9.5 | GlossHook 1.9.2 | Dobby |
| Hooks | 1 (+1 optional) | 40 | 29 + 195 patches |
| AC touched | **none** | ~20 hooks (Estimate*, signature forgery) | **195 functions stubbed** |
| Packet layer | none | none | 49 msg IDs swallowed, skin rewrite |
| License | none | remote (pinikcitt.my.id) | remote (melzmod.biz.id) |
| Input | synth (dead) | n/a | game-state writes |
| Safe-read | process_vm_readv | pipe helper (new) | (patches) |
| Exports | 2 | 1 | ~6,000 |
| Crash posture | peek boundary everywhere | unknown | unknown |

Rain remains the most disciplined: smallest, no AC contact, no network, no
licensing. VYNIX and XNISH both actively fight the anti-cheat; both now have
remote license servers with device binding.

---

## 8. Priority actions

1. **Fix synth VM/loader** (P1-3/P1-4) — reuse notify's captured loader;
   restores all five combat features. Then expect first-run bugs — that path
   has never executed.
2. **Close F4** (P0-1) — peek the container header before reading klass.
3. **Fix DictEach counts** (P0-2/P0-3) — two lambdas, two call sites; kills
   the spell-CD OOB and the monster-snapshot OOB.
4. **Match-scope resets** (P1-6/P1-7) — objective tracker + camp source.
5. **notify ref leak** + ClampConfig on load.
6. **Consider**: the crash A/B test is now more important — tombstone_13 shows
   Rain in a sibling thread during a match that crashed in liblogic.
