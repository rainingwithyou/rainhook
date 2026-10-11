# Account Reset — what it does, and its hard limit (2026-10-11)

**Feature:** Settings → Account → **Reset Account**. Removes the local login
and session files so the game starts without them.

**Limit, proven by experiment:** it does **not** return the game to the login
page on this device, and it cannot remove a ban. The account is anchored to
the device's Android ID, which survives everything short of a factory reset.
The game re-authenticates with that identity and reaches the same account.

This document records the experiment, because the result is counter-intuitive
and worth not re-deriving.

---

## What the feature does

Removes four to six files that make up the *local* session state:

| Path | What it is |
|---|---|
| `…/dragon2017/login_data/` | the login SDK's cache (`native_login`, `login_auth_cache`, prefs) |
| `…/dragon2017/newbieofflineData/` | the offline identity assigned before login |
| `/data/data/com.mobile.legends/shared_prefs/com.mobile.legends.v2.playerprefs.xml` | Unity PlayerPrefs — the game's own prefs |
| `/data/data/com.mobile.legends/shared_prefs/__SharedPreference__.xml` | the SDK wrapper's prefs |
| `…/shared_prefs/ug_install_settings_pref.xml` | SDK install identity |
| `…/shared_prefs/snssdk_openudid.xml` | SDK openudid |

It runs **inside the game's process as the game's own UID**, so it reaches
`/data/data/com.mobile.legends/` without root — the private directory that
`adb shell` cannot touch on a non-debuggable package.

Deliberately **not** removed: `assets/` (game content), `assets/comlibs/`
(where Rain lives), `databases/` and the rest of `shared_prefs/` (third-party
SDK state), and Rain's own `rain_configs/`.

## The experiment

Three escalating wipes, each followed by a fresh launch:

| What was cleared | Result |
|---|---|
| **1. The Reset Account button** (4 files, verified removed in the log) | Ban screen returned. `login_data/` was **regenerated** at launch, `playerprefs.xml` shrank 11811 → 5106 bytes, `newbieofflineData/` stayed gone. |
| **2. `pm clear com.mobile.legends`** (the system's *full* data wipe — every private file, every database, every cache) | **Ban screen returned anyway.** Role ID unchanged: `994207246`. |
| **3. App reinstall** (done by the user before this work) | Ban screen returned. |

Three wipes at three levels of severity, same result each time. The session is
not what produces the ban screen.

## Why

MLBB's login SDK derives the account from the **device's Android ID**:

```
adb shell settings get secure android_id
7d193bdedf41f08d          # unchanged across reinstall and pm clear
```

On Android 8+, the Android ID is derived from the device **and the app's
signing key**, and it is a system-level setting:

- it is not in the app's data directory, so `pm clear` does not touch it
- it is not reset by reinstalling the app
- it cannot be written without root (it lives in the system settings provider)

At launch the game reads it, authenticates to the server, is told the account
for that identity is banned, and shows the ban dialog. Clearing local files
just means the game rebuilds them from the same identity.

**A ban is server-side.** It is attached to the account, not the device's
files, and it follows the account wherever it is signed in. No amount of local
state removal changes a record on the publisher's servers.

## So what is it good for?

Still useful, in the cases where local state *is* the problem:

* a **stale or corrupted session** — a login that hangs, loops, or returns an
  error the game will not clear by itself
* **before signing in as a different account** — clears the cached session so
  the switch is clean rather than fighting a remembered login
* **handing the device over** — the local session token is gone from the
  device's storage (note: the Android ID link remains)

## The three ways to actually change account identity

1. **Sign in with a different account** through the game's own account switch.
   This is the supported path and the only one that works from the app.
2. **Factory reset the device.** Produces a new Android ID, so the login SDK
   sees a new identity. Heavy, and it costs everything else on the phone.
3. **Root, and change `android_id`.** Direct and reversible, but needs root —
   and the device in question does not have it.

None of these remove a ban from an account that has one. A ban expires on the
publisher's schedule or not at all; a different identity means a different
account, starting over.

## What was verified vs. assumed

**Verified on device:**
- the reset removes exactly the listed files, confirmed by the log
- the private directory is reachable from inside the game process (20
  shared_prefs, 8 databases, 16 files listed by the diagnostic)
- the files are regenerated at the next launch
- `pm clear` — the most thorough wipe the system offers — does not clear the
  ban screen
- the Android ID is unchanged across reinstall and `pm clear`

**Assumed, not verified:** that the login SDK uses the Android ID as its
primary identity. The evidence is strong (three wipes, same account) but the
SDK's internals were not read. It could be a different device-stable value
with the same properties; the practical conclusion is identical either way.

## UI wording

The Settings tab states the limit plainly rather than implying the button will
get around a ban:

> Removes the local login and session files so the game starts without them.
>
> Limits, so this is not oversold: MLBB derives the account from the device's
> Android ID, which is a system setting that survives an app reinstall and
> cannot be changed without root. The game will re-authenticate with that
> identity and can reach the same account. A ban is server-side and follows
> the account, so this does not remove one.
