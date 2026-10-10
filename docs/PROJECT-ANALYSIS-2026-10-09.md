# Rain — Full Project Analysis (2026-10-09)

Independent review pass over `D:\rain`: source, docs, builds, crash corpus,
reference binaries, and the live device state. This is a *current-state*
document; where it contradicts earlier rounds, the direct measurement wins and
the contradiction is called out.

The headline: the project is in better shape than the docs claim in some places
and worse in others. The ESP/overlay/drone half is real and verified. The
persistence and input-synthesis half **has never worked on any build**, and the
docs describe it as working. Two bugs explain why, and one of them is a latent
process-abort that the other one is currently masking.

---

## 1. Current state

| Area | State |
|---|---|
| Source | v41, 21 files, ~7.8k LOC C++17, CMake + NDK r26b, arm64-v8a, android-26 |
| Latest build | `build/v41/librain.so`, 1,004,424 B, md5 `20b220f355ed7bc70abe621937f992d8` |
| Binary hygiene | 7 DT_NEEDED, BIND_NOW, exactly 2 defined exports, stripped — all verified |
| Device `5d39a8f2` | **VYNIX is staged, not Rain.** `comlibs/arm64-v8a/librudp.bytes` = 10,855,800 B, md5 `bcc16ce48ca9b4f8a6e55452fe930370`, mtime 2026-10-08 22:26 |
| Restore point | `/data/local/tmp/orig/librudp.bytes` intact (1,285,576 B, md5 `51bac6f79e3e13c5823e44b352686fb4`) |
| Root | **not available** — no `su`, `run-as` refused (non-debuggable). The `su -c cp` deploy/restore recipes in `RAIN.md` no longer run on this device |
| Repo | still no git, no `.gitignore`, no CI. `opencode.jsonc` is gone from the tree |
| Dead weight | `build/` 454 MB (41 version dirs), `analysis/` 3.0 GB |

The deploy folder `analysis/deploy_vynix_1008/` holds a copy of v41
(`rain_v41_librudp.bytes`, md5 matches) plus three screenshots — but those
screenshots are **VYNIX's menu** (`VynixCheat |` title, `AUTO FEATURE` /
`ROOM INFO` / `ACCOUNT` tabs, Indonesian promo art), not Rain's. Whatever
session produced them was a VYNIX session.

---

## 2. Architecture

```
comlibs/arm64-v8a/librudp.bytes        (unsigned hot-update slot, no md5 check on load)
  → app_libs/librudp.so                (copied by the game's own loader)
  → dlopen by MLBB's loader (not the linker)
  → constructor + JNI_OnLoad           (both fire; pthread_once gates the boot)
  → boot thread
      il2cpp::Init()                   resolve liblogic.so via /proc/self/maps,
                                       walk PHDR → PT_DYNAMIC → DT_SYMTAB/DT_GNU_HASH
      ConfigDefaults + persist::Load   <-- broken, see F1
      overlay::Install()               GlossHook eglSwapBuffers
  → render thread (Unity GfxDevice worker, thread_attach'd)
      ImGui frame → esp::Draw / maphack / ApplyDrone / TickSkin / autox::Tick / synth::Tick
```

Load-bearing details, all confirmed in source and against logs:

- **No hardcoded offsets.** Classes and fields resolve by name through the
  IL2CPP reflection API across all loaded images; the image list refreshes
  because HybridCLR loads the game assembly late, and negative lookups are never
  cached. This is why 2.1.67 → 2.2.16 survived with shifted offsets.
- **The peek boundary.** Every entity read goes `SafePeek` (`process_vm_readv`)
  → `PeekEntity` → `Fill`, so a freed page returns `EFAULT` instead of taking a
  SIGSEGV, and `Fill`'s sanity checks (camp ∈ [0,16], HP range) reject
  freed-but-mapped garbage. Writes touch the live object only after the copy
  validates.
- **One snapshot per frame.** `game::Snapshot()` walks each list once and every
  consumer reads that copy; objective HP tracking rides the same walk.
- **Touch in** polls `UnityEngine.Input.get_touchCount/GetTouch` (no Java input
  hooks). **Touch out** synthesizes `MotionEvent`s through the activity's own
  `dispatchTouchEvent`. The second half is broken — F1.

---

## 3. New findings

### F1 — JNI app-class resolution fails from every thread Rain uses. Persistence and input synthesis have never worked.

**Severity: high (functional), and it is the root cause of the "unverified
automation" column in every round's notes.**

`persist.cpp:47` and `synth.cpp:150` both do:

```cpp
jclass up = env->FindClass("com/unity3d/player/UnityPlayer");
```

`com/unity3d/player/UnityPlayer` is an **application** class (present in the
APK's dex — `r5_dexdump2.txt` carries `Lcom/unity3d/player/UnityPlayer;`; it is
not in the boot classpath).

Both call sites run on **native threads with no Java stack frames**:

- `persist::Load()` is called from the boot thread (`rain.cpp:66`), a plain
  `pthread_create` thread.
- `synth::Tick()` is called from the render thread (`overlay.cpp:173`), Unity's
  GfxDevice worker — the same thread the overlay has to `il2cpp_thread_attach`
  because it is not a Java thread.

Per the Android JNI contract, `FindClass` on an attached native thread with no
Java frames resolves through the **system class loader**, which cannot see
application classes. It returns `null` (with a pending exception that the code
clears). Therefore:

- `persist::CurrentActivity()` returns null → `OpenPrefs()` returns null →
  `Load()` returns early **and never retries** (it is called exactly once, from
  `Boot()`).
- `synth::Resolve()` returns false at its first check → `synth::Tick()` returns
  before draining the queue → every `Tap`/`Drag`/`HoldBegin` queues into a
  buffer that never drains. `Tap()` refuses once `g_qn > 2`, so the automation
  silently no-ops forever.

**The logs prove it.** Across every capture in `analysis/`:

- `persist: JNI bridge resolved` — fires **once** (deep6, 2026-10-02 22:19:51).
  `persist::Resolve()` succeeds because it only touches *boot* classes
  (`android/content/Context`, `android/content/SharedPreferences`, `$Editor`),
  which the system class loader can see.
- `persist: config loaded` — **never fires, in any log, on any build.** The
  string is in every binary since v37; it has never been printed.
- `synth: dispatchTouchEvent bound`, `synth: physical display` — **never fire.**
- `auto:`, `kimmy:`, `drone:` — no automation log line has ever appeared.

The asymmetry is the smoking gun: the boot-class half of `Resolve()` logs
success, the app-class half never does.

**Consequences:**

1. Config persistence is a complete no-op in both directions. Nothing is ever
   saved; nothing is ever loaded. Every launch is factory defaults. The Settings
   tab text — *"Settings save automatically a couple of seconds after any change,
   and load when the game starts"* — is false.
2. Every combat feature that needs synthesized input (auto-retribution, basic
   attack assist, skill auto aim, auto combo, Kimmy hold/steer) **has never
   fired**, on any build, ever. The "needs a match to verify" items in the
   v39/v40 notes are not unverified — they are dead.
3. What *does* work is unaffected: ESP, minimap overlay, maphack flag writes,
   drone (hook + field write + FOV), skin changer, objective tracking, the
   status tab. None of those need JNI app classes.

**The fix** is the standard one and it is cheap: `JNI_OnLoad` **is** called on
this load path (the boot log shows `Rain: constructor entry` and
`Rain: JNI_OnLoad` from the same tid, 2 ms apart — the comment at the top of
`rain.cpp` claiming JNI_OnLoad is never called is wrong). Do the `FindClass`
there, `NewGlobalRef` the results, and hand them to `persist`/`synth`. A
`JNI_OnLoad`-time `FindClass` uses the class loader that loaded the library, so
it sees app classes.

### F2 — `putString` is never resolved. It is a latent JNI abort, currently masked by F1.

**Severity: high (latent) — it detonates the moment F1 is fixed.**

`persist.cpp` declares `g_mid_put_string` (line 33) and calls it from
`SaveColor()` (line 245):

```cpp
env->CallObjectMethod(ed, g_mid_put_string, k, s);
```

`Resolve()` resolves `putBoolean`, `putInt`, `putFloat`, `apply` — and **never
assigns `g_mid_put_string`**. Its two validation blocks check the other five
method IDs; `put_string` is in neither list. Confirmed in every binary checked
(v26, v37–v41, verify): the literal `putString` appears in **none** of them,
while `putBoolean`/`putInt`/`putFloat` appear in all.

ART aborts on a null `jmethodID` at the `Call*` entry point
(`CHECK_NON_NULL_ARGUMENT` → `JniAbort`, not a NullPointerException). So the
first call to `SaveColor()` — which `Save()` makes for `col_dead`,
`range_circle_col`, and all nine `cat%d_col` keys — aborts the process.

**Why it has never fired:** `Save()` bails at `OpenPrefs()` → `CurrentActivity()`
→ the same app-class `FindClass` failure as F1. The two bugs are stacked:

```
Save() → OpenPrefs() → CurrentActivity() → FindClass(UnityPlayer) → null → return
                                                                     ^ F1 stops it here
SaveColor() → CallObjectMethod(NULL) → JniAbort                      ^ F2 waits here
```

Fix F1 alone and the very next config save kills the client. **Fix both
together, and resolve `putString` before anything else in `Resolve()`.**

Also worth correcting while in there: the comment in `rain.cpp` says *"the load
is best-effort and retries are unnecessary (Load marks itself done)"* — `Load`
marks nothing, and because it is called exactly once from `Boot()`, its failure
is permanent for the session.

### F3 — Crash corpus attribution: no Rain frame in any of the six MLBB tombstones.

All six MLBB tombstones in `crash_1008/` map a `librudp.so` with Rain's
four-segment layout (the stock `librudp` is three segments; VYNIX is three).
Segment fingerprints:

| Tombstone | When | librudp segment fingerprint | Nearest build |
|---|---|---|---|
| t04–t07 | 2026-10-05 16:17–22:53 | `0x3f000 / 0xa2000 / 0x5000 / 0xc000` | **v38** (`build/verify` identical) |
| t08–t09 | 2026-10-08 00:26 / 16:31 | `0x3f000 / 0xa4000 / 0x6000 / 0xf000` | v40/v41 family (no byte-exact match in the tree) |

- t04–t07 match v38's layout exactly — consistent with v38 being the deployed
  build on Oct 5.
- t08–t09 match the v40/v41 segment *offsets* but not the sizes; t08 is dated
  **two minutes before** v41's build timestamp, so these were intermediate
  builds that were never archived. This is worth knowing: the builds that
  crashed are not in `build/`.
- **In none of the six does a crash PC fall inside librudp's executable range.**
  Every crash thread is in the game's own libs (`libcsharp` / `liblogic` /
  `libfirst`), reached through `libunity`. `backtrace_frames` counting frames
  whose module is librudp = 0 for all six.
- t08/t09 fault addresses (`0x416b1893c1a1c40e`, `0x416a5810c1a14663`) decode as
  two `float`s each, both in world-coordinate range (≈14.7 / −20.2). That
  supports round 7's "a Vector3 dereferenced as a pointer" hypothesis — Rain
  writes Vector3-shaped data (`set_position`, the `offSetPos` detour) — but it
  remains a hypothesis, not a proof.
- t00–t03 are WhatsApp, unrelated (`libessential.so`, SIGABRT).

Net: the crash corpus is consistent with "the mod was loaded and the client
crashed in its own IL2CPP invoke path". It does not implicate Rain, and it does
not exonerate it either — the A/B test in the open-threads list is still the
only way to settle it.

### F4 — `ClassifyContainer` is still the last raw dereference, and it is on the hot path.

`game.cpp:305`:

```cpp
Il2CppClass* k = api.object_get_class ? api.object_get_class(
    reinterpret_cast<Il2CppObject*>(obj)) : nullptr;
```

`il2cpp_object_get_class` dereferences the pointer it is given. This is exactly
the shape of the v41 use-after-free, and it runs **before** any peek:

- `SnapshotPlayers` → `ClassifyContainer(list, &count)` — once per frame, on the
  container read from the manager.
- `ApplyMaphack` → same.
- `SnapshotMonsters` → same, plus again on the dict fallback.

The v41 fix note calls this "lower risk" (one long-lived container, not a
256-slot per-entity loop during teardown). That reasoning holds — but the
container pointer comes from a static field that the game rewrites on scene
change, and `ApplyMaphack` runs on every frame while the maphack toggle is on.
The cheap fix is to read the class pointer from a peeked copy of the container
header, exactly like `IsShowPlayerObject` now does:

```cpp
// peek the container header, read klass from the copy, walk parents from there
uintptr_t k = 0;
alignas(16) uint8_t hdr[kEntityPeek];
if (!SafePeek(obj, sizeof(hdr), hdr)) return Container::Unknown;
memcpy(&k, hdr, sizeof(k));
```

### F5 — Dead and unreachable code.

- `synth::Init()` is defined and declared but **never called** from anywhere.
  (Moot while F1 stands, but it means the resolve never even gets attempted
  from a sane place.)
- `il2cpp::FindClassByMethod()` is defined and declared, **no callers**.
- `target::Query::cc_only` and `use_atk_range` — v40's headline "new filters" —
  are **never set by any caller**. `grep` across `src/` finds only the struct
  defaults and the internal `consider()` use. The menu has no control for them.
  So the two v40 targeting filters are unreachable.
- `target::Query::require_visible` — same, never set.
- `Config::skin_applied` is written by the menu and persisted nowhere useful
  (it is not in any key table), so it resets every launch. Cosmetic.

### F6 — Documentation drift.

- `rain.cpp` header comment: *"JNI_OnLoad is never called — the entry point is a
  static initializer."* The log shows both firing. The `pthread_once` gate makes
  it harmless, but the comment is wrong and it is the kind of wrong that leads
  someone to put a fix in the constructor instead of `JNI_OnLoad`.
- `RAIN.md` still carries the round-2 integrity claim ("no signature check on
  those files") without round 6's correction: `librudp.bytes` **is** in the
  server-delivered `BinaryPatchMD5.xml`; the slot survives only because the
  check is download/repair-time, not load-time. The slot is server-controlled.
- `RAIN.md`'s deploy and restore recipes use `su -c`, which does not exist on
  this device any more.
- `menu.cpp` Settings text promises auto-save/auto-load that F1 makes
  impossible.
- The `persist` comment about "Load marks itself done" (above).
- The v40 doc claims the KDA/gold/bounty data path was "Verified live at bind"
  — the log line it cites (`game: ShowPlayer fields kills=1 gold=1 bounty=1`)
  does not appear in any log in `analysis/`. Bind logging is `plog`-gated and
  may have been observed in a session that was not captured, but it is not in
  the record.

---

## 4. What is actually verified

Verified working, source-and-log consistent:

- Library load, both entry points, the `pthread_once` gate.
- IL2CPP resolution by hand (no `dlsym`, no section headers) — dynamic table
  found with 6,998 symbols on device.
- Class/field binding across all 70 images, including the late HybridCLR load.
- `eglSwapBuffers` hook + ImGui GLES3 overlay (`surface 2772x1280` on the phone,
  `1440x900` on the emulator).
- Touch *input* polling (`input: touchCount=…` lines).
- ESP draw path with the category matrix; the `esp:` heartbeat lines exist in
  both the old (`self=`) and new (`n=… cats=… local=… w2s=…`) formats.
- Minimap overlay markers and the 145 hero icons (round 6).
- Drone hook installation (`drone: set_offSetPos hooked at …`, `hook=1`) —
  installation verified; the visual lift was never confirmed in a match.
- Maphack flag writes (minimap/health only; the crash-prone flags are correctly
  excluded).
- Objective HP tracking and the alert banners' data path.
- The safety model: `SafePeek`/`Fill`/`PtrOk` cover all entity reads; the
  v41 `const void*` signature change is the right pattern.

Not verified, and in most cases blocked by F1:

- Everything in section F1's consequence #2.
- The drone's visual lift, range circle, and the retribution model — the notes
  list these as "needs a match"; they are actually unreachable.
- The `ClassifyContainer` container lifetime under scene churn (F4).

---

## 5. Priority order

1. **Fix the JNI class resolution and the `putString` abort together** (F1+F2).
   Do the `FindClass` in `JNI_OnLoad`, cache global refs, resolve
   `putString`, and add it to the `Resolve()` validation. Without both, any
   fix to one becomes a new crash.
2. **Close the `ClassifyContainer` raw deref** (F4) — it is the last hole of the
   v41 class and it sits on the per-frame path.
3. **Verify the automation in a real match** once (1) lands. Nothing in the
   combat feature set has ever run; expect first-run bugs in exactly the places
   the notes flagged as "needs a match" — and now also in `synth`'s coordinate
   conversion, which has never executed a single dispatch.
4. **Housekeeping:** `git init` + `.gitignore` (the build and analysis dirs are
   3.4 GB of untracked churn), prune `build/` to the last few versions, fix the
   stale comments (F6).
5. **Crash A/B** (open since round 7): N matches with Rain off vs on, same
   content, to settle the t08/t09 Vector3 hypothesis.

---

## 6. Note on the device

The phone currently runs **VYNIX**, not Rain. If the intent was to have v41
staged, the state was changed at 2026-10-08 22:26 and the restore file for the
*stock* library is still in `/data/local/tmp/orig/`. There is no root on this
device any more, so the `RAIN.md` shell recipes will not work — the staging
path now has to go through the game's own external files dir (which the shell
can write, per round 7) or a re-rooted device.

The stock `librudp.bytes` is also preserved in-tree at
`analysis/device_2026-09-28/rain_deep_2026-09-28/original_libs/librudp.bytes`
(md5 `51bac6f79e3e13c5823e44b352686fb4`), so nothing is at risk.

---

*Written as a static + log + live-device pass. Where this document and an
earlier round disagree, this one measured; check the evidence links in each
finding before overriding it.*
