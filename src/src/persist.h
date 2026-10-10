#pragma once
// Config persistence.
//
// The auto-save layer writes one JSON file (`default.json`) into the game's
// external files directory, and named profiles live beside it — the user can
// select, load, delete and save them from the Settings tab, browse the folder
// with any file manager, or pull/push it over ADB. See cfgjson.h for the
// profile API and the directory, and cfgjson.cpp for the file schema.
//
// (This replaced a JNI SharedPreferences bridge that could never bind on this
// load model, and whose colour-save path carried a latent ART abort.)

struct Config;

namespace persist {

// Read the auto-save profile into cfg. Called once at boot. When no file
// exists the config keeps its defaults.
void Load(Config& cfg);

// Write cfg to the auto-save profile immediately.
void Save(const Config& cfg);

// Mark the config dirty; the next tick saves it.
void MarkDirty();

// Called once per rendered frame; saves at most every 2 s while dirty.
void Tick(const Config& cfg);

}  // namespace persist
