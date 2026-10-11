# Project Audit — 2026-10-11

Independent full-project pass: source, docs, analysis artifacts, build state, and
the live device. Every P0/P1 claim below was re-verified against the actual
source by direct read, not carried over from prior reports.

**State at audit time:** v43.3 built (18:55), md5 `1140ed7deb3926e3f0e444897159d1a4`
(arm64) / `6090df3fea91526eced77d2cce1b7242` (armv7), staged live on the Redmi
(`librudp.bytes`, 1,065,704 B — md5 match). Git repo initialized 2026-10-10,
176 files tracked, HEAD `ca87df4`. MLBB not running during audit.

---

## 1. What the project is

Rain is an AArch64 native overlay + runtime-inspection module for MLBB, loaded
by replacing the unsigned `comlibs/arm64-v8a/librudp.bytes` hot-update slot.
The game's own `DynamicSOLoader` copies `*.bytes` → `app_libs/*.so` and
`dlopen`s it — no signature check on that path. The module:

* resolves the IL2CPP runtime by walking `liblogic.so`'s PHDR/PT_DYNAMIC by
  hand (the game's loader bypasses the dynamic linker, so `dlsym` can't see it),
* hooks `eglSwapBuffers` via GlossHook and draws an ImGui GLES3 overlay,
* reads game state entirely through the IL2CPP reflection API by *name* —
  zero hardcoded offsets,
* routes every entity read through a `process_vm_readv` peek boundary,
* and deliberately touches no anti-cheat surface.

That last point is the architectural identity. VYNIX (40 hooks, ~20 on the AC
surface) and XNISH (195 stub sites) survive by lying to the checkers. Rain
survives by not being where the checkers look.

---

## 2. Verified bug list

### P0 — crash class

**P0-1 · `ClassifyContainer` live deref** — `game.cpp:317`

```cpp
Il2CppClass* k = api.object_get_class(reinterpret_cast<Il2CppObject*>(obj));
```

`obj` is a *live* game pointer; `il2cpp_object_get_class` dereferences it. This
is the exact use-after-free class the v41 fix addressed elsewhere — the correct
pattern already exists 60 lines up (`IsShowPlayerObject`, `game.cpp:260-273`,
reads the class pointer from the *peeked copy*). Four call sites funnel through
it: `game.cpp:409` (DeriveLocalCamp), `:950` (SnapshotPlayers), `:1005`
(SnapshotMonsters), `:1210` (ApplyMaphack). The player/monster walks are on the
per-frame path, so this is the standing "random crash" hazard at match
boundaries — the same teardown window that crashed v40.

*Fix:* read the klass from the peeked copy exactly as `IsShowPlayerObject`
does. ~15 lines.

**P0-2 · `DictEach` return-value corruption** — `game.cpp:1025` and `:1964`

```cpp
n = il2cpp::DictEach(dict, [&](int32_t, void* val) { ... n++; });
```

`DictEach` (`il2cpp.h:168-205`) returns the number of entries *visited* — the
lambda's `n++` counts *filled*. Both write `n`; the return value wins. When the
dictionary has more live entries than `max`, `n` ends up larger than the
destination array and the caller loops past it:

* `game.cpp:1025` (monster dict fallback) — worst case ~380 KB read past the
  256-slot `g_snap.monsters[]`.
* `game.cpp:1964` (spell cooldowns) — `esp.cpp:467` uses `SpellCD cds[4]`; a
  hero's CD dict typically carries ~6 live entries, so the overrun is the
  *normal* case on every spell-CD row drawn. `autox.cpp:356/367` use `cds[8]`,
  overrunnable by up to 4096.

*Fix:* count fills in a lambda-local variable; ignore the return value.
The third call site (`game.cpp:2016`) is clean — it already uses a separate
`found` variable. ~6 lines total.

### P1 — correctness / feature-dead

**P1-1 · `synth` is dead — all five combat features cannot act.**
`synth.cpp:150` calls a bare `env->FindClass("com/unity3d/player/UnityPlayer")`
from the render thread (via `overlay.cpp:190` → `synth::Tick` → `Resolve`).
A non-Java thread with no stack frame resolves through the system class loader,
which cannot see the app's class — returns null, every time. `synth::Init()`
has no callers (grep-verified). The class loader *is* captured — in
`notify::CaptureClassLoader` from `JNI_OnLoad` — synth just never uses it.

Consequence: auto-retribution, basic-attack assist, skill auto-aim, auto combo,
and the Kimmy hold are code-path-reachable but every dispatch no-ops. Fixing
this one resolution path restores all five.

*Fix:* resolve `UnityPlayer` through notify's captured loader (`loadClass`), or
re-capture via `JNI_GetCreatedJavaVMs` in `Boot()` as belt-and-braces. Call
`synth::Init()` from boot once the loader exists.

**P1-2 · `notify` leaks JNI global refs on retry** — `notify.cpp:237-267`

`Install()` overwrites `g_cls`/`g_instance` with fresh `NewGlobalRef`s each
attempt. On the *retryable* failure path (`install()` threw → `return false`
at :265), the previous refs are never deleted and `g_gave_up` is not set — the
boot loop (`rain.cpp:77`) retries 150×, leaking two refs per retry. Worst case
~300 global refs against a table that can be as small as 512.

*Fix:* delete old refs before overwriting (or track first-failure and give up).
~10 lines.

**P1-3 · Objective tracker survives match boundaries** — `game.cpp:1775-1795`

`g_turtle`, `g_lord`, `g_buff_camp`, `g_litho_camp`, `g_clock`, and per-track
`last_hit_time`/`have_pos` are not cleared by `ResetMatchState`
(`game.cpp:101-108`). Stale `last_hit_time` can fire a false "Turtle Under
Attack" banner in the first seconds of a new match; `TrackCampGroup`'s
`have_pos` keeps stale pit positions for `ObjectivePos`.

*Fix:* clear the tracker state inside `ResetMatchState`. ~10 lines.

**P1-4 · `target::Select` uses the lagged camp latch** — `target.cpp:83`

`Select` reads `game::LocalCamp()` (the debounced latch) instead of
`snap.local_camp` (live this frame, refreshed at `game.cpp:1104-1113`).
During the camp-change debounce window targeting can treat own team as
enemies — aim/retri/combo cast at allies.

*Fix:* take the camp from the snapshot the caller already holds. 2 lines.

### P2 — worth a pass (spot-checked, not all verified)

| Item | Where |
|---|---|
| Non-finite floats accepted from JSON (`inf`/`nan` → NaN minimap vertices) | `cfgjson.cpp:501-521` |
| Unbounded ints (`esp_glow_layers=1000` → 1000 draw passes) | `cfgjson.cpp:496-499` |
| Kimmy hold can claim the player's own finger (6 px match) | `autox.cpp:298-311` |
| Stale GL texture names across context recreation | `icons.cpp:63-67,101` |
| `EntityName` over-reads peeked buffer (~255 chars stale stack) | `game.cpp:1917` |
| Dead filters `cc_only`/`use_atk_range` (assigned nowhere) | `target.h:96-97` |
| `LastTapPos` never invalidated (stale calibration) | `input.cpp:198-203` |
| `map_minimap` checkbox drives overlay *and* engine flag | `game.cpp:1217,1251` vs `esp.cpp:219` |
| `g_dirty` unsynchronized | `persist.cpp:62,65` |
| `TickAim` cadence 90 frames (1.5 s) — far too slow | `autox.cpp:171` |
| Failed Tap still burns skill cadence | `autox.cpp:401,415` |
| Fixed 780×720 menu overflows 720p | `menu.cpp:1113` |

---

## 3. What is genuinely well-built

* **Reflection-by-name.** Validated by the game itself: the fresh metadata pull
  is byte-identical to Sep 27 across two hotfix builds. Field indices in
  `MLBB-INTERNALS.md` remain valid; the design absorbed a game update without
  a rebuild.
* **The peek boundary.** The right architecture for reading a live process —
  `process_vm_readv` into a stack copy, EFAULT instead of SIGSEGV, field-sanity
  checks for freed-but-mapped garbage. One hole (`ClassifyContainer`) does not
  invalidate the design.
* **The v41 fix pattern.** `IsShowPlayerObject` takes `const void*` so the
  live-deref mistake *can't* be repeated — signature-level enforcement of the
  invariant. This is the model for the P0-1 fix.
* **Build hygiene.** Static libc++ (required — `libc++_shared.so` isn't
  resolvable in the loader context), version script (2 exports), strip,
  BIND_NOW, 16 KB page alignment, dual ABI. 7 DT_NEEDED, matches the
  dependency profile of every other `app_libs` module.
* **Documentation culture.** 23 docs, version-by-version history, honest
  failure write-ups (ACCOUNT-RESET's three-way un-ban experiment is a model of
  negative results documented rather than buried).
* **The no-AC-touch stance.** Defensible and deliberate, and the *only* reason
  Rain can ship to strangers without a remote license server like VYNIX/XNISH.

---

## 4. Documentation state — contradictions found

1. **`librudp` integrity claim.** Rounds 1-5 say the slot is an unverified
   blind spot; DEEP-6 reverses it — the server-delivered `BinaryPatchMD5.xml`
   *does* contain `librudp.bytes` (original md5 `51bac6f7…`); survival is
   because the check is download/repair-time, not continuous. `docs/RAIN.md:59-61`
   still says "no signature check", the correction action was never applied.
2. **`MLBB-INTERNALS.md` stride.** Says method stride 64; corrected to 32 in
   MEGA §5.2; never fixed in place.
3. **Type count.** 20,003 (INTERNALS) vs 20,375 (DEEP-2/4) vs "metadata
   byte-identical" (MEGA). The +372 appears to be a pull artifact — unresolved.
4. **`RAIN.md` deploy recipe.** Still shows `su -c cp` (root) and `build/r`
   paths; the device is non-root and the tutorials supersede it.
5. **Stale "no git" claims.** Repo exists (Oct 10), 176 tracked, .gitignore
   comprehensive. Prior findings predate it.
6. **`MEGA-ANALYSIS-2026-10-11.md` is untracked.** The most authoritative bug
   list in the tree is not in git.
7. **Staged-state drift across docs** (v38 → VYNIX → stock → v43.3) — resolved
   for now: v43.3 verified staged.

---

## 5. Ecosystem (for context)

| | Rain | VYNIX TRIALL | XNISH NEW |
|---|---|---|---|
| Hook engine | GlossHook | GlossHook 1.9.2 | Dobby (static) |
| AC strategy | **none — by design** | 40 hooks, ~20 AC-surface (signature forge, `Estimate*` spoofs, AntiPluginData suppression) | 195 stub sites (ACInterface ×37, Gumiho ×7, DesyncLog, report senders), 49 swallowed packet IDs |
| License | none | remote, `pinikcitt.my.id`, trial expired **2026-10-10** | remote, `melzmod.biz.id`, device-bound serial, TLS verify disabled |
| Survival model | not where the checkers look | lie to the checkers | stub the checkers |

Notable: VYNIX's trial expiry passed yesterday. XNISH's license binds
`UUID.nameUUIDFromBytes(android_id)` — the same device anchor MLBB itself uses,
which is why the ~456-day ban on the test device follows the device across
reinstalls (documented honestly in `ACCOUNT-RESET.md`).

---

## 6. Anti-cheat survivability — the honest read

The comlibs drop-in is safe *today* because `DynamicSOLoader` only md5/xxhash-
verifies the update manifest, and `BinaryPatchMD5.xml`'s coverage of
`librudp.bytes` is checked at download/repair time. That is a server-controlled
property, not a guarantee — the watch item stands.

Rain's exposure surfaces, in order of risk:

1. **Behavioral telemetry (highest).** `BattleInputBehaviorReporter`,
   `AntiCheatReporter` (skill-cast cadence rings), `LogicPunish`. The moment
   P1-1 is fixed and touches actually dispatch, the automation is measurable.
   Default-off posture is correct; keep it.
2. **`HeroLoadDetector`.** Memory-baseline per hero/skin — the skin changer
   writes `m_ShowSkinId` (`game.cpp:1767`), exactly the asset state this
   detector baselines. XNISH stubs the skin-md5 reporter; Rain has no cover.
3. **`DesyncLog`.** Per-frame state hashes. Rain's maphack writes
   (`m_bUnityBloodVisible`/`m_bUnityMinimapVisible`) are presentation flags —
   low but nonzero risk if those bits are in the hash bitmap. ESP/minimap read
   only and are invisible here.
4. **`Gumiho` / `ACInterface.CheckHook` / `Estimate*`.** The `eglSwapBuffers`
   and `set_offSetPos` hooks alter function prologues; the control-flow
   estimators walk basic blocks. VYNIX/XNISH neutralize this layer; Rain bets
   it stays telemetry-shaped. So far, that bet has held.

---

## 7. Recommendations, priority-ordered

1. **Fix P0-1** (`ClassifyContainer`) — the standing crash hazard. Use the
   `IsShowPlayerObject` pattern; change the signature to `const void*` so the
   mistake can't recur.
2. **Fix P0-2** (`DictEach` ×2) — 6 lines; kills the "random crash" class and
   the guaranteed per-row OOB on spell CDs.
3. **Fix P1-1** (synth loader) — restores all five combat features with one
   resolution path + one `Init()` call.
4. **Fix P1-2** (notify refs) — 10 lines, prevents a slow global-ref bleed.
5. **Fix P1-3/P1-4** (match-scope resets) — 12 lines combined.
6. **Doc pass:** RAIN.md integrity claim + deploy recipe; INTERNALS stride;
   commit the MEGA doc; reconcile the type count or mark it resolved-as-artifact.
7. **Housekeeping:** crash A/B test (Rain on vs off, same match conditions) —
   still unrun; delete the 0-byte `nul` artifact.
8. **Do not** add AC-surface hooks to "match" VYNIX/XNISH. The no-touch stance
   is the product's identity and its best long-term bet.

---

*Audit performed by reading: all of `src/` (9,523 lines), 23 docs, the
`rain_review_2026-10-11` report set, and live device state over ADB. P0/P1
findings independently re-verified at the cited lines.*
