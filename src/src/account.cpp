#include "account.h"

#include <android/log.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define LOG_TAG "Rain"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace account {

namespace {

// The two places account state lives. The game's own code reads both: the
// external login_data/ is the cache it consults at startup, and the private
// directory holds the session and the Unity player prefs.
constexpr const char* kExtBase =
    "/sdcard/Android/data/com.mobile.legends/files/dragon2017";
constexpr const char* kPkg = "com.mobile.legends";

// Paths removed by Reset(). Relative to kExtBase (the game's external dir),
// or absolute when they start with '/'.
//
// The list is deliberately surgical. An earlier draft wiped all of
// shared_prefs/, databases/ and files/ — but the diagnostic showed those are
// mostly third-party SDK state (Firebase, Facebook, Google, ByteDance), and
// files/ also holds __Resources_UnzipTimestamp__, whose removal can trigger a
// full resource re-extract. Wiping them would be destructive without being
// useful: none of it is "who is logged in".
//
// What IS the account state, and therefore all that is listed here:
//   login_data/            the login SDK's own cache (auth + native_login)
//   newbieofflineData/     the offline identity the game assigns pre-login
//   playerprefs.xml        Unity PlayerPrefs — the game's own prefs file
//   __SharedPreference__.xml  the SDK wrapper's own prefs
//   *_install* / udid      SDK device-identity caches the login SDK may reuse
//
// IMPORTANT — what this cannot do: MLBB's login SDK derives the account from
// the device's Android ID (a system setting derived from the app signing key,
// unchanged by an app reinstall and unreadable/writable without root). After a
// reset the game simply re-authenticates with that identity and reaches the
// same account. Clearing local files is necessary but not sufficient to change
// accounts; see docs/ACCOUNT-RESET.md for the full explanation.
//
// Deliberately NOT in the list:
//   assets/            game content (1 GB+; would re-download)
//   assets/comlibs/    where Rain itself lives
//   files/rain_configs, rain_icons   Rain's own settings
//   databases/, cache/ , the rest of files/   SDK state, not account state
struct Target {
    const char* path;
    const char* why;
};

const Target kTargets[] = {
    // --- external: the login cache the game reads first ---
    {"login_data",                          "login cache (auth + native_login)"},
    {"newbieofflineData",                   "offline account identity"},

    // --- private: the game's own preference store ---
    {"/data/data/com.mobile.legends/shared_prefs/com.mobile.legends.v2.playerprefs.xml",
     "Unity PlayerPrefs (account + settings)"},
    {"/data/data/com.mobile.legends/shared_prefs/__SharedPreference__.xml",
     "SDK preference wrapper"},
    // The login SDK's device-identity caches. These are re-created from the
    // Android ID, so clearing them alone does not change the account — but
    // leaving stale values behind can make a reset look like it failed, and
    // they are part of "the session" by any reasonable definition.
    {"/data/data/com.mobile.legends/shared_prefs/ug_install_settings_pref.xml",
     "SDK install identity"},
    {"/data/data/com.mobile.legends/shared_prefs/snssdk_openudid.xml",
     "SDK openudid (device identity)"},
};

std::string Full(const Target& t) {
    if (t.path[0] == '/') return t.path;
    return std::string(kExtBase) + "/" + t.path;
}

bool Exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

// Recursive delete. Never follows symlinks (lstat + unlink, no stat on the
// target), so a planted symlink cannot redirect the wipe outside the tree.
bool RemoveTree(const std::string& path, int depth = 0) {
    if (depth > 32) return false;   // paranoia against a symlink loop

    struct stat st;
    if (lstat(path.c_str(), &st) != 0) return false;

    if (!S_ISDIR(st.st_mode)) {
        return unlink(path.c_str()) == 0;
    }

    DIR* d = opendir(path.c_str());
    if (!d) return false;

    bool ok = true;
    while (struct dirent* e = readdir(d)) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        const std::string child = path + "/" + e->d_name;
        struct stat cs;
        if (lstat(child.c_str(), &cs) != 0) { ok = false; continue; }
        if (S_ISDIR(cs.st_mode)) {
            if (!RemoveTree(child, depth + 1)) ok = false;
        } else {
            if (unlink(child.c_str()) != 0) ok = false;
        }
    }
    closedir(d);

    // rmdir after the contents; a partially-failed tree is left in place
    // rather than reported as clean.
    if (rmdir(path.c_str()) != 0) ok = false;
    return ok;
}

// How many entries a directory holds, for the diagnostic report.
int CountEntries(const std::string& path) {
    DIR* d = opendir(path.c_str());
    if (!d) return -1;
    int n = 0;
    while (struct dirent* e = readdir(d)) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        n++;
    }
    closedir(d);
    return n;
}

// List entry names so the log shows exactly what a Reset would remove. Names
// only — no file contents, nothing that could carry a credential.
void ListEntries(const std::string& path, int max_names) {
    DIR* d = opendir(path.c_str());
    if (!d) return;
    int n = 0;
    std::string line;
    while (struct dirent* e = readdir(d)) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (n >= max_names) { line += " ..."; break; }
        if (!line.empty()) line += ", ";
        line += e->d_name;
        n++;
    }
    closedir(d);
    if (!line.empty()) LOGI("account:      -> %s", line.c_str());
}

std::string g_last = "not attempted";

}  // namespace

void Diagnose() {
    LOGI("account: --- local account state ---");
    for (const Target& t : kTargets) {
        const std::string p = Full(t);
        struct stat st;
        if (stat(p.c_str(), &st) != 0) {
            LOGI("account:   [absent] %s", p.c_str());
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            LOGI("account:   [dir, %d entries] %s  (%s)",
                 CountEntries(p), p.c_str(), t.why);
            // Name the entries so the log records exactly what a Reset would
            // take. This is the evidence needed before trusting the wipe.
            ListEntries(p, 12);
        } else {
            LOGI("account:   [file, %lld bytes] %s  (%s)",
                 static_cast<long long>(st.st_size), p.c_str(), t.why);
        }
    }

    // The two files that most directly carry "who is logged in".
    const std::string auth = std::string(kExtBase) + "/login_data/login_auth_cache";
    LOGI("account:   login_auth_cache: %s", Exists(auth) ? "present" : "absent");
    const std::string native = std::string(kExtBase) + "/login_data/native_login";
    LOGI("account:   native_login:     %s", Exists(native) ? "present" : "absent");
    LOGI("account: --- end account state ---");
}

bool Reset() {
    Diagnose();   // log what is about to go, so the record survives it

    int removed = 0, failed = 0;
    std::string failed_list;

    for (const Target& t : kTargets) {
        const std::string p = Full(t);
        struct stat st;
        if (lstat(p.c_str(), &st) != 0) continue;   // already absent

        const bool ok = RemoveTree(p);
        if (ok) {
            removed++;
            LOGI("account: removed %s  (%s)", p.c_str(), t.why);
        } else {
            failed++;
            if (!failed_list.empty()) failed_list += ", ";
            failed_list += p;
            LOGI("account: FAILED to remove %s", p.c_str());
        }
    }

    char buf[192];
    if (removed == 0 && failed == 0) {
        snprintf(buf, sizeof(buf),
                 "Nothing to remove - no local account state found.");
    } else if (failed == 0) {
        snprintf(buf, sizeof(buf),
                 "Removed %d item(s). Restart the game to reach the login page.",
                 removed);
    } else {
        snprintf(buf, sizeof(buf),
                 "Removed %d, failed %d (%s). Restart the game.",
                 removed, failed, failed_list.c_str());
    }
    g_last = buf;
    LOGI("account: reset -> %s", g_last.c_str());
    return removed > 0;
}

const char* LastResult() { return g_last.c_str(); }

}  // namespace account
