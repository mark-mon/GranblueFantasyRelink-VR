// HUD separation.
//
// The game draws its 2D UI (HUD, menus, prompts) at the end of the post-processing command list,
// straight onto the final LDR scene colour target: the post pass binds [X, S] (two render targets,
// no depth), then the UI pass binds [S] alone and draws ~60 alpha-blended quads. While recording on
// the deferred contexts we redirect that UI pass to our own transparent texture (and make the blend
// states accumulate alpha), so the scene images stay clean and the UI can be shown on its own
// OpenXR quad layer at a comfortable distance.
#include <windows.h>
#include <d3d11_4.h>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <cstdio>
#include "log.h"

namespace {
struct CtxState {
    ID3D11DeviceContext* ctx; ID3D11RenderTargetView* pairSecond; bool ui;
    bool pending;                    // lone bind of a known scene target: decided at the next draw
    ID3D11BlendState* blend; FLOAT bf[4]; UINT mask;   // last blend state set by the game
};
std::mutex g_mx;
CtxState g_ctx[64]; int g_nctx = 0;
volatile bool g_enabled = false;
ID3D11Texture2D* g_tex = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
ID3D11ShaderResourceView* g_srv = nullptr;
UINT g_w = 0, g_h = 0; DXGI_FORMAT g_fmt = DXGI_FORMAT_UNKNOWN;
std::unordered_map<ID3D11BlendState*, ID3D11BlendState*> g_blendMap;
volatile LONG g_uiDraws = 0;    // UI passes redirected since the last Present
// Scene targets the UI was found on (diagnostics: other lone binds of them are counted).
ID3D11RenderTargetView* g_knownS[4] = {}; int g_nKnown = 0;
volatile LONG g_viaPair = 0, g_viaKnown = 0, g_viaDraw = 0, g_pendingCount = 0;
// per-frame trace of UI-related events (diagnostics): P pair bind [X,S], U lone S bind taken as UI,
// L lone S bind not taken, F FinishCommandList (only on contexts that saw S this frame)
char g_trace[2048]; int g_traceLen = 0; int g_traceDumpsMissing = 12, g_traceDumpsOk = 2; int g_traceFrames = 0;
int KnownIdx(ID3D11RenderTargetView* r);
void Trace(char kind, int ctxIdx, ID3D11RenderTargetView* r = nullptr) { if (g_traceLen < (int)sizeof(g_trace) - 12) g_traceLen += snprintf(g_trace + g_traceLen, sizeof(g_trace) - g_traceLen, "%c%d:%d ", kind, ctxIdx, KnownIdx(r)); }
int KnownIdx(ID3D11RenderTargetView* r);
bool Known(ID3D11RenderTargetView* r) { for (int i = 0; i < g_nKnown; ++i) if (g_knownS[i] == r) return true; return false; }
int KnownIdx(ID3D11RenderTargetView* r) { for (int i = 0; i < g_nKnown; ++i) if (g_knownS[i] == r) return i; return -1; }
int g_log = 6;

CtxState* Get(ID3D11DeviceContext* c) {
    for (int i = 0; i < g_nctx; ++i) if (g_ctx[i].ctx == c) return &g_ctx[i];
    if (g_nctx < 64) { g_ctx[g_nctx] = {}; g_ctx[g_nctx].ctx = c; g_ctx[g_nctx].mask = 0xffffffff; MemoryBarrier(); return &g_ctx[g_nctx++]; }
    return nullptr;
}

bool LdrFormat(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM ||
           f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_R10G10B10A2_UNORM;
}
bool EnsureTarget(ID3D11RenderTargetView* scene) {
    { D3D11_RENDER_TARGET_VIEW_DESC rd0; scene->GetDesc(&rd0); if (!LdrFormat(rd0.Format) || rd0.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D) return false; }
    ID3D11Resource* r = nullptr; scene->GetResource(&r);
    if (!r) return false;
    D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) { r->Release(); return false; }
    D3D11_TEXTURE2D_DESC td; ((ID3D11Texture2D*)r)->GetDesc(&td);
    D3D11_RENDER_TARGET_VIEW_DESC rd; scene->GetDesc(&rd);
    ID3D11Device* dev = nullptr; r->GetDevice(&dev);
    r->Release();
    if (g_tex && g_w == td.Width && g_h == td.Height && g_fmt == rd.Format) { dev->Release(); return true; }
    if (g_srv) { g_srv->Release(); g_srv = nullptr; }
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_tex) { g_tex->Release(); g_tex = nullptr; }
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = td.Width; d.Height = td.Height; d.MipLevels = 1; d.ArraySize = 1; d.Format = rd.Format;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    std::vector<uint8_t> zeros((size_t)td.Width * td.Height * 8, 0);   // start transparent (8 bytes/px covers all formats used)
    D3D11_SUBRESOURCE_DATA init = { zeros.data(), td.Width * 8, 0 };
    HRESULT hr = dev->CreateTexture2D(&d, &init, &g_tex);
    if (SUCCEEDED(hr)) hr = dev->CreateRenderTargetView(g_tex, nullptr, &g_rtv);
    if (SUCCEEDED(hr)) hr = dev->CreateShaderResourceView(g_tex, nullptr, &g_srv);
    dev->Release();
    if (FAILED(hr)) { LOGE("HUD: target creation failed 0x%08lx (fmt %d)", hr, (int)rd.Format); return false; }
    g_w = td.Width; g_h = td.Height; g_fmt = rd.Format;
    LOGI("HUD: UI target %ux%u fmt %d created", g_w, g_h, (int)g_fmt);
    return true;
}

ID3D11BlendState* AlphaVariant(ID3D11DeviceContext* c, ID3D11BlendState* s) {
    auto it = g_blendMap.find(s);
    if (it != g_blendMap.end()) return it->second;
    D3D11_BLEND_DESC d = {};
    if (s) s->GetDesc(&d);
    else { d.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL; d.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
           d.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO; d.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
           d.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; d.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO; d.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD; }
    // Only RT0 matters (single target). Accumulate coverage in alpha: a = srcA + dstA * (1 - srcA).
    D3D11_RENDER_TARGET_BLEND_DESC& t = d.RenderTarget[0];
    if (t.BlendEnable) {
        t.SrcBlendAlpha = D3D11_BLEND_ONE; t.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; t.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    }
    t.RenderTargetWriteMask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    d.IndependentBlendEnable = FALSE;
    ID3D11Device* dev = nullptr; c->GetDevice(&dev);
    ID3D11BlendState* n = nullptr;
    if (dev) { dev->CreateBlendState(&d, &n); dev->Release(); }
    if (!n) { if (s) s->AddRef(); n = s; }
    g_blendMap[s] = n;
    return n;
}
} // namespace

void Hud_SetEnabled(bool on) { g_enabled = on; }
bool Hud_Enabled() { return g_enabled; }

// Deferred-context OMSetRenderTargets: returns the render target to bind instead of rtvs[0] (or null).
ID3D11RenderTargetView* Hud_OnSetRenderTargets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    std::lock_guard<std::mutex> lk(g_mx);
    CtxState* s = Get(c);
    if (!s) return nullptr;
    ID3D11RenderTargetView* r0 = (n >= 1 && rtvs) ? rtvs[0] : nullptr;
    ID3D11RenderTargetView* r1 = (n >= 2 && rtvs) ? rtvs[1] : nullptr;
    bool single = r0 && !r1 && !dsv;
    for (UINT i = 2; single && i < n; ++i) if (rtvs[i]) single = false;
    ID3D11RenderTargetView* repl = nullptr;
    bool wasUi = s->ui;
    s->ui = false;
    if (s->pending) { s->pending = false; InterlockedDecrement(&g_pendingCount); }
    bool viaPair = single && r0 == s->pairSecond;
    // diagnostics only: a lone bind of a scene target the UI was found on before, not preceded by the post
    // pass in this list (the game also draws scene passes this way, so it must not be redirected)
    int ci = (int)(s - g_ctx);
    // A lone depthless bind of a known scene target that is not right after the post pass on this context:
    // the UI recorded on another thread (title screen, battles) - or one of the game's own full-screen scene
    // passes. Decided at the next draw: UI elements are indexed quads, the scene passes plain draws.
    if (single && !viaPair && Known(r0)) {
        InterlockedIncrement(&g_viaKnown); Trace('L', ci, r0);
        if (g_enabled && EnsureTarget(r0)) { s->pending = true; InterlockedIncrement(&g_pendingCount); }
    }
    if (viaPair) Trace('U', ci, r0);
    if (g_enabled && viaPair && EnsureTarget(r0)) {
        s->ui = true; repl = g_rtv;
        InterlockedIncrement(&g_viaPair);
        if (viaPair && !Known(r0)) {
            if (g_nKnown < 4) g_knownS[g_nKnown++] = r0; else { g_knownS[0] = r0; }
            LOGI("HUD: scene target %p learned (%d known)", (void*)r0, g_nKnown);
        }
        if (!wasUi) InterlockedIncrement(&g_uiDraws);
        if (g_log > 0) { --g_log; LOGD("HUD: UI pass redirected (ctx %p, scene RTV %p)", (void*)c, (void*)r0); }
    }
    // remember the second target of a two-target, depthless bind (the post pass writing the scene colour)
    if (r0 && r1 && !dsv && n >= 2) { bool two = true; for (UINT i = 2; i < n; ++i) if (rtvs[i]) two = false; if (two) { s->pairSecond = r1; Trace('P', ci, r1); } }
    else if (!s->ui && !single) { /* keep pairSecond: other passes may come in between */ }
    return repl;
}
bool Hud_PendingAny() { return g_pendingCount > 0; }
// Deferred-context draw while a lone scene-target bind is pending. Returns true (with the target and the
// blend state to bind first) if the pass is taken as UI.
bool Hud_BeforeDraw(ID3D11DeviceContext* c, bool indexed, ID3D11RenderTargetView** rtv, ID3D11BlendState** bs, FLOAT bf[4], UINT* mask) {
    std::lock_guard<std::mutex> lk(g_mx);
    CtxState* s = nullptr;
    for (int i = 0; i < g_nctx; ++i) if (g_ctx[i].ctx == c) { s = &g_ctx[i]; break; }
    if (!s || !s->pending) return false;
    s->pending = false; InterlockedDecrement(&g_pendingCount);
    int ci = (int)(s - g_ctx);
    if (!indexed || !g_rtv) { Trace('n', ci); return false; }
    Trace('u', ci);
    s->ui = true;
    InterlockedIncrement(&g_viaDraw); InterlockedIncrement(&g_uiDraws);
    *rtv = g_rtv; *bs = AlphaVariant(c, s->blend);
    for (int i = 0; i < 4; ++i) bf[i] = s->bf[i];
    *mask = s->mask;
    return true;
}
// Deferred-context OMSetBlendState: remembers the state; returns the one to bind.
ID3D11BlendState* Hud_OnBlend(ID3D11DeviceContext* c, ID3D11BlendState* b, const FLOAT f[4], UINT m) {
    std::lock_guard<std::mutex> lk(g_mx);
    CtxState* s = Get(c);
    if (!s) return b;
    s->blend = b; s->mask = m;
    for (int i = 0; i < 4; ++i) s->bf[i] = f ? f[i] : 1.f;
    return s->ui ? AlphaVariant(c, b) : b;
}
bool Hud_InUiPass(ID3D11DeviceContext* c) {
    std::lock_guard<std::mutex> lk(g_mx);
    for (int i = 0; i < g_nctx; ++i) if (g_ctx[i].ctx == c) return g_ctx[i].ui;
    return false;
}
ID3D11BlendState* Hud_BlendFor(ID3D11DeviceContext* c, ID3D11BlendState* s) {
    std::lock_guard<std::mutex> lk(g_mx);
    return AlphaVariant(c, s);
}
// The game sometimes ends a command list between the post pass and the UI pass (title screen, battles)
// and records the UI into the next list on the same context, so the post pass's target is remembered
// across FinishCommandList and only forgotten at Present (Hud_FrameReset).
void Hud_OnFinishList(ID3D11DeviceContext* c) {
    std::lock_guard<std::mutex> lk(g_mx);
    for (int i = 0; i < g_nctx; ++i) if (g_ctx[i].ctx == c) { g_ctx[i].ui = false; if (g_ctx[i].pairSecond) Trace('F', i, g_ctx[i].pairSecond); }
}
void Hud_FrameReset(bool uiMissing) {
    std::lock_guard<std::mutex> lk(g_mx);
    ++g_traceFrames;
    bool okDump = !uiMissing && (g_traceFrames == 3500 || g_traceFrames == 3501 || g_traceFrames == 3502);
    if (g_traceLen && ((uiMissing && g_traceDumpsMissing > 0) || okDump)) {
        if (uiMissing) --g_traceDumpsMissing;
        LOGD("HUD trace (%s): %s", uiMissing ? "UI missing" : "ok", g_trace);
    }
    g_traceLen = 0; g_trace[0] = 0;
    for (int i = 0; i < g_nctx; ++i) { g_ctx[i].ui = false; g_ctx[i].pairSecond = nullptr; g_ctx[i].pending = false; }
    g_pendingCount = 0;
}

// Present: did this frame draw UI into our target? Gives the SRV; Hud_Clear() empties it afterwards.
ID3D11ShaderResourceView* Hud_FrameTexture(UINT* w, UINT* h, DXGI_FORMAT* fmt) {
    if (!g_srv) return nullptr;
    *w = g_w; *h = g_h; *fmt = g_fmt;
    return g_srv;
}
LONG Hud_TakeUiCount() { return InterlockedExchange(&g_uiDraws, 0); }
void Hud_Clear(ID3D11DeviceContext* imm) {
    if (!g_rtv) return;
    const float z[4] = { 0, 0, 0, 0 };
    imm->ClearRenderTargetView(g_rtv, z);
}
void Hud_TakeVia(LONG* pair, LONG* known, LONG* viaDraw) { *pair = InterlockedExchange(&g_viaPair, 0); *known = InterlockedExchange(&g_viaKnown, 0); *viaDraw = InterlockedExchange(&g_viaDraw, 0); }
// the game recreated its targets (resize): forget the learned ones
void Hud_ForgetTargets() { std::lock_guard<std::mutex> lk(g_mx); g_nKnown = 0; }
