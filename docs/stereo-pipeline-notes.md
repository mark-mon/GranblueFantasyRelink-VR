
## Frame pipeline (found during native-stereo work, 0.4.x)
Per main-loop iteration (main thread unless noted):
1. Main loop swaps pending payload `0x703ebd0` → current `0x703ef78` (holder at `0x703ef60`: [functor vtbl, storage, +0x10, payload]); if none pending, current is released/nulled.
2. Frame worker thread `0x7d5dd0` is triggered (flag `0x703f418`, set at `0x1b2874`); it runs payload "stages" (consume payload contents — not idempotent), then serves render-frame's task requests (request `0x703f41a`, ack `0x703f419`, task list `0x703efc0..c8`), then returns to its top.
3. `0x1c87d0` (per-frame setup) calls the per-object list producer `0x7fff90` → `0x23fe2c0` (once per game frame, see below) and posts the *retire job* (vtable `0x54bb4a8`, invoke `0x1cbc50`, capture engineDraw obj `0x701ca88`) to queue `0x701ca98` via `0x15ffb0`; it runs `0x22f9000(obj+8)` and releases **semFlush `0x701ce08` (= obj+0x380)**.
4. render-frame `0x1cbcd0`: begin-frame block at `0x1ccbd4` advances a 3-slot per-frame index (`0x71ae324`, prev `0x71ae310`) and resets per-frame pools; uploads per-view constant blocks (main view: 0x220-byte block A from elem+0x4c0 → descriptor elem+0x950, block B from elem+0x6e0 → elem+0x970; descriptor = {ID3D11Buffer*, byte offset, size, upload token}) via `0x7dd4b0` into the CB ring; advances the instance-ring frame slot (`0x1d4c99`, 5 slots); acquires semFlush at `0x1cfbf7`; posts render job `0x1dd4c0` and sync job `0x1da5e0`; handshake with frame worker at `0x1d2e80`; tail: deferred-free list, **sim kick** (`0x1d3691` → `0x703efa8`), payload holder pushed to engineDraw queue `0x703e9f8` via `0x7b61d0`.
5. Main loop: release semFrame `0x701cca8`, set engineDraw pending `0x701cf8b` + signal (`0x701cfe0`/`0x701cff0`), acquire semFrame.
6. **engineDraw thread** (`0x1a9650` loop → `0x1a97d0`, obj `0x701ca88`): flips its double-buffer index and uploads per-object list A (`0x718c908`, uint32, 4 MB SRV) and list B (`0x718c8f8`, 3x4 matrices, 12 MB SRV); flushes dirty ranges of the instance ring C (8 MB VB, slot 15; shadow at `*0x718c918`, dirty vector `0x718d350`, allocator `0x3a0c210`); waits on the frame-latency object (ret `0x1ac3f5`); executes the deferred command lists (ExecuteCommandList at `0x1acced`); waits for sim flag `0x703efa0`; returns the holder to free list `0x703eb78` (`0x1aca13`); releases `0x701cd58`.
7. Main loop waits `0x701cd58` (`0x1b2b15`), presents via `0x7e25e0(*(0x701d060))`.
- Sim thread `0x7b6300` builds the next payload; raises `0x703efa0`.
- Draw calls are recorded on ~6 deferred contexts by job threads; only engineDraw touches the immediate context.
- The immediate context keeps its vtable inline (vtable ptr = ctx+8) and the runtime swaps entries after startup: hooks installed at init on Map/Unmap/ExecuteCommandList never fire. Re-read the live vtable later (Discovery_Rehook, every 60 frames) and hook the new targets.

## Why eye 1 was corrupted (solved 2026-10-20)
Per-frame data is produced once per game frame, outside render-frame:
- Instance ring C: ~3.5k allocations per frame happen before render-frame; the second render-frame allocates nothing and its command lists reference instance offsets that were never uploaded (StartInstanceLocation 0..14k vs ~350k for eye 0).
- Lists A/B: the producer flips its index (`0x7396274` → `0x7ba4d24` A, `0x7ba4d20` B) once per game frame, but engineDraw flips its own index (`0x718c908`/`0x718c8f8`) every run, so with two engineDraw runs per frame each eye uploaded stale data every other frame.
- RenderDoc replays of these frames are useless (geometry explodes in both eyes even when the game rendered eye 0 correctly), because the persistent rings are updated outside the capture.

## Native stereo design (0.5.0, debug_stereo default 3104 = 32 | 1024 | 2048)
1. render-frame runs twice per main-loop iteration (eye 0, eye 1) with per-eye camera, using the 0.4.x patches (payload holder copy, single sim kick, handshake skip for eye 1, eye-0 submit via engineDraw emulation, latency-wait pass-through, sim flag for the second submit, per-frame index rewind, retire job after eye 0 = flag 32).
2. Flag 2048: before each engineDraw run set `0x718c908 = *0x7ba4d24 ^ 1` and `0x718c8f8 = *0x7ba4d20 ^ 1` so both runs upload the producer's current lists.
3. Flag 1024 (replay, `replay.cpp`): during eye 0's submit, the ExecuteCommandList hook records eye 0's 7 command lists (AddRef). During eye 1's submit, on the first call it GPU-copies eye 1's main-view blocks A/B (from eye 1's descriptors) over eye 0's (via a DEFAULT temp buffer, CopySubresourceRegion into the dynamic CB ring works), re-executes eye 0's lists, and drops eye 1's own lists.
- Result: both eyes correct with proper parallax (3/3 samples), ~32 fps on the 3080 Ti test PC.
- Known gaps: TAA history shared between eyes; culling from eye 0 only; eye 1's render-frame still records lists that are thrown away (CPU cost — could be replaced by computing blocks A/B directly); HUD in both eyes at screen depth.

## Convergence fix (0.5.1)
- Replay now copies every constant block render-frame uploads on the main thread (25 per frame, hook on `0x7dd4b0`, paired by order), not just the main view's A/B blocks.
- With asymmetric per-eye FOV (off-axis offsets in P[8]/P[9]), eye 1 came out shifted by eye 0's offset: some pass in eye 0's lists uses a projection offset that is not in the copied blocks (likely uploaded by a job thread, e.g. TAA). Tested on the null driver with `debug_fov_skew`/`debug_fov_vskew`: eye 0 correct, eye 1 horizontal offset cancelled and vertical offset doubled. This misaligned the eyes on the Pimax Dream Air.
- Fix: `symmetric_fov = true` (default) renders both eyes with one symmetric frustum covering both eyes' FOVs and submits that FOV, so no off-axis offset is needed. Far disparity is then 0 and near disparity consistent. This also keeps the screen-space HUD converged at infinity.

## 0.6.0: single-pass stereo + HUD layer (2026-10-21)
- **Crash root cause (0.5.x):** GPU faults, not game bugs. The game "crashes" (AVs in light/shadow job threads) coincided with nvlddmkm TDRs; one soak even took the PC down with bugcheck 0x10E (VIDEO_MEMORY_MANAGEMENT_INTERNAL). The 0.5.x replay used CopySubresourceRegion to write eye-1 blocks into the game's DYNAMIC constant ring (mapped NO_OVERWRITE by the CPU all the time) — drivers don't expect GPU writes into such buffers.
- **Single-pass (debug_stereo 8192, default):** render-frame runs once (eye 0 camera via SetupEye) with none of the two-pass patches active. Eye 1's main-view blocks A/B are computed by us: scratch copy of the view element → oUpdateView(&scratch) with the eye-1 pose → FixProjection → prev-frame fields from our eye-1 history → `BuildViewBlocks` (exact port of exe+0x1cdb86..0x1ce436; validated: 0 difference vs the game's blocks for eye 0). Only blocks A/B differ between eyes (checked all 25 per-frame uploads).
- **CB redirect:** hook on 0x7dd4b0 notes the ring offsets of the main view's blocks; deferred-context VS/PS/CS SetConstantBuffers1 hooks replace binds of (ring, firstConstant == block) with our own two DEFAULT 0x400-byte CBs. After render-frame we UpdateSubresource eye 0's blocks into them; at Present (main thread, engineDraw idle) we capture eye 0, UpdateSubresource eye 1's blocks, re-execute all of the frame's command lists (recorded in the ExecuteCommandList hook), capture eye 1. No writes to game resources from the GPU. 150 s + 330 s soaks with movement/attacks: no crash, no TDR.
- **Do not restore the view's aspect/fov (+0x9d0/+0x9d4) right after render-frame:** its jobs keep running afterwards. They are now kept while stereo runs and restored when it stops.
- **HUD layer:** the UI is drawn at the end of the post list: post pass binds [X, S] (no DSV), then the UI pass binds [S] alone (S = final LDR scene colour, R8G8B8A8) and draws ~60 alpha-blended quads. Deferred OMSetRenderTargets hook redirects that bind to our transparent texture (LDR formats only); OMSetBlendState during the UI pass is replaced by a variant accumulating alpha (SrcBlendAlpha ONE, DestBlendAlpha INV_SRC_ALPHA, alpha writes on) → premultiplied UI. At Present: blit UI (premultiplied, sRGB-aware) to a head-locked quad layer (view space, hud_distance/hud_size), replay, then clear the UI texture.
- Immediate-context hooks: the context's vtable is inline and its entries are swapped after startup; Discovery_Rehook re-reads it every 60 frames (ExecuteCommandList/Map hooks only fire after that).

## 0.7.0: headset resolution, per-eye history, culling (2026-10-21)
- **Render resolution:** the swap chain size alone only letterboxes (internal targets stay at the game's size). The game reloads its render/output size every frame (`0x6bbe70`, called from the main loop at `0x1b0048`) from the current entry of its resolution preset table `exe+0x6b83460` (6 × 0x4c, index `exe+0x7035980`; +0x24/+0x28 render W/H, +0x2c/+0x30 output W/H). Writing our size into the current entry each render-frame makes the game resize its own swap chain (ResizeBuffers from exe+0x7e26ea) and all render targets; the ResizeBuffers hook forces exactly our size (the game asks for e.g. 1856x1901, clamped to the desktop). Writing the derived globals `0x6b832d0..` directly, or the whole table + apply flag `0x7035984`, gave broken/mixed images or got reset.
- Size = runtime recommended per-eye size scaled to the symmetric FOV (density of eye 0's own FOV × symmetric tangent span) × render_scale, capped at render_max, rounded to 8. Null driver: 1852x2056 → 1856x2056. The game occasionally resizes back to 1920x1080 during loading; the table write puts it back next frame.
- The UI is laid out in a centred 16:9 band of the taller target; the HUD quad layer uses that band as its imageRect.
- **Per-eye history (temporal.cpp):** detection over 60 replayed frames, 120 frames after stereo starts and 90 after each resize. Deferred hooks record per-list texture accesses (SRV binds VS/PS/CS = read; RTV/DSV/UAV binds, clears, copy destinations = write), attached to the command list at FinishCommandList and ordered at ExecuteCommandList. A texture is history if it is read before being written in ≥40 % of frames and is written by the lists somewhere in the window (ping-pong pairs are written only every other frame). Found on the test PC: 2× 1x1 R32 (exposure), 2× R11G11B10 RT/UAV/SRV at render size (TAA colour history), 2× R32G8X24 DS/SRV (previous-frame depth), all ping-pong. Per frame: first ExecuteCommandList restores eye 0's copies; at Present save eye 0's results, restore eye 1's, replay, save eye 1's. Removes the TAA ghosting in eye 0 (A/B compared).
- **Culling:** widening +0x9d0/+0x9d4 before UpdateView (even by -25°) had no visible effect — draw culling follows the final view-projection, not these fields. Remaining gap: objects only in the 6 cm strip at the right eye's outer edge. cull_margin kept as a hidden debug key.
- 7 min soak at 1856x2056 per eye on the 3080 Ti: ~32 fps, no crash, no TDR.

## 0.8.0: live render scale, stats panel, mirror vsync, HUD fix (2026-10-22)
- **Render scale hotkeys:** F5/F6 change render_scale in 0.05 steps; size = base (recommended density × symmetric FOV) × scale, capped at render_max; the preset-table write makes the game resize; the value is written back into config.txt.
- **Stats panel:** GDI text → DEFAULT texture → small head-locked quad layer (F7). GPU time from D3D11 timestamp queries on the immediate context: eye 1 = first ExecuteCommandList of the frame → start of our Present work, eye 2 = the replay. "CPU main" = frame time − time blocked in xrWaitFrame. Test PC (3080 Ti, null driver, 1856x2056/eye): GPU ~8–11 ms per eye, CPU main ~8–10 ms; the null driver paces at ~32 fps.
- **Mirror vsync:** the game's Present sync interval is forced to 0 while the XR session runs (desktop_vsync = false), so a 60 Hz monitor can't cap VR at 60 fps.
- **HUD flicker (title screen, battles):** the post pass [X,S] and the UI binds [S] are sometimes recorded on *different* deferred contexts, in either order, so the per-context "UI follows the post pass" rule missed them and the UI went into the scene. The final colour target alternates between two RTVs. The game also binds S alone (no DSV) for its own full-screen scene passes, so "any lone bind of S" is wrong (tried: scene came out empty). Fix: a lone bind of a learned S is held pending and decided at the next draw on that context — DrawIndexed/DrawIndexedInstanced = UI (bind our UI target and the alpha-accumulating variant of the current blend state), Draw/DrawInstanced = scene pass (left alone). Post-pass targets are kept across FinishCommandList and reset at Present. Title screen: UI missing 10–30 frames/10 s → 0.
- **Culling (#1 from the user):** fixed head yaw of ±50° (debug_head_yaw) shows no missing geometry; the reported side-building shadow/texture changes are likely screen-space effects (SSAO/SSR/contact shadows) at the view edges or shadow-cascade fitting. Not yet resolved.

## 0.9.0: post-processing toggles (2026-10-22)
- **Shader identification:** CreatePixelShader/CreateComputeShader hooked on the shared device vtable at DLL load (dummy device in InstallD3DHooks); FNV-1a 64 of the bytecode. Trigger/F10 "passes" writes one frame's deferred draws/dispatches with shader hash + SRV/RTV/UAV/DSV resources (GranblueVR_passes.txt) and D3DDisassemble output per used shader (GranblueVR_shaders\). Reflection names are kept in the bytecode.
- **Town post chain (1856x2056):** 6c50 CS deferred lighting -> T51 (R11G11B10 HDR); 7325 PS "ColorCorrect" (near/far 3D LUT depth colour grading) T51 -> T92; VFX particles (VfxNewFog/VfxHeightFog shaders e308/a14d/04a4/4cfe) into T92; 7ece bright pass + 0536 separable gaussian (half res) = bloom; f4df tonemap (Uncharted2-style, exposure t2) + bloom -> T33 (LDR); **6fc0 TAA** (t3 current, t4 history Catmull-Rom, t5 depth gather, t23 velocity, YCoCg clamp; o0 history, o1 output) -> T119/T206; 7411 final copy -> S (UI target).
- **disable_taa:** 6fc0 replaced by a pass-through PS (o0 = o1 = t3.Load) via the deferred PSSetShader hook; camera jitter zeroed in FixProjection. Clearly sharper.
- **Fog:** no separate fog pass in town; the haze is in the scenery. Skipping each VFX fog-shader family changed nothing visible. 7325 is colour grading -> offered as disable_color_grading.
- **DoF:** not in the town frame (and not in the travel-menu frame, whose blurred background is a UI image). Needs an F10 capture during a cutscene/battle.
- debug_skip_ps = hash,... drops draws of those pixel shaders (diagnostics).

## 0.9.2: edge-popping fix, VRS (2026-10-22)
- **Edge popping:** props/NPCs/parts of buildings vanished near the view edges while turning the head; independent of the render frustum (fov_overscan up to 30 deg changed nothing) and of +0x9d0/+0x9d4 set at render-frame (cull_margin -25..40: draw count unchanged). Cause: the game camera update exe+0x68cb00 (`void(cam)`, view = *(cam+0x60)) copies the camera's own vertical FOV (cam+0x138, 50 deg, aspect 16:9) into view+0x9d4 every update; culling during the update then uses that narrow cone with our head-turned view. Fix: hook 0x68cb00 (prologue signature checked) and write our aspect/FOV back into the view after it (game values kept for restore). Town: 1450 -> ~1800 draws per frame.
- **VRS (vrs.cpp):** NVAPI via nvapi_QueryInterface (CreateShadingRateResourceView 0x99ca2dff, RSSetShadingRateResourceView 0x1b0c2f83, RSSetViewportsPixelShadingRates 0x34f7938f). Works on the game's deferred contexts (0 errors) and through the eye-2 replay. Enabled on deferred OMSetRenderTargets when RT0 is render-size and a DSV is bound; disabled otherwise. Rings: inner 0.35 full, to 0.70 2x2, outside 4x4 -> GPU 22.3 -> 19.8 ms per frame (test PC). Lighting is a compute shader (6c50), not affected.
- fov_overscan default 0 (kept as an option).


## 1.0.1: second culling path, HUD backdrop, depth of field (2026-10-23)
- **Edge culling, part 2:** with fix_cull_fov on, objects 60 deg to the side were still missing (debug_head_yaw = 60 in
  the town start position: a chimney whose smoke was drawn but the chimney itself was not). cull_margin up to 30 deg
  changed nothing. The camera object's own vertical FOV (cam+0x138, 50 deg) feeds a second visibility test. Writing our
  FOV (incl. margin, ~102 deg) into cam+0x138 before the camera update (exe+0x68cb00) brings the chimney back; restored
  when stereo stops. Part of fix_cull_fov. Trigger `camfov=<deg>` (0 auto, > 0 fixed, < 0 leave the camera alone) and
  `yaw=<deg>` for A/B tests; draw counts are too noisy for this (NPCs move), compare eye dumps instead.
- **HUD frosted backdrops:** the UI copies the screen target (scene + UI so far) to a small texture (e5d46dfbe7a817e7),
  blurs it (3b58f6b490180ea6 / 2afd3e011b02e4e6, cbUIGauss) and pastes it behind panels through a mask
  (e24514aebdeabacb). On the VR HUD panel that paste shows a misplaced piece of the scene (user video: a rectangle
  under the location name in battle). hud_blur_backdrop = false (default) skips the masked paste.
- **Depth of field:** compute shaders 7eb0fb99087d7a98 / c201b84553cc07e7 (ParamBuffer: focalDistance_, focusScale_,
  blurSizeMax_ ...; writes g_DofTexture / g_DofMediumDist, also reads the bloom chain). Not dispatched in town,
  menus or the party screen; needs a scene that uses it (cutscene / dialogue) to find the consumer and build a
  disable option. Found with debug_dump_shaders (GranblueVR_allshaders\index.txt).

## 1.0.3: what the remaining edge drop-outs are (2026-10-24)
- Main camera object: the one whose +0x60 is the main view element (a static object in the exe, e.g. exe+0x...4860 in
  this build). Layout: +0x100 eye, +0x110 target, +0x120 up (vec4 each), +0x134 near?, +0x138 vertical FOV (rad);
  +0x140..0x178 a second copy (FOV at +0x178 stays the game's 50 deg). Other UpdateView calls (view indices 2-11)
  are fixed UI/2D views (eye (8,8,22) / (0,0,10)), none share the main camera.
- Turning the camera object's pose with the head (trigger camhead=1: write the head-rotated eye/target/up into
  +0x100.. after the update, restore before the next) is stable (no drift), but at a fixed head yaw of 60 or 110 deg
  nothing was missing without it either: once the view and camera FOV are wide, there is no direction-based culling
  left at a static pose.
- So the remaining drop-outs happen only while the head moves: the game decides visibility with the head pose of
  its camera update, a frame or two before the eyes are rendered with a newer pose. edge_clipping now also widens
  the culling margin (default 15 deg, experimental 25 deg; was 6). No measurable cost on the test PC (draws, CPU,
  GPU unchanged at a static pose).
- Triggers added for this: camdump, viewdump, camhead=0|1, edge=off|default|experimental.

## 1.0.3: addresses by signature scan (2026-11-01)
- A user's log (1.0.0, Quest 3 / Virtual Desktop, RX 9070 XT) showed `UpdateView prologue mismatch - unsupported game
  version`: his exe image size is 0x81dd000, ours 0x8218000 (Steam build 24955526, exe 2.0.6, 123,520,480 bytes,
  PE timestamp 0x6a8ec59e). The test PC had not been updated, so his is another build (older, a beta branch or
  another store). The mod only showed the flat theater screen. Worse, `Trace_Install` hooked its four fixed
  addresses on his build without any check.
- `src/gameaddr.cpp` now finds every address the default single-pass stereo path uses by byte pattern: UpdateView,
  render-frame, camera update, render job, constant upload (functions); view array, view count, current view,
  frame payload, pending payload, frame token, resolution index and table (globals). Two patterns per entry
  (function start + call site, or two RIP-relative accesses), wildcards on RIP displacements and relative branches,
  struct offsets kept (e.g. the view stride 0x9f0). Scan: about 0.8 s at the first Present.
- Patterns were generated in-process on the known build with `debug_make_sigs = 1` (writes GranblueVR_sigs.txt,
  about 35 s). On the test PC all 13 resolve to the old addresses ("the build the mod was made on") and stereo runs.
- Legacy two-pass stereo (debug_stereo without 8192), its patches (kick, handshake, frame worker, stage skip), the
  queue-push / instance-alloc hooks, the resolution experiments and Trace still use fixed addresses. They're only
  installed when every scanned address matches the known build (`GameAddr_KnownBuild()`).
- On another build: if a required entry is missing, head tracking / stereo are disabled with a clear log line, and
  each failing pattern's match count is logged (0 = code changed, 2+ = pattern too short).
- Shared, generalised version: vr-sharedtools `sigscan/`.

## 1.0.3: cutscene clouds (2026-11-08)
- Report: in cutscenes, characters' faces / skin vanish, and "textures stick to the HMD" when looking around (both
  eyes; much worse with head tracking). Reproduced on the test PC with the user's save (airship cutscene).
- Cause: the game's volumetric clouds. CS `9a9dc84eb55f13d0` ray-marches them at half resolution with its own camera
  in its ParamBuffer (cb11: `matView_` rows 0-2 rotate the screen ray to world, `eyePosition_` at row 4, `screenSize_`,
  `nearZ_/farZ_/screenZ_/invAspectRatio_` = a symmetric projection), not the per-eye SceneBuffer. That camera doesn't
  follow the head, so the clouds are head-locked. PS `01a437224bb11c66` (CloudColor/NormalVelocity/Depth/Mask) writes
  them into the G-buffer with depth, so the misplaced cloud depth covers characters. Skipping that PS alone fixes the
  characters (test 009).
- 1.0.3j: `volumetric_clouds` (menu Display; default false in 1.0.3j, back to true in 1.0.3k because the user's
  "textures slide with the HMD" report was not fixed by it) skips both the ray-march dispatch (new CS skip:
  `PostFx_ReplaceCS` marks the context, `hkDispatchD` drops the dispatch) and the composite. Leftover: some flat,
  polygonal cloud cards in the sky become visible where the volumetric layer used to cover them.
- Proper fix (todo): rewrite cb11 per eye (`matView_` / `eyePosition_` from the eye's view, `screenZ_` / aspect
  from the eye FOV; the asymmetric eye frustum needs an approximation) for eye 0, and redirect the bind to a per-eye
  copy in the eye-1 replay. The cloud history textures (1124x1176 fmt 26, "temporal ... skipped") would also need to
  be per eye.
- Still open: the user's in-headset report "looking left / right in cutscenes, the textures stay attached to the HMD
  and slide away" is unchanged with clouds off. It only shows with head motion. Skipping the DoF temporal blend
  (`6fc023ddc2d36a64`, reads g_OldColorTexture / g_PrevZTexture) froze the output and skipping the DoF CS
  (`c201b84553cc07e7`) blacked it out: both are on the path to the final image in cutscenes, so they can't be
  switched off for testing. Cloud sprites (`55beb9f1...`, `94a192cf...`, `7c22a064...`) are billboards.
- 1.0.3l: `debug_menu = true` (hidden key) adds a Debug page to the VR menu, so problems can be investigated in the
  headset without leaving the game: "Record 5 s of frames" (left eye, half resolution, 10 fps, BMPs plus
  frames.csv with head yaw / pitch / position, in GranblueVR_record_<time>; trigger `record` does the same),
  "Capture render passes" (F10), "Re-detect frame history", and toggles for cloud sprites (`debug_cloud_sprites`),
  volumetric clouds, head tracking, AA and edge clipping. Recording costs no visible frame time (staging ring,
  read back a frame later, files written on a worker thread); 5 s is about 175 MB.
- Found (1.0.3n): the clouds are not head-locked through their camera. A dump of the ray-march ParamBuffer (trigger
  `clouddump`, upload size 0x170 through the constant-upload hook) shows `matView_` = the main view's matrix at upload
  time (head-tracked) and `eyePosition_` = the camera eye. But `screenZ_` (+0x78) / `invAspectRatio_` (+0x7c) are
  derived from the view's +0x9d4 / +0x9d0, which the mod sets to the *culling* FOV (eye FOV + edge-clipping margin):
  on the test PC 117.7 x 122.2 deg instead of the eye's 90 x 90. Drawn too wide, the clouds turn slower than the
  world when the head turns, which looks like they stick to the headset. Head tracking off hides it (no head motion).
  Fix: `PatchCloudParams` in hkCbUpload uploads a copy with screenZ_ = (h/2) / tanY and invAspect =
  tanX * screenZ_ / ((w/2)(w/h)) from eye 0's render tangents (`cloud_fov_fix`, default on; Debug page toggle,
  trigger `cloudfix=0/1`). Eye 1's replay reuses eye 0's buffer (same FOV with symmetric_fov; only the IPD differs).
  Other effects that derive a projection from +0x9d4 / +0x9d0 would have the same problem (cloud sprites?).
- 1.0.3s: `cutscene_mode = vr | theater` (menu View → Cutscenes; default theater since 1.0.3t). The cutscene flag was found with the new state
  scanner (src/statescan.cpp; triggers `snap=0..3`, `snapdiff`, `snapwatch=rva,...`): snapshots of the exe's writable
  data in gameplay (A) and in the airship cutscene (B), then candidates watched through title, loading, cutscene,
  skip, gameplay and pause menu. exe+0x7038388 (byte, written with `setne`, near the view globals) is 1 exactly while
  the cutscene plays. GA_Cutscene (two patterns) plus GA_Cutscene2 (exe+0x7396e38, diagnostics). In theater mode the
  stereo path is off and hkUpdateView leaves the camera alone. Tested: detected 3-4 s after stereo resumes, theater
  for the cutscene, back to stereo after skipping.
- HUD copy flashing in cutscenes (VR mode): the scene-target table had 4 slots and new targets replaced slot 0, so two
  cutscene targets evicted each other every frame (log spam "learned (4 known)"); now 16, round robin. That stopped the
  thrashing but "UI missing" is still high in cutscene frames: open. Trace (debug log) now also records binds that
  involve a known target with depth or several targets (`B`) and lone binds of unknown targets (`S`).
