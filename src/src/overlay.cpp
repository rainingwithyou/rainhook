#include "overlay.h"

#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <ctime>
#include <cstring>
#include <cstdio>

#include <imgui.h>
#include <backends/imgui_impl_opengl3.h>

#include "Gloss.h"
#include "autox.h"
#include "config.h"
#include "esp.h"
#include "game.h"
#include "il2cpp.h"
#include "input.h"
#include "icons.h"
#include "maphack.h"
#include "menu.h"
#include "notify.h"
#include "persist.h"
#include "synth.h"

#define LOG_TAG "Rain"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace overlay {

namespace {

typedef EGLBoolean (*SwapFn)(EGLDisplay, EGLSurface);
SwapFn g_orig_swap = nullptr;
bool g_installed = false;
bool g_imgui_ready = false;
char g_err[192] = {0};

void SetupStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 8.0f;
    s.FrameRounding = 5.0f;
    s.GrabRounding = 5.0f;
    s.PopupRounding = 6.0f;
    s.ScrollbarRounding = 6.0f;
    s.WindowBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.WindowPadding = ImVec2(12, 12);
    s.FramePadding = ImVec2(8, 6);
    s.ItemSpacing = ImVec2(8, 7);

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.07f, 0.10f, 0.94f);
    c[ImGuiCol_Border] = ImVec4(0.25f, 0.55f, 0.95f, 0.55f);
    c[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.10f, 0.16f, 1.00f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.18f, 0.30f, 1.00f);
    c[ImGuiCol_FrameBg] = ImVec4(0.13f, 0.15f, 0.21f, 1.00f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.24f, 0.36f, 1.00f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.20f, 0.28f, 0.42f, 1.00f);
    c[ImGuiCol_CheckMark] = ImVec4(0.35f, 0.75f, 1.00f, 1.00f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.35f, 0.70f, 1.00f, 1.00f);
    c[ImGuiCol_Button] = ImVec4(0.16f, 0.22f, 0.34f, 1.00f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.34f, 0.52f, 1.00f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.28f, 0.42f, 0.62f, 1.00f);
    c[ImGuiCol_Header] = ImVec4(0.16f, 0.22f, 0.34f, 1.00f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.22f, 0.34f, 0.52f, 1.00f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.28f, 0.42f, 0.62f, 1.00f);
    c[ImGuiCol_Text] = ImVec4(0.92f, 0.94f, 0.98f, 1.00f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.54f, 0.62f, 1.00f);
}

void ImGuiInit(EGLDisplay dpy, EGLSurface surf) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    // A larger default font: the stock 13px is unreadable on a 1440x900 screen.
    ImFontConfig cfg;
    cfg.SizePixels = 22.0f;
    io.Fonts->AddFontDefault(&cfg);
    io.FontGlobalScale = 1.0f;

    SetupStyle();

    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    game::SetScreenSize(w, h);
    io.DisplaySize = ImVec2(static_cast<float>(w), static_cast<float>(h));

    if (!ImGui_ImplOpenGL3_Init("#version 300 es")) {
        snprintf(g_err, sizeof(g_err), "ImGui GL3 backend init failed");
        LOGE("%s", g_err);
        return;
    }
    g_imgui_ready = true;
    LOGI("overlay: ImGui ready, surface %dx%d", w, h);
}

void RenderFrame(EGLDisplay dpy, EGLSurface surf) {
    // Any il2cpp_runtime_invoke must run on a thread the IL2CPP runtime knows
    // about. This runs on Unity's GfxDevice worker, which the runtime has not
    // attached; invoking from it faults on a null thread state and takes the
    // render thread down with it. Thread-local: if the hook ever executes on
    // more than one render thread, each must attach itself.
    static thread_local bool t_attached = false;
    if (!t_attached && il2cpp::api.thread_attach && il2cpp::api.domain_get) {
        il2cpp::api.thread_attach(il2cpp::api.domain_get());
        t_attached = true;
    }

    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w <= 0 || h <= 0) return;
    game::SetScreenSize(w, h);
    // Tell the touch synthesizer what coordinate space its callers use: every
    // caller passes surface pixels (they come from Unity's own touch list), and
    // synth has to convert those to physical display pixels before dispatching.
    synth::SetUnitySize(w, h);

    // Re-read the camera before anything projects, so the ESP tracks a moving
    // camera rather than lagging it.
    game::BeginFrame();

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(w), static_cast<float>(h));

    // Touch input comes from UnityEngine.Input, which exists as soon as Unity is
    // up — independent of whether the battle classes have been bound yet.
    static bool input_ready = false;
    if (!input_ready) input_ready = input::Init();

    // Hero icons: needs the GL context. Rescans on an internal timer so newly
    // dropped <id>.png files appear without restarting the game.
    icons::Init();

    // Battle classes only exist once a match scene is live. Keep retrying, but
    // never let that gate the menu: the overlay has to be usable (and visible)
    // while sitting in the lobby. Throttled to ~500 ms — until ShowEntity
    // binds, each retry walks every loaded image, and an every-frame walk
    // would burn the render thread in the lobby.
    if (!game::Ready()) {
        static long long last_init_ms = 0;
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        const long long now = static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
        if (now - last_init_ms >= 500) {
            last_init_ms = now;
            game::Init();
        }
    }

    input::Pump();

    // The notification's button flips this. Hiding the UI stops every visual
    // draw but leaves the game reads, the entity snapshot and the automation
    // running — the toggle is about what is on screen, not about turning the
    // mod off.
    const bool ui_visible = notify::UiVisible();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    if (ui_visible) {
        if (menu::Visible()) {
            menu::Draw();
        } else {
            menu::DrawLauncher();
        }

        esp::Draw();
        menu::DrawWatermark();
        // In-match floating toggles draw last so nothing covers their hit
        // boxes.
        menu::DrawFloatingButtons();
    }

    // Engine-side features run regardless of what is drawn: maphack flag
    // writes, the drone offset, the skin re-apply, the combat automation and
    // the config auto-save are all gameplay state, not overlay pixels.
    maphack::Tick();
    game::ApplyDrone();
    game::TickSkin();
    autox::Tick(ui_visible && menu::Visible());
    synth::Tick();
    persist::Tick(g_cfg);
    notify::Tick();

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

EGLBoolean HookSwap(EGLDisplay dpy, EGLSurface surf) {
    // Retryable init: a static "tried" flag latched the very first surface,
    // which can be invalid during activity startup; if ImGui init failed or
    // the surface was dead, the overlay never came back. Now a failed init
    // retries on the next swap, and a previously-good init re-runs when the
    // surface dimensions change (context/surface recreation).
    static bool tried = false;
    static EGLint last_w = 0, last_h = 0;
    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    const bool surface_changed =
        g_imgui_ready && (w != last_w || h != last_h) && w > 0 && h > 0;
    // A pending failed init also needs a destroy: ImGuiInit calls
    // CreateContext before the backend init, so a failed init leaves a context
    // behind — without destroying it, a retry leaks one context per swap.
    if (!tried || !g_imgui_ready || surface_changed) {
        tried = true;
        if (g_imgui_ready && surface_changed) {
            // Surface was recreated: shut down the GL backend before re-init.
            ImGui_ImplOpenGL3_Shutdown();
            ImGui::DestroyContext();
            g_imgui_ready = false;
        } else if (!g_imgui_ready && ImGui::GetCurrentContext()) {
            // Previous init failed mid-way: free its context before retrying.
            ImGui::DestroyContext();
        }
        if (w > 0 && h > 0) {
            last_w = w; last_h = h;
            ImGuiInit(dpy, surf);
        }
    }
    if (g_imgui_ready) RenderFrame(dpy, surf);
    return g_orig_swap(dpy, surf);
}

}  // namespace

bool IsInstalled() { return g_installed; }
const char* LastError() { return g_err; }

bool Install() {
    GlossInit(false);

    void* target = nullptr;
    GHook h = GlossHookByName("libEGL.so", "eglSwapBuffers",
                              reinterpret_cast<void*>(HookSwap),
                              reinterpret_cast<void**>(&g_orig_swap), nullptr);
    if (!h) {
        // Fall back to resolving the symbol ourselves.
        GHandle lib = GlossOpen("libEGL.so");
        if (!lib) {
            snprintf(g_err, sizeof(g_err), "libEGL.so not openable");
            return false;
        }
        target = reinterpret_cast<void*>(GlossSymbol(lib, "eglSwapBuffers", nullptr));
        GlossClose(lib, false);
        if (!target) {
            snprintf(g_err, sizeof(g_err), "eglSwapBuffers not found");
            return false;
        }
        h = GlossHook(target, reinterpret_cast<void*>(HookSwap),
                      reinterpret_cast<void**>(&g_orig_swap));
        if (!h) {
            snprintf(g_err, sizeof(g_err), "GlossHook(eglSwapBuffers) failed");
            return false;
        }
    }

    g_installed = (g_orig_swap != nullptr);
    LOGI("overlay: eglSwapBuffers hooked (%d), orig=%p", g_installed, (void*)g_orig_swap);
    return g_installed;
}

}  // namespace overlay
