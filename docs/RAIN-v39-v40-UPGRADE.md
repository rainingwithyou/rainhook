# Rain v39 / v40 — VYNIX & XNISH Parity Upgrade

**Date:** 2026-10-07
**Phone:** Redmi 2607DRA18G (5d39a8f2), MLBB 2.2.16.12322
**Both builds deployed and verified live.**

---

## v39 — Visual (982,920 B, md5 `8a0e95bb…`)

### ESP per-category dead handling
Replaced the single global `esp_show_dead` flag with a per-row `dead_mode`:

| mode | behaviour |
|---|---|
| 0 | hide the entity when dead |
| 1 | keep drawing in its own category colour |
| 2 | draw in the shared dead colour |

Defaults: enemy heroes use mode 2 (a dead hero is information), minions use
mode 0 (noise), everything else 1. The shared dead colour stays editable in the
ESP master section. `config.h` `CatCfg.dead_mode`, applied in `esp.cpp` `DrawUnit`.

### ESP per-category distance cap
`CatCfg.max_dist` (0 = use the global cap). Applied in `DrawPlayers` /
`DrawEntities` **before** `DrawUnit`, so a filtered entity can never leave a
tracer line pointing at it — the round-1 finding about tracers surviving the
distance filter is now structurally impossible.

### Drone rebuild — hook `set_offSetPos` (the VYNIX model)
The old implementation wrote the backing field, which the camera controller
overwrites on every follow tick — that race is the flicker. VYNIX instead
intercepts the setter.

New: a GlossHook on `SmoothFollow.set_offSetPos`:
- `MethodCodePtr()` reads the IL2CPP `MethodInfo`'s code pointer, trying offset
  0 then 8, and validates it against `/proc/self/maps` as an **executable**
  mapping before hooking (a wrong offset would otherwise patch a data address).
- The detour adds VYNIX's per-axis delta (`±0.234·h`, `−0.307·h`, `±0.235·h`)
  to the value the game is already writing, then forwards. The controller reads
  the modified offset and carries it itself — nothing races.
- Fallback chain preserved: **hook → read-back-guarded field write → transform
  lift**, selected per frame by whether the hook installed.
- `g_drone_hook_height` is `volatile` and cleared on release/disable, so the
  detour can never lift while the drone is off.
- Menu shows the live hook state (`live` / `off` / `waiting for SmoothFollow`).

### Minimap portraits + polish
`map_icons` now defaults **on** (the game's own hero atlas supplies all 0–134),
`mm_icon_size` 22 → 26, plus a drop shadow under the portrait plate so markers
read over bright terrain tiles.

### Range / FOV circle
New draw-only ground ring around the local hero at the active targeting range
(VYNIX's "Range FOV" readout). Source can be **auto** (first armed feature:
aim → skill → Kimmy) or a **fixed** slider. 48 world-space segments projected
per frame; segments behind the camera are skipped. Touches no engine state.

---

## v40 — Combat (1,004,424 B, md5 `039f0038…`)

### Targeting v2
`target::Class` went from 3 buckets to **6** — `Minion, Hero, Turtle, Lord,
Buff, Creep` — so the priority list can express "Lord first, then heroes, then
creeps" instead of lumping every jungle unit together. Default order:
Hero → Lord → Turtle → Buff → Creep → Minion.

New priorities: **Most Kills**, **Richest**, **Most Fed (bounty)**, **Closest
to crosshair** (screen-space projection, targets behind the camera sort last
rather than being dropped).

New filters in `Query`: `cc_only` (only `m_bIsFrozen` targets) and
`use_atk_range` (use the entity's own `m_AtkDis` instead of a flat radius).

**Migration:** the class list changed shape, so the persist key moved to
`target_order_v2_*`. Old three-entry keys are ignored and the six-entry default
applies — no stale data mis-ranks the list.

### KDA / gold / bounty data path
These live on **ShowPlayer**, not ShowEntity. Instead of building a guid map
(a second walk plus per-entry lifetime guards), `IsShowPlayerObject()` compares
the object's own `Il2CppClass*` against ShowPlayer's class, walking the parent
chain so per-mode derived types still match. One pointer read per entity, done
on the already-peeked copy.

New `Entity` fields: `kills, deaths, assists, gold, total_gold, bounty`.
Verified live at bind: `game: ShowPlayer fields kills=1 gold=1 bounty=1`.

Targets with no ShowPlayer data keep zero and fall back to distance ordering —
the priority degrades rather than misfiring.

### Kimmy M1 Aura — steer mode
`kimmy_aim_mode`: **0 = hold only** (unchanged), **1 = hold + steer**. In steer
mode the held press is dragged toward the selected target's screen position;
only the direction comes from the target, the magnitude is the fixed
`kimmy_drag_len`, so the same steer works at any range. No target → the press
returns to the anchor. All release paths, finger masking and hysteresis are
untouched. This is XNISH's "Kimmy Auto Aim" layered on the hold.

### Skill auto aim — four fixes
1. **CD offset bug.** `m_isCoolDown` was read at `0x24`, which is
   `iNewCoolChangeStep`. Correct layout (six 4-byte fields after the 0x10
   header): `iSpellID 0x10 / uiCoolTime 0x14 / originalMaxCdTime 0x18 /
   uiStartTime 0x1C / m_isCoolDown 0x20 / iNewCoolChangeStep 0x24`.
2. **Per-slot cooldown mapping.** `SkillSlotSpellId()` walks the owner's
   OwnSkillData dictionary for `m_iUseIndex`/`m_iSpellID`, so S1/S2/S3 gate on
   their own spell instead of the all-or-nothing "every known spell is cooling"
   test. Falls back to the conservative test when the mapping does not resolve.
3. **Cadence bug.** `g_last_skill_frame` was stamped *before* the drag
   projection, so a failed projection burned the window. It now stamps only
   after a real cast.
4. **Smart-cast fallback.** A failed drag projection now taps instead of
   skipping.

### Auto-retribution — dead toggles fixed
`retri_buff` and `retri_litho` pointed at `ObjectivePos`/`TrackedObjectiveHp`,
which only knew `"turtle"` and `"lord"` — both checkboxes did nothing. Buffs and
Lithowanderer are multiple entities, so they get a new `CampTrack` model that
tracks the **lowest-HP live member** of each kind (the one worth stealing) and
clears `seen` when the camp disappears, so a stale HP can never trigger a cast.

### Combo through shared targeting
The combo trigger had its own hero-only scan that ignored the class priority.
It now runs `target::Select` with `combo_priority` and the shared class order,
so combos obey the same policy as every other feature.

---

## Verification

Both builds:

| Check | v39 | v40 |
|---|---|---|
| DT_NEEDED | 7 | 7 |
| BIND_NOW | yes | yes |
| defined exports | 2 (`JNI_OnLoad`/`JNI_OnUnload`) | 2 |
| stripped (`.debug`/`.symtab`) | yes | yes |
| boots, all classes bind | ✓ | ✓ |
| menu renders new controls | ✓ | ✓ |
| errors/warnings in logcat | none | none |

Live evidence:
- v39: menu screenshot showing the "Dead entities" row; icons loaded (144)
- v40: `game: ShowPlayer fields kills=1 gold=1 bounty=1`; Combat tab showing the
  6-row class priority list and the KDA priority hint

Deployed as `comlibs/arm64-v8a/librudp.bytes`, md5-matched to the build.

---

## Notes for the next session

- **Drone needs a match to verify.** The hook installs when `SmoothFollow`
  resolves (battle scene). Watch logcat for
  `drone: set_offSetPos hooked at …` and `drone: engaged … hook=1`, then confirm
  the lift holds through walking and camera drags.
- **Kimmy steer** also needs a match (hero id 71).
- **Buff/litho retribution** needs a jungle camp in view.
- The `SkillSlotSpellId` field-name probe logs `skillmap: component field = …`
  at first use; if it logs "no skill component field bound", the per-slot CD
  gate silently falls back to the conservative test (correct, just less exact).
- Everything still defaults **off** except the ESP categories that were already
  on. The behaviour reporters watch input cadence, so the automation features
  stay opt-in.
