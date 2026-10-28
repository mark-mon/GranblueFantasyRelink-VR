#include "config.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <algorithm>

static const char* kDefaultConfig =
"# Granblue Fantasy Relink VR - configuration\n"
"# Lines starting with # are comments. Format: key = value\n"
"# Delete this file to regenerate it with defaults.\n"
"\n"
"# Logging written to GranblueVR.log next to this file.\n"
"# none | error | info | debug\n"
"log_level = info\n"
"\n"
"# Set to false to turn the VR mod off without uninstalling it.\n"
"vr_enabled = true\n"
"\n"
"# VR mode: stereo = native stereo (the scene is rendered once per eye every frame, with head tracking)\n"
"#          flat   = the game on a virtual screen\n"
"vr_mode = stereo\n"
"\n"
"# Flat mode: virtual screen distance and width, in metres.\n"
"screen_distance = 2.5\n"
"screen_width = 3.0\n"
"\n"
"# Key that recenters the view (F1-F12).\n"
"recenter_key = F8\n"
"\n"
"# Head tracking: move the game camera with the headset (6DoF).\n"
"head_tracking = true\n"
"# decoupled_pitch: keep the horizon level. The right stick still moves the camera up and down around your\n"
"# character, but the view no longer tilts with it; look up and down with your head instead.\n"
"decoupled_pitch = false\n"
"# Game units per metre of head movement.\n"
"world_scale = 1.0\n"
"# Render each eye with one symmetric field of view covering both eyes (true), or with the\n"
"# headset's exact per-eye field of view (false). true keeps the 2D HUD/menus converged.\n"
"symmetric_fov = true\n"
"# Extra field of view (degrees, each side) rendered beyond what the headset shows, so the game's\n"
"# frame edges - where textures/objects can pop in and out - stay outside your view. Costs pixels\n"
"# (about 6% per axis per 2 degrees on a wide headset).\n"
"fov_overscan = 0\n"
"# Stop the game from culling props/NPCs with its own (narrower) camera FOV, which made objects pop in\n"
"# and out near the edges of the view as you turn your head.\n"
"fix_cull_fov = true\n"
"# edge_clipping: how wide the game's own camera looks when it decides what to load and draw. Wider stops\n"
"# textures and objects at the sides of your view from dropping out while you turn your head.\n"
"#   off = the headset's field of view   default = very wide   experimental = beyond the camera's normal\n"
"#   range (best result in testing, but may have unexpected effects). Needs fix_cull_fov = true.\n"
"edge_clipping = default\n"
"\n"
"# HUD: the game's 2D HUD and menus are shown on their own panel that follows your head.\n"
"hud_layer = true\n"
"# HUD panel distance and width, in metres.\n"
"hud_distance = 1.5\n"
"hud_size = 1.6\n"
"# hud_follow_head: true = the HUD panel moves with your head; false = it stays fixed in space straight ahead\n"
"# (in front of where you face after recentering), so you can look around it.\n"
"hud_follow_head = true\n"
"# hud_blur_backdrop: the game's frosted-glass panel backgrounds (a blurred copy of the screen). They show a\n"
"# misplaced piece of the scene on the VR HUD, so they are off by default.\n"
"hud_blur_backdrop = false\n"
"\n"
"# Render resolution: match the headset (the game renders each eye at the runtime's recommended\n"
"# per-eye size, widened to the field of view rendered). false = keep the game's own resolution.\n"
"match_headset_resolution = true\n"
"# Per-eye resolution scale (1.0 = runtime recommended pixel density). Lower for more performance.\n"
"render_scale = 1.0\n"
"# Largest render width/height allowed, in pixels.\n"
"render_max = 8192\n"
"# Keys to lower/raise render_scale while playing (steps of 0.05; the value is saved here),\n"
"# and to show/hide the performance stats panel.\n"
"scale_down_key = F5\n"
"scale_up_key = F6\n"
"stats_key = F7\n"
"show_stats = false\n"
"# Keep separate frame-to-frame history (anti-aliasing, exposure...) for each eye.\n"
"per_eye_history = true\n"
"# Let the game's desktop window wait for the monitor's refresh (vsync) while VR runs. false lets\n"
"# the headset set the frame rate (otherwise a 60 Hz monitor caps the game at 60 fps).\n"
"desktop_vsync = false\n"
"\n"
"# Foveated rendering (NVIDIA RTX/GTX 16xx and newer): shade the centre of each eye at full rate and the\n"
"# periphery coarser. vrs_inner / vrs_outer: radii as a fraction of half the image width (the overscan\n"
"# border is included in the image). Middle ring and outside rates: 1x1, 2x1, 1x2, 2x2, 4x2, 2x4, 4x4.\n"
"# vrs_center_x / _y move the full-rate centre (-1..1 of half the image; +x right, +y up).\n"
"# vrs_key toggles it while playing (compare the GPU times on the stats panel, F7).\n"
"vrs = true\n"
"vrs_inner = 0.35\n"
"vrs_outer = 0.70\n"
"vrs_mid_rate = 2x2\n"
"vrs_outer_rate = 4x4\n"
"vrs_center_x = 0\n"
"vrs_center_y = 0\n"
"vrs_key = F4\n"
"# Move the full-rate centre to where you look (needs eye tracking; see below).\n"
"vrs_follow_gaze = true\n"
"\n"
"# Eye tracking through OpenXR (XR_EXT_eye_gaze_interaction). Master switch for vrs_follow_gaze;\n"
"# eye_tracking_key toggles it while playing. The log reports gaze samples every\n"
"# 10 seconds (\"Eye tracking: ...\") and the stats panel shows gaze / no gaze.\n"
"eye_tracking = true\n"
"eye_tracking_key = F2\n"
"\n"
"# In-game settings menu: hold menu_chord for menu_hold_ms, or press menu_key. menu_chord:\n"
"#   l3r3 = click both sticks in (default)   ltrt = both triggers   none = keyboard only\n"
"# menu_chord_pass: true = the game still receives the chord (L3 + R3 is the game's party skill), so a\n"
"#   short press works in the game and only holding it for menu_hold_ms opens the menu; false = the chord is\n"
"#   hidden from the game while held.\n"
"# menu_key: insert, home, end, pause, scrolllock, F1-F12 or none.\n"
"# In the menu: D-pad / left stick to move and change,\n"
"# A = toggle, Y = undo (value at start), LB / RB = page, B = close. Keyboard: arrows, Enter, Delete, PgUp / PgDn,\n"
"# Backspace. Changes apply right away; this file is updated when the menu closes.\n"
"# menu_anchor: world = the panel stays fixed where it opened (turn your head to read its edges),\n"
"#   head = it follows your head. menu_distance / menu_size / menu_height: distance, width and height\n"
"#   offset from eye level, in metres.\n"
"menu_chord = l3r3\n"
"menu_chord_pass = true\n"
"menu_hold_ms = 500\n"
"menu_key = insert\n"
"menu_anchor = world\n"
"menu_distance = 1.0\n"
"menu_size = 0.8\n"
"menu_height = -0.05\n"
"\n"
"# Anti-aliasing (replaces the game's TAA pass). aa_mode:\n"
"#   game = the game's own TAA (soft)      off  = none (sharpest, edges shimmer)\n"
"#   fxaa = single-frame edge smoothing    taa  = mild TAA, tuned by taa_strength / taa_jitter\n"
"# taa_strength: how much of previous frames is blended in (0-1; the game's TAA is ~0.9).\n"
"# taa_jitter: camera jitter amount for mild TAA (0-1; more = smoother edges, more softness).\n"
"# aa_key cycles the modes while playing (shown on the stats panel). If aa_mode is not set,\n"
"# disable_taa = true means aa_mode = off.\n"
"aa_mode = off\n"
"taa_strength = 0.4\n"
"taa_jitter = 0.5\n"
"aa_key = F1\n"
"\n"
"# Post-processing.\n"
"# disable_taa: older switch, see aa_mode.\n"
"# disable_color_grading: turn off the game's depth-based colour grading (flatter, lighter image).\n"
"# Motion blur: use the game's own option (System > Game Options > Graphics > Blur).\n"
"disable_taa = false\n"
"disable_color_grading = false\n"
"# Key that saves what each eye sees: GranblueVR_eye0.bmp, GranblueVR_eye1.bmp, GranblueVR_hud.bmp.\n"
"screenshot_key = F11\n"
"# Diagnostics: key that writes the post-processing passes of the next frame to\n"
"# GranblueVR_passes.txt and GranblueVR_shaders\\ (press it while depth of field is visible).\n"
"capture_key = F10\n";

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
static std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    return s;
}

static bool ParseFloat(const std::string& v, float& out, float lo, float hi) {
    char* end = nullptr;
    float f = strtof(v.c_str(), &end);
    if (end == v.c_str() || *end != 0 || f < lo || f > hi) return false;
    out = f;
    return true;
}

bool LoadConfig(const std::wstring& path, Config& cfg) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        FILE* w = _wfopen(path.c_str(), L"wb");
        if (w) { fputs(kDefaultConfig, w); fclose(w); cfg.createdDefault = true; }
        return true;
    }
    char line[1024];
    int lineNo = 0;
    while (fgets(line, sizeof(line), f)) {
        ++lineNo;
        std::string s = line;
        size_t c = s.find_first_of("#;");
        if (c != std::string::npos) s = s.substr(0, c);
        s = Trim(s);
        if (s.empty()) continue;
        size_t eq = s.find('=');
        char buf[256];
        if (eq == std::string::npos) {
            snprintf(buf, sizeof(buf), "config line %d: missing '=' (ignored)", lineNo);
            cfg.messages.push_back(buf);
            continue;
        }
        std::string key = Lower(Trim(s.substr(0, eq)));
        std::string val = Trim(s.substr(eq + 1));
        std::string lval = Lower(val);
        bool ok = true;
        if (key == "log_level") {
            if (lval == "none") cfg.logLevel = LogLevel::None;
            else if (lval == "error") cfg.logLevel = LogLevel::Error;
            else if (lval == "info") cfg.logLevel = LogLevel::Info;
            else if (lval == "debug") cfg.logLevel = LogLevel::Debug;
            else ok = false;
        } else if (key == "vr_enabled") {
            if (lval == "true" || lval == "1" || lval == "on" || lval == "yes") cfg.vrEnabled = true;
            else if (lval == "false" || lval == "0" || lval == "off" || lval == "no") cfg.vrEnabled = false;
            else ok = false;
        } else if (key == "head_tracking") {
            if (lval == "true" || lval == "1" || lval == "on" || lval == "yes") cfg.headTracking = true;
            else if (lval == "false" || lval == "0" || lval == "off" || lval == "no") cfg.headTracking = false;
            else ok = false;
        } else if (key == "world_scale") ok = ParseFloat(val, cfg.worldScale, 0.01f, 100.f);
        else if (key == "debug_head_yaw") ok = ParseFloat(val, cfg.debugHeadYaw, -180.f, 180.f);
        else if (key == "debug_fake_head") ok = ParseFloat(val, cfg.debugFakeHead, 0.f, 90.f);
        else if (key == "renderdoc_path") cfg.renderdocPath = val;
        else if (key == "debug_noxr") cfg.debugNoXr = (lval == "1" || lval == "true");
        else if (key == "render_size") { int a = 0, b = 0; if (sscanf(val.c_str(), "%dx%d", &a, &b) == 2 && a >= 0 && b >= 0 && a <= 8192 && b <= 8192) { cfg.forceW = a; cfg.forceH = b; } else ok = false; }
        else if (key == "cull_margin") ok = ParseFloat(val, cfg.cullMarginDeg, -30.f, 30.f);
        else if (key == "hud_layer") { if (lval == "true" || lval == "1") cfg.hudLayer = true; else if (lval == "false" || lval == "0") cfg.hudLayer = false; else ok = false; }
        else if (key == "symmetric_fov") { if (lval == "true" || lval == "1") cfg.symmetricFov = true; else if (lval == "false" || lval == "0") cfg.symmetricFov = false; else ok = false; }
        else if (key == "debug_fov_skew") ok = ParseFloat(val, cfg.debugFovSkew, -40.f, 40.f);
        else if (key == "debug_fov_vskew") ok = ParseFloat(val, cfg.debugFovVSkew, -40.f, 40.f);
        else if (key == "debug_stereo") { float f = 0; ok = ParseFloat(val, f, 0.f, 16777215.f); cfg.debugStereo = (int)f; }
        else if (key == "vr_mode") {
            if (lval == "flat" || lval == "stereo") cfg.vrMode = lval;
            else if (lval == "aer") cfg.vrMode = "stereo";
            else ok = false;
        } else if (key == "screen_distance") ok = ParseFloat(val, cfg.screenDistance, 0.3f, 50.f);
        else if (key == "screen_width") ok = ParseFloat(val, cfg.screenWidth, 0.2f, 50.f);
        else if (key == "match_headset_resolution") { if (lval == "true" || lval == "1") cfg.matchHeadsetRes = true; else if (lval == "false" || lval == "0") cfg.matchHeadsetRes = false; else ok = false; }
        else if (key == "per_eye_history") { if (lval == "true" || lval == "1") cfg.perEyeHistory = true; else if (lval == "false" || lval == "0") cfg.perEyeHistory = false; else ok = false; }
        else if (key == "render_max") { float f = 0; ok = ParseFloat(val, f, 512.f, 8192.f); if (ok) cfg.renderMax = (int)f; }
        else if (key == "vrs") { if (lval == "true" || lval == "1") cfg.vrs = true; else if (lval == "false" || lval == "0") cfg.vrs = false; else ok = false; }
        else if (key == "vrs_inner") ok = ParseFloat(val, cfg.vrsInner, 0.f, 3.f);
        else if (key == "vrs_outer") ok = ParseFloat(val, cfg.vrsOuter, 0.f, 3.f);
        else if (key == "vrs_center_x") ok = ParseFloat(val, cfg.vrsCenterX, -1.f, 1.f);
        else if (key == "vrs_center_y") ok = ParseFloat(val, cfg.vrsCenterY, -1.f, 1.f);
        else if (key == "vrs_mid_rate" || key == "vrs_outer_rate") {
            if (lval == "1x1" || lval == "2x1" || lval == "1x2" || lval == "2x2" || lval == "4x2" || lval == "2x4" || lval == "4x4") (key == "vrs_mid_rate" ? cfg.vrsMidRate : cfg.vrsOuterRate) = lval; else ok = false;
        }
        else if (key == "edge_clipping") { if (lval == "off" || lval == "default" || lval == "experimental") cfg.edgeClipping = lval; else ok = false; }
        else if (key == "decoupled_pitch") { if (lval == "true" || lval == "1") cfg.decoupledPitch = true; else if (lval == "false" || lval == "0") cfg.decoupledPitch = false; else ok = false; }
        else if (key == "fix_cull_fov") { if (lval == "true" || lval == "1") cfg.fixCullFov = true; else if (lval == "false" || lval == "0") cfg.fixCullFov = false; else ok = false; }
        else if (key == "eye_tracking") { if (lval == "true" || lval == "1") cfg.eyeTracking = true; else if (lval == "false" || lval == "0") cfg.eyeTracking = false; else ok = false; }
        else if (key == "vrs_follow_gaze") { if (lval == "true" || lval == "1") cfg.vrsFollowGaze = true; else if (lval == "false" || lval == "0") cfg.vrsFollowGaze = false; else ok = false; }
        else if (key == "quad_views") { if (lval == "true" || lval == "1") cfg.quadViews = true; else if (lval == "false" || lval == "0") cfg.quadViews = false; else ok = false; }
        else if (key == "quad_follow_gaze") { if (lval == "true" || lval == "1") cfg.quadFollowGaze = true; else if (lval == "false" || lval == "0") cfg.quadFollowGaze = false; else ok = false; }
        else if (key == "quad_inner_size") ok = ParseFloat(val, cfg.quadInnerSize, 0.1f, 0.9f);
        else if (key == "quad_feather") ok = ParseFloat(val, cfg.quadFeather, 0.f, 0.5f);
        else if (key == "quad_center_x") ok = ParseFloat(val, cfg.quadCenterX, -1.f, 1.f);
        else if (key == "quad_center_y") ok = ParseFloat(val, cfg.quadCenterY, -1.f, 1.f);
        else if (key == "quad_vrs_hidden_rate") {
            if (lval == "1x1" || lval == "2x1" || lval == "1x2" || lval == "2x2" || lval == "4x2" || lval == "2x4" || lval == "4x4") cfg.quadVrsHiddenRate = lval; else ok = false;
        }
        else if (key == "debug_fake_gaze") ok = ParseFloat(val, cfg.debugFakeGaze, 0.f, 40.f);
        else if (key == "fov_overscan") ok = ParseFloat(val, cfg.fovOverscan, 0.f, 15.f);
        else if (key == "render_scale") ok = ParseFloat(val, cfg.renderScale, 0.25f, 3.f);
        else if (key == "hud_distance") ok = ParseFloat(val, cfg.hudDistance, 0.3f, 20.f);
        else if (key == "hud_follow_head") { if (lval == "true" || lval == "1") cfg.hudFollowHead = true; else if (lval == "false" || lval == "0") cfg.hudFollowHead = false; else ok = false; }
        else if (key == "hud_blur_backdrop") { if (lval == "true" || lval == "1") cfg.hudBlurBackdrop = true; else if (lval == "false" || lval == "0") cfg.hudBlurBackdrop = false; else ok = false; }
        else if (key == "hud_size") ok = ParseFloat(val, cfg.hudSize, 0.1f, 20.f);
        else if (key == "scale_down_key" || key == "scale_up_key" || key == "stats_key" || key == "capture_key" || key == "screenshot_key" || key == "vrs_key" || key == "quad_key" || key == "eye_tracking_key" || key == "aa_key") {
            int n = 0, vk = 0;
            if (lval == "none" || lval == "off") vk = 0;
            else if (lval.size() >= 2 && lval[0] == 'f' && (n = atoi(lval.c_str() + 1)) >= 1 && n <= 12) vk = VK_F1 + n - 1;
            else ok = false;
            if (ok) (key == "scale_down_key" ? cfg.scaleDownKey : key == "scale_up_key" ? cfg.scaleUpKey : key == "stats_key" ? cfg.statsKey : key == "screenshot_key" ? cfg.screenshotKey : key == "vrs_key" ? cfg.vrsKey : key == "quad_key" ? cfg.quadKey : key == "eye_tracking_key" ? cfg.eyeTrackingKey : key == "aa_key" ? cfg.aaKey : cfg.captureKey) = vk;
        }
        else if (key == "desktop_vsync") { if (lval == "true" || lval == "1") cfg.desktopVsync = true; else if (lval == "false" || lval == "0") cfg.desktopVsync = false; else ok = false; }
        else if (key == "debug_dump_shaders") { if (lval == "true" || lval == "1") cfg.debugDumpShaders = true; else if (lval == "false" || lval == "0") cfg.debugDumpShaders = false; else ok = false; }
        else if (key == "debug_skip_ps") cfg.debugSkipPs = val;
        else if (key == "menu_chord") { if (lval == "l3r3" || lval == "ltrt" || lval == "none") cfg.menuChord = lval; else ok = false; }
        else if (key == "menu_chord_pass") { if (lval == "true" || lval == "1") cfg.menuChordPass = true; else if (lval == "false" || lval == "0") cfg.menuChordPass = false; else ok = false; }
        else if (key == "menu_hold_ms") { int v = atoi(val.c_str()); ok = v >= 100 && v <= 5000; if (ok) cfg.menuHoldMs = v; }
        else if (key == "menu_distance") ok = ParseFloat(val, cfg.menuDistance, 0.3f, 5.f);
        else if (key == "menu_size") ok = ParseFloat(val, cfg.menuSize, 0.2f, 5.f);
        else if (key == "menu_height") ok = ParseFloat(val, cfg.menuHeight, -1.f, 1.f);
        else if (key == "menu_anchor") { if (lval == "world" || lval == "head") cfg.menuAnchor = lval; else ok = false; }
        else if (key == "menu_key") {
            int n = 0;
            if (lval == "none" || lval == "off") cfg.menuKey = 0;
            else if (lval == "insert" || lval == "ins") cfg.menuKey = VK_INSERT;
            else if (lval == "home") cfg.menuKey = VK_HOME;
            else if (lval == "end") cfg.menuKey = VK_END;
            else if (lval == "pause") cfg.menuKey = VK_PAUSE;
            else if (lval == "scrolllock") cfg.menuKey = VK_SCROLL;
            else if (lval.size() >= 2 && lval[0] == 'f' && (n = atoi(lval.c_str() + 1)) >= 1 && n <= 12) cfg.menuKey = VK_F1 + n - 1;
            else ok = false;
        }
        else if (key == "aa_mode") { if (lval == "game" || lval == "off" || lval == "fxaa" || lval == "taa") cfg.aaMode = lval; else ok = false; }
        else if (key == "taa_strength") ok = ParseFloat(val, cfg.taaStrength, 0.f, 1.f);
        else if (key == "taa_jitter") ok = ParseFloat(val, cfg.taaJitter, 0.f, 1.f);
        else if (key == "disable_taa") { if (lval == "true" || lval == "1") cfg.disableTaa = true; else if (lval == "false" || lval == "0") cfg.disableTaa = false; else ok = false; }
        else if (key == "disable_fog" || key == "disable_color_grading") { if (lval == "true" || lval == "1") cfg.disableFog = true; else if (lval == "false" || lval == "0") cfg.disableFog = false; else ok = false; }
        else if (key == "show_stats") { if (lval == "true" || lval == "1") cfg.showStats = true; else if (lval == "false" || lval == "0") cfg.showStats = false; else ok = false; }
        else if (key == "recenter_key") {
            int n = 0;
            if (lval.size() >= 2 && lval[0] == 'f' && (n = atoi(lval.c_str() + 1)) >= 1 && n <= 12) cfg.recenterKey = VK_F1 + n - 1;
            else ok = false;
        } else {
            snprintf(buf, sizeof(buf), "config line %d: unknown key '%s' (ignored)", lineNo, key.c_str());
            cfg.messages.push_back(buf);
            continue;
        }
        if (!ok) {
            snprintf(buf, sizeof(buf), "config line %d: invalid value '%s' for '%s' (default kept)", lineNo, val.c_str(), key.c_str());
            cfg.messages.push_back(buf);
        }
    }
    fclose(f);
    return true;
}
