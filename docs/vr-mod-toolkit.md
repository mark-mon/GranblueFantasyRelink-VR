# VR mod toolkit: reusable parts from GranblueVR (D3D11 + OpenXR)

How the game-independent parts of the Granblue Fantasy Relink VR mod work, what each one needs from a game, and the pitfalls hit along the way. Source: `src/` (files named below). The plan is to move these into a shared library (`vrkit/`) with a small per-game adapter; this doc is the spec for that. The first part already in `vrkit/` is the in-game settings menu: see `vrkit/README.md`.

## Architecture in one paragraph
The mod is a `dinput8.dll` proxy. It hooks DXGI Present/ResizeBuffers (dummy swapchain vtable), the game's D3D11 deferred contexts (dummy deferred context vtable) and a few game functions (render-frame, camera update, view update). The game renders the **left eye** normally with our camera. Its deferred command lists are recorded at `ExecuteCommandList` and **re-executed at Present** for every further view (right eye; with quad views also two insets), after swapping the per-view constant buffers. Each view's back buffer is copied into its OpenXR swapchain. One render-frame of CPU work feeds all views.

## Components and their game-specific inputs

| Component | Files | Generic | Game adapter must provide |
|---|---|---|---|
| OpenXR session, frame loop, swapchains, layers | vr.cpp | yes | back buffer, device |
| Single-pass replay (N views) | replay.cpp | mostly | which constant blocks hold the view (here: 2 x 0x220-byte blocks uploaded via a CB-ring function) and how to compute them for another camera |
| Per-view temporal history | temporal.cpp | yes | nothing (auto-detects "read before written" textures) |
| HUD separation | hud.cpp | pattern | how the UI pass is recognised (here: lone depthless bind of the post pass's 2nd target, decided at the first draw: indexed = UI) |
| Render resolution | vr.cpp + game | no | how to make the game render at a given size (here: preset table entry rewritten every frame) |
| VRS foveation | vrs.cpp | yes | render size; "scene pass" test (RT0 = render size and a DSV bound) |
| Quad views | vr.cpp, camera.cpp, replay.cpp | mostly | per-view camera/projection blocks (same as eye 2) |
| Eye gaze | vr.cpp (InitGaze/UpdateGaze) | yes | nothing |
| Shader hashing, pass capture, post-fx swap | passes.cpp, postfx.cpp | yes | hashes of the passes to replace |
| Perf stats panel | perf.cpp, vr.cpp | yes | nothing |
| **In-game settings menu (vrkit)** | vrkit/*, src/menu_options.cpp | **yes, library** | option list with pointers into the config struct + live-apply callbacks; one OpenXR quad layer |

## VRS (NVIDIA, D3D11) - vrs.cpp
- NVAPI without the SDK lib: `LoadLibrary("nvapi64.dll")`, `nvapi_QueryInterface(id)`. IDs: Initialize 0x0150e828, D3D11_CreateShadingRateResourceView 0x99ca2dff, D3D11_RSSetShadingRateResourceView 0x1b0c2f83, D3D11_RSSetViewportsPixelShadingRates 0x34f7938f. Structs are small; declare them yourself (version = sizeof | ver<<16).
- Shading-rate image: R8_UINT, one texel per 16x16 tile, values index a 16-entry per-viewport table (we use 0 = 1x1, 1 = mid, 2 = outer, 3 = hidden-by-inset).
- **Works on deferred contexts** and is recorded into command lists, so command-list replays inherit it. Bind in the deferred OMSetRenderTargets hook: on when RT0 has the render size and a DSV is bound (G-buffer, forward, sky), off otherwise (post-processing, shadow maps, half-res passes).
- Changing the pattern between views: `UpdateSubresource` on the immediate context between executions (the lists reference the texture, not its contents). Only upload when the pattern key changes (view kind, centre tile, inset rect).
- Gains depend on how much of the frame is pixel-shader bound. Compute-shader lighting is not affected. Measured: 11% on a 3080 Ti at 1856x2056, about 35% fps (33 -> 45) at 5500x4800 per eye on a 5090.

## Quad views - vr.cpp / camera.cpp / replay.cpp
- Views 0/1 = wide (both eyes), 2/3 = insets. All views render at the game's render size (the replay reuses the game's targets), so an inset of size f (fraction of the wide view in tangent space) has 1/f the pixel density.
- Inset frustum: tangents `centre +- f * halfSpan`, centre = gaze (or offset), clamped inside the wide view. Off-centre projection needs a correct off-axis projection in the camera blocks (FixProjection writes P[8]/P[9]).
- Each replayed view needs its own previous-frame camera slots (motion vectors) and its own temporal-history copies.
- Submission: second XrCompositionLayerProjection with the inset FOVs and `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT`; the inset is blitted with a premultiplied feathered alpha edge. If a runtime ignores a second projection layer, fall back to compositing into one high-resolution projection image.
- With VRS, shade the wide view's area under the inset (minus the feather) at 4x4.
- Cost: every view repeats the game's resolution-independent GPU work (shadows, culling compute). Pays off only with a clearly lower render scale.
- **Outcome in GranblueVR (1.0): shelved.** Each extra view cost about 8 ms, and the inset juddered under SteamVR reprojection. Eye-tracked VRS gave more for free (about 25-35% fps, hardly visible). Quad views stay in the code but are hidden: not in the menu or config template, off by default, `quad_key` unbound. For a new game, try VRS + gaze first. Consider quad views only if the game's resolution-independent work is small, or it can be skipped in the extra views.

## Eye gaze - vr.cpp
- Enable `XR_EXT_eye_gaze_interaction` if listed; check `XrSystemEyeGazeInteractionPropertiesEXT.supportsEyeGazeInteraction`.
- Action set + pose action bound to `/interaction_profiles/ext/eye_gaze_interaction` : `/user/eyes_ext/input/gaze_ext/pose`, attach, action space. Per frame: `xrSyncActions`, `xrLocateSpace(gaze, VIEW space, predictedDisplayTime)`, forward = q * (0,0,-1), tangents = x/-z, y/-z. Light smoothing.
- `debug_fake_gaze` (circling gaze) tests everything without a tracker.
- Verified working on a Pimax Dream Air through SteamVR (gaze-following VRS). Earlier "no gaze" reports were a headset-side issue.

## Shader identification and post-fx swaps - passes.cpp / postfx.cpp
- Hook CreatePixelShader/CreateComputeShader on the shared device vtable **at DLL load** (dummy device), FNV-1a hash of the bytecode. Hashes are stable across machines (it's game data).
- Pass capture (F10): one frame of deferred draws/dispatches with shader hash and bound SRV/RTV/UAV/DSV resources + `D3DDisassemble` of each shader. Reflection names usually survive (`g_TextureSceneColorHDR`, cbuffer names) and identify passes quickly.
- Replace a pass: swap the pixel shader in the deferred PSSetShader hook for a runtime-compiled (D3DCompile) pass-through with the same input/output signature. TAA off = output the current colour to both outputs, and also remove the camera jitter.
- `debug_skip_ps = hash,...` drops draws by shader (quick "what is this pass" experiments).

## In-game settings menu - vrkit/ (library)
- Hold L3+R3 (both stick clicks, 300 ms; LT+RT optional) or press Insert. The pages are built from an option registry: pointers into the mod's config struct, range/step, help text and an `onChange` that applies the value live. Changes are saved back into config.txt on close, with comments and order kept.
- Dear ImGui draws into an R8G8B8A8_TYPELESS texture (premultiplied) inside a private `ID3DDeviceContextState`. The host copies it into an OpenXR swapchain and submits a head-locked quad layer, last in the list.
- The game's XInputGetState/Ex is hooked so the game sees a neutral pad while the menu is open, and until all buttons are released.
- Per-game work is a single adapter file (GranblueVR: `src/menu_options.cpp`, about 120 lines). Full integration checklist: `vrkit/README.md`.

## Lessons / pitfalls (save time next game)
- **Never GPU-write the game's dynamic constant ring** (CopySubresourceRegion into a MAP_WRITE_NO_OVERWRITE ring caused TDRs and a 0x10E bugcheck). Redirect binds to your own DEFAULT buffers instead.
- The immediate context's vtable can be swapped after startup; re-hook from the live vtable periodically.
- Use a symmetric render frustum covering both eyes: screen-space content (HUD) then converges, and some passes that bake the projection offset stop mattering.
- Game culling may use a FOV other than the render projection. Measure **draws per frame** while toggling candidate fixes; it's an objective culling metric. Here the camera update copied its 50 deg FOV into the view every update; culling during the update used that, so things popped at the edges.
- Things recorded on parallel deferred contexts have no reliable order (UI vs post pass). Decide lazily (at the first draw) instead of by bind order.
- Detect temporal resources automatically: "read before written in the frame" over a 60-frame window, with "written by the lists at some point" (ping-pong pairs).
- Render-size changes recreate the game's targets: forget cached RTV/texture pointers (HUD, temporal) on resize.
- Desktop vsync of the mirror swapchain can cap VR at the monitor rate; present the mirror with sync interval 0 while VR runs.
- Test PC automation: an agent script on the test machine runs command files from an inbox (install DLL, set config, launch, press keys, trigger dumps, read the log). Eye/HUD BMP dumps + log lines verify almost everything without a headset (SteamVR null driver).
