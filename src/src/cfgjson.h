#pragma once
// Named JSON config profiles, stored in a user-visible directory on the
// device's shared storage:
//
//     /sdcard/Android/data/com.mobile.legends/files/rain_configs/
//
// That path is the game's own external files directory — the same place
// `rain_icons/` lives — so it needs no runtime permissions, the user can
// browse it with any file manager, and it can be pulled/pushed over ADB:
//
//     adb pull /sdcard/Android/data/com.mobile.legends/files/rain_configs/
//
// One file per profile: `<name>.json`. The format is flat and readable —
// module toggles first, then the settings each module reads. See the writer
// in cfgjson.cpp for the exact schema; it round-trips every field of Config.

struct Config;

namespace cfgjson {

// Directory profiles live in. Created on first use.
const char* Dir();

// Scan the directory and fill `names` with the profile stems (no .json),
// sorted. `max` entries at most; returns how many were written.
int List(char names[][64], int max);

// Write `cfg` to <name>.json. Returns false on any I/O failure. The name is
// sanitized: path separators and control characters are rejected, so a
// profile can never escape the directory.
bool Save(const Config& cfg, const char* name);

// Read <name>.json into `cfg`. Fields absent from the file keep their current
// value, so a partial file loads cleanly. Returns false when the file does
// not exist or is not parseable as a JSON object.
bool Load(Config& cfg, const char* name);

// Delete <name>.json. Returns false when it does not exist or cannot be
// removed.
bool Delete(const char* name);

// True when `name` is a legal profile name (non-empty, no separators, no
// traversal, reasonable length).
bool NameOk(const char* name);

}  // namespace cfgjson
