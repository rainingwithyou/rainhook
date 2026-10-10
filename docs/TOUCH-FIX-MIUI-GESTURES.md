# Touch Intermittently Dead in MLBB — Diagnosis & Fix (2026-10-10)

**Symptom:** taps work sometimes, then stop working — in Mobile Legends only.
System-wide touch (launcher, drawer, other apps) always works.
**Cause:** MIUI's gesture navigation zones. **Not Rain.**
**Fix:** one setting. Applied and verified.

---

## Evidence

### 1. Touch works everywhere else

Swipes and taps on the launcher, the app drawer, and the notification shade all
worked throughout the investigation. The digitizer, the kernel input path, and
Rain's touch *reading* (`UnityEngine.Input` polling) were never involved.

### 2. The game-specific mechanism, in the log

With gesture navigation active, edge touches in MLBB were published to MIUI's
gesture handler and **never delivered to the game**:

```
I MIUIInput: [MotionEvent] publisher action=0x0, deviceId=8, channel '[Gesture Monitor] miui-gesture'
D MiuiFreeformModeGestureHandler: start handle MotionEvents:(30.0,1400.0)
D MulWinSwitchEventHandler: handleReceivedMotionEvent: mTouchedBottomTaskId=-1
```

and the gesture stubs were live in the window list while the game was
foreground:

```
MiuiSpecialWindowInfo: ...MobaGameUnityActivity(...);
  GestureStubLeft(6,0x104,0x0,[256,0,1024,62],1.00);
  GestureStubRight(5,0x104,0x0,[256,2710,1024,2772],1.00);
  GestureStubHome(7,0x104,0x0,[0,0,69,2772],1.00);
```

Those rectangles are the screen edges. In landscape (MLBB runs
`rotation 0` but the game surface is landscape) they cover the **bottom-left
joystick** and the **bottom-right skill cluster** — the two places a player
touches constantly.

### 3. The failure shape

- A stationary tap in the middle of the screen: delivered.
- A touch that starts on or near an edge and moves: classified as a back
  gesture, swallowed by `miui-gesture`, never reaches the game.

That is exactly "sometimes I can't touch": taps fine, drags dead, and only in
the game — because only in the game do the gesture zones overlap the controls
you actually use.

There was also an earlier one-off: `InputDispatcher: Pruning input queue
because user touched a different application while waiting for
ActivityRecord{...MobaGameUnityActivity}` — the system dropped queued input
while the game's window was slow to accept. That is a symptom of the same
window not being in the foreground-focus path it expects; it did not recur
after the fix.

---

## Fix applied

```bash
# Game Booster master switch — MIUI's per-game mode that suppresses the
# gesture zones while a game is foreground.
adb shell settings put secure pref_open_game_booster 1
```

**Verified after relaunch, with the user's gesture navigation restored:**

| Check | Before | After |
|---|---|---|
| `GestureStubLeft/Right/Home` for MLBB | active (edge strips) | `<none>` |
| Edge tap (30,640) | eaten by `miui-gesture` | delivered to MLBB |
| Edge tap (2740,640) | eaten | delivered |
| Edge drag → 42 move events | gesture cancelled | `moveCount:42`, one gesture |
| `[Gesture Monitor] miui-gesture` events during game input | every touch | 0 |

Final device state: `navigation_mode=2` (gestures — the user's original),
`pref_open_game_booster=1`. Game running, untouched otherwise.

---

## Why this is worth keeping in the record

1. **It is not a Rain bug.** Rain reads touches through
   `UnityEngine.Input.get_touchCount/GetTouch` and never consumes them; the
   overlay is not a touch target. The system-level logs show the events being
   dropped before the game's window ever received them.
2. **Any mod or overlay on a MIUI device hits this**, because the fix is a
   device setting, not code. VYNIX/XNISH users on MIUI would see the same
   symptom.
3. **The diagnostic pattern is reusable:** when touch fails in exactly one
   app, grep for `MiuiSpecialWindowInfo` + `GestureStub` and
   `Gesture Monitor] miui-gesture` with the app foreground. If the stubs cover
   the app's window, it is this.
4. **For the RAIN.md troubleshooting section:** if a user reports dead touches
   in-game on a Xiaomi device, the first question is whether Game Booster is
   enabled for MLBB, and the first command is the `pref_open_game_booster 1`
   put above.

## What was NOT changed

- Rain's source, build, or the staged `librudp.bytes` (v42 remains staged,
  md5 `56c36ad7c51c67158b439676543f5791`).
- The user's navigation mode (restored to gestures after the A/B).
- The game install, account, or any game data.
