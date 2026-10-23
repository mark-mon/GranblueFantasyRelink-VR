// Foveated rendering with NVIDIA Variable Rate Shading (NVAPI, D3D11, Turing and newer).
//
// A shading-rate image (one byte per 16x16 pixel tile, an index into a per-viewport rate table) is bound
// on the deferred contexts whenever the game renders into a target of the VR render size with a depth
// buffer (G-buffer, sky, transparents). Post-processing (no depth buffer), shadow maps and half-res passes
// keep full rate. Centre: full rate; middle ring: vrs_mid_rate; outside: vrs_outer_rate. The eye-2 replay
// re-executes the same command lists, so it gets the same shading-rate state; with the symmetric render
// frustum each eye's straight-ahead direction is the image centre.
#include <windows.h>
#include <d3d11.h>
#include <mutex>
#include <vector>
#include <cmath>
#include <string>
#include <cstdint>
#include <cstring>
#include "log.h"
#include "globals.h"

bool VR_ViewTangents(int k, float t[4]); bool VR_Gaze(float* tx, float* ty);
namespace {
typedef unsigned int NvU32;
enum { NV_PIXEL_X1_PER_RASTER_PIXEL = 5, NV_PIXEL_X1_PER_2X1_RASTER_PIXELS = 6, NV_PIXEL_X1_PER_1X2_RASTER_PIXELS = 7,
       NV_PIXEL_X1_PER_2X2_RASTER_PIXELS = 8, NV_PIXEL_X1_PER_4X2_RASTER_PIXELS = 9, NV_PIXEL_X1_PER_2X4_RASTER_PIXELS = 10,
       NV_PIXEL_X1_PER_4X4_RASTER_PIXELS = 11 };
struct VpRate { bool enable; int table[16]; };
struct VpRates { NvU32 version; NvU32 numViewports; VpRate* pViewports; };
struct SrrvDesc { NvU32 version; DXGI_FORMAT Format; int ViewDimension; union { UINT MipSlice; struct { UINT a, b, c; } arr; }; };
#define NVVER(T, v) (NvU32)(sizeof(T) | ((v) << 16))

using QueryFn = void* (__cdecl*)(unsigned int);
using InitFn = int(__cdecl*)();
using CreateSrrvFn = int(__cdecl*)(ID3D11Device*, ID3D11Resource*, SrrvDesc*, IUnknown**);
using SetSrrvFn = int(__cdecl*)(IUnknown*, IUnknown*);
using SetRatesFn = int(__cdecl*)(IUnknown*, VpRates*);
CreateSrrvFn pCreate = nullptr; SetSrrvFn pSetView = nullptr; SetRatesFn pSetRates = nullptr;
bool g_tried = false, g_ok = false;
std::mutex g_mx;
ID3D11Texture2D* g_tex = nullptr; IUnknown* g_view = nullptr;
UINT g_w = 0, g_h = 0;
VpRate g_rate[16]; VpRates g_on = {}, g_off = {};
volatile bool g_active = false;      // runtime toggle (config + hotkey)
volatile bool g_everOn = false;      // VRS was switched on at some point (keeps disabling it on contexts after F4)
volatile LONG g_applied = 0, g_errors = 0;
int g_logErr = 5;

int RateOf(const std::string& s, int def) {
    if (s == "1x1") return NV_PIXEL_X1_PER_RASTER_PIXEL;
    if (s == "2x1") return NV_PIXEL_X1_PER_2X1_RASTER_PIXELS;
    if (s == "1x2") return NV_PIXEL_X1_PER_1X2_RASTER_PIXELS;
    if (s == "2x2") return NV_PIXEL_X1_PER_2X2_RASTER_PIXELS;
    if (s == "4x2") return NV_PIXEL_X1_PER_4X2_RASTER_PIXELS;
    if (s == "2x4") return NV_PIXEL_X1_PER_2X4_RASTER_PIXELS;
    if (s == "4x4") return NV_PIXEL_X1_PER_4X4_RASTER_PIXELS;
    return def;
}

bool Init() {
    if (g_tried) return g_ok;
    g_tried = true;
    HMODULE nv = LoadLibraryW(L"nvapi64.dll");
    if (!nv) { LOGI("VRS: nvapi64.dll not found (not an NVIDIA GPU) - foveated rendering unavailable"); return false; }
    auto q = (QueryFn)GetProcAddress(nv, "nvapi_QueryInterface");
    if (!q) { LOGE("VRS: nvapi_QueryInterface missing"); return false; }
    auto init = (InitFn)q(0x0150e828);
    pCreate = (CreateSrrvFn)q(0x99ca2dff); pSetView = (SetSrrvFn)q(0x1b0c2f83); pSetRates = (SetRatesFn)q(0x34f7938f);
    if (!init || !pCreate || !pSetView || !pSetRates) { LOGE("VRS: NVAPI functions missing"); return false; }
    int st = init();
    if (st != 0) { LOGE("VRS: NvAPI_Initialize failed (%d)", st); return false; }
    for (auto& r : g_rate) {
        r.enable = true;
        for (auto& t : r.table) t = NV_PIXEL_X1_PER_RASTER_PIXEL;
        r.table[1] = RateOf(g_cfg.vrsMidRate, NV_PIXEL_X1_PER_2X2_RASTER_PIXELS);
        r.table[2] = RateOf(g_cfg.vrsOuterRate, NV_PIXEL_X1_PER_4X4_RASTER_PIXELS);
        r.table[3] = RateOf(g_cfg.quadVrsHiddenRate, NV_PIXEL_X1_PER_4X4_RASTER_PIXELS);   // wide view behind the quad-view inset
    }
    g_on = { NVVER(VpRates, 1), 16, g_rate };
    g_off = { NVVER(VpRates, 1), 0, g_rate };
    g_ok = true;
    LOGI("VRS: NVAPI ready (centre radius %.2f full rate, ring to %.2f at %s, outside at %s)",
         g_cfg.vrsInner, g_cfg.vrsOuter, g_cfg.vrsMidRate.c_str(), g_cfg.vrsOuterRate.c_str());
    return true;
}

// Shading-rate pattern. Outer (eye) views: rings around the centre (the gaze when eye tracking is on);
// with quad views, the part hidden behind the inset gets index 3. Inset views: full rate.
struct PatternKey { UINT w, h; int kind; int cx, cy; int hx0, hy0, hx1, hy1; bool operator==(const PatternKey& o) const { return !memcmp(this, &o, sizeof(*this)); } };
PatternKey g_cur = {};   // content of the texture now
std::vector<uint8_t> g_px;
volatile LONG g_uploads = 0;

void Build(const PatternKey& k) {
    UINT tw = (k.w + 15) / 16, th = (k.h + 15) / 16;
    g_px.assign((size_t)tw * th, 0);
    if (k.kind == 1) return;   // inset: full rate
    float half = tw * 0.5f;    // radii are fractions of the half width
    for (UINT y = 0; y < th; ++y) for (UINT x = 0; x < tw; ++x) {
        float dx = (x + 0.5f - k.cx / 16.f) / half, dy = (y + 0.5f - k.cy / 16.f) / half;
        float r = sqrtf(dx * dx + dy * dy);
        uint8_t v = r < g_cfg.vrsInner ? 0 : r < g_cfg.vrsOuter ? 1 : 2;
        if ((int)x * 16 >= k.hx0 && (int)(x + 1) * 16 <= k.hx1 && (int)y * 16 >= k.hy0 && (int)(y + 1) * 16 <= k.hy1) v = 3;
        g_px[(size_t)y * tw + x] = v;
    }
}
// the pattern wanted for view k (target size w x h)
PatternKey Want(int view, UINT w, UINT h) {
    PatternKey k = {}; k.w = w; k.h = h; k.hx0 = k.hy0 = k.hx1 = k.hy1 = -1;
    float t[4];
    if (view >= 2) { k.kind = 1; return k; }
    k.cx = (int)(w * 0.5f + g_cfg.vrsCenterX * w * 0.5f); k.cy = (int)(h * 0.5f - g_cfg.vrsCenterY * h * 0.5f);
    if (!VR_ViewTangents(view, t)) return k;
    auto px = [&](float tx) { return (tx - t[0]) / (t[1] - t[0]) * w; };
    auto py = [&](float ty) { return (t[2] - ty) / (t[2] - t[3]) * h; };
    float gx, gy;
    if (g_cfg.vrsFollowGaze && VR_Gaze(&gx, &gy)) { k.cx += (int)(px(gx) - w * 0.5f); k.cy += (int)(py(gy) - h * 0.5f); }
    float it[4];
    if (VR_ViewTangents(view + 2, it)) {   // quad views: inset of this eye covers this rectangle
        float fx = (it[1] - it[0]) * g_cfg.quadFeather, fy = (it[2] - it[3]) * g_cfg.quadFeather;   // keep the feathered border shaded
        k.hx0 = (int)px(it[0] + fx); k.hx1 = (int)px(it[1] - fx); k.hy0 = (int)py(it[2] - fy); k.hy1 = (int)py(it[3] + fy);
    }
    k.cx = k.cx / 16 * 16; k.cy = k.cy / 16 * 16;   // only re-upload when the centre moves by a tile
    return k;
}
// texture + view for a w x h target (content uploaded on the immediate context)
bool EnsureImage(ID3D11Device* dev, UINT w, UINT h) {
    if (g_view && g_w == w && g_h == h) return true;
    if (g_view) { g_view->Release(); g_view = nullptr; }
    if (g_tex) { g_tex->Release(); g_tex = nullptr; }
    UINT tw = (w + 15) / 16, th = (h + 15) / 16;
    g_cur = Want(0, w, h);
    Build(g_cur);
    D3D11_TEXTURE2D_DESC d = {}; d.Width = tw; d.Height = th; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8_UINT;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = { g_px.data(), tw, 0 };
    if (FAILED(dev->CreateTexture2D(&d, &init, &g_tex))) { LOGE("VRS: shading-rate texture creation failed"); return false; }
    SrrvDesc sd = {}; sd.version = NVVER(SrrvDesc, 1); sd.Format = DXGI_FORMAT_R8_UINT; sd.ViewDimension = 4; sd.MipSlice = 0;
    int st = pCreate(dev, g_tex, &sd, &g_view);
    if (st != 0 || !g_view) { LOGE("VRS: CreateShadingRateResourceView failed (%d) - VRS unsupported on this GPU?", st); g_ok = false; return false; }
    g_w = w; g_h = h;
    size_t n0 = 0, n1 = 0; for (auto v : g_px) { if (v == 0) ++n0; else if (v == 1) ++n1; }
    LOGI("VRS: shading-rate image %ux%u tiles for %ux%u: %.0f%% full rate, %.0f%% middle, %.0f%% outer",
         tw, th, w, h, 100.0 * n0 / g_px.size(), 100.0 * n1 / g_px.size(), 100.0 * (g_px.size() - n0 - n1) / g_px.size());
    return true;
}
} // namespace

bool VR_RenderSize(UINT* w, UINT* h);
void Vrs_SetActive(bool on) { g_active = on; if (on) g_everOn = true; }
bool Vrs_Active() { return g_active; }

// Deferred-context OMSetRenderTargets (after the game's call): VRS on for scene passes at the render size.
void Vrs_OnSetRenderTargets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    if (!g_everOn) return;
    bool want = false;
    UINT W = 0, H = 0;
    if (g_active && dsv && rtvs && n > 0 && rtvs[0] && VR_RenderSize(&W, &H)) {
        ID3D11Resource* r = nullptr; rtvs[0]->GetResource(&r);
        if (r) {
            D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
            if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) { D3D11_TEXTURE2D_DESC td; ((ID3D11Texture2D*)r)->GetDesc(&td); want = td.Width == W && td.Height == H; }
            r->Release();
        }
    }
    std::lock_guard<std::mutex> lk(g_mx);
    if (!Init()) return;
    if (want) {
        ID3D11Device* dev = nullptr; c->GetDevice(&dev);
        bool ok = dev && EnsureImage(dev, W, H);
        if (dev) dev->Release();
        if (!ok) return;
        int a = pSetView(c, g_view), b = pSetRates(c, &g_on);
        if (a || b) { InterlockedIncrement(&g_errors); if (g_logErr > 0) { --g_logErr; LOGE("VRS: enabling on context %p failed (%d, %d)", (void*)c, a, b); } }
        else InterlockedIncrement(&g_applied);
    } else {
        pSetRates(c, &g_off);
    }
}
void Vrs_LogStats() {
    if (!g_everOn) return;
    LOGI("VRS: %s, %ld scene passes shaded at variable rate, %ld pattern updates in the last interval (%ld errors)", g_active ? "on" : "off",
         InterlockedExchange(&g_applied, 0), InterlockedExchange(&g_uploads, 0), InterlockedExchange(&g_errors, 0));
}

// Present (immediate context): set the shading-rate image for the view rendered next (0 = next frame's
// own render, 1..3 = the replays). Uploads only when the pattern changes (view kind, gaze tile, inset).
void Vrs_BeforeView(ID3D11DeviceContext* imm, int view) {
    if (!g_everOn) return;
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_ok || !g_tex || !g_w) return;
    PatternKey k = Want(view, g_w, g_h);
    if (k == g_cur) return;
    Build(k);
    imm->UpdateSubresource(g_tex, 0, nullptr, g_px.data(), (g_w + 15) / 16, 0);
    g_cur = k;
    InterlockedIncrement(&g_uploads);
}

// Settings menu: vrs_inner / vrs_outer / rates changed. Rebuild the rate table and force the pattern to be
// re-uploaded before the next view.
void Vrs_ConfigChanged() {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_ok) for (auto& r : g_rate) {
        r.table[1] = RateOf(g_cfg.vrsMidRate, NV_PIXEL_X1_PER_2X2_RASTER_PIXELS);
        r.table[2] = RateOf(g_cfg.vrsOuterRate, NV_PIXEL_X1_PER_4X4_RASTER_PIXELS);
        r.table[3] = RateOf(g_cfg.quadVrsHiddenRate, NV_PIXEL_X1_PER_4X4_RASTER_PIXELS);
    }
    memset(&g_cur, 0xff, sizeof(g_cur));
}
