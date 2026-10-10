# Rain — Config System

**Date:** 2026-10-10
**Build:** v42 (`build/v42/librain.so`, md5 `56c36ad7c51c67158b439676543f5791`)
**Status:** implemented, host-tested, deployed and verified live on the phone.

---

## What it is

A file-based JSON config system. Every module's enable flag and settings live
in plain JSON files under

```
/sdcard/Android/data/com.mobile.legends/files/rain_configs/
```

That directory is the game's own external files dir — the process owns it, no
runtime permission is needed, any file manager can browse it, and ADB can
pull/push it. One file per profile: `<name>.json`.

## Why it replaced SharedPreferences

The old persistence went through the app's `SharedPreferences` over JNI. That
path was dead on this load model:

- `FindClass("com/unity3d/player/UnityPlayer")` from a native thread with no
  Java frames resolves through the system class loader, which cannot see
  application classes → null → the bridge never bound → nothing ever saved or
  loaded (project finding F1).
- `g_mid_put_string` was called by every colour save but never resolved — a
  latent ART `CHECK_NON_NULL_ARGUMENT` abort waiting for F1 to be fixed (F2).

Files need none of that. The replacement is smaller, has no JNI dependency,
and is user-visible, which the preferences file never was. F1 and F2 are
moot for persistence: the code that carried them is gone.

## Architecture

| Piece | File | Role |
|---|---|---|
| Schema writer + parser + file ops | `src/cfgjson.{h,cpp}` | Emit config as JSON; parse JSON back; list/save/load/delete profiles; name validation |
| Auto-save + boot load | `src/persist.{h,cpp}` | `default.json` written ≤2 s after a change, loaded at boot |
| Profile UI | `src/menu.cpp` (`TabSettings`) | List, Load, Overwrite, Delete, New profile, Refresh |
| Scale persistence | `src/config.h` | `ui_scale` moved from a menu global into Config so it persists |

### Write path

```
menu change → persist::MarkDirty() → persist::Tick() (≤2 s)
  → cfgjson::Save(g_cfg, "default")
      → emit JSON → write <name>.json.tmp → rename() over <name>.json
```

The rename is atomic: a process death mid-write leaves the previous file
intact.

### Read path

```
boot → persist::Load(g_cfg) → cfgjson::Load("default")
  → read file → parse whole document → apply present fields only
```

A partial file (hand-written, three keys) loads cleanly — absent fields keep
their current value. A malformed file fails whole; the live config is never
half-applied. The parser is a small strict JSON reader (objects, arrays,
strings with `\uXXXX`, numbers, bools, null) — no external dependency.

### Schema

One top-level object per feature area, each holding its enable flag and its
settings together:

```json
{
  "version": 1,
  "esp":        { "enabled": true, "box_style": 0, "categories": { "enemy_hero": { "color": [1, 0.25, 0.25, 1], "dead_mode": 2 } } },
  "range_circle": { "enabled": false, "fixed_radius": 12 },
  "alerts":     { "enabled": true, "turtle": true, "lord": true },
  "skin":       { "enabled": false, "skin_id": 0 },
  "drone":      { "enabled": false, "height": 0, "fov": 0, "hook": true },
  "maphack":    { "enabled": false, "minimap_flags": true, "health_flags": false },
  "minimap":    { "x": 0, "y": 0, "w": 268, "h": 245, "icons": true },
  "retribution": { "enabled": false, "hp_pct": 20, "stacks": 0, "base_dmg": 600 },
  "aim":        { "enabled": false, "mode": 0, "range": 12 },
  "combo":      { "enabled": false, "seq": [1, 0, 2, 1, 0], "steps": 5 },
  "kimmy":      { "enabled": false, "aim_mode": 0, "range": 14 },
  "targeting":  { "order": [1, 3, 2, 4, 5, 0] },
  "skills":     [ { "enabled": false, "aim_mode": 0, "range": 12 } ],
  "menu":       { "watermark": true, "ui_scale": 1 }
}
```

Keys are stable. The loader ignores unknown keys, so an older file loads into
a newer build cleanly.

### Profile management

* **New profile** — snapshots the current settings as `profile_N` (first free
  number; repeated saves never overwrite an existing snapshot) and selects it.
* **Load** — applies the selected profile to the live config, then writes it
  as `default` so the choice survives a restart.
* **Overwrite** — writes the current settings over the selected file.
* **Delete** — removes the selected file.
* **Refresh** — rescans the directory (for files pushed over ADB while the
  game is running).

Profile names are validated: no empty names, no leading `.`, no `..`, no path
separators, ≤48 chars. A profile can never escape the directory.

**Renaming:** there is no keyboard path on this platform (Rain reads touches,
not key events), so names are auto-generated and renamed by renaming the file
— `mv profile_1.json gusion.json` in any file manager or over ADB. That is
the point of the format.

## Verification

### Host round-trip test (`analysis/deep10/cfgtest/`)

Compiled `cfgjson.cpp` on the host with an `android/log.h` shim and exercised:

- Full round-trip: 30+ fields across every module, save → load → compare, all
  equal.
- Name validation: `../etc`, `a/b`, `.hidden`, empty — all rejected.
- Partial file: only present fields applied, everything else untouched.
- Malformed file: rejected whole, live config unchanged.
- List / save / delete lifecycle.

**Result: ALL PASS (0 failures).**

### Live on the phone (2026-10-10, MLBB 2.2.16.12322, `5d39a8f2`)

| Step | Evidence |
|---|---|
| Boot with no file | `persist: boot load 'default': no file (defaults)` |
| Auto-save fires | `cfgjson: saved 'default' (6695 bytes)` every 2 s while dirty |
| Boot with a file | `persist: boot load 'default': ok` — **first working persistence in the project's history** |
| New profile | `profile_1.json` appeared in the directory |
| Load | `cfgjson: loaded 'profile_1'` then re-saved as default |
| Delete | `cfgjson: deleted 'profile_1'`, file gone |
| ADB push | `handmade.json` pushed while running → **Refresh** → appeared in list |
| Hand-edited load | every value applied exactly: `esp.enabled=false`, `box_style=2`, `thickness=5`, `retri.hp_pct=33.5`, `stacks=7`, `kimmy.aim_mode=1` — verified by re-reading `default.json` |

The `default.json` on the phone was reset to factory defaults afterwards.

## Binary hygiene (v42)

| Check | Value |
|---|---|
| Size | 1,049,320 B |
| DT_NEEDED | 7 (unchanged) |
| BIND_NOW | yes |
| Exports | 2 (`JNI_OnLoad` / `JNI_OnUnload`) |
| Stripped | yes |

## Files

| File | Change |
|---|---|
| `src/cfgjson.h` | new — profile API |
| `src/cfgjson.cpp` | new — writer, parser, file ops |
| `src/persist.h` | rewritten — file-based contract |
| `src/persist.cpp` | rewritten — JNI bridge removed (F1/F2 moot) |
| `src/config.h` | `ui_scale` added to Config |
| `src/menu.cpp` | Settings tab: profile manager |
| `src/CMakeLists.txt` | `cfgjson.cpp` added |
| `analysis/deep10/cfgtest/` | host round-trip test + shims |
