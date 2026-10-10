# Deployment Tutorial

How to put Rain on your own Android device, start to finish.

Rain replaces one of MLBB's own hot-update native modules (`librudp.bytes`).
The game copies that file into its private `app_libs/` directory at launch and
loads it — so once the file is in place, Rain comes up with the game.

**No root required** for the standard method. **No permanent modification** —
the original file is restored with one command.

> **No computer?** See [`TUTORIAL-NO-PC.md`](TUTORIAL-NO-PC.md) — the same
> deployment done entirely from the phone with a file manager.

---

## 1. What you need

| | |
|---|---|
| **Device** | Android 8.0+ (arm64 recommended; see the ABI note below) |
| **Game** | Mobile Legends: Bang Bang, installed and launched at least once |
| **Computer** | Windows / macOS / Linux with `adb` installed |
| **USB cable** | Data-capable, not charge-only |

### Which file to download

From the [Releases page](https://github.com/rainingwithyou/rainhook/releases):

| File | Use it when |
|---|---|
| `librain-arm64-v8a.so` | **Almost everyone.** Any phone from ~2017 onward. |
| `librain-armeabi-v7a.so` | Only for older 32-bit devices. |

> **x86 / x86_64 do not exist.** MLBB ships arm64 only, and the hook backend
> is ARM-only. Emulator users run the arm64 game under translation, which
> loads the arm64 binary.

### Enable USB debugging

1. **Settings → About phone → tap "Build number" 7 times** (unlocks Developer
   options)
2. **Settings → Developer options → USB debugging → ON**
3. Plug in the cable, accept the "Allow USB debugging?" prompt on the phone

Verify from your computer:

```bash
adb devices
# 5d39a8f2   device      <- your serial, state must be "device"
```

If it says `unauthorized`, check the phone for the prompt. If it says
`offline`, unplug/replug and try `adb kill-server && adb start-server`.

---

## 2. Quick start

For the impatient — five commands. Run them one at a time.

```bash
# 0. Find your game's comlibs directory (path varies slightly by device)
GAMEDIR=/sdcard/Android/data/com.mobile.legends/files/dragon2017/assets/comlibs/arm64-v8a

# 1. Back up the original — you will want this to uninstall
adb shell "cp $GAMEDIR/librudp.bytes /sdcard/Download/librudp.original.bytes"

# 2. Push the Rain binary
adb push librain-arm64-v8a.so /sdcard/Download/rain.so

# 3. Stop the game (the file is read at launch)
adb shell "am force-stop com.mobile.legends"

# 4. Install it as librudp.bytes
adb shell "cp /sdcard/Download/rain.so $GAMEDIR/librudp.bytes"

# 5. Start the game
adb shell "monkey -p com.mobile.legends -c android.intent.category.LAUNCHER 1"
```

The rest of this tutorial explains each step and how to check it worked.

---

## 3. Detailed steps

### Step 1 — Confirm the game is installed

```bash
adb shell pm path com.mobile.legends
# package:/data/app/.../base.apk
```

Empty output means the game is not installed (or not visible to this user).

### Step 2 — Locate the comlibs directory

This is the folder the game loads its hot-update modules from. Check it
exists and contains `librudp.bytes`:

```bash
adb shell "ls -la /sdcard/Android/data/com.mobile.legends/files/dragon2017/assets/comlibs/arm64-v8a/"
```

You should see a list including `librudp.bytes` (about 1.2 MB). If the folder
is missing, launch the game once and let it download its resources.

> The path may differ by game version/region. If `dragon2017` is not there,
> search for it (scope the search to the game's files directory — a full scan
> of the external data dir is slow):
> ```bash
> adb shell "find /sdcard/Android/data/com.mobile.legends/files -name 'librudp.bytes' 2>/dev/null"
> ```

### Step 3 — Back up the original file

**Do this before anything else.** It is the only thing standing between you
and a one-command uninstall.

```bash
adb shell "cp $GAMEDIR/librudp.bytes /sdcard/Download/librudp.original.bytes"
adb shell "md5sum /sdcard/Download/librudp.original.bytes"
```

Write the md5 down, or keep the file safe. You will need it in Step 8.

### Step 4 — Push Rain

```bash
adb push librain-arm64-v8a.so /sdcard/Download/rain.so
adb shell "md5sum /sdcard/Download/rain.so"
```

Compare with the md5 on the release page. They must match.

### Step 5 — Stop the game

The game copies `comlibs/*.bytes` into its private `app_libs/` at launch. If
it is already running, it will not notice the new file until the next start —
and worse, a running instance can be holding the old one. Always stop first:

```bash
adb shell "am force-stop com.mobile.legends"
```

### Step 6 — Install Rain as librudp.bytes

```bash
adb shell "cp /sdcard/Download/rain.so $GAMEDIR/librudp.bytes"
adb shell "ls -la $GAMEDIR/librudp.bytes"
```

The file size should now be Rain's (~1 MB), not the original's (~1.2 MB).

### Step 7 — Launch the game

```bash
adb shell "monkey -p com.mobile.legends -c android.intent.category.LAUNCHER 1"
```

Or just tap the game icon.

### Step 8 — Verify it loaded

Watch Rain's log while the game starts:

```bash
adb logcat -s Rain:*
```

You should see, within the first minute:

```
Rain: constructor entry
Rain: JNI_OnLoad
il2cpp: dynamic table ok (symtab=... n=6998)
il2cpp: API resolved, domain=...
persist: boot load 'default': ok
notify: installed — notification button toggles the overlay
overlay: eglSwapBuffers hooked (1), orig=...
overlay: ImGui ready, surface 2772x1280
il2cpp: images now 70 (was 0)
```

If you see these, Rain is in. If you see nothing at all, go to
[Troubleshooting](#6-troubleshooting).

---

## 4. What you should see

Once the game reaches the lobby:

* A **`Rain` watermark** in the top-left corner
* A round **`Rain` button** on the left edge of the screen
* A **notification**: "Rain — Overlay: visible" with a **Hide UI** button
  (posted under the game's own name, since it runs in the game's process)

### Opening the menu

Tap the round **Rain** button. The menu opens as a sidebar:

| Tab | What's there |
|---|---|
| **Visual** | ESP master, per-category rows (9 classes), colours, geometry, alerts, drone, skin |
| **Combat** | Targeting policy, attack assist, skill aim, combo, auto-retribution, Kimmy |
| **Map** | Minimap markers, portraits, calibration, maphack flags |
| **Status** | Binding state, live entity table, retribution estimate |
| **Settings** | UI scale, config profiles, touch release |

> **Note:** taps on the menu also pass through to the game underneath (Rain
> reads touches, it does not swallow them). Position the launcher over a dead
> area of the screen.

### The notification toggle

Swipe down the notification shade and find the **Rain** notification:

```
Mobile Legends: Bang Bang
  Rain
  Overlay: visible            [ Hide UI ]
```

* **Hide UI** — every Rain visual disappears (watermark, menu, ESP, minimap
  markers). Combat automation, maphack and the config auto-save keep running.
* **Show UI** — brings it all back.

This is the fastest way to hide the overlay mid-match without opening the menu.

---

## 5. Configuration

### Config profiles

Settings are plain JSON files here:

```
/sdcard/Android/data/com.mobile.legends/files/rain_configs/
  default.json      <- auto-saved, loaded at boot
  profile_1.json    <- your snapshots
```

Manage them from **Settings → Config profiles** (Load / Overwrite / Delete /
New profile). Or handle the files directly:

```bash
# Pull all profiles to your computer
adb pull /sdcard/Android/data/com.mobile.legends/files/rain_configs/

# Push a hand-edited one (then tap Refresh in the menu)
adb push my_config.json /sdcard/Android/data/com.mobile.legends/files/rain_configs/
```

The files are readable JSON — open one and you will see exactly what each
setting is. Partial files work: any key you leave out keeps its current value.

Renaming a file renames the profile (`mv profile_1.json gusion.json`).

### Hero portrait icons (optional)

The minimap and ESP can draw hero portraits instead of plain dots. Drop
`<hero_id>.png` files here:

```
/sdcard/Android/data/com.mobile.legends/files/rain_icons/
```

Rain rescans that folder every 10 seconds, so new icons appear without a
restart. Hero IDs: Zilong is `16`, Kimmy is `71` (useful anchors when you are
building a set). A hero with no icon file falls back to a dot.

---

## 6. Troubleshooting

### Nothing happens — no watermark, no notification

1. Check Rain's log at all:
   ```bash
   adb logcat -d -s "Rain:*" | tail -30
   ```
2. **No output at all** → the file was not loaded. Check:
   ```bash
   adb shell "ls -la $GAMEDIR/librudp.bytes"     # is it Rain-sized (~1MB)?
   adb shell "md5sum $GAMEDIR/librudp.bytes"     # does it match the release?
   ```
   If the size is ~1.2 MB, the game restored the original. Re-do Step 6.
3. **Wrong ABI** → did you download the arm64 file for an arm64 device?
   ```bash
   adb shell getprop ro.product.cpu.abilist
   ```

### The game updated itself and Rain disappeared

Game updates restore the original module. After any MLBB update: re-do
Steps 5–6 (the game is stopped by the update anyway). Your config profiles
survive — they live outside the game's module directory.

### Touch feels dead in-game, but only sometimes (Xiaomi/Redmi/POCO)

This is MIUI's gesture zones eating edge touches before the game sees them —
not a Rain problem. Fix:

```bash
adb shell "settings put secure pref_open_game_booster 1"
```

Then relaunch the game. This is persistent. Full explanation in
`docs/TOUCH-FIX-MIUI-GESTURES.md`.

### No notification appears

The game needs notification permission:

**Settings → Apps → Mobile Legends → Notifications → allow.**

Rain posts through the game's identity, so if the game is blocked, the
notification is blocked.

### The menu opens but the game gets weird touches

Rain does not consume touches — taps on the menu also reach the game. Move
the launcher/menu over an unused part of the screen, or use the notification
toggle to hide the UI when you are done adjusting.

### Rain loaded once, then the game crashed on the second match

Known issue in older builds (v40 era) fixed in v41. Use the latest release.

---

## 7. Updating Rain

```bash
# Download the new release file, then:
adb push librain-arm64-v8a.so /sdcard/Download/rain.so
adb shell "am force-stop com.mobile.legends"
adb shell "cp /sdcard/Download/rain.so $GAMEDIR/librudp.bytes"
```

Your `rain_configs/` and `rain_icons/` are untouched. New settings added in
an update use defaults until you change them; your saved values carry over.

---

## 8. Uninstalling / restoring the original

```bash
adb shell "am force-stop com.mobile.legends"
adb shell "cp /sdcard/Download/librudp.original.bytes $GAMEDIR/librudp.bytes"
```

That is it — the game is back to stock. Optionally clean up:

```bash
adb shell "rm /sdcard/Download/rain.so"
adb shell "rm -rf /sdcard/Android/data/com.mobile.legends/files/rain_configs"
adb shell "rm -rf /sdcard/Android/data/com.mobile.legends/files/rain_icons"
```

If you lost the backup, force the game to re-download its resources
(clear cache in app settings, or reinstall) — the game re-fetches the
original module.

---

## 9. Advanced: hot-attach with rinject (root only)

The standard method needs a game restart. If your device is rooted, the
`rinject` injector attaches to a **running** game and loads Rain without
force-stopping it:

```bash
adb push rinject /data/local/tmp/rinject
adb push librain-arm64-v8a.so /data/local/tmp/rain.so
adb shell "chmod 755 /data/local/tmp/rinject"
adb shell "su -c '/data/local/tmp/rinject com.mobile.legends /data/local/tmp/rain.so'"
```

Build `rinject` from `src/injector/` (see `docs/RAIN.md`). This is useful
when you do not want to lose the current session state.

---

## 10. FAQ

**Does this work on other games?**
No. Rain is built specifically for MLBB's IL2CPP module layout. It targets
`com.mobile.legends` and its regional packages.

**Will a game update break it?**
Usually not — Rain resolves classes and fields by name, not by hardcoded
offsets, so field movement between builds is tolerated. A large update may
need a fresh build; check the releases page.

**Does it patch the anti-cheat?**
No. Rain reads game state and writes only presentation-side values (ESP
drawing, camera offset, skin id). It touches none of the anti-cheat surface.
That is deliberate, and it is the main reason the design is survivable across
game updates.

**Is my account at risk?**
The features that generate input (aim assist, combo, retribution) are the
ones a behaviour reporter can observe, which is why they ship off by default
and behind individual toggles. ESP/minimap/drone generate no input at all.
Read `docs/MLBB-INTERNALS.md` if you want the detail.

**Where do I report a problem?**
Open an issue on this repository with: your device model, Android version,
game version, the Rain log (`adb logcat -d -s "Rain:*"`), and what you
expected vs. saw.
