# ESP Offset Fix — Projection Fallback (2026-10-17)

**Symptom (user report):** ESP boxes sit off the enemy "always when the enemy is
lower than my camera" — upper lane looking at mid/bottom lane, mid looking at
bottom. Occasionally a giant offset rectangle with the correct name/distance
attached to it.

**Root cause:** not the entity position. The world→screen path had two
projectors, and the second one was both broken and reachable on the hot path.

---

## Mechanism

`game::WorldToScreenEx` (game.cpp) projected every point as:

```
UnityProject(point)  ||  GameProject(point)
```

* `UnityProject` — the renderer's real matrices (`Camera.get_main` →
  `worldToCameraMatrix` / `projectionMatrix`). Correct while the camera is
  reachable and the point is in front of the near plane.
* `GameProject` — a self-built VP from the logic camera's fields
  (`ShowBattleCamera.m_CamPos/m_CamRotVect/m_iFieldOfView`), intended as a
  fallback "when the managed Camera is unreachable".

Two things were wrong:

1. **The fallback triggered on rejection, not only on unreachability.** When an
   entity crossed below the bottom of the view (exactly the "enemy lower than
   my camera" case: feet near/behind the near plane) `ProjectPoint` returned
   false, and the code moved on to `GameProject` — a *different matrix* — which
   accepted the point.
2. **On 2.2.16 the logic camera's fields do not read as a usable pose at all.**
   Measured live, every frame: `game=(2.69e38, 0.0, 0.0) rot=(0.0,0.0,0.0)
   fov=0` (position reads as pointer garbage, rotation and fov as zero). The
   matrix built from those fields is finite but meaningless, and it maps fed
   points to roughly screen centre.

Net effect: every entity that UnityProject rejected got a box **parked at a
fixed wrong screen position**. Observed live in practice camp: the computer
bot 74 m away (off-camera) had its box, health bar and "Computer" name drawn
beside the *local hero*. When the feet point took the fallback and the head
point happened to pass UnityProject (1.35 m higher, past the near plane), the
two projections came from different matrices and the box stretched into the
giant lane-to-lane rectangle from the report screenshot.

Also latent: a point just inside the near plane divides by ~0, exploding
`box_h` and drawing a screen-filling rectangle.

## Evidence

Diag build logging per hero box (`espdiag`): position source, projector id,
raw feet/head projections, both camera states. Practice camp, live bot
("Computer", guid 33):

```
before fix  13× proj=1, 18× proj=2   game=(2.69e38, 0, 0) rot=(0,0,0) fov=0
            proj=2 samples: pos frozen, feet=(1059,454) CONSTANT while the
            camera moved 50-74 m — a projection that ignores the camera
after fix   13/13 proj=1             game=(97954.8, 0, 0) ... (still garbage,
            refused by validation)
            75 m off-camera case: real projection (off-screen right), no box
```

Screenshots: `analysis/pull_2026-10-17/re/user_test_now.png` (bug: bot box on
the local hero) vs `re/verify1.png` (fixed: nothing stray).

## Fix (v45.3)

* `WorldToScreenEx`: if the Unity matrices exist, a rejected point is a **cull**
  — no second projector. `GameProject` runs only when there is no Unity matrix
  at all. One box can no longer mix matrices.
* `BuildVpFromGameCamera`: validates position/rotation/fov (finite, in range,
  rotation not all-zero) before building; garbage is refused.
* `esp.cpp`: `box_h > 2 * screen_h` → skip (near-plane explosion guard).
* Calibration logging stays in the tree behind `RAIN_ESP_DIAG` (0 in releases).

## Verified

Live, on device, 2.2.16.12322, practice camp: bot visible → box/bar/name sit on
the bot at the screen edges; bot 75 m behind the camera → no box at all (old
build: parked at centre). No crash lines. Builds: arm64 `abd4c2bc…`, armv7
`fd07edbf…`; deployed to comlibs as `librudp.bytes` = `abd4c2bc…`.

## Side notes from the session

* Rollback copies on `/sdcard/Download/`: `librudp.v452.backup.bytes` (clean
  v45.2), `rain_v453.so` (this build), `rain_fixdiag_final.so` (fixed build
  with diagnostics on).
* MIUI: `pref_open_game_booster` had reverted in effect (gesture stubs active
  again over the game's bottom edge — the TOUCH-FIX symptom returned).
  Re-applied; navigation mode was temporarily switched to 3-button for
  testing and restored to gestures (2) afterwards.
