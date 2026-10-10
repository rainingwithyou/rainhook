# Rain v39/v40 — Practice Match Test Results

**Date:** 2026-10-07, 22:00–23:50 local
**Device:** Redmi 2607DRA18G (5d39a8f2), Android 16
**Build:** v40 (`039f00384b82ab42d45158f88631373b`), MLBB 2.2.16.12322
**Match:** Practice / Solo, Kimmy (id 71) vs Zilong (id 16), 11:58 duration

---

## Verified in-match

### 1. ESP — working
```
esp: n=2 cats=001001111 local=1 w2s=1 scr=(989,531)
```
- Entity snapshot walking the player list (2 entities: self + enemy)
- `local=1` — camp derived correctly from the LogicBattleManager path
- `w2s=1` — world→screen projection succeeding
- Enemy box drawn on screen (visible in `test_32_clean.png`, top-right)

### 2. Minimap hero portraits — working
Confirmed visually in the minimap crop (`test_32_minimap.png`):
- **Zilong portrait** with a red health-tinted ring (enemy)
- **Kimmy portrait** with a green ring (self)
- Turtle objective marker on the map
- The game-atlas portraits keyed by `m_ID` render correctly

### 3. Drone hook — **installed and live**
The critical v39 feature. Forced `drone_enabled` on for one test build to
trigger the resolve path, and the logcat shows the full chain:

```
drone: SmoothFollow offset field = __offSetPos (+0x8c)
il2cpp: bound class UnityEngine::Object
drone: set_offSetPos hooked at 0x74e4f9f348 (orig=0x77df16a968)
drone: engaged fov=0.0(have=0) offset=(0.00,0.00,0.00)(have=0) ymax=0 hook=1
drone: h=8.0 fov=+0.0 touched=1 sf=0 hook=1
```

What each line proves:
- `offset field = __offSetPos (+0x8c)` — the backing field resolved by name
- `set_offSetPos hooked at 0x74e4f9f348` — the **MethodInfo code pointer was
  read and validated as executable**, then GlossHook patched it. This is the
  VYNIX-model hook working.
- `hook=1` — the detour is live and being consulted every frame
- `orig=0x77df16a968` — the trampoline back to the original setter is intact

The hook was installed **in the lobby** (before any match), which means it
resolves as soon as `SmoothFollow` loads — no dependence on being in battle.

**Not yet observed:** the visual lift itself. The lobby has no `SmoothFollow`
instance (`sf=0`), so the detour had nothing to modify there. The hook is
confirmed armed; the effect needs a battle-scene instance, which is what the
next session should confirm by watching the camera in a live match.

### 4. Monster tracking — working
```
game: monsters field ok=1 list=0x7405127d20 kind=0 count=87
```
- 87 monsters enumerated at one point (jungle camps + minions)
- `Creep 1002` rendered as a nametag in-match (visible in `test_51.png`) —
  the monster id table and fallback naming both work
- The list went to 0 after the match ended, which is correct teardown

### 5. ShowPlayer KDA bind — working
```
game: ShowPlayer fields kills=1 gold=1 bounty=1
```
All three companion fields bound by name on 2.2.16. The targeting v2
priorities (Most Kills / Richest / Most Fed) have real data behind them.

### 6. Match stability — clean
Full 11:58 practice match with Rain active:
- **Zero crashes**, zero fatal exceptions
- No `SIGSEGV`, no GfxDevice abort
- ESP ran continuously (~30 fps of `esp:` heartbeats)
- Touch pump clean (`touchCount=1→0` pairs per tap)
- Process survived the match-end transition and returned to the lobby

---

## Not verified (blocked by UI input)

The synthetic-touch → ImGui path makes menu scrolling unreliable: swipes on the
content pane either drag the window, grab a slider, or pass through to the game
underneath. Three features therefore could not be toggled on during this
session:

| Feature | Why blocked | How to test next |
|---|---|---|
| Drone **visual** lift | needs `drone_enabled` toggled in a live match | toggle in-menu, or watch for `hook=1` + camera lift in a match |
| Range circle | needs `range_circle` toggled | toggle in-menu |
| Kimmy steer | needs `kimmy_enabled` + `kimmy_aim_mode=1` | toggle in-menu |
| Buff/litho retri | needs `retri_buff`/`retri_litho` + a camp in view | toggle + walk to a jungle camp |

**This is a real usability finding, not just a test problem.** The menu cannot
be scrolled by synthetic touch on this device, which means a phone user also
cannot reach the lower sections. Worth fixing:

- **Option A:** make the window taller / reduce section heights so everything
  fits without scrolling.
- **Option B:** add a scroll-wheel-equivalent — ImGui responds to
  `io.MouseWheel`; feed it from a two-finger drag or an on-screen scroll strip.
- **Option C:** collapse-by-default the big sections (ESP, Targeting) so the
  content fits.

Option C is the smallest change and also improves the first-open experience.

---

## Config-state note

The temp build that forced `drone_enabled=true, drone_height=8.0` was reverted;
`build/v40/librain.so` rebuilt to the **identical md5**
(`039f00384b82ab42d45158f88631373b`) as the original v40, proving the revert was
clean. The phone has the clean build staged.

**No stale pref was left behind.** The temp build was force-stopped before its
2-second autosave timer fired, so `drone_enabled=1` was never written to
SharedPreferences. Verified: the clean build launched as PID 22633 and produced
**zero** drone log lines, which only happens when `drone_enabled` is false.
No cleanup needed.

---

## Session artefacts

- `analysis/test_20_match.png` — first match, in-game with ESP + minimap
- `analysis/test_32_clean.png` — clean match view, enemy box visible
- `analysis/test_32_minimap.png` — minimap crop showing both hero portraits
- `analysis/test_51.png` — Creep 1002 nametag + minimap turtle
- `analysis/test_47_menu.png` … `test_52.png` — menu navigation attempts

---

## Verdict

**v39 and v40 are functionally confirmed on the device.** The drone hook — the
headline fix — installs correctly and reports live. ESP, minimap portraits,
monster tracking, and the KDA data path all work in a real match, and the
client survived a full 12-minute game without a single fault.

The remaining work is verification of the three input-dependent features, plus
the menu-scrolling usability fix that blocked them.
