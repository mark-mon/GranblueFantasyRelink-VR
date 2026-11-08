// Game code / data addresses found at runtime by byte-pattern ("signature") scans, so a game update that only
// moves code around doesn't break the mod. See gameaddr.cpp.
#pragma once
#include <cstdint>

enum GameAddrId : int {
    // functions (hooked or called)
    GA_UpdateView,      // look-at view builder (head tracking)
    GA_RenderFrame,     // render-frame (stereo)
    GA_CamUpdate,       // game camera update (culling FOV fix)
    GA_RenderJob,       // render-worker job (null-payload guard)
    GA_CbUpload,        // constant-ring upload (stereo replay needs the main view's uploads)
    // globals
    GA_Views,           // pointer to the render-view array
    GA_ViewCount,       // int: number of views
    GA_CurView,         // int: current view (diagnostics)
    GA_Payload,         // current frame payload
    GA_Pending,         // pending payload (diagnostics)
    GA_FrameToken,      // per-frame value copied into the view constant block
    GA_ResIndex,        // index of the current resolution preset
    GA_ResTable,        // resolution preset table (6 x 0x4c)
    GA_Cutscene,        // int: non-zero while an in-engine cutscene plays (found by memory diff, 1.0.3)
    GA_Cutscene2,       // int: second cutscene flag (cross-check, diagnostics)
    GA_Count
};

// Scans for every address once (call before any game-code hook is installed). True if all required ones were found.
bool GameAddr_Resolve();
// Absolute address, or 0 if not found.
uintptr_t GA(GameAddrId id);
// True if every scanned address equals the build the mod was developed on: then the fixed addresses used by the
// legacy / debug code paths are valid too.
bool GameAddr_KnownBuild();
// debug_make_sigs: writes GranblueVR_sigs.txt with fresh patterns for every entry (needs the known build).
void GameAddr_MakeSigs();
