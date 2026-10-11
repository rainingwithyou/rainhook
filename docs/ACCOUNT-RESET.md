# Account Reset — Reset Guest (2026-10-12)

**Feature:** Settings → Account → **Reset Guest**. Replaces the stored device
identity, wipes the local login caches, and restarts the game as a fresh guest.

**Verified live:** from the ban screen (Role ID 994207246, 10947h remaining) to
the **Create Character** page in one reset. The old identity does not come
back.

This document supersedes the 2026-10-11 experiment, which concluded a reset
"cannot" change the account. That conclusion was wrong — it was drawn from a
method that deleted files without changing the identity they were keyed to.
The correct target is the *stored* device ID, and the reference
implementation's **Reset Guest** feature pointed at it.

---

## The method (reverse-engineered from VYNIX TRIALL)

The login SDK persists its device identity inside the game's Unity PlayerPrefs
XML under a key family:

```
JsonDeviceID            (84-char value, e.g. cd9e459e…-…)
__Java_JsonDeviceID__   (the Java-layer mirror of the same value)
```

The guest account is keyed to that **stored** value. The SDK trusts a present
value over re-deriving one from hardware identifiers — so replacing the value
is what changes the account, and deleting everything *around* it does nothing.

The reference implementation's reset does three things, in this order:

1. **Backup** the prefs + login caches.
2. **Replace** every device-id value in the PlayerPrefs XML with a fresh
   random value in the same 84-char shape (56-4-4-4-12 hex groups).
3. **SIGKILL the process.** This is not optional: Unity holds PlayerPrefs in
   memory and flushes them on a clean exit. A normal quit writes the old
   identity straight back over the surgery.

Rain implements all three (`src/src/account.cpp`). The live log:

```
account: key 'JsonDeviceID' = 'cd9e459e…' (84 chars)
account: key '__Java_JsonDeviceID__' = 'cd9e459e…' (84 chars)
account: backup -> …/rain_account_backup/20261011-231437 (6 item(s))
account: com.mobile.legends.v2.playerprefs.xml: JsonDeviceID ('cd9e459e…', 84 chars -> '3db7f856…')
account: com.mobile.legends.v2.playerprefs.xml: __Java_JsonDeviceID__ ('cd9e459e…', 84 chars -> '3db7f856…')
account: removed …/login_data  (login cache)
account: removed …/newbieofflineData  (offline account identity)
account: removed …/ug_install_settings_pref.xml  (SDK install identity)
account: removed …/snssdk_openudid.xml  (SDK openudid)
account: reset -> New guest identity 3db7f856…, 2 id key(s) replaced, 4 item(s) removed, 0 failed.
account: killing pid=11670 so the old identity cannot be flushed back
```

Next launch: **Buat Karakter** (Create Character), auto-name `Kaio.Mike~28`,
with **Ganti Akun** (Switch Account) available at the bottom.

## What the old experiment got wrong

The 2026-10-11 pass tried three escalating *file* wipes and concluded the
identity was unreachable without root:

| What was cleared | Result | Why it failed |
|---|---|---|
| Reset Account button (4 files) | Ban screen | Files deleted, identity value untouched — SDK rebuilt them |
| `pm clear com.mobile.legends` | Ban screen | Same: the identity value is in the wiped data, but re-derived from hardware on next launch |
| App reinstall | Ban screen | Same |

The conclusion "the SDK derives the account from Android ID" was an inference
from those failures. The actual mechanism is subtler: the SDK stores a device
ID it computes *partly* from hardware, and trusts the stored copy when one
exists. Delete the copy and it re-derives (same account); **replace** the copy
and it uses the new value (different account). The Android ID is an input to
the derivation, not the identity itself.

## The limit that remains true

**This does not un-ban an account.** It gives the device a *new* guest
identity. The banned account still exists on the publisher's servers and still
has its ban; you are simply not signing in as it any more. The UI says so:

> Resets the stored guest identity: every device-ID key in the game's
> preferences is replaced with a fresh value, and the login caches are
> removed. The game restarts as a new guest and reaches the login page.
>
> The process is killed at the end of the reset - that is deliberate.
>
> Limit, stated plainly: this does not un-ban an account. A ban is
> server-side; a fresh guest identity is a different account, not a cleared
> one.

## What the reset touches

| Path | Action |
|---|---|
| `…/shared_prefs/com.mobile.legends.v2.playerprefs.xml` | device-id values **replaced**, file kept |
| `…/shared_prefs/__SharedPreference__.xml` | scanned; edited only if it carries id keys |
| `…/dragon2017/login_data/` | removed |
| `…/dragon2017/newbieofflineData/` | removed |
| `…/shared_prefs/ug_install_settings_pref.xml` | removed |
| `…/shared_prefs/snssdk_openudid.xml` | removed |

Backup of everything touched → `files/rain_account_backup/<timestamp>/`
(6 items on the verified run), readable over ADB or any file manager.

Deliberately untouched: `assets/` (game content), `assets/comlibs/` (where
Rain lives), the rest of `shared_prefs/` (third-party SDK state), Rain's own
`rain_configs/`.

## Why the identity is reachable at all

The reset runs **inside the game's process as the game's own UID**. That is
what makes `/data/data/com.mobile.legends/` writable without root — the
private directory `adb shell` cannot touch on a non-debuggable package. This
is also why the same trick is not available from outside the process.

## Restore

The backup is a plain directory copy. To restore the previous identity, stop
the game and copy `rain_account_backup/<stamp>/com.mobile.legends.v2.playerprefs.xml`
back over the live file (plus the `login_data/` tree if wanted), then launch.
The identity value is the only thing that has to go back; the caches
regenerate.

## Verification log

- Live: ban screen → reset → **Create Character** (`Kaio.Mike~28`), Role ID
  gone, `Ganti Akun` present. 2026-10-11 23:14.
- Identity swap confirmed in the log for both keys, correct 84-char format.
- Backup verified on disk: `rain_account_backup/20261011-231437/` with all
  six items.
- Reboot-persistence: the new guest survived relaunch (the create-character
  screen is the post-reset state, not an error page).

## UI wording

The Settings tab states what it does and does not do:

> Resets the stored guest identity: every device-ID key in the game's
> preferences is replaced with a fresh value, and the login caches are
> removed. The game restarts as a new guest and reaches the login page.
>
> The process is killed at the end of the reset - that is deliberate. Unity
> flushes its preferences on a clean exit, which would write the old identity
> back. A full backup goes to rain_account_backup/ first.
>
> Limit, stated plainly: this does not un-ban an account. A ban is
> server-side; a fresh guest identity is a different account, not a cleared
> one.
