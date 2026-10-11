#include "account.h"

#include <android/log.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
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
constexpr const char* kFilesDir =
    "/sdcard/Android/data/com.mobile.legends/files";

// The preference files that carry the persisted device identity. The login
// SDK's SDKPlayerPrefs class stores its keys inside the game's Unity
// PlayerPrefs XML — the key family is "JsonDeviceID" / "__Java_JsonDeviceID__"
// (the native name and its Java-layer mirror). This is the anchor the guest
// account is keyed to, and the exact target of the reference implementation's
// "Reset Guest" feature.
constexpr const char* kPlayerPrefs =
    "/data/data/com.mobile.legends/shared_prefs/com.mobile.legends.v2.playerprefs.xml";
constexpr const char* kSdkPrefs =
    "/data/data/com.mobile.legends/shared_prefs/__SharedPreference__.xml";

// Paths removed wholesale by Reset(). Relative to kExtBase (the game's
// external dir), or absolute when they start with '/'.
//
// What IS account state, and therefore all that is listed here:
//   login_data/            the login SDK's own cache (auth + native_login)
//   newbieofflineData/     the offline identity the game assigns pre-login
//   *_install* / udid      SDK device-identity caches the login SDK may reuse
//
// Deliberately NOT in the list:
//   assets/            game content (1 GB+; would re-download)
//   assets/comlibs/    where Rain itself lives
//   files/rain_configs, rain_icons   Rain's own settings
//   the playerprefs XML files   surgically edited, not deleted (they also
//                               carry every other setting the game stores)
struct Target {
    const char* path;
    const char* why;
};

const Target kTargets[] = {
    // --- external: the login cache the game reads first ---
    {"login_data",                          "login cache (auth + native_login)"},
    {"newbieofflineData",                   "offline account identity"},

    // --- private: SDK device-identity caches (regenerate on next launch) ---
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

// ---- file IO -------------------------------------------------------------

bool ReadFile(const std::string& p, std::string* out) {
    int fd = open(p.c_str(), O_RDONLY);
    if (fd < 0) return false;
    out->clear();
    char buf[8192];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0) { close(fd); return false; }
        if (n == 0) break;
        out->append(buf, static_cast<size_t>(n));
    }
    close(fd);
    return true;
}

// Atomic replace: write a temp file in the same directory, match the original
// mode, fsync, rename over. A crash mid-write leaves the original intact.
bool WriteFileAtomic(const std::string& p, const std::string& data) {
    const std::string tmp = p + ".raintmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;

    size_t off = 0;
    while (off < data.size()) {
        ssize_t w = write(fd, data.data() + off, data.size() - off);
        if (w <= 0) { close(fd); unlink(tmp.c_str()); return false; }
        off += static_cast<size_t>(w);
    }
    struct stat st;
    if (stat(p.c_str(), &st) == 0) fchmod(fd, st.st_mode & 0777);
    fsync(fd);
    close(fd);

    if (rename(tmp.c_str(), p.c_str()) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

// ---- identity ------------------------------------------------------------

bool IsDeviceKey(const std::string& name) {
    std::string low;
    low.reserve(name.size());
    for (char c : name) low += static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return low.find("deviceid") != std::string::npos ||
           low.find("device_id") != std::string::npos ||
           low.find("deviceuniqueid") != std::string::npos ||
           low.find("device_unique_id") != std::string::npos;
}

// The stored identity is an opaque 84-character string in five hex groups
// (56-4-4-4-12, dash-separated) — e.g. the value observed live:
//   cd9e459ea708a948d5c2f5a6ca8838cfac4beaf841202ef76504d36d
//     -ac6d-4434-9213-389efb011584
// The login SDK treats it as an opaque string, so a fresh random value in the
// same shape is indistinguishable from a freshly assigned one. Generating the
// same format matters: a differently-shaped value would be a fingerprint.
std::string NewDeviceId() {
    unsigned char buf[40] = {0};   // 80 hex chars = 40 bytes
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        size_t got = 0;
        while (got < sizeof buf) {
            ssize_t r = read(fd, buf + got, sizeof(buf) - got);
            if (r <= 0) break;
            got += static_cast<size_t>(r);
        }
        close(fd);
    }
    bool zero = true;
    for (unsigned char b : buf) if (b) { zero = false; break; }
    if (zero) {
        // urandom unavailable — fall back to time/pid mixed through an LCG
        unsigned long long seed = static_cast<unsigned long long>(time(nullptr)) ^
                                  (static_cast<unsigned long long>(getpid()) << 17);
        for (size_t i = 0; i < sizeof buf; i++) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            buf[i] = static_cast<unsigned char>(seed >> 33);
        }
    }
    static const char* hex = "0123456789abcdef";
    std::string h;
    h.reserve(80);
    for (unsigned char b : buf) {
        h += hex[b >> 4];
        h += hex[b & 0x0F];
    }
    // 56-4-4-4-12 groups, dash-separated — 84 chars total.
    return h.substr(0, 56) + "-" + h.substr(56, 4) + "-" + h.substr(60, 4) +
           "-" + h.substr(64, 4) + "-" + h.substr(68, 12);
}

// Replace the value of every <string name="...">VALUE</string> entry whose
// name is a device-identity key. Returns how many entries changed; appends
// "name (old -> new prefix)" notes to `notes` for the log.
int ReplaceDeviceIds(std::string* xml, const std::string& newid,
                     std::vector<std::string>* notes) {
    int replaced = 0;
    size_t pos = 0;
    for (;;) {
        const size_t s = xml->find("<string name=\"", pos);
        if (s == std::string::npos) break;
        const size_t name_begin = s + 14;
        const size_t name_end = xml->find('"', name_begin);
        if (name_end == std::string::npos) break;
        const std::string name = xml->substr(name_begin, name_end - name_begin);

        const size_t gt = xml->find('>', name_end);
        if (gt == std::string::npos) break;
        const size_t val_begin = gt + 1;
        const size_t val_end = xml->find("</string>", val_begin);
        if (val_end == std::string::npos) break;

        if (IsDeviceKey(name)) {
            const std::string old = xml->substr(val_begin, val_end - val_begin);
            std::string oldpfx = old.substr(0, 8);
            xml->replace(val_begin, val_end - val_begin, newid);
            if (notes) {
                notes->push_back(name + " ('" + oldpfx + "…', " +
                                 std::to_string(old.size()) + " chars -> '" +
                                 newid.substr(0, 8) + "…')");
            }
            replaced++;
            pos = val_begin + newid.size();
        } else {
            pos = val_end + 9;
        }
    }
    return replaced;
}

// ---- backup --------------------------------------------------------------

// Recursive copy (used to preserve the old identity before it is touched).
bool CopyTreeTo(const std::string& src, const std::string& dst, int depth = 0) {
    if (depth > 32) return false;
    struct stat st;
    if (lstat(src.c_str(), &st) != 0) return false;

    if (!S_ISDIR(st.st_mode)) {
        int in = open(src.c_str(), O_RDONLY);
        if (in < 0) return false;
        int out = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
        if (out < 0) { close(in); return false; }
        char buf[16384];
        bool ok = true;
        for (;;) {
            ssize_t n = read(in, buf, sizeof buf);
            if (n < 0) { ok = false; break; }
            if (n == 0) break;
            ssize_t off = 0;
            while (off < n) {
                ssize_t w = write(out, buf + off, static_cast<size_t>(n - off));
                if (w <= 0) { ok = false; break; }
                off += w;
            }
            if (!ok) break;
        }
        close(in);
        close(out);
        return ok;
    }

    if (mkdir(dst.c_str(), 0770) != 0 && errno != EEXIST) return false;
    DIR* d = opendir(src.c_str());
    if (!d) return false;
    bool ok = true;
    while (struct dirent* e = readdir(d)) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (!CopyTreeTo(src + "/" + e->d_name, dst + "/" + e->d_name, depth + 1))
            ok = false;
    }
    closedir(d);
    return ok;
}

std::string BaseName(const std::string& p) {
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

// /sdcard/.../files/rain_account_backup/<timestamp>/ — next to rain_configs,
// readable over ADB and from any file manager.
std::string BackupRoot() {
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char ts[32];
    strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &tmv);
    return std::string(kFilesDir) + "/rain_account_backup/" + ts;
}

void Mkdirs(const std::string& p) {
    std::string cur;
    for (size_t i = 0; i < p.size(); i++) {
        cur += p[i];
        if (p[i] == '/' && cur.size() > 1) mkdir(cur.c_str(), 0770);
    }
    mkdir(p.c_str(), 0770);
}

// ---- wipe ----------------------------------------------------------------

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

// Log every device-identity key found in a preference XML, without printing
// the whole value (a prefix is enough to tell two identities apart).
void DiagnoseXml(const char* path) {
    std::string xml;
    if (!ReadFile(path, &xml)) {
        LOGI("account:   [unreadable] %s", path);
        return;
    }
    int found = 0;
    size_t pos = 0;
    for (;;) {
        const size_t s = xml.find("<string name=\"", pos);
        if (s == std::string::npos) break;
        const size_t nb = s + 14;
        const size_t ne = xml.find('"', nb);
        if (ne == std::string::npos) break;
        const std::string name = xml.substr(nb, ne - nb);
        const size_t gt = xml.find('>', ne);
        if (gt == std::string::npos) break;
        const size_t vb = gt + 1;
        const size_t ve = xml.find("</string>", vb);
        if (ve == std::string::npos) break;
        if (IsDeviceKey(name)) {
            const std::string val = xml.substr(vb, ve - vb);
            LOGI("account:   key '%s' = '%.8s…' (%zu chars)", name.c_str(),
                 val.c_str(), val.size());
            found++;
        }
        pos = ve + 9;
    }
    LOGI("account:   %s: %d device-id key(s)", path, found);
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

    // The persisted device identity — what "Reset Guest" actually resets.
    DiagnoseXml(kPlayerPrefs);
    DiagnoseXml(kSdkPrefs);

    // The two files that most directly carry "who is logged in".
    const std::string auth = std::string(kExtBase) + "/login_data/login_auth_cache";
    LOGI("account:   login_auth_cache: %s", Exists(auth) ? "present" : "absent");
    const std::string native = std::string(kExtBase) + "/login_data/native_login";
    LOGI("account:   native_login:     %s", Exists(native) ? "present" : "absent");
    LOGI("account: --- end account state ---");
}

bool Reset() {
    Diagnose();   // log what is about to go, so the record survives it

    // 1. Backup everything that will be touched, before touching it.
    const std::string backup = BackupRoot();
    Mkdirs(backup);
    int backed = 0;
    for (const char* p : {kPlayerPrefs, kSdkPrefs}) {
        if (Exists(p) && CopyTreeTo(p, backup + "/" + BaseName(p))) backed++;
    }
    for (const Target& t : kTargets) {
        const std::string p = Full(t);
        if (!Exists(p)) continue;
        if (CopyTreeTo(p, backup + "/" + BaseName(p))) backed++;
    }
    LOGI("account: backup -> %s (%d item(s))", backup.c_str(), backed);

    // 2. Identity surgery: replace every device-id value with a fresh random
    //    one. This is the reference implementation's Reset Guest method —
    //    the guest account is keyed to this stored value, and the login SDK
    //    trusts a present value over re-deriving from hardware identifiers.
    const std::string newid = NewDeviceId();
    int replaced = 0;
    for (const char* p : {kPlayerPrefs, kSdkPrefs}) {
        std::string xml;
        if (!ReadFile(p, &xml)) {
            LOGI("account: %s: unreadable, skipped", p);
            continue;
        }
        std::vector<std::string> notes;
        const int n = ReplaceDeviceIds(&xml, newid, &notes);
        if (n == 0) {
            LOGI("account: %s: no device-id keys found", p);
            continue;
        }
        if (WriteFileAtomic(p, xml)) {
            replaced += n;
            for (const std::string& note : notes)
                LOGI("account: %s: %s", BaseName(p).c_str(), note.c_str());
        } else {
            LOGI("account: %s: WRITE FAILED", p);
        }
    }

    // 3. Wipe the login caches.
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

    char buf[256];
    snprintf(buf, sizeof buf,
             "New guest identity %s…, %d id key(s) replaced, %d item(s) "
             "removed, %d failed. Backup: rain_account_backup/%s",
             newid.c_str(), replaced, removed, failed, BaseName(backup).c_str());
    g_last = buf;
    LOGI("account: reset -> %s", g_last.c_str());

    // 4. SIGKILL the process. This is not optional: Unity holds PlayerPrefs
    //    in memory and flushes them on a clean exit — a normal quit would
    //    write the old identity straight back over the surgery. The reference
    //    implementation kills here too.
    LOGI("account: killing pid=%d so the old identity cannot be flushed back",
         getpid());
    kill(getpid(), SIGKILL);
    return true;   // not reached
}

const char* LastResult() { return g_last.c_str(); }

}  // namespace account
