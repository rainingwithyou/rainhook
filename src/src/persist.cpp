#include "persist.h"

#include <android/log.h>
#include <chrono>
#include <mutex>

#include "cfgjson.h"
#include "config.h"

#define LOG_TAG "Rain"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

// File-based persistence.
//
// The old implementation went through the app's SharedPreferences over JNI.
// That path is dead on this load model: `FindClass("com/unity3d/player/
// UnityPlayer")` from a native thread with no Java frames resolves through the
// system class loader, which cannot see application classes, so the bridge
// never bound and nothing ever saved or loaded. It also carried a latent
// abort (`putString` was never resolved but was called by every colour save).
//
// Files under the game's own external files dir need none of that: the
// process owns the directory, the user can browse it, and ADB can pull it.
// The profile format is cfgjson's (see cfgjson.cpp for the schema).

namespace persist {

namespace {

// The profile the auto-save layer writes and the boot load reads.
constexpr const char* kAutoName = "default";

std::mutex g_mtx;
bool g_dirty = false;
long long g_last_save_ms = 0;

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

void Load(Config& cfg) {
    std::lock_guard<std::mutex> lk(g_mtx);
    // First run has no file yet — defaults stand, and the first save creates
    // it. A present-but-malformed file is reported by cfgjson and ignored.
    const bool ok = cfgjson::Load(cfg, kAutoName);
    g_dirty = false;
    LOGI("persist: boot load '%s': %s", kAutoName, ok ? "ok" : "no file (defaults)");
}

void Save(const Config& cfg) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!cfgjson::Save(cfg, kAutoName)) {
        LOGI("persist: save '%s' failed", kAutoName);
        return;
    }
    g_dirty = false;
}

void MarkDirty() { g_dirty = true; }

void Tick(const Config& cfg) {
    if (!g_dirty) return;
    const long long now = NowMs();
    if (now - g_last_save_ms < 2000) return;
    g_last_save_ms = now;
    Save(cfg);
}

}  // namespace persist
