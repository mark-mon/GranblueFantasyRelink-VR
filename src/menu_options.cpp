// Granblue VR adapter for the vrkit in-game settings menu.
// Registers the mod's config.txt options with vrkit::Options (pointing straight at g_cfg fields) and wires
// each one to the code that applies it live. Everything game-independent lives in vrkit/.
#include "vrkit_options.h"
#include "vrkit_menu.h"
#include "globals.h"
#include "log.h"
#include <string>
#include <cstring>

extern std::wstring g_modDir;
extern const char* kModVersion;
bool Perf_Text(char* out, size_t n);
void VR_ApplyRenderScale();
void VR_SetStatsOn(bool on);
void VR_SetQuadOn(bool on);
void VR_SetEyeTrackingOn(bool on);
void VR_Recenter();
void Vrs_SetActive(bool on);
void Vrs_ConfigChanged();
void PostFx_AaFromConfig();
void PostFx_TaaChanged();
void PostFx_SkipConfigChanged();
const char* PostFx_AaValue();

namespace {
void LogToMod(const char* m) { LOGI("%s", m); }

vrkit::input::Settings MenuInput() {
    vrkit::input::Settings in;
    in.chordHoldMs = g_cfg.menuHoldMs;
    in.chordButtons = g_cfg.menuChord == "l3r3" ? (0x0040 | 0x0080) : 0;   // XINPUT_GAMEPAD_LEFT_THUMB | RIGHT_THUMB
    in.chordTriggers = g_cfg.menuChord == "ltrt";
    in.chordPassThrough = g_cfg.menuChordPass;
    in.keyboardToggleVk = g_cfg.menuKey;
    return in;
}
void ApplyMenuInput() { vrkit::input::Configure(MenuInput()); }

void Register() {
    using vrkit::Options;
    Options& o = Options::Get();
    if (!o.All().empty()) return;
    static const std::vector<std::string> kRates = { "1x1", "2x1", "1x2", "2x2", "4x2", "2x4", "4x4" };

    // ---- Display
    o.AddFloat("Display", "render_scale", "Render scale", &g_cfg.renderScale, 0.25f, 3.f, 0.05f, 2,
               "Per-eye resolution relative to the headset's recommended size.", [] { VR_ApplyRenderScale(); });
    o.AddInt("Display", "render_max", "Max render size", &g_cfg.renderMax, 1024, 16384, 256,
             "Upper limit for the per-eye width / height in pixels.", [] { VR_ApplyRenderScale(); });
    o.AddBool("Display", "show_stats", "Stats panel", &g_cfg.showStats,
              "Frame rate, CPU / GPU times and the active settings.", [] { VR_SetStatsOn(g_cfg.showStats); });
    o.AddFloat("Display", "hud_distance", "HUD distance (m)", &g_cfg.hudDistance, 0.3f, 10.f, 0.1f, 1,
               "How far away the game's HUD panel floats.");
    o.AddFloat("Display", "hud_size", "HUD width (m)", &g_cfg.hudSize, 0.2f, 10.f, 0.1f, 1,
               "Width of the HUD panel.");
    o.AddBool("Display", "hud_blur_backdrop", "HUD frosted backdrops", &g_cfg.hudBlurBackdrop,
              "The game's blurred panel backgrounds. They show a misplaced piece of the scene in VR.", [] { PostFx_SkipConfigChanged(); });
    o.AddAction("Display", "Recenter view", [] { VR_Recenter(); }, "Make the current head position the centre.");

    // ---- Anti-aliasing
    auto& aa = o.AddChoice("Anti-aliasing", "aa_mode", "Mode", &g_cfg.aaMode, { "off", "fxaa", "taa", "game" },
                           "Off = sharpest. FXAA = edge smoothing. Mild TAA = tunable. Game = the game's own soft TAA.",
                           [] { PostFx_AaFromConfig(); });
    aa.choiceLabels = { "Off", "FXAA", "Mild TAA", "Game TAA" };
    auto isTaa = [] { return g_cfg.aaMode == "taa"; };
    o.AddFloat("Anti-aliasing", "taa_strength", "TAA strength", &g_cfg.taaStrength, 0.f, 1.f, 0.05f, 2,
               "How much of previous frames is blended in (the game's TAA is about 0.9).", [] { PostFx_TaaChanged(); }).visible = isTaa;
    o.AddFloat("Anti-aliasing", "taa_jitter", "TAA jitter", &g_cfg.taaJitter, 0.f, 1.f, 0.05f, 2,
               "Camera jitter amount: more = smoother edges, softer image.").visible = isTaa;
    o.AddBool("Anti-aliasing", "disable_color_grading", "Disable colour grading", &g_cfg.disableFog,
              "Turns off the game's depth-based colour grading (flatter, lighter image).");

    // ---- Foveation (VRS)
    o.AddBool("Foveation", "vrs", "Variable rate shading", &g_cfg.vrs,
              "Shade the edges of the image at lower rate (NVIDIA). Big performance win.", [] { Vrs_SetActive(g_cfg.vrs); });
    o.AddFloat("Foveation", "vrs_inner", "Full-rate radius", &g_cfg.vrsInner, 0.05f, 1.5f, 0.05f, 2,
               "Radius of the full-rate centre (fraction of half the image width).", [] { Vrs_ConfigChanged(); });
    o.AddFloat("Foveation", "vrs_outer", "Middle ring radius", &g_cfg.vrsOuter, 0.05f, 2.f, 0.05f, 2,
               "Outer radius of the middle-rate ring; outside it the outer rate is used.", [] { Vrs_ConfigChanged(); });
    o.AddChoice("Foveation", "vrs_mid_rate", "Middle rate", &g_cfg.vrsMidRate, kRates,
                "Pixels per shading sample in the middle ring.", [] { Vrs_ConfigChanged(); });
    o.AddChoice("Foveation", "vrs_outer_rate", "Outer rate", &g_cfg.vrsOuterRate, kRates,
                "Pixels per shading sample outside the ring (4x4 is the maximum).", [] { Vrs_ConfigChanged(); });
    o.AddBool("Foveation", "eye_tracking", "Eye tracking", &g_cfg.eyeTracking,
              "Use the headset's eye tracking, if it provides it.", [] { VR_SetEyeTrackingOn(g_cfg.eyeTracking); });
    o.AddBool("Foveation", "vrs_follow_gaze", "VRS follows gaze", &g_cfg.vrsFollowGaze,
              "Move the full-rate centre with your eyes (needs eye tracking).", [] { Vrs_ConfigChanged(); });

    // Quad views: not offered (VRS + eye tracking is better in this game). Hidden config keys still work.

    // ---- View
    o.AddBool("View", "head_tracking", "Head tracking", &g_cfg.headTracking, "Apply the headset pose to the game camera.");
    o.AddFloat("View", "world_scale", "World scale", &g_cfg.worldScale, 0.1f, 10.f, 0.05f, 2,
               "Game units per metre: higher = the world feels smaller.");
    o.AddBool("View", "fix_cull_fov", "Edge culling fix", &g_cfg.fixCullFov,
              "Stops objects popping in and out at the edges of the view.");
    auto& ec = o.AddChoice("View", "edge_clipping", "Edge clipping", &g_cfg.edgeClipping, { "off", "default", "experimental" },
                           "Helps textures and objects at the sides of your view stop dropping out when you turn your head. "
                           "Experimental: best result, but may have unexpected results.");
    ec.choiceLabels = { "Off", "Default", "Experimental" };
    ec.visible = [] { return g_cfg.fixCullFov; };
    o.AddBool("View", "desktop_vsync", "Desktop vsync", &g_cfg.desktopVsync,
              "Let the game's vsync limit the frame rate (normally off in VR).");

    // ---- Menu (this panel and how to open it)
    auto& an = o.AddChoice("Menu", "menu_anchor", "Placement", &g_cfg.menuAnchor, { "world", "head" },
                           "Fixed: stays where it opened, so you can turn your head to read it. Follows head: moves with you.");
    an.choiceLabels = { "Fixed in space", "Follows head" };
    o.AddFloat("Menu", "menu_distance", "Distance (m)", &g_cfg.menuDistance, 0.4f, 3.f, 0.05f, 2, "How far away this menu panel is.");
    o.AddFloat("Menu", "menu_size", "Width (m)", &g_cfg.menuSize, 0.3f, 2.f, 0.05f, 2, "Width of this menu panel.");
    o.AddFloat("Menu", "menu_height", "Height (m)", &g_cfg.menuHeight, -1.f, 1.f, 0.02f, 2, "Up (+) or down (-) from eye level.");
    auto& ch = o.AddChoice("Menu", "menu_chord", "Menu button", &g_cfg.menuChord, { "l3r3", "ltrt", "none" },
                           "Controller combination that opens this menu when held (Insert always works).", [] { ApplyMenuInput(); });
    ch.choiceLabels = { "L3 + R3", "LT + RT", "Keyboard only" };
    o.AddBool("Menu", "menu_chord_pass", "Menu button reaches game", &g_cfg.menuChordPass,
              "On: a short press still does the game's action (L3 + R3 = party skill); only holding opens the menu.", [] { ApplyMenuInput(); });
    o.AddInt("Menu", "menu_hold_ms", "Menu hold time (ms)", &g_cfg.menuHoldMs, 100, 3000, 100,
             "How long the menu button must be held.", [] { ApplyMenuInput(); });
}
} // namespace

// Called from InitD3D once the game's device exists.
void MenuOptions_Init(ID3D11Device* dev, ID3D11DeviceContext* imm) {
    vrkit::SetLog(LogToMod);
    if (g_cfg.aaMode.empty()) g_cfg.aaMode = PostFx_AaValue();   // older configs: derived from disable_taa
    Register();
    vrkit::menu::Settings s;
    s.title = std::string("Granblue VR ") + kModVersion;
    s.configPath = g_modDir + L"config.txt";
    s.statusLine = [] {
        char perf[512];
        if (!Perf_Text(perf, sizeof(perf))) return std::string();
        char* nl = strchr(perf, '\n'); if (nl) *nl = 0;   // first line: fps and frame time
        return std::string(perf);
    };
    s.input = MenuInput();
    if (!vrkit::menu::Init(dev, imm, s)) LOGE("Settings menu: init failed (menu unavailable)");
    else LOGI("Settings menu ready: hold %s %d ms (%s)%s", g_cfg.menuChord == "l3r3" ? "L3+R3" : g_cfg.menuChord == "ltrt" ? "LT+RT" : "(no pad chord)", g_cfg.menuHoldMs, g_cfg.menuChordPass ? "game also sees it" : "hidden from the game", g_cfg.menuKey ? " or press menu_key" : "");
}
