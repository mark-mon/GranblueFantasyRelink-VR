// vrkit - in-game settings menu for VR mods (D3D11).
// Draws the options registered in vrkit::Options with Dear ImGui into an off-screen texture. The host
// shows that texture in the headset (an OpenXR quad layer) and calls Update() once per frame on the
// thread that owns the immediate context (e.g. in its Present hook).
//
// Controls: hold L3+R3 (both stick clicks; configurable, see input::Settings) or press Insert to open / close. D-pad or left stick up/down: select;
// left/right: change; A / Enter: toggle or run; Y / Delete: back to default; LB/RB or PgUp/PgDn: page;
// B / Backspace: close. Changes apply live (or are marked "restart"); the config file is saved on close.
#pragma once
#include <d3d11.h>
#include <string>
#include <functional>
#include "vrkit_input.h"

namespace vrkit { namespace menu {

struct Settings {
    std::string title = "VR mod";
    UINT texW = 1280, texH = 960;     // menu texture size (pixels)
    float fontPx = 34.f;              // text size in texture pixels
    std::wstring configPath;          // where Options::Save writes on close (empty: not saved)
    std::function<std::string()> statusLine;   // optional line under the title (fps, version...)
    input::Settings input;
};

bool Init(ID3D11Device* dev, ID3D11DeviceContext* imm, const Settings& s);
void Shutdown();
// Poll input, handle open/close and navigation, and redraw the texture while open. Returns true while open.
bool Update();
bool IsOpen();
void SetOpen(bool open);
// R8G8B8A8_TYPELESS texture (drawn as UNORM, colours as the menu style sets them), premultiplied alpha.
// Copy it into an R8G8B8A8_UNORM(_SRGB) swapchain image with CopyResource.
ID3D11Texture2D* Texture();
UINT Width();
UINT Height();

} } // namespace vrkit::menu
