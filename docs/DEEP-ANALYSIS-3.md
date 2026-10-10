# Rain Project — Deep Analysis (Round 3) + v29–v32 Feature Campaign

Date: 2026-09-30
Session: live emulator (`emulator-5556`), game `2.1.67.11733`, Rain staged as `librudp`
Companion to `ANALYSIS-REPORT.md` (round 1) and `docs/DEEP-ANALYSIS-2.md` (round 2).

This round covers: full VYNIX/XNISH lineage decode, the Java/dex layer, the practice-mode
test campaign, and the v29–v32 feature work (title bar, bug-fix sweep, projection
calibration). Artefacts under `analysis/round3/`.

---

## 1. New project findings

### 1.1 VYNIX lineage: same hook framework as Rain
VYNIX 64-bit contains the string `Hook failed, Linker proxy function
(GlossLinkerFuncProxy) is invalid.` — the exact identifier present in Rain's
`third_party/GlossHook/GlossHook/include/Gloss.h`. VYNIX and Rain are built on the
same hooking framework (GlossHook).

### 1.2 VYNIX deployment slot
VYNIX ships as `librudp.bytes` in both architecture folders of the trial zip — the
*same slot* Rain displaces. A device can host exactly one of them. VYNIX also
supports the Vietnam publisher (`/data/data/com.hhgame.mlbbvn/...`) and reads MLBB
battle-record files (`BattlePlayerInfoCache`, `BattleRecord`, `FightHistory`,
`OfflineReport`) — deeper account/meta integration than Rain has.

### 1.3 XNISH feature surface (beyond round 2)
- **Kimmy subsystem**: `Kimmy Quick-Clear Mode`, `##KimmyMinionRange`,
  `Status: Kimmy Quick-Clear` — dedicated basic-attack minion farming.
- **Auto-retri damage model**: "Damage calculation includes level, jungle stacks,
  kills, and assists" — retribution timing is model-driven, not just HP-threshold.
- Resizable main menu (`##menu_resize_handle`, `##MainMenuBar`), ImGui debug-log build.

### 1.4 `hb_DynlibFiles.bytes`
`refs/game/hb_DynlibFiles.bytes` decodes to `{"name":"dynegcmode",
"assemblyList":["MobaGame"]}` — the game's hot-loaded assembly is **MobaGame**.
Rain currently walks all 70 images per class lookup; `il2cpp_domain_assembly_open
("MobaGame")` would target the game image directly (optimisation, not a bug).

### 1.5 Java / dex layer (new)
- 27,398 classes across three dex files (8,792 + 9,032 + 9,574); dumps in
  `analysis/round3/dexdump_classes*.txt` (`dexdump -d`; plain dump hits a
  `ReadFileToString` quirk on classes.dex).
- Launcher activity: `com.moba.unityplugin.MobaGameMainActivityWithExtractor`
  (activity-alias) → `MobaGameUnityActivity` in `:UnityKillsMe`.
- Loader chain: `DynamicSOLoader`, `AddtionDexLoader`, `CopySOResult` — the Java side
  of the hot-update native-module path Rain rides.
- Both Play Asset Pack splits are **empty stubs** (manifest + SourceStamp only); all
  content arrives via APK splits + the hot-update channel.

### 1.6 IL2CPP metadata verification (Rain bindings, all confirmed)
Every Rain-bound field verified present: ShowEntity (`m_RoleName` = entity name field,
binds first), BattleManager (7 collections + `Instance`), ShowBattleCamera (31 fields:
`m_screenPtCastDis`, `m_cameraDepth`, `m_iYMax`, `m_CamPos`, ...),
LogicBattleManager (`m_SelfCampType`, `m_RealSelfPlayer`, per-camp lists).
Live offsets logged at bind: `hp=420 hpMax=424 camp=208 guid=392 id=396 type=120
pos=652 radius=824 minimap=679` — all sane, non-zero.

---

## 2. Rain v29–v32 campaign (live-verified)

### 2.1 v29 — classic title bar
Native ImGui title bar restored: `▼ Rain` collapse arrow (minimize), `✕` close
(p_open), title bar as drag handle. Drag strip removed. **All verified live**:
close → launcher, minimize → title-bar-only, restore, drag. Matches the reference
clickgui look.

### 2.2 v30/v31/v32 — the six-issue bug sweep (LO's practice-mode report)

| Issue | Root cause | Fix | Status |
|---|---|---|---|
| Self ESP draws with toggle off | `LogicBattleManager.m_SelfCampType` read garbage (`30264`/`30263` — pointer fragments) and, being `> 0`, latched `g_local_camp_strong`, disabling the reliable fallback | Logic camp validated against live entity camps; garbage rejected → falls back to `m_LocalPlayerShow` (log: `logic camp 30263 rejected` → `local camp = 1`) | **verified live** |
| Player boxed red (enemy colour) | same garbage camp → every entity "enemy" | camp fix + belt-and-braces: local player's guid skipped in the enemy pass | **verified live** |
| Drone view no effect | wrote `m_CamPos` (the controller's *output*) / camera params, all overwritten or unused | **SOLVED (v34/v35)**: the controller recomputes the camera only when it acts (follow ticks, drag, events), not unconditionally — so lifting the real Unity camera (`Camera.get_transform` → `get_position`/`set_position`, resolved via IL2CPP) from our render hook *sticks*, with a read-back guard that re-lifts after any fresh game write and can never accumulate. `m_iYMax` raised while on so nothing clamps. Verified live: idle lift read back at y=11.0→30.3, held through walking (x/z followed the hero, y stable), bird's-eye screenshots captured | **verified live (screenshots)** |
| Tracer not working | camp garbage (wrong colours) + projection offset made endpoints meaningless | camp fix + projection fix; tracer line renders from hero (Player mode) to the enemy, coloured red | **verified live (screenshot)** |
| Box offset from sprite | (a) positions are logic-space but hero `m_vUnityCachePosition` reads zero, and the 45°-rotation hypothesis was wrong — **raw Unity projection is correct** (self projects to within ~13 px of the sprite); (b) fixed 1.9-unit box height is ~40% too tall for heroes and far too small for turtle/lord | projection settled as raw; hero height 1.35; per-category monster heights (turtle 3.6, lord 4.2, buff 2.4, creep 1.6, minion 1.2) | projection **verified** (13 px); heights eyeballed-verified next session |
| Maphack markers noisy/self-marked | camp garbage + raw id labels + no hero filter | markers: enemy heroes only, no labels, self skipped (debug-self kept for calibration) | code done; visual confirm blocked by process restarts resetting the toggle |

Bonus fixes landed in the same sweep:
- **ImGui ID conflicts fixed** (three `Enable` checkboxes, two `Max distance`, two
  `Up`/`Down` pairs shared IDs — the "3 visible items with conflicting ID" error box)
  → unique `##suffix` ids; error gone live.
- **Menu window position clamped** to screen (a body drag could strand the title bar
  off-screen — observed live). Note: ImGui windows also drag from any empty body
  space by default, which is how it happened.
- Objective alerts verified live ("Lord Under Attack" banner).
- Hero id calibration from practice: `m_ID` 71 = Kimmy, 16 = Zilong (opponent select
  confirmed the picks) — added to the table.

### 2.3 Deployment chain (re-verified twice)
`build/vNN/librain.so` → `comlibs/arm64-v8a/librudp.bytes` (staging) →
`app_libs/librudp.so` on launch, md5-stable through the game's own copy/verify
pipeline (patch-list blind spot re-confirmed). v29–v32 all deployed this way.

---

## 3. Open items (next session)

1. **Box-height eyeball**: one jungle screenshot with turtle/lord/buff on screen to
   tune the category heights (hero 1.35 already matched at ~13 px; buff boxes landed
   on the monsters in the drone screenshots).
2. **Marker visual re-check**: enable maphack in a match, zoom the minimap, confirm
   clean red enemy-hero dots (code is done; earlier attempts were invalidated by
   process restarts resetting the toggle).
3. Managed-object lifetime guard (round-1 finding, still open).
4. `git init` + `.gitignore` + credential rotation (round-1 findings, still open).

### Drone implementation notes (for future maintenance)

- The camera census (`docs` method-slot scan, 2026-09-30) found the relevant method
  families: `MainCamera_SetCameraPos`, `SetCameraPosLerp`, `SetCameraPos`,
  `SetCameraPosMoving_Entity`, `FollowTargetCanUpdateCamera`, `OnCameraUpdatePosition`
  (class name resolution in the metadata tool is unreliable for late type records;
  raw method-slot scans are reliable).
- Hooking those methods was measured as unnecessary (fire counters stayed at zero
  through idle and moving follow) — the render-hook lift alone covers this build.
  If a future build writes the camera unconditionally every frame, re-hook
  `SetCameraPosLerp` (the follow setter) post-way and lift there instead.

---

## 4. Emulator notes (for future sessions)

- v29–v32 flow: force-stop → push to `/data/local/tmp` + comlibs → `su -c cp` into
  `app_libs` (chown u0_a60:u0_a60, chmod 600) → launch → wait ~75 s.
- Git Bash: `MSYS_NO_PATHCONV=1` for adb push destinations.
- Build: `cmake -G Ninja -DCMAKE_MAKE_PROGRAM=<sdk>/cmake/3.22.1/bin/ninja.exe`
  (ninja ships with cmake, not the NDK) + NDK 26.1.10909125 toolchain, arm64-v8a,
  android-26, Release (POST_BUILD strip → ~849 KB).
- The lobby re-plays event popups on every launch (Lunox/TopUp/Asian Games/Manuver);
  stray taps open portrait webviews. Close popups one at a time and verify with a
  screenshot/pixel check before proceeding. Practice matchmaking path: Mode icon
  (1012,782) → VS A.I. & Custom (1280,745) → PRACTICE (490,430) → Solo (450,478) →
  hero → OK → Pilih lawanmu → Mulai (1286,816, needs a clean separate tap).
- Practice matches end on their own (observed 3–12 min); config is per-process and
  resets on any game restart (hot updates restart the process).
