# Dead-entity ESP fix (2026-10-13)

**Symptom (reported with screenshots):** a dead enemy hero's ESP stayed on
screen at a frozen position — the box/portrait/name hung in a lane the hero
had already left, while the game's own minimap correctly showed the death
elsewhere. The user's words: *"ESP off from the entity... the minimap shows
saber already died in down lane, but the ESP shows it starting from mid lane
to top lane."*

## Root cause

Two compounding problems:

1. **The position fields freeze on death.** When an entity dies the engine
   stops updating `m_vUnityCachePosition` / `m_vCachePosition`. The corpse's
   ESP was drawn at the last *live* position, which is where the hero was a
   moment before dying — not where the corpse lies. Projected through the
   camera it lands in the wrong place entirely, which reads as "the ESP
   detached from the hero".

2. **Death detection was incomplete.** Rain only read `m_bDeadAnima` (the
   death *animation* flag). That flag:
   - lags the actual death (fires first, engine verdicts follow ~40 ms later),
   - can miss corpses whose death animation was skipped,
   - and is the only signal the ESP consulted.

The engine carries two more authoritative verdicts that Rain never read:

| Field | Meaning |
|---|---|
| `m_bUnityBloodDeath` | the engine considers the blood bar dead |
| `m_bUnityMinimapDeath` | the engine considers the minimap icon dead |

## Fix

**`game.cpp` / `game.h`:**

- Bind `m_bUnityBloodDeath` and `m_bUnityMinimapDeath`.
- `is_dead` is now the **union** of all three signals:
  `m_bDeadAnima || m_bUnityBloodDeath || m_bUnityMinimapDeath`.
- The maphack dead-skip uses the same union (previously anima-only).

**`esp.cpp`:**

- `CAT_ENEMY_HERO.dead_mode` default changed **2 → 0** (hide).
- `CAT_ALLY_HERO.dead_mode` default changed **1 → 0** (hide).

A corpse now disappears from ESP the moment it dies, instead of lingering at
a stale position. Users who *want* the corpse marker can set the row's
`dead_mode` back to 1 (own colour) or 2 (shared dead colour) in the menu.

## Live verification

Diagnostic build logged every first-death with all three flags and all three
position fields. Killed minions in a practice match:

```
16:44:33.209  dead guid=4278194261 anima=1 blood=0 minimap=0   <- anima first
16:44:33.249  dead guid=4278194261 anima=1 blood=1 minimap=1   <- engine flags
16:44:33.249  dead pos u=(-33.9,0.0,-0.8) c=(-33.9,0.0,-0.8) o=(-36.4,0.0,0.0)
```

Confirms: the engine flags fire and latch within ~40 ms of the anima flag,
and the position fields are already stale at that moment. With `dead_mode=0`
the draw path returns before projection, so nothing is drawn at the stale
spot.

Shipped as v45.1. The first-death log line is kept in release builds — it is
one line per entity per match and is the evidence trail if the bug ever
returns on a future game build.

## What was NOT changed

The position-selection logic itself (unity cache → logic cache → obj pos) is
untouched. It is correct for live entities; the problem was only that a
corpse kept being drawn from it. Hiding the corpse is the correct fix — the
alternative (tracking a separate death position) would need a field the
engine does not expose on `ShowEntity` (the death-position data lives in
`m_ReliveData` / report structures, not as a world vector).
