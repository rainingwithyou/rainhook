# Second-Match Team Inversion — Root Cause & Fix (2026-10-11)

**Symptom:** on the second match, "enemy" ESP drew **your own team** and the
minimap marked them as enemies. First match was always correct.

**Cause:** the local camp was latched once and only invalidated on a
`BattleManager.Instance` *pointer* change — which the allocator can defeat by
reusing the address. **Fix:** v43.2 takes the camp from the live self entity,
which the snapshot already parses every frame. **Verified live.**

---

## Why it only happened on the second match

`SnapshotPlayers` guarded match state like this:

```cpp
if (g_mgr_seen != 0 && g_mgr_seen != mgr) ResetMatchState();
g_mgr_seen = mgr;
...
if (!g_local_camp_strong) DeriveLocalCamp(...);   // once latched, never again
```

Two things had to line up for the bug:

1. **The manager pointer had to be reused.** IL2CPP frees the previous match's
   `BattleManager` and the allocator can hand the next one the same address.
   When that happens `g_mgr_seen == mgr`, so `ResetMatchState()` never runs.
2. **The player had to be on the other team.** Camp 1 vs camp 2. Practice mode
   tends to put you on the same side every time, which is why it can take a
   real match to see it.

With both, the camp from match 1 survives into match 2 and everything
downstream inverts:

```cpp
// esp.cpp — a wrong `local` makes every teammate read as an enemy
const bool enemy = (e.camp != local);
```

```cpp
// game.cpp ApplyMaphack — the same comparison decides who gets revealed
if (g_local_camp <= 0 || camp == g_local_camp) continue;
```

One stale integer, two symptoms: ESP on the wrong team and maphack writing
onto your own side.

## The fix

The reliable value was already being read every frame and thrown away.
`Snapshot()` resolves the local entity through `m_LocalPlayerShow` and parses
it with `Fill` — which validates the camp range — so its `camp` field is the
freshest truth available:

```cpp
g_snap.has_self = SelfEntity(&g_snap.self) && g_snap.self.obj != nullptr;

if (g_snap.has_self && g_snap.self.camp > 0 && g_snap.self.camp < 17) {
    g_snap.local_camp = g_snap.self.camp;      // live: cannot be stale
    if (g_local_camp != g_snap.self.camp) {    // keep the latch in step
        g_local_camp = g_snap.self.camp;
        g_local_camp_strong = true;
    }
} else {
    g_snap.local_camp = LocalCamp();           // lobby / first frame
}
```

The derived/latched camp remains as the fallback for frames before the local
entity exists. There is no boundary to detect and nothing to reset: the value
simply comes from the object it describes, every frame.

### Defence in depth

Two additions, in case a future build hides the local entity:

* **guid as a second boundary signal.** The local player's entity guid is
  tracked alongside the manager pointer. A changed guid resets match state
  even when the pointer is reused.
* **Debounced camp change.** A camp read from the manager's local-player
  reference that differs from the latch must persist for several consecutive
  frames before it is accepted, and the counter resets the moment the read
  agrees again. A teardown artifact does not persist; a real team change does.
  Without this, the fix could latch the same bug in the other direction.

## Verification

Practice matches were run back to back in one process. Both reported the
correct camp and drew the correct minimap markers (self green, enemy red)
with no inversion:

```
game: local camp = 1 (from m_LocalPlayerShow)
esp: n=2 cats=010001110 local=1 w2s=1
```

**Honest limit:** practice mode placed the player on the same side both times,
so the *team-swap* case that triggers the original bug could not be forced
from a practice match. What is verified is that the camp now comes from the
live entity every frame (the failure mode is structurally impossible), and
that normal matches are unaffected. The debounce and guid tracking are
untested against a real swap and are there as belt-and-braces.

## Launcher button scale

Unrelated but shipped in the same build: **Settings → Launcher button size**
(0.5x – 2.0x). The window, corner rounding and inner button scale together, so
the circle stays a circle at any size. Persisted as `menu.launcher_scale`;
`Reset everything` preserves it along with the UI scale.

Verified on device: at 1.5x the button renders visibly larger and still
centred and circular.

## Shipped

**v43.2** — both ABIs, md5 `4e28c0f0…` (arm64) / `9ec31400…` (v7a).

| Path | Cost |
|---|---|
| Live self-entity camp (the fix) | zero — the entity is already parsed for ESP |
| guid tracking | one 4-byte read of the already-peeked local entity |
| debounced re-validation | one peek per frame, only when no local entity exists |
