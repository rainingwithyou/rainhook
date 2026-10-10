#include "icons.h"

#include <android/log.h>
#include <dirent.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <ctime>
#include <cstdlib>
#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>

#define LOG_TAG "Rain"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace icons {

namespace {

// The game's own external files dir: adb push reaches it directly (the
// emulator's shell can write Android/data on this API level) and the game
// process owns it, so it is readable without root or extra copies.
constexpr const char* kIconDir =
    "/storage/emulated/0/Android/data/com.mobile.legends/files/rain_icons";

struct Icon {
    int hero_id = 0;
    ImTextureID tex = (ImTextureID)0;
};

Icon g_icons[192];
int g_icon_count = 0;
bool g_ready = false;

ImTextureID UploadRGBA(unsigned char* data, int w, int h) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glBindTexture(GL_TEXTURE_2D, 0);
    return (ImTextureID)(uintptr_t)tex;
}

bool LoadOne(const char* path, int hero_id) {
    int w = 0, h = 0, n = 0;
    unsigned char* data = stbi_load(path, &w, &h, &n, 4);
    if (!data) return false;
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) {
        stbi_image_free(data);
        return false;
    }
    const ImTextureID tex = UploadRGBA(data, w, h);
    stbi_image_free(data);
    if (!tex) return false;

    if (g_icon_count < 192) {
        g_icons[g_icon_count++] = Icon{hero_id, tex};
        return true;
    }
    return false;
}

}  // namespace

bool Init() {
    // Rescan on a timer instead of latching. The first successful scan used to
    // set g_ready forever, so dropping a new <id>.png into the directory needed
    // a full game restart to be picked up. The scan only ever adds: icons
    // already uploaded are left alone, and the directory is one readdir.
    static long long last_ms = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const long long now = static_cast<long long>(ts.tv_sec) * 1000 +
                          ts.tv_nsec / 1000000;
    if (last_ms != 0 && now - last_ms < 10000) return g_ready;
    last_ms = now;

    DIR* d = opendir(kIconDir);
    if (!d) return g_ready;

    int added = 0;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        const char* name = ent->d_name;
        // <hero_id>.png only
        const size_t len = strlen(name);
        if (len < 5 || strcmp(name + len - 4, ".png") != 0) continue;

        char idbuf[16] = {0};
        memcpy(idbuf, name, len - 4 > 15 ? 15 : len - 4);
        char* end = nullptr;
        const long id = strtol(idbuf, &end, 10);
        if (!end || *end != '\0' || id <= 0 || id > 100000) continue;
        if (IconFor(static_cast<int>(id)) != (ImTextureID)0) continue;  // have it

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", kIconDir, name);
        if (LoadOne(path, static_cast<int>(id))) added++;
    }
    closedir(d);

    if (added > 0) {
        LOGI("icons: loaded %d new hero icons from %s (%d total)", added,
             kIconDir, g_icon_count);
    }
    if (g_icon_count > 0) g_ready = true;
    return g_ready;
}

bool Ready() { return g_ready; }

ImTextureID IconFor(int hero_id) {
    for (int i = 0; i < g_icon_count; i++)
        if (g_icons[i].hero_id == hero_id) return g_icons[i].tex;
    return (ImTextureID)0;
}

}  // namespace icons
