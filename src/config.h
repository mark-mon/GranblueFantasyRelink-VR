#pragma once
#include <string>
#include <vector>
#include "log.h"

struct Config {
    LogLevel logLevel = LogLevel::Info;
    bool vrEnabled = true;           // false = skip OpenXR entirely (diagnostics on a PC without a headset)
    std::string vrMode = "stereo";   // stereo (native, both eyes every frame) | flat
    float fovOverscan = 0.0f;
    bool fixCullFov = true;          // keep the game camera update from narrowing the view FOV (edge popping)
    std::string edgeClipping = "default";   // off (headset FOV) | default (game camera 165 deg) | experimental (200 deg)
    bool vrs = true;                 // NVIDIA variable rate shading (foveated)
    float vrsInner = 0.35f, vrsOuter = 0.70f;   // radii (fraction of half the image width)
    std::string vrsMidRate = "2x2", vrsOuterRate = "4x4";
    float vrsCenterX = 0.f, vrsCenterY = 0.f;   // centre offset (fraction of half size, +x right, +y up)
    int vrsKey = 0x73;               // VK_F4: toggle VRS
    bool vrsFollowGaze = true;       // move the VRS centre with eye tracking
    bool quadViews = false;          // quad views: wide view + high-density inset per eye
    float quadInnerSize = 0.4f;      // inset size, fraction of the wide view's width/height (in tangent space)
    float quadFeather = 0.08f;       // inset edge blend width, fraction of the inset
    float quadCenterX = 0.f, quadCenterY = 0.f;   // inset centre offset, fraction of the half view (+x right, +y up)
    bool quadFollowGaze = true;      // move the inset with eye tracking
    std::string quadVrsHiddenRate = "4x4";   // VRS rate of the wide view behind the inset
    int quadKey = 0;                 // hidden (quad views are experimental): toggle key, e.g. quad_key = F3
    float debugFakeGaze = 0.f;
    bool eyeTracking = true;         // use eye tracking (VRS centre / quad inset follow the gaze)
    int eyeTrackingKey = 0x71;       // VK_F2: toggle eye tracking       // debug: simulated gaze circling this many degrees        // degrees rendered beyond the headset FOV on every side
    float renderScale = 1.0f;        // per-eye render resolution scale
    bool matchHeadsetRes = true;     // render at the headset's per-eye size
    int renderMax = 8192;
    int scaleDownKey = 0x74, scaleUpKey = 0x75, statsKey = 0x76;   // VK_F5, VK_F6, VK_F7
    bool showStats = false;
    // in-game settings menu (vrkit): hold the menu_chord buttons menu_hold_ms or press menu_key
    std::string menuChord = "l3r3";  // l3r3 (both stick clicks) | ltrt (both triggers) | none
    int menuHoldMs = 500;
    bool menuChordPass = true;       // the game still sees the chord until the hold time (Granblue: L3+R3 = party skill)
    int menuKey = 0x2D;              // VK_INSERT
    float menuDistance = 1.0f;       // metres in front of the eyes
    float menuSize = 0.8f;           // panel width in metres
    float menuHeight = -0.05f;       // metres up (+) / down (-) from eye level
    std::string menuAnchor = "world"; // world (fixed where it opened) | head (follows the headset)
    int screenshotKey = 0x7A;        // VK_F11: save both eye images
    int captureKey = 0x79;           // VK_F10: write GranblueVR_passes.txt + GranblueVR_shaders (diagnostics)
    bool desktopVsync = false;
    bool disableTaa = false, disableFog = false;
    std::string aaMode;               // game | off | fxaa | taa (empty: from disable_taa)
    float taaStrength = 0.4f, taaJitter = 0.5f;   // mild TAA: history weight (0-1), jitter amount (0-1)
    int aaKey = 0x70;                 // VK_F1: cycle anti-aliasing modes
    std::string debugSkipPs;          // debug: pixel shader hashes whose draws are dropped   // post-processing overrides       // let the game wait for the monitor refresh while VR runs            // cap for render width/height
    bool perEyeHistory = true;       // per-eye temporal resources
    float hudDistance = 1.5f;        // reserved: metres from the eyes
    float hudSize = 1.6f;            // reserved: HUD panel width in metres
    bool hudBlurBackdrop = false;    // keep the game's frosted-glass HUD panel backdrops (show a misplaced copy of the scene in VR)
    float screenDistance = 2.5f;     // flat mode: virtual screen distance (m)
    float screenWidth = 3.0f;        // flat mode: virtual screen width (m)
    int recenterKey = 0x77;          // VK_F8
    bool headTracking = true;        // apply HMD pose to the game camera
    bool decoupledPitch = false;     // level horizon: drop the game camera's pitch / roll (look up / down with your head)
    float worldScale = 1.0f;         // game units per metre
    float debugFakeHead = 0.0f;
    float debugHeadYaw = 0.0f;     // debug: fixed head yaw (degrees)
    bool symmetricFov = true;
    bool hudLayer = true;
    int forceW = 0, forceH = 0;
    float cullMarginDeg = 6.f;
    float debugFovSkew = 0.f, debugFovVSkew = 0.f;
    int debugStereo = 8192;   // single render-frame + command-list replay of eye 1 at Present
    std::string renderdocPath;
    bool debugDumpShaders = false;   // hidden: disassemble every pixel/compute shader the game creates into GranblueVR_allshaders\\ (+ index.txt)
    bool debugNoXr = false;          // hidden: stereo rendering without an OpenXR session (diagnostics/captures)       // hidden: load this renderdoc.dll at startup (frame captures via trigger "rdcapture")             // hidden: 1 no per-eye camera, 2 pose only (no projection), 3 no history restore      // >0: fake head sway amplitude in degrees (testing without a headset)
    std::vector<std::string> messages;  // parse warnings, logged after the log opens
    bool createdDefault = false;
};

bool LoadConfig(const std::wstring& path, Config& cfg);
