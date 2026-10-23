// OpenXR session driven from the game's Present. Milestone 1: the game's back buffer is
// shown on a world-locked quad layer ("flat screen"), recentered on startup / hotkey.
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include "vrkit_menu.h"
#include <cmath>
#include <vector>
#include <intrin.h>
#include <atomic>
#include <string>
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "globals.h"
#include "MinHook.h"
#include "camera.h"
void Replay_SetContext(ID3D11DeviceContext* c);
void MapLog_Start(int); void Camera_InstLog(int); void Discovery_Rehook(); void MapLog_OnPresent(); void MapLog_Marker(char, uintptr_t);

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ---------------------------------------------------------------- state
namespace {
struct VR {
    // lifecycle
    bool disabled = false;            // permanent failure (e.g. not D3D11)
    DWORD nextInitTick = 0;
    bool loggedRuntimeMissing = false;
    IDXGISwapChain* target = nullptr; // game swapchain we follow (not AddRef'd)

    // D3D
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11DeviceContext1* ctx1 = nullptr;
    ID3DDeviceContextState* ourState = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11SamplerState* sampler = nullptr;
    ID3D11Buffer* cb = nullptr;
    ID3D11Texture2D* copyTex = nullptr;
    ID3D11ShaderResourceView* copySrv = nullptr;
    UINT copyW = 0, copyH = 0; DXGI_FORMAT copyFmt = DXGI_FORMAT_UNKNOWN;

    // OpenXR
    XrInstance inst = XR_NULL_HANDLE;
    XrSystemId sys = XR_NULL_SYSTEM_ID;
    XrSession sess = XR_NULL_HANDLE;
    XrSpace local = XR_NULL_HANDLE, view = XR_NULL_HANDLE;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false;
    std::vector<int64_t> runtimeFormats;

    XrSwapchain quad = XR_NULL_HANDLE;
    UINT quadW = 0, quadH = 0;
    DXGI_FORMAT quadFmt = DXGI_FORMAT_UNKNOWN;
    bool quadDstLinear = true;
    std::vector<ID3D11RenderTargetView*> quadRtv;

    XrPosef quadPose = { {0, 0, 0, 1}, {0, 0, -2.5f} };

    // HUD layer (single-pass stereo)
    XrSwapchain hud = XR_NULL_HANDLE;
    UINT hudW = 0, hudH = 0;
    UINT recW = 0, recH = 0, maxW = 0, maxH = 0;   // runtime's recommended per-eye size
    UINT autoW = 0, autoH = 0;                      // per-eye render size we ask the game for
    float baseW = 0, baseH = 0;                     // size at render_scale 1 (before the cap)
    float scale = 0;                                // current render scale (hotkeys), 0 = not set yet
    // settings menu (vrkit)
    XrSwapchain menuSc = XR_NULL_HANDLE; std::vector<ID3D11Texture2D*> menuImg; UINT menuW = 0, menuH = 0; bool menuReady = false;
    // stats overlay
    XrSwapchain stats = XR_NULL_HANDLE;
    std::vector<ID3D11Texture2D*> statsImg;
    ID3D11Texture2D* statsTex = nullptr;
    DXGI_FORMAT statsFmt = DXGI_FORMAT_UNKNOWN;
    bool statsOn = false, statsReady = false;
    DWORD statsTick = 0, scaleMsgUntil = 0;
    bool hudDstLinear = true;
    std::vector<ID3D11RenderTargetView*> hudRtv;
    bool hudReady = false;          // HUD image for the current stereo frame
    uint64_t hudFrames = 0, hudMissing = 0, monoFrames = 0, switches = 0;   // diagnostics (per log interval)
    bool lastStereo = false, lastUiMissing = false;
    bool needRecenter = true;
    // head-tracking reference (set on recenter): yaw-only orientation + position
    XrQuaternionf refYaw = { 0, 0, 0, 1 };
    XrVector3f refPos = { 0, 0, 0 };
    bool haveRef = false;
    XrTime lastDisplayTime = 0;
    XrDuration period = 0;
    bool cameraHooked = false;

    // native stereo
    XrSwapchain eyeSc[4] = { XR_NULL_HANDLE, XR_NULL_HANDLE, XR_NULL_HANDLE, XR_NULL_HANDLE };   // 2/3: quad-view insets
    std::vector<ID3D11Texture2D*> eyeImg[4];
    std::vector<ID3D11RenderTargetView*> eyeRtv[4];   // insets only (feathered blit)
    bool eyeDstLinear[4] = {};
    int nViews = 2, scViews = 0;   // views this frame / views the swapchains were made for
    XrFovf innerFov[2] = {};
    float viewTan[4][4] = {};
    // eye tracking (XR_EXT_eye_gaze_interaction)
    bool gazeExt = false, gazeSupported = false;
    XrActionSet gazeSet = XR_NULL_HANDLE; XrAction gazeAction = XR_NULL_HANDLE; XrSpace gazeSpace = XR_NULL_HANDLE;
    bool gazeValid = false; float gazeTx = 0, gazeTy = 0;   // gaze direction as view-space tangents
    uint64_t gazeSamples = 0, gazeMisses = 0; bool gazeValidOnce = false;
    UINT eyeW = 0, eyeH = 0;
    DXGI_FORMAT eyeFmt = DXGI_FORMAT_UNKNOWN;
    bool stFrameBegun = false;     // xrBeginFrame called for a stereo frame, xrEndFrame pending
    bool stShouldRender = false;
    bool stCaptured[4] = { false, false, false, false };
    XrView stViews[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
    XrTime stDisplayTime = 0;
    int dumpEyes = 0;          // 1 requested, 2 eye 0 written (waiting for eye 1)
    bool loggedViews = false;

    // stats
    uint64_t frames = 0, xrFrames = 0;
    DWORD statTick = 0;
};
VR g;
}
bool VR_Gaze(float* tx, float* ty);
// ---- quad views state (runtime toggle + centre)
static bool g_quadOn = false, g_quadInit = false;
static bool Quad_Active() {
    if (!g_quadInit) { g_quadInit = true; g_quadOn = g_cfg.quadViews; }
    return g_quadOn && g_cfg.symmetricFov;
}
// where the inset is centred, as view-space tangents (gaze if available and enabled, + offset)
static void Quad_Center(float th, float tv, float* x, float* y) {
    *x = g_cfg.quadCenterX * th; *y = g_cfg.quadCenterY * tv;
    float gx, gy;
    if (g_cfg.quadFollowGaze && VR_Gaze(&gx, &gy)) { *x += gx; *y += gy; }
}
void Hud_TakeVia(LONG*, LONG*, LONG*); void Hud_ForgetTargets(); void Hud_FrameReset(bool uiMissing);
void Passes_Start(); void Passes_OnPresent(); void PostFx_LogStats(); LONG PostFx_TakeDrawCount(); LONG Camera_TakeFovFixes(); void Vrs_SetActive(bool); bool Vrs_Active();
void Perf_OnPresent(); double Perf_Now(); void Perf_BeforeReplay(); void Perf_AfterReplay(); void Perf_AddPresentWork(double); void Perf_AddXrWait(double);

// ---------------------------------------------------------------- helpers
static const char* XrStr(XrResult r) {
    static char buf[XR_MAX_RESULT_STRING_SIZE];
    if (g.inst && XR_SUCCEEDED(xrResultToString(g.inst, r, buf))) return buf;
    snprintf(buf, sizeof(buf), "XrResult(%d)", (int)r);
    return buf;
}
#define XR_CHECK(call)                                                       \
    do { XrResult _r = (call);                                               \
         if (XR_FAILED(_r)) { LOGE("%s failed: %s", #call, XrStr(_r)); return false; } } while (0)

static bool IsSrgb(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}
static DXGI_FORMAT RawOf(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
    default: return f;
    }
}
// Does sampling this format give linear-light values (vs gamma-encoded)?
static bool SourceIsLinear(DXGI_FORMAT f) {
    return IsSrgb(f) || f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R32G32B32A32_FLOAT;
}

static const char* kShader = R"(
Texture2D tex : register(t0);
SamplerState smp : register(s0);
cbuffer Params : register(b0) { int mode; int keepAlpha; float feather; int pad; };
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vsMain(uint id : SV_VertexID) {
    V o; o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float3 ToLinear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float3 ToSrgb(float3 c) { c = saturate(c); return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }
float4 psMain(V i) : SV_Target {
    float4 s = tex.Sample(smp, i.uv);
    float3 c = s.rgb;
    float a = keepAlpha ? saturate(s.a) : 1;
    if (keepAlpha && a > 0.0001) c /= a;           // premultiplied in, premultiplied out
    if (mode == 1) c = ToSrgb(c); else if (mode == 2) c = ToLinear(c);
    if (keepAlpha) c *= a;
    if (feather > 0) {   // quad-view inset: fade out towards the edges (premultiplied)
        float2 e2 = min(i.uv, 1 - i.uv);
        float f = smoothstep(0, feather, min(e2.x, e2.y));
        return float4(c * f, f);
    }
    return float4(c, a);
}
)";

// ---------------------------------------------------------------- D3D resources
static bool CreateBlitResources() {
    HMODULE comp = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!comp) { LOGE("d3dcompiler_47.dll not found"); return false; }
    auto compile = (pD3DCompile)GetProcAddress(comp, "D3DCompile");
    if (!compile) { LOGE("D3DCompile not found"); return false; }
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    HRESULT hr = compile(kShader, strlen(kShader), "blit", nullptr, nullptr, "vsMain", "vs_5_0", 0, 0, &vsb, &err);
    if (FAILED(hr)) { LOGE("VS compile: %s", err ? (char*)err->GetBufferPointer() : "?"); SafeRelease(err); return false; }
    hr = compile(kShader, strlen(kShader), "blit", nullptr, nullptr, "psMain", "ps_5_0", 0, 0, &psb, &err);
    if (FAILED(hr)) { LOGE("PS compile: %s", err ? (char*)err->GetBufferPointer() : "?"); SafeRelease(err); SafeRelease(vsb); return false; }
    g.dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g.vs);
    g.dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g.ps);
    vsb->Release(); psb->Release();

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    g.dev->CreateSamplerState(&sd, &g.sampler);

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g.dev->CreateBuffer(&bd, nullptr, &g.cb);

    ID3D11Device1* dev1 = nullptr;
    if (FAILED(g.dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&dev1))) { LOGE("ID3D11Device1 unavailable"); return false; }
    D3D_FEATURE_LEVEL fl = g.dev->GetFeatureLevel();
    UINT flags = (g.dev->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
    hr = dev1->CreateDeviceContextState(flags, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &g.ourState);
    dev1->Release();
    if (FAILED(hr)) { LOGE("CreateDeviceContextState failed 0x%08lx", hr); return false; }
    if (FAILED(g.ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&g.ctx1))) { LOGE("ID3D11DeviceContext1 unavailable"); return false; }
    return g.vs && g.ps && g.sampler && g.cb;
}

static bool EnsureCopyTexture(const D3D11_TEXTURE2D_DESC& bb) {
    if (g.copyTex && g.copyW == bb.Width && g.copyH == bb.Height && g.copyFmt == bb.Format) return true;
    SafeRelease(g.copySrv); SafeRelease(g.copyTex);
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = bb.Width; d.Height = bb.Height; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = bb.Format; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = g.dev->CreateTexture2D(&d, nullptr, &g.copyTex);
    if (FAILED(hr)) { LOGE("Copy texture create failed 0x%08lx (fmt %d)", hr, (int)bb.Format); return false; }
    hr = g.dev->CreateShaderResourceView(g.copyTex, nullptr, &g.copySrv);
    if (FAILED(hr)) { LOGE("Copy SRV create failed 0x%08lx", hr); SafeRelease(g.copyTex); return false; }
    g.copyW = bb.Width; g.copyH = bb.Height; g.copyFmt = bb.Format;
    LOGI("Back buffer copy: %ux%u fmt %d (samples %u)", bb.Width, bb.Height, (int)bb.Format, bb.SampleDesc.Count);
    return true;
}

// ---------------------------------------------------------------- OpenXR setup / teardown
static void DestroyHudSwapchain() {
    for (auto r : g.hudRtv) if (r) r->Release();
    g.hudRtv.clear();
    if (g.hud) { xrDestroySwapchain(g.hud); g.hud = XR_NULL_HANDLE; }
    g.hudW = g.hudH = 0; g.hudReady = false;
}
static void DestroyStatsSwapchain() {
    if (g.statsTex) { g.statsTex->Release(); g.statsTex = nullptr; }
    g.statsImg.clear();
    if (g.stats) { xrDestroySwapchain(g.stats); g.stats = XR_NULL_HANDLE; }
    g.statsReady = false;
}
static void DestroyMenuSwapchain();
static void DestroyQuadSwapchain() {
    DestroyHudSwapchain();
    DestroyStatsSwapchain();
    DestroyMenuSwapchain();
    for (auto r : g.quadRtv) if (r) r->Release();
    g.quadRtv.clear();
    if (g.quad) { xrDestroySwapchain(g.quad); g.quad = XR_NULL_HANDLE; }
    g.quadW = g.quadH = 0;
}

static void DestroyEyeSwapchains();
static void DestroyXr(const char* why) {
    LOGI("Tearing down OpenXR (%s)", why);
    DestroyQuadSwapchain();
    DestroyEyeSwapchains();
    g.stFrameBegun = false;
    if (g.gazeSpace) { xrDestroySpace(g.gazeSpace); g.gazeSpace = XR_NULL_HANDLE; }
    if (g.gazeSet) { xrDestroyActionSet(g.gazeSet); g.gazeSet = XR_NULL_HANDLE; g.gazeAction = XR_NULL_HANDLE; }
    g.gazeValid = false;
    if (g.view) { xrDestroySpace(g.view); g.view = XR_NULL_HANDLE; }
    if (g.local) { xrDestroySpace(g.local); g.local = XR_NULL_HANDLE; }
    if (g.sess) { xrDestroySession(g.sess); g.sess = XR_NULL_HANDLE; }
    if (g.inst) { xrDestroyInstance(g.inst); g.inst = XR_NULL_HANDLE; }
    g.sys = XR_NULL_SYSTEM_ID;
    g.running = false;
    g.state = XR_SESSION_STATE_UNKNOWN;
    g.needRecenter = true;
    g.loggedViews = false;
    g.nextInitTick = GetTickCount() + 3000;
}

static bool CreateQuadSwapchain(UINT w, UINT h) {
    DestroyQuadSwapchain();
    static const DXGI_FORMAT prefs[] = { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                                         DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM };
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    for (auto p : prefs) {
        for (auto rf : g.runtimeFormats) if (rf == (int64_t)p) { fmt = p; break; }
        if (fmt != DXGI_FORMAT_UNKNOWN) break;
    }
    if (fmt == DXGI_FORMAT_UNKNOWN) { LOGE("Runtime supports none of the preferred swapchain formats"); return false; }

    XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = fmt; ci.sampleCount = 1; ci.width = w; ci.height = h;
    ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
    XR_CHECK(xrCreateSwapchain(g.sess, &ci, &g.quad));

    uint32_t n = 0;
    XR_CHECK(xrEnumerateSwapchainImages(g.quad, 0, &n, nullptr));
    std::vector<XrSwapchainImageD3D11KHR> imgs(n, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
    XR_CHECK(xrEnumerateSwapchainImages(g.quad, n, &n, (XrSwapchainImageBaseHeader*)imgs.data()));

    // Prefer writing raw (gamma-encoded) values through a UNORM view of an sRGB image; this
    // works when the runtime allocates typeless textures, as the D3D11 binding spec requires.
    bool raw = IsSrgb(fmt);
    for (uint32_t i = 0; i < n; ++i) {
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rd.Format = raw ? RawOf(fmt) : fmt;
        ID3D11RenderTargetView* rtv = nullptr;
        HRESULT hr = g.dev->CreateRenderTargetView(imgs[i].texture, &rd, &rtv);
        if (FAILED(hr) && raw) {
            raw = false; rd.Format = fmt;
            for (auto r : g.quadRtv) r->Release();
            g.quadRtv.clear(); i = (uint32_t)-1; continue;   // restart with typed views
        }
        if (FAILED(hr)) { LOGE("Quad RTV create failed 0x%08lx", hr); return false; }
        g.quadRtv.push_back(rtv);
    }
    g.quadFmt = fmt; g.quadW = w; g.quadH = h;
    // Encoded output when we write raw into an sRGB image; otherwise the runtime/RTV expects linear.
    g.quadDstLinear = !(IsSrgb(fmt) && raw);
    LOGI("Quad swapchain %ux%u fmt %d, %u images, %s writes", w, h, (int)fmt, n, raw ? "raw-unorm" : "typed");
    return true;
}

static bool CreateHudSwapchain(UINT w, UINT h) {
    DestroyHudSwapchain();
    static const DXGI_FORMAT prefs[] = { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                                         DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM };
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    for (auto p : prefs) {
        for (auto rf : g.runtimeFormats) if (rf == (int64_t)p) { fmt = p; break; }
        if (fmt != DXGI_FORMAT_UNKNOWN) break;
    }
    if (fmt == DXGI_FORMAT_UNKNOWN) return false;
    XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = fmt; ci.sampleCount = 1; ci.width = w; ci.height = h;
    ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
    XR_CHECK(xrCreateSwapchain(g.sess, &ci, &g.hud));
    uint32_t n = 0;
    XR_CHECK(xrEnumerateSwapchainImages(g.hud, 0, &n, nullptr));
    std::vector<XrSwapchainImageD3D11KHR> imgs(n, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
    XR_CHECK(xrEnumerateSwapchainImages(g.hud, n, &n, (XrSwapchainImageBaseHeader*)imgs.data()));
    bool raw = IsSrgb(fmt);
    for (uint32_t i = 0; i < n; ++i) {
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rd.Format = raw ? RawOf(fmt) : fmt;
        ID3D11RenderTargetView* rtv = nullptr;
        HRESULT hr = g.dev->CreateRenderTargetView(imgs[i].texture, &rd, &rtv);
        if (FAILED(hr) && raw) {
            raw = false;
            for (auto r : g.hudRtv) r->Release();
            g.hudRtv.clear(); i = (uint32_t)-1; continue;
        }
        if (FAILED(hr)) { LOGE("HUD RTV create failed 0x%08lx", hr); return false; }
        g.hudRtv.push_back(rtv);
    }
    g.hudW = w; g.hudH = h;
    g.hudDstLinear = !(IsSrgb(fmt) && raw);
    LOGI("HUD swapchain %ux%u fmt %d, %u images, %s writes", w, h, (int)fmt, n, raw ? "raw-unorm" : "typed");
    return true;
}

ID3D11ShaderResourceView* Hud_FrameTexture(UINT* w, UINT* h, DXGI_FORMAT* fmt);
LONG Hud_TakeUiCount(); void Hud_Clear(ID3D11DeviceContext* imm);
// Copies the separated UI into the HUD swapchain (alpha kept, premultiplied).
static bool BlitHud() {
    UINT w = 0, h = 0; DXGI_FORMAT fmt;
    ID3D11ShaderResourceView* srv = Hud_FrameTexture(&w, &h, &fmt);
    if (!srv || !g.sess) return false;
    if (!g.hud || g.hudW != w || g.hudH != h) if (!CreateHudSwapchain(w, h)) return false;
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (XR_FAILED(xrAcquireSwapchainImage(g.hud, &ai, &idx))) return false;
    XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = 100000000;
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    if (XR_FAILED(xrWaitSwapchainImage(g.hud, &wi))) { xrReleaseSwapchainImage(g.hud, &ri); return false; }
    bool srcLinear = SourceIsLinear(fmt);
    int mode[4] = { srcLinear == g.hudDstLinear ? 0 : (srcLinear ? 1 : 2), 1, 0, 0 };
    ID3DDeviceContextState* prev = nullptr;
    g.ctx1->SwapDeviceContextState(g.ourState, &prev);
    g.ctx->UpdateSubresource(g.cb, 0, nullptr, mode, 0, 0);
    D3D11_VIEWPORT vp = { 0, 0, (float)g.hudW, (float)g.hudH, 0, 1 };
    g.ctx->RSSetViewports(1, &vp);
    g.ctx->OMSetRenderTargets(1, &g.hudRtv[idx], nullptr);
    g.ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->IASetInputLayout(nullptr);
    g.ctx->VSSetShader(g.vs, nullptr, 0);
    g.ctx->PSSetShader(g.ps, nullptr, 0);
    g.ctx->PSSetShaderResources(0, 1, &srv);
    g.ctx->PSSetSamplers(0, 1, &g.sampler);
    g.ctx->PSSetConstantBuffers(0, 1, &g.cb);
    g.ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    g.ctx->PSSetShaderResources(0, 1, &nullSrv);
    g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    g.ctx1->SwapDeviceContextState(prev, nullptr);
    SafeRelease(prev);
    xrReleaseSwapchainImage(g.hud, &ri);
    return true;
}

static Vec3f QRot(Quat q, Vec3f v);
// ---- eye tracking: an action set with one pose action bound to the eye-gaze interaction profile
static void InitGaze() {
    XrActionSetCreateInfo asci = { XR_TYPE_ACTION_SET_CREATE_INFO };
    strcpy_s(asci.actionSetName, "granbluevr_gaze"); strcpy_s(asci.localizedActionSetName, "GranblueVR eye gaze");
    if (XR_FAILED(xrCreateActionSet(g.inst, &asci, &g.gazeSet))) { LOGE("Eye tracking: action set failed"); return; }
    XrActionCreateInfo aci = { XR_TYPE_ACTION_CREATE_INFO };
    aci.actionType = XR_ACTION_TYPE_POSE_INPUT;
    strcpy_s(aci.actionName, "gaze"); strcpy_s(aci.localizedActionName, "Eye gaze");
    if (XR_FAILED(xrCreateAction(g.gazeSet, &aci, &g.gazeAction))) { LOGE("Eye tracking: action failed"); return; }
    XrPath profile = 0, path = 0;
    xrStringToPath(g.inst, "/interaction_profiles/ext/eye_gaze_interaction", &profile);
    xrStringToPath(g.inst, "/user/eyes_ext/input/gaze_ext/pose", &path);
    XrActionSuggestedBinding b = { g.gazeAction, path };
    XrInteractionProfileSuggestedBinding sb = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    sb.interactionProfile = profile; sb.countSuggestedBindings = 1; sb.suggestedBindings = &b;
    XrResult r = xrSuggestInteractionProfileBindings(g.inst, &sb);
    if (XR_FAILED(r)) { LOGE("Eye tracking: binding failed: %s", XrStr(r)); return; }
    XrSessionActionSetsAttachInfo ai = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    ai.countActionSets = 1; ai.actionSets = &g.gazeSet;
    r = xrAttachSessionActionSets(g.sess, &ai);
    if (XR_FAILED(r)) { LOGE("Eye tracking: attach failed: %s", XrStr(r)); return; }
    XrActionSpaceCreateInfo sci = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
    sci.action = g.gazeAction; sci.poseInActionSpace.orientation.w = 1;
    r = xrCreateActionSpace(g.sess, &sci, &g.gazeSpace);
    if (XR_FAILED(r)) { LOGE("Eye tracking: action space failed: %s", XrStr(r)); g.gazeSpace = XR_NULL_HANDLE; return; }
    LOGI("Eye tracking: ready");
}
// per frame (stereo begin, after xrWaitFrame): gaze direction in view space as tangents
static bool g_eyeTracking = true, g_eyeInit = false;
static void UpdateGaze(XrTime t) {
    g.gazeValid = false;
    if (!g_eyeInit) { g_eyeInit = true; g_eyeTracking = g_cfg.eyeTracking; }
    if (!g_eyeTracking) return;
    if (g_cfg.debugFakeGaze > 0.f) {   // test without an eye tracker: the gaze circles around the centre
        double s = GetTickCount64() / 1000.0;
        float a = g_cfg.debugFakeGaze * 0.0174533f;
        g.gazeTx = tanf(a * (float)cos(s * 0.7)); g.gazeTy = tanf(a * 0.6f * (float)sin(s * 0.7));
        g.gazeValid = true; return;
    }
    if (!g.gazeSpace || !g.running) return;
    XrActiveActionSet aas = { g.gazeSet, XR_NULL_PATH };
    XrActionsSyncInfo si = { XR_TYPE_ACTIONS_SYNC_INFO }; si.countActiveActionSets = 1; si.activeActionSets = &aas;
    if (XR_FAILED(xrSyncActions(g.sess, &si))) { ++g.gazeMisses; return; }
    XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
    if (XR_FAILED(xrLocateSpace(g.gazeSpace, g.view, t, &loc)) || !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) { ++g.gazeMisses; return; }
    Quat q{ loc.pose.orientation.x, loc.pose.orientation.y, loc.pose.orientation.z, loc.pose.orientation.w };
    Vec3f fwd = QRot(q, Vec3f{ 0, 0, -1 });
    if (fwd.z > -0.1f) { ++g.gazeMisses; return; }
    float tx = fwd.x / -fwd.z, ty = fwd.y / -fwd.z;
    // light smoothing against tracker noise
    float k = g.gazeValidOnce ? 0.5f : 1.f; g.gazeValidOnce = true;
    g.gazeTx += (tx - g.gazeTx) * k; g.gazeTy += (ty - g.gazeTy) * k;
    g.gazeValid = true; ++g.gazeSamples;
}
bool VR_Gaze(float* tx, float* ty) { if (!g.gazeValid) return false; *tx = g.gazeTx; *ty = g.gazeTy; return true; }
static bool InitXr(IDXGISwapChain* sc) {
    // Instance
    uint32_t extCount = 0;
    XrResult r = xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    if (XR_FAILED(r)) {
        if (!g.loggedRuntimeMissing) LOGE("No OpenXR runtime available (%d). Is SteamVR / your OpenXR runtime installed and set active? Will keep retrying.", (int)r);
        g.loggedRuntimeMissing = true;
        return false;
    }
    std::vector<XrExtensionProperties> exts(extCount, { XR_TYPE_EXTENSION_PROPERTIES });
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());
    bool hasD3D11 = false;
    static bool loggedExts = false;
    for (auto& e : exts) {
        if (!loggedExts) LOGD("  runtime ext: %s v%u", e.extensionName, e.extensionVersion);
        if (!strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME)) hasD3D11 = true;
    }
    loggedExts = true;
    if (!hasD3D11) { LOGE("Runtime lacks %s", XR_KHR_D3D11_ENABLE_EXTENSION_NAME); g.disabled = true; return false; }

    g.gazeExt = false;
    for (auto& e : exts) if (!strcmp(e.extensionName, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME)) g.gazeExt = true;
    if (!g.gazeExt) LOGI("Eye tracking: runtime has no %s - gaze-following foveation unavailable", XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
    const char* enabled[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME };
    XrInstanceCreateInfo ici = { XR_TYPE_INSTANCE_CREATE_INFO };
    strcpy_s(ici.applicationInfo.applicationName, "GranblueVR");
    ici.applicationInfo.applicationVersion = 1;
    strcpy_s(ici.applicationInfo.engineName, "GranblueVR");
    ici.applicationInfo.engineVersion = 1;
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = g.gazeExt ? 2 : 1; ici.enabledExtensionNames = enabled;
    r = xrCreateInstance(&ici, &g.inst);
    if (XR_FAILED(r)) { LOGE("xrCreateInstance failed: %d", (int)r); g.inst = XR_NULL_HANDLE; return false; }
    g.loggedRuntimeMissing = false;

    XrInstanceProperties ip = { XR_TYPE_INSTANCE_PROPERTIES };
    xrGetInstanceProperties(g.inst, &ip);
    LOGI("OpenXR runtime: %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
         XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));

    XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = xrGetSystem(g.inst, &sgi, &g.sys);
    if (XR_FAILED(r)) {
        LOGI("xrGetSystem: %s (headset not ready?) - retrying", XrStr(r));
        xrDestroyInstance(g.inst); g.inst = XR_NULL_HANDLE;
        return false;
    }
    XrSystemProperties sp = { XR_TYPE_SYSTEM_PROPERTIES };
    XrSystemEyeGazeInteractionPropertiesEXT egp = { XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT };
    if (g.gazeExt) sp.next = &egp;
    xrGetSystemProperties(g.inst, g.sys, &sp);
    g.gazeSupported = g.gazeExt && egp.supportsEyeGazeInteraction;
    if (g.gazeExt) LOGI("Eye tracking: %s", g.gazeSupported ? "supported by this headset/runtime" : "extension present, but the system reports no eye tracker");
    LOGI("System: %s (vendor 0x%x), max swapchain %ux%u", sp.systemName, sp.vendorId,
         sp.graphicsProperties.maxSwapchainImageWidth, sp.graphicsProperties.maxSwapchainImageHeight);

    uint32_t nv = 0;
    xrEnumerateViewConfigurationViews(g.inst, g.sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &nv, nullptr);
    std::vector<XrViewConfigurationView> vcv(nv, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
    xrEnumerateViewConfigurationViews(g.inst, g.sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, nv, &nv, vcv.data());
    if (nv >= 1) { g.recW = vcv[0].recommendedImageRectWidth; g.recH = vcv[0].recommendedImageRectHeight; g.maxW = vcv[0].maxImageRectWidth; g.maxH = vcv[0].maxImageRectHeight; }
    for (uint32_t i = 0; i < nv; ++i)
        LOGI("View %u: recommended %ux%u (max %ux%u)", i, vcv[i].recommendedImageRectWidth,
             vcv[i].recommendedImageRectHeight, vcv[i].maxImageRectWidth, vcv[i].maxImageRectHeight);

    // Graphics requirements: the runtime must be on the same adapter as the game.
    PFN_xrGetD3D11GraphicsRequirementsKHR getReq = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(g.inst, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&getReq));
    XrGraphicsRequirementsD3D11KHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
    XR_CHECK(getReq(g.inst, g.sys, &req));
    IDXGIDevice* dxgiDev = nullptr; IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(g.dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev)) && SUCCEEDED(dxgiDev->GetAdapter(&adapter))) {
        DXGI_ADAPTER_DESC ad; adapter->GetDesc(&ad);
        bool same = ad.AdapterLuid.LowPart == req.adapterLuid.LowPart && ad.AdapterLuid.HighPart == req.adapterLuid.HighPart;
        char name[128]; WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof(name), nullptr, nullptr);
        LOGI("Game adapter: %s, %s runtime adapter", name, same ? "matches" : "DOES NOT MATCH");
        if (!same) LOGE("Game is rendering on a different GPU than the headset runtime; session creation will likely fail");
    }
    SafeRelease(adapter); SafeRelease(dxgiDev);
    LOGI("Game feature level 0x%x (runtime min 0x%x)", g.dev->GetFeatureLevel(), req.minFeatureLevel);

    // Session on the game's own device.
    XrGraphicsBindingD3D11KHR gb = { XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
    gb.device = g.dev;
    XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO };
    sci.next = &gb; sci.systemId = g.sys;
    XR_CHECK(xrCreateSession(g.inst, &sci, &g.sess));

    XrReferenceSpaceCreateInfo rs = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rs.poseInReferenceSpace.orientation.w = 1;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XR_CHECK(xrCreateReferenceSpace(g.sess, &rs, &g.local));
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XR_CHECK(xrCreateReferenceSpace(g.sess, &rs, &g.view));
    if (g.gazeSupported) InitGaze();

    uint32_t nf = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(g.sess, 0, &nf, nullptr));
    g.runtimeFormats.resize(nf);
    XR_CHECK(xrEnumerateSwapchainFormats(g.sess, nf, &nf, g.runtimeFormats.data()));
    std::string fl;
    for (auto f : g.runtimeFormats) fl += std::to_string(f) + " ";
    LOGD("Runtime swapchain formats: %s", fl.c_str());

    LOGI("OpenXR session created; waiting for READY");
    return true;
}

void MenuOptions_Init(ID3D11Device* dev, ID3D11DeviceContext* imm);
static void MenuLayer(XrCompositionLayerQuad& q);
static bool InitD3D(IDXGISwapChain* sc) {
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&g.dev))) {
        LOGE("Game swapchain is not backed by a D3D11 device; disabling");
        g.disabled = true;
        return false;
    }
    g.dev->GetImmediateContext(&g.ctx);
    ID3D11Multithread* mt = nullptr;
    if (SUCCEEDED(g.ctx->QueryInterface(__uuidof(ID3D11Multithread), (void**)&mt))) {
        BOOL was = mt->SetMultithreadProtected(TRUE);
        LOGD("D3D11 multithread protection enabled (was %d)", was);
        mt->Release();
    }
    DXGI_SWAP_CHAIN_DESC d;
    if (SUCCEEDED(sc->GetDesc(&d)))
        LOGI("Game swapchain: %ux%u fmt %d, %u buffers, swap effect %d, windowed %d, flags 0x%x",
             d.BufferDesc.Width, d.BufferDesc.Height, (int)d.BufferDesc.Format, d.BufferCount,
             (int)d.SwapEffect, d.Windowed, d.Flags);
    if (!CreateBlitResources()) { LOGE("Blit resource creation failed; disabling"); g.disabled = true; return false; }
    Discovery_Init(g.dev, g.ctx);
    Replay_SetContext(g.ctx);
    MenuOptions_Init(g.dev, g.ctx);
    return true;
}

// ---------------------------------------------------------------- per-frame
static void PollEvents() {
    XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
    while (g.inst && xrPollEvent(g.inst, &ev) == XR_SUCCESS) {
        switch (ev.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            auto* e = (XrEventDataSessionStateChanged*)&ev;
            g.state = e->state;
            LOGI("Session state -> %d", (int)e->state);
            if (e->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                XrResult r = xrBeginSession(g.sess, &bi);
                g.running = XR_SUCCEEDED(r);
                if (!g.running) LOGE("xrBeginSession: %s", XrStr(r)); else LOGI("Session running");
            } else if (e->state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(g.sess);
                g.running = false;
            } else if (e->state == XR_SESSION_STATE_EXITING || e->state == XR_SESSION_STATE_LOSS_PENDING) {
                DestroyXr(e->state == XR_SESSION_STATE_EXITING ? "session exiting" : "session loss pending");
                return;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            DestroyXr("instance loss pending");
            return;
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
            LOGI("Reference space change pending; will recenter");
            g.needRecenter = true;
            break;
        default: break;
        }
        ev = { XR_TYPE_EVENT_DATA_BUFFER };
    }
}

static void Recenter(XrTime t) {
    XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
    if (XR_FAILED(xrLocateSpace(g.view, g.local, t, &loc)) ||
        !(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ||
        !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return;
    const XrQuaternionf& q = loc.pose.orientation;
    // forward = q * (0,0,-1)
    float fx = -(2 * (q.x * q.z + q.w * q.y));
    float fz = -(1 - 2 * (q.x * q.x + q.y * q.y));
    float yaw = atan2f(-fx, -fz);
    float d = g_cfg.screenDistance;
    g.quadPose.orientation = { 0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f) };
    g.quadPose.position = { loc.pose.position.x - sinf(yaw) * d, loc.pose.position.y, loc.pose.position.z - cosf(yaw) * d };
    g.refYaw = { 0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f) };
    g.refPos = loc.pose.position;
    g.haveRef = true;
    g.needRecenter = false;
    LOGI("Recentered: head (%.2f, %.2f, %.2f) yaw %.1f deg", loc.pose.position.x, loc.pose.position.y,
         loc.pose.position.z, yaw * 57.2958f);
}

static void LogViewsOnce(XrTime t) {
    XrViewLocateInfo li = { XR_TYPE_VIEW_LOCATE_INFO };
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = t; li.space = g.view;
    XrViewState vs = { XR_TYPE_VIEW_STATE };
    XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
    uint32_t n = 0;
    if (XR_FAILED(xrLocateViews(g.sess, &li, &vs, 2, &n, views)) || n < 2 ||
        !(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) return;
    for (uint32_t i = 0; i < n; ++i) {
        const XrView& v = views[i];
        LOGI("Eye %u: fov L%.1f R%.1f U%.1f D%.1f deg, pos (%.4f, %.4f, %.4f), rot (%.4f, %.4f, %.4f, %.4f)", i,
             v.fov.angleLeft * 57.2958f, v.fov.angleRight * 57.2958f, v.fov.angleUp * 57.2958f, v.fov.angleDown * 57.2958f,
             v.pose.position.x, v.pose.position.y, v.pose.position.z,
             v.pose.orientation.x, v.pose.orientation.y, v.pose.orientation.z, v.pose.orientation.w);
    }
    float dx = views[1].pose.position.x - views[0].pose.position.x;
    LOGI("IPD %.1f mm", fabsf(dx) * 1000.f);
    g.loggedViews = true;
}

// Copies the game's current back buffer into the quad swapchain.
static bool BlitToQuad(IDXGISwapChain* sc) {
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) return false;
    D3D11_TEXTURE2D_DESC bd; bb->GetDesc(&bd);
    if (!EnsureCopyTexture(bd)) { bb->Release(); return false; }
    if (bd.SampleDesc.Count > 1) g.ctx->ResolveSubresource(g.copyTex, 0, bb, 0, bd.Format);
    else g.ctx->CopyResource(g.copyTex, bb);
    bb->Release();

    if (!g.quad || g.quadW != bd.Width || g.quadH != bd.Height)
        if (!CreateQuadSwapchain(bd.Width, bd.Height)) return false;

    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (XR_FAILED(xrAcquireSwapchainImage(g.quad, &ai, &idx))) return false;
    XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = 100000000;  // 100 ms
    if (XR_FAILED(xrWaitSwapchainImage(g.quad, &wi))) {
        XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        xrReleaseSwapchainImage(g.quad, &ri);
        return false;
    }

    bool srcLinear = SourceIsLinear(bd.Format);
    int mode[4] = { srcLinear == g.quadDstLinear ? 0 : (srcLinear ? 1 : 2), 0, 0, 0 };

    ID3DDeviceContextState* prev = nullptr;
    g.ctx1->SwapDeviceContextState(g.ourState, &prev);
    g.ctx->UpdateSubresource(g.cb, 0, nullptr, mode, 0, 0);
    D3D11_VIEWPORT vp = { 0, 0, (float)g.quadW, (float)g.quadH, 0, 1 };
    g.ctx->RSSetViewports(1, &vp);
    g.ctx->OMSetRenderTargets(1, &g.quadRtv[idx], nullptr);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->IASetInputLayout(nullptr);
    g.ctx->VSSetShader(g.vs, nullptr, 0);
    g.ctx->PSSetShader(g.ps, nullptr, 0);
    g.ctx->PSSetShaderResources(0, 1, &g.copySrv);
    g.ctx->PSSetSamplers(0, 1, &g.sampler);
    g.ctx->PSSetConstantBuffers(0, 1, &g.cb);
    g.ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    g.ctx->PSSetShaderResources(0, 1, &nullSrv);
    g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    g.ctx1->SwapDeviceContextState(prev, nullptr);
    SafeRelease(prev);

    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(g.quad, &ri);
    return true;
}

static void RunXrFrame(IDXGISwapChain* sc) {
    XrFrameWaitInfo fwi = { XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState fs = { XR_TYPE_FRAME_STATE };
    XrResult r = xrWaitFrame(g.sess, &fwi, &fs);
    if (XR_FAILED(r)) { LOGE("xrWaitFrame: %s", XrStr(r)); return; }
    g.lastDisplayTime = fs.predictedDisplayTime;
    g.period = fs.predictedDisplayPeriod;
    XrFrameBeginInfo fbi = { XR_TYPE_FRAME_BEGIN_INFO };
    r = xrBeginFrame(g.sess, &fbi);
    if (XR_FAILED(r)) { LOGE("xrBeginFrame: %s", XrStr(r)); return; }

    if (GetAsyncKeyState(g_cfg.recenterKey) & 1) { LOGI("Recenter key pressed"); g.needRecenter = true; }
    if (g.needRecenter) Recenter(fs.predictedDisplayTime);
    if (!g.loggedViews) LogViewsOnce(fs.predictedDisplayTime);

    XrCompositionLayerQuad quad = { XR_TYPE_COMPOSITION_LAYER_QUAD }, mq = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    const XrCompositionLayerBaseHeader* layers[2];
    uint32_t layerCount = 0;
    if (fs.shouldRender && BlitToQuad(sc)) {
        quad.space = g.local;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = g.quad;
        quad.subImage.imageRect = { { 0, 0 }, { (int32_t)g.quadW, (int32_t)g.quadH } };
        quad.pose = g.quadPose;
        quad.size = { g_cfg.screenWidth, g_cfg.screenWidth * (float)g.quadH / (float)g.quadW };
        layers[layerCount++] = (XrCompositionLayerBaseHeader*)&quad;
    }
    if (fs.shouldRender && g.menuReady && g.menuSc && g.view) { MenuLayer(mq); layers[layerCount++] = (XrCompositionLayerBaseHeader*)&mq; }
    XrFrameEndInfo fei = { XR_TYPE_FRAME_END_INFO };
    fei.displayTime = fs.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = layerCount; fei.layers = layers;
    r = xrEndFrame(g.sess, &fei);
    if (XR_FAILED(r)) LOGE("xrEndFrame: %s", XrStr(r));
    else ++g.xrFrames;
}

// ---------------------------------------------------------------- head pose for the camera hook
static Quat QMul(Quat a, Quat b) {
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
             a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
             a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
static Vec3f QRot(Quat q, Vec3f v) {
    Quat p{ v.x, v.y, v.z, 0 };
    Quat c{ -q.x, -q.y, -q.z, q.w };
    Quat r = QMul(QMul(q, p), c);
    return { r.x, r.y, r.z };
}

bool VR_GetHeadPose(Pose* out) {
    if (g_cfg.debugFakeHead > 0.f || g_cfg.debugHeadYaw != 0.f) {
        double t = GetTickCount64() / 1000.0;
        float yaw = g_cfg.debugFakeHead * 0.0174533f * (float)sin(t * 0.8) + g_cfg.debugHeadYaw * 0.0174533f;
        float pitch = g_cfg.debugFakeHead * 0.3f * 0.0174533f * (float)sin(t * 0.5);
        Quat qy{ 0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f) };
        Quat qp{ sinf(pitch * 0.5f), 0, 0, cosf(pitch * 0.5f) };
        out->ori = QMul(qy, qp);
        float a = g_cfg.debugFakeHead > 0.f ? 1.f : 0.f;
        out->pos = { a * 0.15f * (float)sin(t * 0.6), a * 0.05f * (float)sin(t * 0.9), 0.f };
        return true;
    }
    if (g_cfg.vrMode == "flat") return false;   // flat-screen mode keeps the game camera untouched
    if (!g.sess || !g.running || !g.lastDisplayTime || !g.haveRef) return false;
    XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
    if (XR_FAILED(xrLocateSpace(g.view, g.local, g.lastDisplayTime + g.period, &loc))) return false;
    if (!(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) || !(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) return false;
    Quat inv{ -g.refYaw.x, -g.refYaw.y, -g.refYaw.z, g.refYaw.w };
    Quat o{ loc.pose.orientation.x, loc.pose.orientation.y, loc.pose.orientation.z, loc.pose.orientation.w };
    Vec3f d{ loc.pose.position.x - g.refPos.x, loc.pose.position.y - g.refPos.y, loc.pose.position.z - g.refPos.z };
    out->ori = QMul(inv, o);
    out->pos = QRot(inv, d);
    return true;
}

// ---------------------------------------------------------------- native stereo
static void DestroyEyeSwapchains() {
    for (int e = 0; e < 4; ++e) {
        for (auto r : g.eyeRtv[e]) if (r) r->Release();
        g.eyeRtv[e].clear();
        g.eyeImg[e].clear();
        if (g.eyeSc[e]) { xrDestroySwapchain(g.eyeSc[e]); g.eyeSc[e] = XR_NULL_HANDLE; }
    }
    g.eyeW = g.eyeH = 0; g.scViews = 0;
}

// Eye swapchains match the game's back buffer so each eye is a straight CopyResource.
static bool EnsureEyeSwapchains(const D3D11_TEXTURE2D_DESC& bb) {
    if (g.eyeSc[0] && g.eyeW == bb.Width && g.eyeH == bb.Height && g.scViews >= g.nViews) return true;
    DestroyEyeSwapchains();
    DXGI_FORMAT want;
    switch (bb.Format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: want = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; break;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: want = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; break;
    default: want = bb.Format; break;
    }
    bool supported = false;
    for (auto f : g.runtimeFormats) if (f == (int64_t)want) supported = true;
    if (!supported) { LOGE("Stereo: runtime does not support eye format %d (back buffer %d)", (int)want, (int)bb.Format); return false; }
    for (int e = 0; e < g.nViews; ++e) {
        XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        ci.format = want; ci.sampleCount = 1; ci.width = bb.Width; ci.height = bb.Height;
        ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
        XrResult r = xrCreateSwapchain(g.sess, &ci, &g.eyeSc[e]);
        if (XR_FAILED(r)) { LOGE("Stereo: xrCreateSwapchain eye %d: %s", e, XrStr(r)); DestroyEyeSwapchains(); return false; }
        uint32_t n = 0;
        xrEnumerateSwapchainImages(g.eyeSc[e], 0, &n, nullptr);
        std::vector<XrSwapchainImageD3D11KHR> imgs(n, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
        xrEnumerateSwapchainImages(g.eyeSc[e], n, &n, (XrSwapchainImageBaseHeader*)imgs.data());
        for (auto& i : imgs) g.eyeImg[e].push_back(i.texture);
        if (e >= 2) {   // inset views are drawn with a feathered edge: render target views
            bool raw = IsSrgb(want);
            for (size_t i = 0; i < imgs.size(); ++i) {
                D3D11_RENDER_TARGET_VIEW_DESC rd = {}; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D; rd.Format = raw ? RawOf(want) : want;
                ID3D11RenderTargetView* rtv = nullptr;
                HRESULT hr = g.dev->CreateRenderTargetView(imgs[i].texture, &rd, &rtv);
                if (FAILED(hr) && raw) { raw = false; for (auto r : g.eyeRtv[e]) r->Release(); g.eyeRtv[e].clear(); i = (size_t)-1; continue; }
                if (FAILED(hr)) { LOGE("Stereo: inset RTV failed 0x%08lx", hr); DestroyEyeSwapchains(); return false; }
                g.eyeRtv[e].push_back(rtv);
            }
            g.eyeDstLinear[e] = !(IsSrgb(want) && raw);
        }
    }
    g.scViews = g.nViews;
    g.eyeW = bb.Width; g.eyeH = bb.Height; g.eyeFmt = want;
    LOGI("Stereo: %d view swapchains %ux%u fmt %d", g.nViews, bb.Width, bb.Height, (int)want);
    return true;
}

bool VR_StereoWanted() {
    if (g_cfg.debugNoXr) return g_cfg.vrMode == "stereo" && g.target;
    return g_cfg.vrMode == "stereo" && !g.disabled && g.sess && g.running && g.local;
}

static Pose RelPose(const XrPosef& p) {
    Quat inv{ -g.refYaw.x, -g.refYaw.y, -g.refYaw.z, g.refYaw.w };
    Quat o{ p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w };
    Vec3f d{ p.position.x - g.refPos.x, p.position.y - g.refPos.y, p.position.z - g.refPos.z };
    Pose r; r.ori = QMul(inv, o); r.pos = QRot(inv, d);
    return r;
}

static void UpdateAutoSize();
// ---- in-game settings menu (vrkit): its texture is copied into a swapchain and shown as a head-locked quad
static bool CreateMenuSwapchain(UINT w, UINT h) {
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    for (auto p : { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM }) { for (auto rf : g.runtimeFormats) if (rf == (int64_t)p) { fmt = p; break; } if (fmt != DXGI_FORMAT_UNKNOWN) break; }
    if (fmt == DXGI_FORMAT_UNKNOWN) { LOGE("Menu: runtime has no R8G8B8A8 swapchain format"); return false; }
    XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = fmt; ci.sampleCount = 1; ci.width = w; ci.height = h; ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(g.sess, &ci, &g.menuSc))) { g.menuSc = XR_NULL_HANDLE; return false; }
    uint32_t n = 0;
    xrEnumerateSwapchainImages(g.menuSc, 0, &n, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> imgs(n, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
    xrEnumerateSwapchainImages(g.menuSc, n, &n, (XrSwapchainImageBaseHeader*)imgs.data());
    for (auto& i : imgs) g.menuImg.push_back(i.texture);
    g.menuW = w; g.menuH = h;
    return true;
}
static void DestroyMenuSwapchain() {
    g.menuImg.clear();
    if (g.menuSc) { xrDestroySwapchain(g.menuSc); g.menuSc = XR_NULL_HANDLE; }
    g.menuReady = false;
}
// Present: run the menu (input, navigation, drawing) and copy it into its swapchain while open.
static void UpdateMenu() {
    g.menuReady = false;
    if (!vrkit::menu::Update() || !g.sess) return;
    ID3D11Texture2D* t = vrkit::menu::Texture();
    if (!t) return;
    if (!g.menuSc && !CreateMenuSwapchain(vrkit::menu::Width(), vrkit::menu::Height())) return;
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (XR_FAILED(xrAcquireSwapchainImage(g.menuSc, &ai, &idx))) return;
    XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO }; wi.timeout = 100000000;
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    if (XR_SUCCEEDED(xrWaitSwapchainImage(g.menuSc, &wi)) && idx < g.menuImg.size()) { g.ctx->CopyResource(g.menuImg[idx], t); g.menuReady = true; }
    xrReleaseSwapchainImage(g.menuSc, &ri);
}
static void MenuLayer(XrCompositionLayerQuad& q) {
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    q.space = g.view; q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = g.menuSc;
    q.subImage.imageRect = { { 0, 0 }, { (int32_t)g.menuW, (int32_t)g.menuH } };
    q.pose = { { 0, 0, 0, 1 }, { 0.f, 0.f, -g_cfg.menuDistance } };
    q.size = { g_cfg.menuSize, g_cfg.menuSize * (float)g.menuH / (float)g.menuW };
}
// ---- stats overlay: text drawn with GDI, shown as a small head-locked quad layer
static const UINT kStatsW = 1024, kStatsH = 304;
static bool CreateStatsSwapchain() {
    static const DXGI_FORMAT prefs[] = { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                                         DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM };
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    for (auto p : prefs) { for (auto rf : g.runtimeFormats) if (rf == (int64_t)p) { fmt = p; break; } if (fmt != DXGI_FORMAT_UNKNOWN) break; }
    if (fmt == DXGI_FORMAT_UNKNOWN) return false;
    XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = fmt; ci.sampleCount = 1; ci.width = kStatsW; ci.height = kStatsH; ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(g.sess, &ci, &g.stats))) { g.stats = XR_NULL_HANDLE; return false; }
    uint32_t n = 0;
    xrEnumerateSwapchainImages(g.stats, 0, &n, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> imgs(n, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
    xrEnumerateSwapchainImages(g.stats, n, &n, (XrSwapchainImageBaseHeader*)imgs.data());
    for (auto& i : imgs) g.statsImg.push_back(i.texture);
    D3D11_TEXTURE2D_DESC d = {}; d.Width = kStatsW; d.Height = kStatsH; d.MipLevels = 1; d.ArraySize = 1; d.Format = fmt;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    if (FAILED(g.dev->CreateTexture2D(&d, nullptr, &g.statsTex))) { DestroyStatsSwapchain(); return false; }
    g.statsFmt = fmt;
    return true;
}
// Renders the text and copies it into the overlay swapchain.
static void UpdateStatsOverlay(const char* text) {
    if (!g.sess || !g.dev) return;
    if (!g.stats && !CreateStatsSwapchain()) return;
    static HDC dc = nullptr; static HBITMAP bmp = nullptr; static uint32_t* bits = nullptr; static HFONT font = nullptr;
    if (!dc) {
        dc = CreateCompatibleDC(nullptr);
        BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = kStatsW; bi.bmiHeader.biHeight = -(LONG)kStatsH;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0);
        SelectObject(dc, bmp);
        font = CreateFontW(40, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, FIXED_PITCH, L"Consolas");
        SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
    }
    if (!bits) return;
    memset(bits, 0, kStatsW * kStatsH * 4);
    RECT rc = { 16, 10, (LONG)kStatsW - 16, (LONG)kStatsH - 10 };
    DrawTextA(dc, text, -1, &rc, DT_LEFT | DT_TOP | DT_NOPREFIX);
    GdiFlush();
    // white text over a translucent dark panel, premultiplied alpha
    bool bgr = g.statsFmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || g.statsFmt == DXGI_FORMAT_B8G8R8A8_UNORM;
    static std::vector<uint32_t> px(kStatsW * kStatsH);
    for (UINT i = 0; i < kStatsW * kStatsH; ++i) {
        uint32_t c = bits[i]; uint32_t cov = ((c & 0xff) + ((c >> 8) & 0xff) + ((c >> 16) & 0xff)) / 3;
        uint32_t a = 150 + cov * 105 / 255;
        uint32_t r = cov, gg = cov, b = cov;
        px[i] = bgr ? (b | gg << 8 | r << 16 | a << 24) : (r | gg << 8 | b << 16 | a << 24);
    }
    g.ctx->UpdateSubresource(g.statsTex, 0, nullptr, px.data(), kStatsW * 4, 0);
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (XR_FAILED(xrAcquireSwapchainImage(g.stats, &ai, &idx))) return;
    XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO }; wi.timeout = 100000000;
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    if (XR_SUCCEEDED(xrWaitSwapchainImage(g.stats, &wi)) && idx < g.statsImg.size()) { g.ctx->CopyResource(g.statsImg[idx], g.statsTex); g.statsReady = true; }
    xrReleaseSwapchainImage(g.stats, &ri);
}
// Writes render_scale back into config.txt so the chosen value is kept.
static void SaveRenderScale(float v) {
    std::wstring path = g_modDir + L"config.txt";
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return;
    std::string all; char buf[4096]; size_t k;
    while ((k = fread(buf, 1, sizeof(buf), f)) > 0) all.append(buf, k);
    fclose(f);
    char line[64]; snprintf(line, sizeof(line), "render_scale = %.2f", v);
    size_t pos = 0; bool done = false;
    while (pos < all.size()) {
        size_t e = all.find('\n', pos); if (e == std::string::npos) e = all.size();
        size_t a = pos; while (a < e && (all[a] == ' ' || all[a] == '\t')) ++a;
        if (all.compare(a, 12, "render_scale") == 0) {
            size_t ce = e; if (ce > pos && all[ce - 1] == '\r') --ce;
            all.replace(pos, ce - pos, line); done = true; break;
        }
        pos = e + 1;
    }
    if (!done) { all += "\r\n"; all += line; all += "\r\n"; }
    f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    fwrite(all.data(), 1, all.size(), f); fclose(f);
}
static bool KeyPressed(int vk) { return vk && (GetAsyncKeyState(vk) & 1); }
bool Perf_Text(char* out, size_t n);
// Present: hotkeys (render scale, stats) and the overlay text
void Passes_Start(); void Vrs_SetActive(bool); bool Vrs_Active(); void Vrs_LogStats();
void PostFx_CycleAa(); const char* PostFx_AaName(); const char* PostFx_AaValue();
static const char* QuadVrsLabel() {
    static char b[160];
    snprintf(b, sizeof(b), "AA %s%s%s%s", PostFx_AaName(), Vrs_Active() ? "  VRS" : "  VRS off", Quad_Active() ? "  QUAD" : "", !g_eyeTracking ? "  eyes off" : g.gazeValid ? "  gaze" : g.gazeSpace ? "  no gaze" : "");
    return b;
}
static void HandleTuning() {
    { static bool vi = false; if (!vi) { vi = true; Vrs_SetActive(g_cfg.vrs); } }
    if (KeyPressed(g_cfg.eyeTrackingKey)) { g_eyeInit = true; g_eyeTracking = !g_eyeTracking; g_cfg.eyeTracking = g_eyeTracking; LOGI("Eye tracking %s (key)", g_eyeTracking ? "on" : "off"); g.scaleMsgUntil = GetTickCount() + 4000; }
    if (KeyPressed(g_cfg.aaKey)) { PostFx_CycleAa(); g_cfg.aaMode = PostFx_AaValue(); g.scaleMsgUntil = GetTickCount() + 4000; }
    if (KeyPressed(g_cfg.quadKey)) { Quad_Active(); g_quadOn = !g_quadOn; g_cfg.quadViews = g_quadOn; LOGI("Quad views %s (key)", g_quadOn ? "on" : "off"); g.scaleMsgUntil = GetTickCount() + 4000; }
    if (KeyPressed(g_cfg.vrsKey)) { Vrs_SetActive(!Vrs_Active()); g_cfg.vrs = Vrs_Active(); LOGI("VRS %s (key)", Vrs_Active() ? "on" : "off"); g.scaleMsgUntil = GetTickCount() + 4000; }
    if (KeyPressed(g_cfg.captureKey)) { LOGI("Pass capture key pressed"); Passes_Start(); }
    if (KeyPressed(g_cfg.screenshotKey)) { LOGI("Screenshot key pressed: writing GranblueVR_eye0/eye1/hud.bmp"); g.dumpEyes = 1; }
    static bool init = false; if (!init) { init = true; g.statsOn = g_cfg.showStats; }
    if (KeyPressed(g_cfg.statsKey)) { g.statsOn = !g.statsOn; g_cfg.showStats = g.statsOn; LOGI("Stats overlay %s", g.statsOn ? "on" : "off"); }
    int step = KeyPressed(g_cfg.scaleUpKey) ? 1 : KeyPressed(g_cfg.scaleDownKey) ? -1 : 0;
    if (step && g.baseW && g_cfg.forceW <= 0) {
        g.scale = (std::min)(2.0f, (std::max)(0.3f, roundf((g.scale + step * 0.05f) * 20.f) / 20.f));
        g_cfg.renderScale = g.scale;
        UpdateAutoSize();
        SaveRenderScale(g.scale);
        g.scaleMsgUntil = GetTickCount() + 4000;
    }
    DWORD now = GetTickCount();
    bool show = g.statsOn || now < g.scaleMsgUntil;
    if (!show) { g.statsReady = false; return; }
    if (now - g.statsTick < 250 && !step) return;
    g.statsTick = now;
    char perf[512] = "(measuring...)";
    Perf_Text(perf, sizeof(perf));
    char text[768];
    char keys[64]; snprintf(keys, sizeof(keys), "F%d/F%d scale  F%d stats", g_cfg.scaleDownKey - VK_F1 + 1, g_cfg.scaleUpKey - VK_F1 + 1, g_cfg.statsKey - VK_F1 + 1);
    if (g.autoW) snprintf(text, sizeof(text), "Render %ux%u/eye  scale %.2f (%.0f%%)\n%s\n%s\n%s",
                          g.autoW, g.autoH, g.scale, 100.f * g.autoW * g.autoH / (g.baseW * g.baseH),
                          QuadVrsLabel(), perf, keys);
    else snprintf(text, sizeof(text), "Render size: game's own\n%s\n%s\n%s", QuadVrsLabel(), perf, keys);
    UpdateStatsOverlay(text);
}

static void StereoEndFrame() {
    if (!g.stFrameBegun) return;
    if (g_cfg.debugNoXr) { g.stFrameBegun = false; for (auto& c : g.stCaptured) c = false; return; }
    XrCompositionLayerProjectionView pv[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
    XrCompositionLayerProjection proj = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerQuad hq = { XR_TYPE_COMPOSITION_LAYER_QUAD }, sq = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    XrCompositionLayerProjectionView pvi[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
    XrCompositionLayerProjection proji = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerQuad mq = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    const XrCompositionLayerBaseHeader* layers[5];
    uint32_t n = 0;
    if (g.stShouldRender && g.stCaptured[0] && g.stCaptured[1]) {
        for (int e = 0; e < 2; ++e) {
            pv[e].pose = g.stViews[e].pose;
            pv[e].fov = g.stViews[e].fov;
            pv[e].subImage.swapchain = g.eyeSc[e];
            pv[e].subImage.imageRect = { { 0, 0 }, { (int32_t)g.eyeW, (int32_t)g.eyeH } };
        }
        proj.space = g.local; proj.viewCount = 2; proj.views = pv;
        layers[n++] = (XrCompositionLayerBaseHeader*)&proj;
        if (g.nViews == 4 && g.stCaptured[2] && g.stCaptured[3]) {
            // quad views: the high-density insets as a second projection layer, alpha-blended (feathered edge)
            for (int e = 0; e < 2; ++e) {
                pvi[e].pose = g.stViews[e].pose;
                pvi[e].fov = g.innerFov[e];
                pvi[e].subImage.swapchain = g.eyeSc[2 + e];
                pvi[e].subImage.imageRect = { { 0, 0 }, { (int32_t)g.eyeW, (int32_t)g.eyeH } };
            }
            proji.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            proji.space = g.local; proji.viewCount = 2; proji.views = pvi;
            layers[n++] = (XrCompositionLayerBaseHeader*)&proji;
        }
        if (g.hudReady && g.hud && g.view) {
            // head-locked HUD panel
            hq.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            hq.space = g.view;
            hq.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            hq.subImage.swapchain = g.hud;
            // the game lays its UI out in a centred 16:9 band of a taller target: show just that band
            int32_t bandH = (int32_t)g.hudH, y0 = 0;
            if ((uint64_t)g.hudH * 16 > (uint64_t)g.hudW * 9) { bandH = (int32_t)((uint64_t)g.hudW * 9 / 16); y0 = ((int32_t)g.hudH - bandH) / 2; }
            hq.subImage.imageRect = { { 0, y0 }, { (int32_t)g.hudW, bandH } };
            hq.pose = { { 0, 0, 0, 1 }, { 0, 0, -g_cfg.hudDistance } };
            hq.size = { g_cfg.hudSize, g_cfg.hudSize * (float)bandH / (float)g.hudW };
            layers[n++] = (XrCompositionLayerBaseHeader*)&hq;
        }
        if (g.statsReady && g.stats && g.view) {
            sq.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            sq.space = g.view; sq.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            sq.subImage.swapchain = g.stats;
            sq.subImage.imageRect = { { 0, 0 }, { (int32_t)kStatsW, (int32_t)kStatsH } };
            sq.pose = { { 0, 0, 0, 1 }, { 0.f, -0.22f, -1.0f } };
            sq.size = { 0.55f, 0.55f * kStatsH / kStatsW };
            layers[n++] = (XrCompositionLayerBaseHeader*)&sq;
        }
        if (g.menuReady && g.menuSc && g.view) { MenuLayer(mq); layers[n++] = (XrCompositionLayerBaseHeader*)&mq; }
    }
    g.hudReady = false;
    XrFrameEndInfo fei = { XR_TYPE_FRAME_END_INFO };
    fei.displayTime = g.stDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = n; fei.layers = layers;
    XrResult r = xrEndFrame(g.sess, &fei);
    if (XR_FAILED(r)) LOGE("Stereo xrEndFrame: %s", XrStr(r)); else ++g.xrFrames;
    g.stFrameBegun = false;
    for (auto& c : g.stCaptured) c = false;
}

// Extra field of view rendered around the headset's (radians). The compositor only shows the headset's
// own FOV, so the frame's border - where the game's culling, texture streaming and screen-space effects
// misbehave - is outside what you see.
static float Overscan() { return (std::min)((std::max)(g_cfg.fovOverscan, 0.f), 15.f) * 0.0174533f; }
static void UpdateAutoSize() {
    if (!g.baseW) return;
    float w = g.baseW * g.scale, hh = g.baseH * g.scale;
    float cap = (float)g_cfg.renderMax, k = (std::min)(1.f, (std::min)(cap / w, cap / hh));
    UINT nw = (std::max)(((UINT)(w * k) + 4) & ~7u, 256u), nh = (std::max)(((UINT)(hh * k) + 4) & ~7u, 256u);
    if (nw != g.autoW || nh != g.autoH)
        LOGI("Render size: render_scale %.2f -> %ux%u per eye%s", g.scale, nw, nh, k < 1.f ? " (capped by render_max)" : "");
    g.autoW = nw; g.autoH = nh;
}

bool VR_StereoBegin(EyeSetup eyes[4]) {
    if (g_cfg.debugNoXr) {
        g.stFrameBegun = true; g.stShouldRender = false;
        for (int e = 0; e < 2; ++e) {
            eyes[e].tanL = -1.f; eyes[e].tanR = 1.f; eyes[e].tanU = 1.f; eyes[e].tanD = -1.f;
            eyes[e].pose.ori = { 0, 0, 0, 1 }; eyes[e].pose.pos = { e == 0 ? -0.0315f : 0.0315f, 0, 0 };
        }
        return true;
    }
    if (g.stFrameBegun) StereoEndFrame();   // previous frame never reached Present
    PollEvents();
    if (!VR_StereoWanted()) return false;
    XrFrameWaitInfo fwi = { XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState fs = { XR_TYPE_FRAME_STATE };
    double tw0 = Perf_Now();
    XrResult r = xrWaitFrame(g.sess, &fwi, &fs);
    Perf_AddXrWait(Perf_Now() - tw0);
    if (XR_FAILED(r)) { LOGE("Stereo xrWaitFrame: %s", XrStr(r)); return false; }
    XrFrameBeginInfo fbi = { XR_TYPE_FRAME_BEGIN_INFO };
    r = xrBeginFrame(g.sess, &fbi);
    if (XR_FAILED(r)) { LOGE("Stereo xrBeginFrame: %s", XrStr(r)); return false; }
    g.stFrameBegun = true;
    g.stShouldRender = fs.shouldRender;
    g.stDisplayTime = fs.predictedDisplayTime;
    g.lastDisplayTime = fs.predictedDisplayTime;
    g.period = fs.predictedDisplayPeriod;
    if (GetAsyncKeyState(g_cfg.recenterKey) & 1) { LOGI("Recenter key pressed"); g.needRecenter = true; }
    if (g.needRecenter) Recenter(fs.predictedDisplayTime);
    if (!g.haveRef) return false;
    UpdateGaze(fs.predictedDisplayTime);

    XrViewLocateInfo li = { XR_TYPE_VIEW_LOCATE_INFO };
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = fs.predictedDisplayTime; li.space = g.local;
    XrViewState vs = { XR_TYPE_VIEW_STATE };
    uint32_t nv = 0;
    g.stViews[0] = { XR_TYPE_VIEW }; g.stViews[1] = { XR_TYPE_VIEW };
    r = xrLocateViews(g.sess, &li, &vs, 2, &nv, g.stViews);
    if (XR_FAILED(r) || nv < 2 || !(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) return false;
    if (g_cfg.debugFovSkew != 0.f || g_cfg.debugFovVSkew != 0.f) {
        // Debug: make the frusta asymmetric (eye 0 window shifted left, eye 1 right; both shifted up by vskew)
        // to verify off-axis projection on runtimes with symmetric views.
        float s = g_cfg.debugFovSkew * 0.0174533f, t = g_cfg.debugFovVSkew * 0.0174533f;
        g.stViews[0].fov.angleLeft -= s; g.stViews[0].fov.angleRight -= s;
        g.stViews[1].fov.angleLeft += s; g.stViews[1].fov.angleRight += s;
        for (int e = 0; e < 2; ++e) { g.stViews[e].fov.angleUp += t; g.stViews[e].fov.angleDown += t; }
    }
    if (!g.baseW && g.recW && g.recH) {
        // Per-eye render size: the runtime's recommended size (pixel density for eye 0's own FOV), extended
        // to the symmetric frustum we actually render; render_scale and render_max are applied in UpdateAutoSize.
        const XrFovf& f0 = g.stViews[0].fov;
        float dx = g.recW / (tanf(f0.angleRight) - tanf(f0.angleLeft)), dy = g.recH / (tanf(f0.angleUp) - tanf(f0.angleDown));
        float tw = tanf(f0.angleRight) - tanf(f0.angleLeft), th = tanf(f0.angleUp) - tanf(f0.angleDown);
        if (g_cfg.symmetricFov) {
            float h = 0.f, vv = 0.f;
            for (int e = 0; e < 2; ++e) { const XrFovf& f = g.stViews[e].fov; h = (std::max)(h, (std::max)(-f.angleLeft, f.angleRight)); vv = (std::max)(vv, (std::max)(f.angleUp, -f.angleDown)); }
            h += Overscan(); vv += Overscan();
            tw = 2.f * tanf(h); th = 2.f * tanf(vv);
        } else {
            float o = Overscan();
            tw = tanf(f0.angleRight + o) - tanf(f0.angleLeft - o); th = tanf(f0.angleUp + o) - tanf(f0.angleDown - o);
        }
        g.baseW = dx * tw; g.baseH = dy * th;
        if (g.scale <= 0.f) g.scale = g_cfg.renderScale;
        UpdateAutoSize();
        LOGI("Render size: runtime recommends %ux%u per eye; with %s FOV that is %.0fx%.0f at render_scale 1.0",
             g.recW, g.recH, g_cfg.symmetricFov ? "symmetric" : "per-eye", g.baseW, g.baseH);
    }
    if (g_cfg.symmetricFov) {
        // Render both eyes with one symmetric frustum covering both eyes' FOVs (the compositor crops it).
        // Screen-space content (HUD, menus) drawn at the same pixel in both eyes then points the same
        // direction in both eyes (converges at infinity) instead of diverging on headsets whose
        // per-eye FOV is asymmetric.
        float h = 0.f, vv = 0.f;
        for (int e = 0; e < 2; ++e) {
            const XrFovf& f = g.stViews[e].fov;
            h = (std::max)(h, (std::max)(-f.angleLeft, f.angleRight));
            vv = (std::max)(vv, (std::max)(f.angleUp, -f.angleDown));
        }
        h += Overscan(); vv += Overscan();
        for (int e = 0; e < 2; ++e) { g.stViews[e].fov = { -h, h, vv, -vv }; }
        static bool logged = false;
        if (!logged) { logged = true; LOGI("Symmetric render FOV: +-%.1f x +-%.1f deg (incl. %.1f deg overscan)", h * 57.2958f, vv * 57.2958f, g_cfg.fovOverscan); }
    } else if (Overscan() > 0.f) {
        float o = Overscan();
        for (int e = 0; e < 2; ++e) { XrFovf& f = g.stViews[e].fov; f.angleLeft -= o; f.angleRight += o; f.angleUp += o; f.angleDown -= o; }
    }

    Pose fake; bool useFake = (g_cfg.debugFakeHead > 0.f || g_cfg.debugHeadYaw != 0.f) && VR_GetHeadPose(&fake);
    Pose head = RelPose(g.stViews[0].pose);   // only used for the fake-head composition below
    (void)head;
    for (int e = 0; e < 2; ++e) {
        const XrView& v = g.stViews[e];
        eyes[e].tanL = tanf(v.fov.angleLeft); eyes[e].tanR = tanf(v.fov.angleRight);
        eyes[e].tanU = tanf(v.fov.angleUp); eyes[e].tanD = tanf(v.fov.angleDown);
        eyes[e].pose = RelPose(v.pose);
        if (useFake) {
            // static null-driver head: put the eye offset on top of the fake head motion
            Vec3f off = QRot(fake.ori, eyes[e].pose.pos);
            eyes[e].pose.ori = QMul(fake.ori, eyes[e].pose.ori);
            eyes[e].pose.pos = { fake.pos.x + off.x, fake.pos.y + off.y, fake.pos.z + off.z };
        }
    }
    // Quad views: per eye a narrower inset view at the same render size (so higher pixel density), centred
    // on quad_center (later: the gaze), composited over the wide view with a feathered edge.
    g.nViews = 2;
    if (Quad_Active()) {
        float f = (std::min)((std::max)(g_cfg.quadInnerSize, 0.1f), 0.9f);
        for (int e = 0; e < 2; ++e) {
            const EyeSetup& o = eyes[e];
            float th = (o.tanR - o.tanL) * 0.5f, tv = (o.tanU - o.tanD) * 0.5f;
            float mx = (o.tanR + o.tanL) * 0.5f, my = (o.tanU + o.tanD) * 0.5f;
            float ix = f * th, iy = f * tv;
            float qx, qy; Quad_Center(th, tv, &qx, &qy);
            float cx = (std::min)((std::max)(qx, mx - (th - ix)), mx + (th - ix)), cy = (std::min)((std::max)(qy, my - (tv - iy)), my + (tv - iy));
            EyeSetup& in = eyes[2 + e];
            in.pose = o.pose;
            in.tanL = cx - ix; in.tanR = cx + ix; in.tanU = cy + iy; in.tanD = cy - iy;
            g.innerFov[e] = { atanf(in.tanL), atanf(in.tanR), atanf(in.tanU), atanf(in.tanD) };
        }
        g.nViews = 4;
    }
    for (int k = 0; k < g.nViews; ++k) { g.viewTan[k][0] = eyes[k].tanL; g.viewTan[k][1] = eyes[k].tanR; g.viewTan[k][2] = eyes[k].tanU; g.viewTan[k][3] = eyes[k].tanD; }
    return true;
}
int VR_ViewCount() { return g.nViews; }
// tangents (L, R, U, D) of view k this frame (0/1 = eyes, 2/3 = quad-view insets)
bool VR_ViewTangents(int k, float t[4]) { if (k < 0 || k >= g.nViews) return false; for (int i = 0; i < 4; ++i) t[i] = g.viewTan[k][i]; return true; }

static void WriteBmp(const std::wstring& path, ID3D11Texture2D* src, bool keepAlpha = false) {
    D3D11_TEXTURE2D_DESC d; src->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    sd.MipLevels = 1; sd.ArraySize = 1; sd.SampleDesc.Count = 1;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(g.dev->CreateTexture2D(&sd, nullptr, &st))) { LOGE("eye dump: staging create failed"); return; }
    g.ctx->CopyResource(st, src);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g.ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        FILE* f = _wfopen(path.c_str(), L"wb");
        if (f) {
            uint32_t w = d.Width, h = d.Height, rowBytes = w * 4, size = 54 + rowBytes * h;
            uint8_t hdr[54] = { 'B', 'M' };
            *(uint32_t*)(hdr + 2) = size; *(uint32_t*)(hdr + 10) = 54; *(uint32_t*)(hdr + 14) = 40;
            *(int32_t*)(hdr + 18) = (int32_t)w; *(int32_t*)(hdr + 22) = -(int32_t)h;
            *(uint16_t*)(hdr + 26) = 1; *(uint16_t*)(hdr + 28) = 32; *(uint32_t*)(hdr + 34) = rowBytes * h;
            fwrite(hdr, 1, 54, f);
            bool rgba = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
            std::vector<uint8_t> row(rowBytes);
            for (uint32_t y = 0; y < h; ++y) {
                const uint8_t* s = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
                for (uint32_t x = 0; x < w; ++x) {
                    row[x * 4 + 0] = rgba ? s[x * 4 + 2] : s[x * 4 + 0];
                    row[x * 4 + 1] = s[x * 4 + 1];
                    row[x * 4 + 2] = rgba ? s[x * 4 + 0] : s[x * 4 + 2];
                    row[x * 4 + 3] = keepAlpha ? s[x * 4 + 3] : 255;
                }
                fwrite(row.data(), 1, rowBytes, f);
            }
            fclose(f);
        }
        g.ctx->Unmap(st, 0);
    }
    st->Release();
}

void VR_StereoCaptureEye(int eye) {
    if (!g.stFrameBegun || !g.target) return;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(g.target->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) return;
    D3D11_TEXTURE2D_DESC bd; bb->GetDesc(&bd);
    if (g.dumpEyes == eye + 1) {   // eye 0, eye 1, then the quad-view insets (eye2 = left inset, eye3 = right)
        wchar_t name[64]; swprintf(name, 64, L"GranblueVR_eye%d.bmp", eye);
        WriteBmp(g_modDir + name, bb);
        LOGI("Stereo: wrote eye %d image (%ux%u)", eye, bd.Width, bd.Height);
        g.dumpEyes = eye + 2 > g.nViews ? 0 : eye + 2;
    }
    if (g.stShouldRender && eye < g.nViews && EnsureEyeSwapchains(bd)) {
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO }; wi.timeout = 100000000;
        if (XR_SUCCEEDED(xrAcquireSwapchainImage(g.eyeSc[eye], &ai, &idx)) && XR_SUCCEEDED(xrWaitSwapchainImage(g.eyeSc[eye], &wi))) {
            if (eye < 2 || idx >= g.eyeRtv[eye].size() || !EnsureCopyTexture(bd)) g.ctx->CopyResource(g.eyeImg[eye][idx], bb);
            else {
                // inset: feathered edge so it blends into the wide view
                g.ctx->CopyResource(g.copyTex, bb);
                bool srcLinear = SourceIsLinear(bd.Format);
                struct { int mode, keepAlpha; float feather; int pad; } cb = { srcLinear == g.eyeDstLinear[eye] ? 0 : (srcLinear ? 1 : 2), 0, (std::max)(g_cfg.quadFeather, 0.001f), 0 };
                ID3DDeviceContextState* prev = nullptr;
                g.ctx1->SwapDeviceContextState(g.ourState, &prev);
                g.ctx->UpdateSubresource(g.cb, 0, nullptr, &cb, 0, 0);
                D3D11_VIEWPORT vp = { 0, 0, (float)g.eyeW, (float)g.eyeH, 0, 1 };
                g.ctx->RSSetViewports(1, &vp);
                g.ctx->OMSetRenderTargets(1, &g.eyeRtv[eye][idx], nullptr);
                g.ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
                g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                g.ctx->IASetInputLayout(nullptr);
                g.ctx->VSSetShader(g.vs, nullptr, 0);
                g.ctx->PSSetShader(g.ps, nullptr, 0);
                g.ctx->PSSetShaderResources(0, 1, &g.copySrv);
                g.ctx->PSSetSamplers(0, 1, &g.sampler);
                g.ctx->PSSetConstantBuffers(0, 1, &g.cb);
                g.ctx->Draw(3, 0);
                ID3D11ShaderResourceView* nullSrv = nullptr;
                g.ctx->PSSetShaderResources(0, 1, &nullSrv);
                g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
                g.ctx1->SwapDeviceContextState(prev, nullptr);
                SafeRelease(prev);
            }
            XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            xrReleaseSwapchainImage(g.eyeSc[eye], &ri);
            g.stCaptured[eye] = true;
        }
    }
    bb->Release();
}

// The game's engineDraw thread waits on the swapchain's frame-latency object (1 s timeout) before
// each submit; with two submits per Present the second would time out. The object can't be
// signalled from outside (no modify access), so let the next wait on it pass straight through.
static HANDLE g_latencyHandle = nullptr;
static std::atomic<int> g_latencyPasses{ 0 };
using WaitExFn = DWORD(WINAPI*)(HANDLE, DWORD, BOOL);
static WaitExFn oWaitEx = nullptr;
static uintptr_t g_latencyCallRet = 0;   // exe+0x1ac3f5: return address of engineDraw's latency wait
static std::atomic<uint64_t> g_latencyPassed{ 0 };
void Replay_AtLatencyWait(ID3D11DeviceContext* c);
void Replay_SetContext(ID3D11DeviceContext* c); bool Replay_PresentPending(); void Replay_AtPresent(ID3D11DeviceContext* c); void Replay_DropRecorded();
void Hud_SetEnabled(bool on);
void Temporal_BeforeReplay(ID3D11DeviceContext*); void Temporal_AfterReplay(ID3D11DeviceContext*); void Temporal_Reset(); void Temporal_Schedule(int);
void Temporal_BeforeView(ID3D11DeviceContext*, int); void Temporal_AfterViews(ID3D11DeviceContext*, int);
int Replay_ViewCount(); void Replay_RunView(ID3D11DeviceContext*, int); void Vrs_BeforeView(ID3D11DeviceContext*, int);
static void PresentReplay() {
    if (Replay_PresentPending()) {
        if (g.stFrameBegun) {
            bool dumpHud = g.dumpEyes == 1;
            VR_StereoCaptureEye(0);
            if (dumpHud) {
                UINT w, h; DXGI_FORMAT f; ID3D11ShaderResourceView* srv = Hud_FrameTexture(&w, &h, &f);
                if (srv) { ID3D11Resource* r = nullptr; srv->GetResource(&r);
                           if (r) { WriteBmp(g_modDir + L"GranblueVR_hud.bmp", (ID3D11Texture2D*)r, true); r->Release(); LOGI("HUD: wrote UI image"); } }
                if (g.statsTex) WriteBmp(g_modDir + L"GranblueVR_stats.bmp", g.statsTex, true);
            }
            if (g_cfg.hudLayer) {
                LONG ui = Hud_TakeUiCount();
                ++g.hudFrames; if (ui == 0) ++g.hudMissing;
                g.lastUiMissing = ui == 0;
                if (!g.lastStereo) { ++g.switches; g.lastStereo = true; }
                g.hudReady = !g_cfg.debugNoXr && BlitHud();
                static int hl = 0;
                if (hl < 5) { ++hl; LOGD("HUD: frame UI passes %ld, layer %s", ui, g.hudReady ? "submitted" : "not submitted"); }
            }
            double t0 = Perf_Now();
            Perf_BeforeReplay();
            // replay the frame for each further view (eye 1; with quad views also the two insets) and
            // capture each into its swapchain
            int nv = (std::min)(Replay_ViewCount(), g.nViews);
            for (int k = 1; k < nv; ++k) {
                if (g_cfg.perEyeHistory) Temporal_BeforeView(g.ctx, k);
                Vrs_BeforeView(g.ctx, k);
                Replay_RunView(g.ctx, k - 1);
                VR_StereoCaptureEye(k);
            }
            if (g_cfg.perEyeHistory) Temporal_AfterViews(g.ctx, nv);
            Vrs_BeforeView(g.ctx, 0);   // next frame's own render is view 0
            Perf_AfterReplay();
            Perf_AddPresentWork(Perf_Now() - t0);
            if (g_cfg.hudLayer) Hud_Clear(g.ctx);
            Hud_FrameReset(g.lastUiMissing);
            return;
        }
        else Replay_DropRecorded();
    }
    ++g.monoFrames;
    Hud_FrameReset(false);
    if (g.lastStereo) { ++g.switches; g.lastStereo = false; }
    Temporal_Reset();
}
static DWORD WINAPI hkWaitEx(HANDLE h, DWORD ms, BOOL alertable) {
    if ((uintptr_t)_ReturnAddress() == g_latencyCallRet) {
        DWORD r;
        if (g_latencyPasses.load() > 0) { --g_latencyPasses; ++g_latencyPassed; r = WAIT_OBJECT_0; }
        else r = oWaitEx(h, ms, alertable);
        if (g.ctx) Replay_AtLatencyWait(g.ctx);
        return r;
    }
    return oWaitEx(h, ms, alertable);
}
void VR_ReturnLatencyToken() {
    static bool tried = false;
    if (!g.target) return;
    if (!tried) {
        tried = true;
        IDXGISwapChain2* sc2 = nullptr;
        if (SUCCEEDED(g.target->QueryInterface(__uuidof(IDXGISwapChain2), (void**)&sc2)) && sc2) {
            g_latencyHandle = sc2->GetFrameLatencyWaitableObject();
            sc2->Release();
        }
        g_latencyCallRet = (uintptr_t)GetModuleHandleW(nullptr) + 0x1ac3f5;
        void* fn = (void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "WaitForSingleObjectEx");
        bool ok = fn && MH_CreateHook(fn, (void*)hkWaitEx, (void**)&oWaitEx) == MH_OK && MH_EnableHook(fn) == MH_OK;
        LOGI("Stereo: frame latency object %p, wait hook %s", g_latencyHandle, ok ? "installed" : "FAILED");
    }
    if (oWaitEx) g_latencyPasses = 1;
    static int n = 0;
    if ((++n % 600) == 3) LOGD("Stereo: latency waits passed %llu", g_latencyPassed.load());
}

void VR_GpuSync() {
    static ID3D11Query* q = nullptr;
    if (!q) { D3D11_QUERY_DESC d = { D3D11_QUERY_EVENT, 0 }; if (FAILED(g.dev->CreateQuery(&d, &q))) return; }
    g.ctx->End(q);
    g.ctx->Flush();
    BOOL done = FALSE; DWORD t0 = GetTickCount();
    while (g.ctx->GetData(q, &done, sizeof(done), 0) != S_OK && GetTickCount() - t0 < 200) YieldProcessor();
}

// ---------------------------------------------------------------- entry points from hooks
void VR_OnPresent(IDXGISwapChain* sc) {
    if (g.disabled) return;
    if (!g.target) {
        DXGI_SWAP_CHAIN_DESC d;
        if (FAILED(sc->GetDesc(&d)) || d.BufferDesc.Width < 640) return;  // ignore tiny/aux swapchains
        g.target = sc;
        LOGI("Following game swapchain %p", (void*)sc);
        if (!InitD3D(sc)) return;
        if (!g.cameraHooked) { g.cameraHooked = true; Camera_Install(); Trace_Install(); }
    }
    if (sc != g.target) return;
    if (Camera_InEye0Present()) { MapLog_Marker('E', 0); if (g.stFrameBegun) VR_StereoCaptureEye(0); return; }
    ++g.frames;
    { static int pl = 0; if (g.stFrameBegun && pl < 8) { ++pl; LOGD("Present (stereo frame) execs %llu", Discovery_ExecCount()); } }
    if ((g.frames & 63) == 1) { DXGI_SWAP_CHAIN_DESC sd; if (SUCCEEDED(sc->GetDesc(&sd))) Discovery_SetScreen(sd.BufferDesc.Width, sd.BufferDesc.Height); }
    // File trigger for automation: write a command word into GranblueVR.trigger next to the DLL.
    if ((g.frames % 30) == 0) {
        std::wstring tp = g_modDir + L"GranblueVR.trigger";
        FILE* tf = _wfopen(tp.c_str(), L"rb");
        if (tf) {
            char cmd[64] = {};
            fread(cmd, 1, sizeof(cmd) - 1, tf);
            fclose(tf);
            DeleteFileW(tp.c_str());
            for (char* c = cmd; *c; ++c) if (*c == '\r' || *c == '\n' || *c == ' ') { *c = 0; break; }
            LOGI("Trigger file: '%s'", cmd);
            if (!strcmp(cmd, "discovery")) Discovery_Trigger();
            else if (!strcmp(cmd, "recenter")) g.needRecenter = true;
            else if (!strcmp(cmd, "eyedump")) g.dumpEyes = 1;
            else if (!strcmp(cmd, "trace")) Trace_Start();
            else if (!strcmp(cmd, "rdcapture")) RDoc_Trigger();
            else if (!strcmp(cmd, "maplog")) MapLog_Start(3);
            else if (!strcmp(cmd, "passes")) Passes_Start();
            else if (!strncmp(cmd, "cull=", 5)) { g_cfg.cullMarginDeg = (float)atof(cmd + 5); LOGI("cull_margin -> %.1f", g_cfg.cullMarginDeg); }
            else if (!strncmp(cmd, "cullfix=", 8)) { g_cfg.fixCullFov = cmd[8] == '1'; LOGI("fix_cull_fov -> %d", (int)g_cfg.fixCullFov); }
            else if (!strncmp(cmd, "quad=", 5)) { Quad_Active(); g_quadOn = cmd[5] == '1'; LOGI("Quad views -> %d", (int)g_quadOn); }
            else if (!strncmp(cmd, "vrs=", 4)) { Vrs_SetActive(cmd[4] == '1'); LOGI("VRS -> %d", (int)Vrs_Active()); }
            else if (!strncmp(cmd, "scanres", 7)) {
                // log every place in the exe's writable data holding the render size pair as int32 or float
                uintptr_t b = (uintptr_t)GetModuleHandleW(nullptr);
                UINT W = 1920, H = 1080; sscanf(cmd + 7, "=%ux%u", &W, &H);
                int found = 0;
                for (uintptr_t a = b + 0x6b35000; a + 8 < b + 0x7c567bc && found < 60; a += 4) {
                    uint32_t x = *(uint32_t*)a, y = *(uint32_t*)(a + 4);
                    float fx = *(float*)a, fy = *(float*)(a + 4);
                    if ((x == W && y == H) || (fx == (float)W && fy == (float)H)) { LOGI("scanres: exe+0x%llx %s", (unsigned long long)(a - b), x == W ? "int" : "float"); ++found; }
                }
                LOGI("scanres: %d hits for %ux%u", found, W, H);
            }
            else if (!strcmp(cmd, "inststats")) Camera_InstLog(4);
            else if (!strncmp(cmd, "watch=", 6)) {
                // watch=rva1,rva2,... (hex, up to 4): log writers for 5 s
                static uintptr_t wa[4]; static char wd[4][24]; static const char* wdp[4];
                static int wn; wn = 0;
                uintptr_t b = (uintptr_t)GetModuleHandleW(nullptr);
                for (char* t = cmd + 6; *t && wn < 4;) {
                    char* end; unsigned long long r = strtoull(t, &end, 16);
                    if (end == t) break;
                    wa[wn] = b + (uintptr_t)r; snprintf(wd[wn], sizeof(wd[wn]), "exe+0x%llx", r); wdp[wn] = wd[wn]; ++wn;
                    t = (*end == ',') ? end + 1 : end;
                }
                HANDLE t = CreateThread(nullptr, 0, [](LPVOID) -> DWORD { Discovery_WatchAddresses(wa, wdp, wn); return 0; }, nullptr, 0, nullptr);
                if (t) CloseHandle(t);
            }
            else if (!strcmp(cmd, "watchpayload")) {
                HANDLE t = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
                    uintptr_t b = (uintptr_t)GetModuleHandleW(nullptr);
                    uintptr_t a[3] = { b + 0x703ef78, b + 0x703ebd0, b + 0x701ce08 };
                    const char* d[3] = { "frame payload", "pending payload", "flush semaphore count" };
                    Discovery_WatchAddresses(a, d, 3);
                    return 0; }, nullptr, 0, nullptr);
                if (t) CloseHandle(t);
            }
        }
    }
    Discovery_OnPresent();
    if ((g.frames % 60) == 5) Discovery_Rehook();
    MapLog_OnPresent();
    Trace_Event("Present");
    RDoc_Poll();
    Trace_Poll();

    Perf_OnPresent();
    Passes_OnPresent();
    if (!g_cfg.vrEnabled) return;
    if (g_cfg.debugNoXr) { PresentReplay(); if (g.stFrameBegun) { if (!g.stCaptured[1]) VR_StereoCaptureEye(1); StereoEndFrame(); } return; }
    if (!g.sess) {
        if (GetTickCount() < g.nextInitTick) return;
        if (!InitXr(sc)) {
            if (g.sess || g.inst) DestroyXr("init failed");
            g.nextInitTick = GetTickCount() + 3000;
            return;
        }
    }
    PresentReplay();
    HandleTuning();
    UpdateMenu();
    if (g.stFrameBegun) { if (!g.stCaptured[1]) VR_StereoCaptureEye(1); StereoEndFrame(); }
    else { PollEvents(); if (g.sess && g.running) RunXrFrame(sc); }   // flat mode, or menus/loading in stereo mode

    DWORD now = GetTickCount();
    if (now - g.statTick >= 10000) {
        if (g.statTick) {
            LOGD("Stats: %llu game frames, %llu XR frames, session state %d", g.frames, g.xrFrames, (int)g.state); Camera_LogStats();
            LONG vp = 0, vk = 0, vd = 0; Hud_TakeVia(&vp, &vk, &vd);
            LOGI("HUD: %llu stereo frames, UI missing in %llu; UI passes %ld after the post pass, %ld recorded apart; %ld other lone binds of the scene target; %llu non-stereo frames, %llu stereo/non-stereo switches",
                 g.hudFrames, g.hudMissing, vp, vd, vk - vd, g.monoFrames, g.switches);
            { uint64_t fr = g.hudFrames + g.monoFrames; LONG d = PostFx_TakeDrawCount(); LOGI("Draws: %.0f per frame (deferred contexts, eye 1); camera FOV corrections %ld", fr ? (double)d / (double)fr : 0.0, Camera_TakeFovFixes()); }
            g.hudFrames = g.hudMissing = g.monoFrames = g.switches = 0;
            PostFx_LogStats(); Vrs_LogStats();
            if (g.gazeSpace || g_cfg.debugFakeGaze > 0.f)
                LOGI("Eye tracking: %s, %llu gaze samples, %llu without valid gaze in the last interval; last gaze %.1f / %.1f deg (right / up)",
                     g_eyeTracking ? "on" : "off", g.gazeSamples, g.gazeMisses, atanf(g.gazeTx) * 57.2958f, atanf(g.gazeTy) * 57.2958f);
            g.gazeSamples = g.gazeMisses = 0;
            char perf[512]; if (Perf_Text(perf, sizeof(perf))) { for (char* c = perf; *c; ++c) if (*c == '\n') *c = '|'; LOGI("Perf: render %ux%u | %s", g.autoW, g.autoH, perf); }
        }
        g.statTick = now;
    }
}

void VR_OnResize(IDXGISwapChain* sc) {
    if (sc != g.target) return;
    Temporal_Schedule(90);   // the game recreates its render targets: find the history textures again
    Hud_ForgetTargets();
    // We hold no back buffer references between frames; only our copy needs rebuilding,
    // which EnsureCopyTexture / CreateQuadSwapchain do lazily on the next Present.
}

// Back-buffer size the game should render at (stereo). debug: render_size = WxH forces a size.
bool VR_Active() { return g_cfg.vrEnabled && !g.disabled && g.sess && g.running; }
// The per-eye size the game should render at: render_size if set, else derived from the headset (0 = not known yet / off).
bool VR_RenderSize(UINT* w, UINT* h) {
    if (g_cfg.forceW > 0 && g_cfg.forceH > 0) { *w = g_cfg.forceW; *h = g_cfg.forceH; return true; }
    if (g_cfg.vrMode != "stereo" || !g_cfg.matchHeadsetRes || !g.autoW) return false;
    *w = g.autoW; *h = g.autoH; return true;
}
bool VR_RenderSizeOverride(UINT* w, UINT* h) {
    UINT a, b;
    if (!VR_RenderSize(&a, &b)) return false;
    if (*w == a && *h == b) return false;
    *w = a; *h = b; return true;
}

// ---- live-apply helpers for the settings menu (menu_options.cpp)
void VR_ApplyRenderScale() { if (!g.baseW) return; g.scale = g_cfg.renderScale; UpdateAutoSize(); }
void VR_SetStatsOn(bool on) { g.statsOn = on; }
void VR_SetQuadOn(bool on) { Quad_Active(); g_quadOn = on; }
void VR_SetEyeTrackingOn(bool on) { g_eyeInit = true; g_eyeTracking = on; }
void VR_Recenter() { g.needRecenter = true; }
