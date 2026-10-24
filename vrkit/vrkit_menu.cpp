#include "vrkit_menu.h"
#include "vrkit_options.h"
#include <windows.h>
#include <d3d11_1.h>
#include <vector>
#include <algorithm>
#include "imgui.h"
#include "imgui_impl_dx11.h"

namespace vrkit { namespace menu {

namespace {
Settings g_s;
ID3D11Device* g_dev = nullptr; ID3D11DeviceContext* g_ctx = nullptr; ID3D11DeviceContext1* g_ctx1 = nullptr;
ID3DDeviceContextState* g_state = nullptr;
ID3D11Texture2D* g_tex = nullptr; ID3D11RenderTargetView* g_rtv = nullptr;
ImGuiContext* g_im = nullptr;
bool g_ok = false, g_open = false;
int g_page = 0, g_sel = 0;
LARGE_INTEGER g_qpf, g_last;

std::vector<int> PageItems(int page) {
    auto& opts = Options::Get().All();
    auto pages = Options::Get().Pages();
    std::vector<int> r;
    if (pages.empty()) return r;
    page = (std::min)(page, (int)pages.size() - 1);
    for (int k = 0; k < (int)opts.size(); ++k)
        if (opts[k].page == pages[page] && (!opts[k].visible || opts[k].visible())) r.push_back(k);
    return r;
}

void Close() {
    g_open = false;
    input::SetBlocking(false);
    if (Options::Get().Dirty() && !g_s.configPath.empty()) {
        if (Options::Get().Save(g_s.configPath)) Log("vrkit: settings saved");
    }
}

void Navigate(uint32_t b) {
    auto pages = Options::Get().Pages();
    if (pages.empty()) return;
    if (b & input::PagePrev) { g_page = (g_page + (int)pages.size() - 1) % (int)pages.size(); g_sel = 0; }
    if (b & input::PageNext) { g_page = (g_page + 1) % (int)pages.size(); g_sel = 0; }
    auto items = PageItems(g_page);
    if (items.empty()) return;
    int n = (int)items.size();
    if (b & input::Up) g_sel = (g_sel + n - 1) % n;
    if (b & input::Down) g_sel = (g_sel + 1) % n;
    g_sel = (std::min)(g_sel, n - 1);
    Option& o = Options::Get().All()[items[g_sel]];
    if (b & input::Left) Options::Get().Step(o, -1);
    if (b & input::Right) Options::Get().Step(o, +1);
    if (b & input::Accept) Options::Get().Activate(o);
    if (b & input::Reset) Options::Get().Reset(o);
}

void Draw() {
    ImGuiIO& io = ImGui::GetIO();
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    io.DeltaTime = (std::max)(0.001f, (float)(now.QuadPart - g_last.QuadPart) / (float)g_qpf.QuadPart); g_last = now;
    io.DisplaySize = ImVec2((float)g_s.texW, (float)g_s.texH);
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    const float W = (float)g_s.texW, H = (float)g_s.texH, pad = g_s.fontPx * 0.6f;
    ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(W, H));
    ImGui::Begin("##vrkit", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar);
    const ImU32 cText = IM_COL32(235, 238, 245, 255), cDim = IM_COL32(150, 158, 175, 255), cAcc = IM_COL32(110, 180, 255, 255),
                cSel = IM_COL32(60, 90, 140, 255), cWarn = IM_COL32(255, 190, 90, 255);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float y = pad;
    // title + status
    dl->AddText(ImVec2(pad, y), cText, g_s.title.c_str());
    if (g_s.statusLine) { std::string st = g_s.statusLine(); ImVec2 ts = ImGui::CalcTextSize(st.c_str()); dl->AddText(ImVec2(W - pad - ts.x, y), cDim, st.c_str()); }
    y += g_s.fontPx * 1.4f;
    // page tabs
    auto pages = Options::Get().Pages();
    float x = pad;
    for (int p = 0; p < (int)pages.size(); ++p) {
        ImVec2 ts = ImGui::CalcTextSize(pages[p].c_str());
        if (p == g_page) dl->AddRectFilled(ImVec2(x - 8, y - 4), ImVec2(x + ts.x + 8, y + ts.y + 4), cSel, 6.f);
        dl->AddText(ImVec2(x, y), p == g_page ? cText : cDim, pages[p].c_str());
        x += ts.x + 28;
    }
    y += g_s.fontPx * 1.5f;
    dl->AddLine(ImVec2(pad, y - 8), ImVec2(W - pad, y - 8), cDim, 2.f);
    // rows (scrolled so the selection stays visible)
    auto items = PageItems(g_page);
    float rowH = g_s.fontPx * 1.35f, listTop = y, listBottom = H - g_s.fontPx * 5.2f;
    int visibleRows = (std::max)(1, (int)((listBottom - listTop) / rowH));
    int first = (std::max)(0, (std::min)(g_sel - visibleRows / 2, (int)items.size() - visibleRows));
    auto& opts = Options::Get().All();
    for (int r = first; r < (int)items.size() && r < first + visibleRows; ++r) {
        Option& o = opts[items[r]];
        float ry = listTop + (r - first) * rowH;
        if (r == g_sel) dl->AddRectFilled(ImVec2(pad * 0.5f, ry - 4), ImVec2(W - pad * 0.5f, ry + rowH - 6), cSel, 6.f);
        dl->AddText(ImVec2(pad, ry), cText, o.label.c_str());
        std::string v = Options::Get().DisplayText(o);
        bool adjustable = o.type != OptType::Action;
        std::string shown = o.type == OptType::Action ? "[ run ]" : (r == g_sel && adjustable ? "<  " + v + "  >" : v);
        ImVec2 vs = ImGui::CalcTextSize(shown.c_str());
        float vx = W - pad - vs.x;
        if (o.needsRestart) {
            const char* rs = "restart";
            ImVec2 rsz = ImGui::CalcTextSize(rs);
            dl->AddText(ImVec2(vx - rsz.x - 24, ry), cWarn, rs);
        }
        dl->AddText(ImVec2(vx, ry), r == g_sel ? cAcc : cText, shown.c_str());
    }
    if (items.empty()) dl->AddText(ImVec2(pad, listTop), cDim, "(nothing on this page)");
    // help for the selected option + controls
    float hy = H - g_s.fontPx * 4.6f;
    dl->AddLine(ImVec2(pad, hy - 10), ImVec2(W - pad, hy - 10), cDim, 2.f);
    if (!items.empty()) {
        const Option& o = opts[items[(std::min)(g_sel, (int)items.size() - 1)]];
        std::string h = o.help;
        if (!o.key.empty()) h += (h.empty() ? "" : "   ") + std::string("[") + o.key + "]";
        ImGui::SetCursorScreenPos(ImVec2(pad, hy));
        ImGui::PushTextWrapPos(W - pad);
        ImGui::PushStyleColor(ImGuiCol_Text, cDim);
        ImGui::TextWrapped("%s", h.c_str());
        ImGui::PopStyleColor(); ImGui::PopTextWrapPos();
    }
    const char* keys = "A: change   Y: undo   LB/RB: page   B: close";
    dl->AddText(ImVec2(pad, H - g_s.fontPx * 1.4f), cDim, keys);
    ImGui::End();
    ImGui::Render();

    // draw into our texture with our own pipeline state (the game's state is untouched)
    ID3DDeviceContextState* prev = nullptr;
    if (g_ctx1 && g_state) g_ctx1->SwapDeviceContextState(g_state, &prev);
    const float clear[4] = { 0, 0, 0, 0 };
    g_ctx->ClearRenderTargetView(g_rtv, clear);
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    D3D11_VIEWPORT vp = { 0, 0, W, H, 0, 1 }; g_ctx->RSSetViewports(1, &vp);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    if (g_ctx1 && g_state) { g_ctx1->SwapDeviceContextState(prev, nullptr); if (prev) prev->Release(); }
}
} // namespace

bool Init(ID3D11Device* dev, ID3D11DeviceContext* imm, const Settings& s) {
    if (g_ok) return true;
    g_s = s; g_dev = dev; g_ctx = imm;
    input::Configure(s.input);
    input::InstallHooks();
    imm->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&g_ctx1);
    ID3D11Device1* dev1 = nullptr;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&dev1))) {
        D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel(), chosen;
        dev1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), &chosen, &g_state);
        dev1->Release();
    }
    D3D11_TEXTURE2D_DESC d = {}; d.Width = s.texW; d.Height = s.texH; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &g_tex))) { Log("vrkit: menu texture failed"); return false; }
    D3D11_RENDER_TARGET_VIEW_DESC rd = {}; rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(dev->CreateRenderTargetView(g_tex, &rd, &g_rtv))) { Log("vrkit: menu RTV failed"); return false; }

    IMGUI_CHECKVERSION();
    g_im = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_im);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.LogFilename = nullptr;
    io.DisplaySize = ImVec2((float)s.texW, (float)s.texH);
    // a clear UI font from Windows at the requested size; ImGui's built-in font as fallback
    ImFont* f = nullptr;
    wchar_t win[MAX_PATH]; char path[MAX_PATH * 2];
    if (GetWindowsDirectoryW(win, MAX_PATH)) {
        for (const wchar_t* name : { L"\\Fonts\\segoeui.ttf", L"\\Fonts\\arial.ttf" }) {
            std::wstring p = std::wstring(win) + name;
            WideCharToMultiByte(CP_UTF8, 0, p.c_str(), -1, path, sizeof(path), nullptr, nullptr);
            if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES && (f = io.Fonts->AddFontFromFileTTF(path, s.fontPx))) break;
        }
    }
    if (!f) { ImFontConfig fc; fc.SizePixels = s.fontPx; io.Fonts->AddFontDefault(&fc); }
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 14.f; st.WindowBorderSize = 0.f; st.WindowPadding = ImVec2(0, 0);
    st.Colors[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.08f, 0.11f, 0.94f);
    if (!ImGui_ImplDX11_Init(dev, imm)) { Log("vrkit: ImGui D3D11 init failed"); return false; }
    QueryPerformanceFrequency(&g_qpf); QueryPerformanceCounter(&g_last);
    g_ok = true;
    Log("vrkit: menu ready (%zu options on %zu pages)", Options::Get().All().size(), Options::Get().Pages().size());
    return true;
}

void Shutdown() {
    if (g_im) { ImGui::SetCurrentContext(g_im); ImGui_ImplDX11_Shutdown(); ImGui::DestroyContext(g_im); g_im = nullptr; }
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_tex) { g_tex->Release(); g_tex = nullptr; }
    if (g_state) { g_state->Release(); g_state = nullptr; }
    if (g_ctx1) { g_ctx1->Release(); g_ctx1 = nullptr; }
    g_ok = false; g_open = false;
}

bool Update() {
    if (!g_ok) return false;
    static int rehook = 0;
    if ((++rehook % 300) == 1) input::InstallHooks();   // the game may load XInput late
    uint32_t b = input::Poll();
    if (b & input::Toggle) { if (g_open) { Close(); Log("vrkit: menu closed"); } else { g_open = true; input::SetBlocking(true); Log("vrkit: menu opened"); } b &= ~input::Toggle; }
    else if (g_open && (b & input::Back)) { Close(); Log("vrkit: menu closed (back)"); return false; }
    if (!g_open) return false;
    Navigate(b);
    ImGui::SetCurrentContext(g_im);
    Draw();
    return true;
}
bool IsOpen() { return g_open; }
void SetOpen(bool open) { if (open == g_open) return; if (open) { g_open = true; input::SetBlocking(true); } else Close(); }
ID3D11Texture2D* Texture() { return g_tex; }
UINT Width() { return g_s.texW; }
UINT Height() { return g_s.texH; }

} } // namespace vrkit::menu
