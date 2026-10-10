# Deployment Tutorial — No Computer (Android only)

Put Rain on your phone using only the phone. No PC, no ADB, no root.

You will need: the phone, an internet connection, and a file manager app.

**Time:** about five minutes. **Risk:** low — you back up the original file
first, and restoring it is one copy operation.

---

## 1. What you need on the phone

### A file manager that can browse `Android/data`

Since Android 11, apps cannot freely browse `Android/data/` — that is where
the game's module folder lives. You need a file manager that handles this.

| App | Where | Notes |
|---|---|---|
| **ZArchiver** | Play Store | Recommended. Free, asks for the folder permission once. |
| **MT Manager** | mt2.cn | Also works; heavier. |
| **X-plore** | Play Store | Works; more of a power-user tool. |

The stock file manager (MIUI's, Google Files, Samsung My Files) **will not
work** — they hide `Android/data` entirely. Install one of the above.

### Which Rain file to download

From the [Releases page](https://github.com/rainingwithyou/rainhook/releases),
pick the one matching your phone:

| Download | Use it when |
|---|---|
| **`librudp-arm64.bytes`** | **Almost every phone from ~2017 onward.** If unsure, use this. |
| `librudp-armv7.bytes` | Only for older 32-bit devices. |

These are pre-named — the file is already called `librudp.bytes`, so there is
no renaming step. Just download it.

> Not sure which your phone is? Settings → About phone → look for "64-bit" in
> the CPU/chipset line, or just try arm64 first — it is correct for the
> overwhelming majority of devices.

---

## 2. Install the file manager

1. Open the Play Store (or your app source)
2. Install **ZArchiver**
3. Open it once. When it asks for access, **allow** it — on Android 11+ it
   will specifically ask to access files in `Android/data`. Grant that.

If it does not ask automatically and you cannot see `Android/data`, see
[Section 6](#6-if-you-cannot-see-androiddata).

---

## 3. Find the game's module folder

In ZArchiver, navigate to:

```
/storage/emulated/0/Android/data/com.mobile.legends/files/dragon2017/assets/comlibs/arm64-v8a/
```

Or step by step:

```
Internal storage
  └ Android
      └ data
          └ com.mobile.legends
              └ files
                  └ dragon2017
                      └ assets
                          └ comlibs
                              └ arm64-v8a        <- you want to be here
```

In this folder you will see files ending in `.bytes`. Find **`librudp.bytes`**
(about 1.2 MB — the original).

> **Folder does not exist?** Open MLBB once and let it finish downloading its
> resources, then come back. The folder is created on first launch.
>
> **`dragon2017` not there?** Use the search function in ZArchiver (magnifier
> icon) and search for `librudp.bytes` inside `Android/data/com.mobile.legends`.

---

## 4. Back up the original file

**Do not skip this.** It is your uninstall.

1. Long-press `librudp.bytes`
2. Choose **Copy** (not Move)
3. Navigate to a safe folder you can reach easily — for example:
   ```
   /storage/emulated/0/Download/
   ```
   or a folder inside your normal Documents area (somewhere that survives
   clearing the game's cache)
4. **Paste** it
5. Rename the copy to something unmistakable, e.g.
   `librudp.ORIGINAL.backup.bytes`

Verify: you should now have two copies — the original still in `comlibs/`,
and your backup elsewhere. Check the backup is about 1.2 MB.

> Keep this file. If anything goes wrong, copying it back restores the game
> exactly.

---

## 5. Install Rain

1. Download `librudp-arm64.bytes` (from Section 1). It lands in your
   **Download** folder.
2. In ZArchiver, go to `Download`, long-press `librudp-arm64.bytes`, choose
   **Copy**
3. Navigate back to the module folder from Section 3:
   ```
   .../comlibs/arm64-v8a/
   ```
4. **Paste** it here.

Now you have both files in the folder. You need to replace the original with
Rain:

5. Long-press the **original** `librudp.bytes` → **Delete** (you already have
   the backup)
6. Long-press `librudp-arm64.bytes` → **Rename** → change the name to exactly:
   ```
   librudp.bytes
   ```
   (no `.bytes.bytes`, no extra spaces)

Final state of the folder:

```
librudp.bytes          <- Rain, about 1.0 MB
libAkSoundEngine.bytes
libmobile_bbai.bytes
... (other .bytes files untouched)
```

**Double-check the size.** Rain is about **1.0 MB**. If it still says 1.2 MB,
you are looking at the original — the rename or delete did not take.

---

## 6. If you cannot see Android/data

Android 11+ hides this folder from most apps. Fixes, in order of preference:

**ZArchiver:** it has its own request. Open ZArchiver → menu → *Settings* →
look for an option about `Android/data` or "Storage access", and follow the
prompt. On Android 11-12 it opens a system screen where you tap **"Use this
folder"** to grant access.

**If ZArchiver still refuses:**
1. Try **MT Manager** instead — it requests the same permission.
2. Or connect the phone to a PC once and use the ADB method in
   [`TUTORIAL.md`](TUTORIAL.md) — that path bypasses the restriction entirely.

**On Android 13+**, some file managers only see `Android/data` after you grant
access through *Settings → Apps → [file manager] → Permissions → Files and
media → Allow management of all files*.

---

## 7. Start the game and check

1. **Force-stop MLBB first** if it is running:
   - Settings → Apps → Mobile Legends → **Force stop**
   - (Or swipe it away from the recent-apps screen)
2. Launch MLBB normally

The game copies `comlibs/*.bytes` into its private folder at launch and loads
them. Within the first minute you should see:

* A **`Rain` watermark** in the top-left corner
* A round **`Rain` button** on the left edge
* A **notification**: "Rain — Overlay: visible" with a **Hide UI** button

### No computer to check logs? Use the notification.

The notification is your proof Rain loaded:

```
Mobile Legends: Bang Bang
  Rain
  Overlay: visible            [ Hide UI ]
```

If you see it, Rain is running. Tap **Hide UI** to hide the overlay; **Show UI**
to bring it back.

If there is no watermark, no button, and no notification, see
[Section 9](#9-troubleshooting-without-adb).

---

## 8. Using Rain without a computer

Everything Rain needs is reachable on the phone.

### The menu

Tap the round **Rain** button on the left edge. Tabs: Visual / Combat / Map /
Status / Settings.

> Taps on the menu also pass through to the game underneath — put the launcher
> over an unused corner of the screen.

### Hiding the overlay

Swipe down the notification shade → find the **Rain** notification → tap
**Hide UI**. This is the quickest way to hide everything mid-match.

### Config profiles

Profiles are plain files here:

```
/storage/emulated/0/Android/data/com.mobile.legends/files/rain_configs/
```

Manage them from **Settings → Config profiles** inside Rain (Load / Overwrite
/ Delete / New profile), or copy/rename the `.json` files directly in
ZArchiver. Renaming the file renames the profile.

To back up your settings, copy that whole folder somewhere safe.

### Hero icons (optional)

Drop `<hero_id>.png` files here to show hero portraits on the minimap:

```
/storage/emulated/0/Android/data/com.mobile.legends/files/rain_icons/
```

Rain rescans the folder every 10 seconds — no restart needed. Zilong is `16`,
Kimmy is `71`.

---

## 9. Troubleshooting without ADB

### Nothing appears when the game starts

Work through these in order:

1. **Is the file actually Rain?** Open the module folder, long-press
   `librudp.bytes` → *Properties*. Size should be about **1.0 MB**. If it is
   1.2 MB, it is still the original — redo Section 5.
2. **Did you force-stop the game before launching?** If the game was running,
   it may have kept the old module. Force-stop and relaunch.
3. **Wrong ABI?** Try the other file (`librudp-armv7.bytes`) — some older
   devices are 32-bit.
4. **The game restored the original.** Some game versions re-download their
   modules. Recheck the size; if it went back to 1.2 MB, the game overwrote
   you. Redo Section 5, then launch and check quickly.

### The game updated and Rain disappeared

Normal — updates restore the original module. Redo Section 5. Your config
profiles are safe (they are outside the module folder).

### Touch feels dead in-game (Xiaomi / Redmi / POCO)

This is MIUI's gesture zones stealing edge touches before the game sees them,
not Rain. Without a PC, fix it in Settings:

* **Settings → Additional settings → Gesture shortcuts** (or **Settings →
  Special features → Game Turbo**), and make sure **Game Turbo / Game
  Booster** is enabled for Mobile Legends.

Or switch navigation to buttons temporarily: **Settings → Additional settings
→ Full screen display → System navigation → Buttons**.

### No notification

Check the game has notification permission: **Settings → Apps → Mobile
Legends → Notifications → allow**. Rain posts through the game's identity, so
if the game is blocked, the notification is blocked.

### The game will not start at all

Restore your backup: copy `librudp.ORIGINAL.backup.bytes` back into the
`comlibs/arm64-v8a/` folder and rename it to `librudp.bytes` (delete the
broken one first). The game is then exactly as it was.

If you lost the backup: reinstall MLBB, or clear its cache and let it
re-download resources — the original module comes back.

---

## 10. Updating Rain

1. Download the new `librudp-arm64.bytes`
2. Force-stop the game
3. Copy it into `comlibs/arm64-v8a/`, deleting the old `librudp.bytes` and
   renaming the new one
4. Launch

Your `rain_configs/` and `rain_icons/` are untouched.

---

## 11. Uninstalling

1. Force-stop the game
2. In the module folder: delete `librudp.bytes`
3. Copy your backup (`librudp.ORIGINAL.backup.bytes`) here and rename it to
   `librudp.bytes`
4. Launch the game — it is back to stock

Optionally delete `Android/data/com.mobile.legends/files/rain_configs/` and
`.../rain_icons/` too.

---

## Appendix — doing it with a computer instead

If you have a PC and a cable, the ADB method is more reliable and gives you
real logs to diagnose with. See [`TUTORIAL.md`](TUTORIAL.md).

You can also do the file operations over ADB from a PC:

```bash
GAMEDIR=/sdcard/Android/data/com.mobile.legends/files/dragon2017/assets/comlibs/arm64-v8a
adb shell "cp $GAMEDIR/librudp.bytes /sdcard/Download/librudp.ORIGINAL.backup.bytes"
adb push librudp-arm64.bytes /sdcard/Download/
adb shell "am force-stop com.mobile.legends"
adb shell "cp /sdcard/Download/librudp-arm64.bytes $GAMEDIR/librudp.bytes"
```
