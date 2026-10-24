# Granblue Fantasy: Relink VR

A native VR mod for **Granblue Fantasy: Relink** (Steam, PC). The game is rendered in true stereo, one image per eye every
frame, with 6DoF head tracking. The HUD and menus float on their own panel. Settings are changed from an in-headset menu.

Version **1.0.2**. Unofficial fan mod, not affiliated with or endorsed by Cygames.

## Features

- **Native stereo 3D** with head tracking (not a flat screen in VR). Each eye gets its own camera, and the per-eye
  temporal buffers keep anti-aliasing clean.
- **HUD and menus on a separate panel**, so they stay readable and converged.
- **Resolution control.** Renders at your headset's recommended resolution by default, with live scaling
  (`render_scale`, or F5 / F6).
- **Foveated rendering (NVIDIA VRS).** The centre of each eye is shaded at full rate and the edges at a lower rate.
  It usually gives 25-35% more fps and is hard to notice. With an **eye-tracking headset**, the sharp area follows
  your eyes.
- **Anti-aliasing choices.** The game's TAA is quite soft in VR. You can pick off (sharpest), FXAA, a mild TAA with
  adjustable strength, or the game's own TAA.
- **Edge culling fix.** Buildings, props, NPCs and textures at the sides of your view keep being drawn when you turn your
  head away from where the game camera points (`edge_clipping`).
- **In-game settings menu.** Hold both sticks clicked in (L3 + R3) for half a second, or press Insert. The panel stays
  fixed in space where it opened (or follows your head, if you prefer). Changes apply live and are saved to `config.txt`.
- **Pre-rendered movies** play on a flat screen in front of you.
- **Stats panel** (F7) with fps and CPU / GPU frame times.

## Requirements

- Granblue Fantasy: Relink, Steam version (tested with the October 2026 build, `granblue_fantasy_relink.exe`
  123,520,480 bytes). A game update can change the code the mod hooks. If it stops working after an update,
  please open an issue.
- A PC VR headset with **SteamVR** (or another OpenXR runtime set as the active runtime). Tested with a Pimax Dream Air
  on SteamVR.
- An Xbox-style controller (XInput).
- A strong GPU. The game is rendered twice per frame at high resolution. Foveated rendering needs an NVIDIA
  RTX 20-series / GTX 16-series or newer; on other GPUs the mod works without it.

## Install

1. Download `GranblueVR-1.0.2.zip` from [Releases](../../releases).
2. Copy `dinput8.dll` and `config.txt` into the game folder, next to `granblue_fantasy_relink.exe`
   (Steam: right-click the game > Manage > Browse local files).
3. In the game's graphics options, **turn HDR off**, set frame rate to 120, and turn Motion blur (Blur) off.
4. Start SteamVR, put the headset on, and launch the game from Steam.

To uninstall, delete `dinput8.dll` (and `config.txt` / `GranblueVR.log`) from the game folder.

## Controls

| | Controller | Keyboard |
|---|---|---|
| Open / close the VR settings menu | hold **L3 + R3** together for 0.5 s | **Insert** |
| Menu: select / change | D-pad or left stick | arrow keys |
| Menu: toggle, next choice | A | Enter |
| Menu: undo (back to the value at start) | Y | Delete |
| Menu: previous / next page | LB / RB | PgUp / PgDn |
| Menu: close | B | Backspace |
| Recenter view | menu: Display > Recenter | F8 |
| Anti-aliasing mode | | F1 |
| Eye tracking on / off | | F2 |
| Foveated rendering (VRS) on / off | | F4 |
| Render scale down / up | | F5 / F6 |
| Stats panel | | F7 |

A short L3 + R3 press still does the game's party skill; only holding it opens the menu (`menu_chord_pass`). To use a
different button, set `menu_chord = ltrt` (both triggers) or `none` (keyboard only), in `config.txt` or the menu.
While the menu is open, the game doesn't receive controller input. All keys can be changed in `config.txt`.

## Settings worth knowing

Everything is documented in `config.txt`, and most of it can be changed from the in-game menu.

- `render_scale`: per-eye resolution relative to the headset's recommended size (1.0). Lower it for more fps.
- `render_max`: cap on the per-eye width / height (default 8192).
- `aa_mode`: `off` | `fxaa` | `taa` | `game`. With `taa`, `taa_strength` (0-1) sets how much of previous frames is
  blended in. 0.3-0.5 is a good start; the game's own TAA is about 0.9.
- `vrs`, `vrs_inner`, `vrs_outer`, `vrs_mid_rate`, `vrs_outer_rate`: foveated rendering. The inner circle is full rate.
  The ring out to `vrs_outer` uses the middle rate (2x2) and everything beyond uses the outer rate (up to 4x4).
  Radii are fractions of half the image width. With eye tracking you can make the inner circle smaller.
- `eye_tracking`, `vrs_follow_gaze`: use the headset's eye tracking (OpenXR `XR_EXT_eye_gaze_interaction`) to move
  the full-rate area.
- `hud_distance`, `hud_size`, `menu_distance`, `menu_size`: panel placement in metres.
- `edge_clipping`: `off` | `default` | `experimental`. How wide the game's own camera looks when it decides what to load
  and draw. Wider stops textures and objects at the sides of your view dropping out as you turn your head. `experimental`
  is wider still and worked best in testing, but may have unexpected effects.
- `menu_anchor`: `world` (the menu stays where it opened) or `head` (it follows your head); `menu_distance`, `menu_size`
  and `menu_height` place it.
- `world_scale`: how big the world feels (game units per metre).
- `hud_blur_backdrop`: the game's frosted-glass panel backgrounds. Off by default, because in VR they show a misplaced
  piece of the scene behind HUD panels.
- `disable_color_grading`: turns off the game's depth-based colour grading for a flatter, lighter image.

## Known limitations

- Depth of field can't be turned off yet.
- HDR must be off.
- The mod blocks only XInput controllers while the menu is open. Arrow keys still reach the game.
- Performance depends heavily on resolution. Start with `render_scale = 1.0`, VRS on, and use the stats panel (F7).

## Reporting problems

The mod writes `GranblueVR.log` next to the game exe. If the game crashes, it may also write `GranblueVR_crash.dmp`.
Please attach both to an issue, along with your headset, GPU and `config.txt`. Set `log_level = debug` for more detail.

## How it works

The mod is a `dinput8.dll` proxy. It hooks Direct3D 11 / DXGI and a few game functions. The game renders the left
eye with a head-tracked camera. Its recorded command lists are then replayed for the right eye with swapped view
constants, so the CPU work of a frame is done only once. Both images go to the headset through OpenXR. The game's DRM
and game files are not modified. Technical notes are in [`docs/`](docs/).

## Building

On Windows (Visual Studio 2019 / 2022): run `build_msvc.cmd` from an "x64 Native Tools Command Prompt".

On Linux (clang-cl / lld-link, LLVM 16+): get the MSVC CRT and Windows SDK with
[xwin](https://github.com/Jake-Shadle/xwin) (`xwin --accept-license splat --output ~/xwin`), then run `XWIN=~/xwin ./build.sh`.

The output is `out/dinput8.dll`. All dependencies are in the repo: [Dear ImGui](https://github.com/ocornut/imgui)
(MIT), [MinHook](https://github.com/TsudaKageyu/minhook) (BSD-2), and the [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK)
headers and static loader (Apache-2.0). NVIDIA VRS is called through `nvapi64.dll` at runtime, with no SDK needed.

| Folder | Contents |
|---|---|
| `src/` | the mod: OpenXR (`vr.cpp`), stereo replay (`replay.cpp`, `camera.cpp`), temporal history, HUD, VRS, post-processing, config |
| `src/menu_options.cpp` | the settings shown in the in-game menu |
| `vrkit/` | reusable in-game VR settings menu (see `vrkit/README.md`) |
| `third_party/` | MinHook, OpenXR |
| `docs/` | technical notes |

## License

MIT. See [LICENSE](LICENSE). Third-party components keep their own licenses (see their folders).
