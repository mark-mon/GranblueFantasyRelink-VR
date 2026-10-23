// Post-processing overrides. The game's post passes are recognised by the hash of their shader bytecode
// (see passes.cpp, trigger "passes") and their pixel shaders are swapped on the deferred contexts for
// small pass-through shaders of ours:
//   TAA   (6fc023ddc2d36a64): reads current colour t3, history t4, depth t5, velocity t23; writes the new
//         history (o0) and the output (o1). Replacement: both = current colour. The camera jitter is
//         removed as well (camera.cpp), or the image would shimmer.
//   Colour grading (7325c6b940ab86b9): "ColorCorrect" pass - depth-based colour grading with near and far
//         3D LUTs over the HDR scene colour t0 (not fog: the town's haze is in the scenery itself).
//         Replacement: o0 = scene colour. Config disable_color_grading (internally disableFog).
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <mutex>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <string>
#include <algorithm>
#include <cstdio>
#include "log.h"
#include "globals.h"

namespace {
enum Fx { FX_TAA, FX_FOG, FX_COUNT };
const uint64_t kHash[FX_COUNT] = { 0x6fc023ddc2d36a64ull, 0x7325c6b940ab86b9ull };
const char* kName[FX_COUNT] = { "TAA", "colour grading" };
const char* kSrc[FX_COUNT] = {
    // TAA off: history and output = current frame colour
    "Texture2D<float4> cur : register(t3);\n"
    "struct O { float4 h : SV_Target0; float4 c : SV_Target1; };\n"
    "O main(float4 pos : SV_Position, float2 uv : TEXCOORD0) { float4 c = cur.Load(int3(pos.xy, 0)); O o; o.h = c; o.c = c; return o; }\n",
    // fog off: scene colour unchanged
    "Texture2D<float4> scene : register(t0);\n"
    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 { return float4(scene.Load(int3(pos.xy, 0)).rgb, 1); }\n",
};
volatile void* g_game[FX_COUNT][4] = {};     // the game's shader objects with that hash (usually one)
ID3D11PixelShader* g_repl[FX_COUNT] = {};
bool g_failed[FX_COUNT] = {};
std::mutex g_mx;
volatile LONG g_swaps[FX_COUNT] = {};

// ---- anti-aliasing modes (replace the game's TAA pass, 6fc0: t3 current colour (LDR), t4 history,
// t5 depth, t23 velocity in UV units (prev uv = uv - v), sampler s3 linear clamp; o0 new history, o1 output)
enum AaMode { AA_GAME, AA_OFF, AA_FXAA, AA_TAA, AA_COUNT };
const char* kAaName[AA_COUNT] = { "game TAA", "off", "FXAA", "mild TAA" };
volatile int g_aa = -1;
int AaMode_() {
    if (g_aa < 0) {
        const std::string& m = g_cfg.aaMode;
        g_aa = m == "game" ? AA_GAME : m == "off" ? AA_OFF : m == "fxaa" ? AA_FXAA : m == "taa" ? AA_TAA : (g_cfg.disableTaa ? AA_OFF : AA_GAME);
    }
    return g_aa;
}
const char* kAaSrc[AA_COUNT] = {
    "",
    // off: history and output = current frame colour
    "Texture2D<float4> cur : register(t3);\n"
    "struct O { float4 h : SV_Target0; float4 c : SV_Target1; };\n"
    "O main(float4 pos : SV_Position, float2 uv : TEXCOORD0) { float4 c = cur.Load(int3(pos.xy, 0)); O o; o.h = c; o.c = c; return o; }\n",
    // FXAA (console-style): single-frame edge smoothing
    "Texture2D<float4> cur : register(t3); SamplerState smp : register(s3);\n"
    "struct O { float4 h : SV_Target0; float4 c : SV_Target1; };\n"
    "float L(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }\n"
    "O main(float4 pos : SV_Position, float2 uv : TEXCOORD0) {\n"
    "  uint w, h; cur.GetDimensions(w, h); float2 px = 1.0 / float2(w, h); int3 p = int3(pos.xy, 0);\n"
    "  float4 cM = cur.Load(p);\n"
    "  float lNW = L(cur.Load(p, int2(-1,-1)).rgb), lNE = L(cur.Load(p, int2(1,-1)).rgb), lSW = L(cur.Load(p, int2(-1,1)).rgb), lSE = L(cur.Load(p, int2(1,1)).rgb), lM = L(cM.rgb);\n"
    "  float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE))), lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));\n"
    "  O o; o.h = cM; o.c = cM;\n"
    "  if (lMax - lMin < max(0.0312, lMax * 0.125)) return o;\n"
    "  float2 dir = float2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));\n"
    "  float red = max((lNW + lNE + lSW + lSE) * 0.03125, 1.0 / 128.0);\n"
    "  dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + red), -8.0, 8.0) * px;\n"
    "  float3 a = 0.5 * (cur.SampleLevel(smp, uv + dir * (1.0/3.0 - 0.5), 0).rgb + cur.SampleLevel(smp, uv + dir * (2.0/3.0 - 0.5), 0).rgb);\n"
    "  float3 b = a * 0.5 + 0.25 * (cur.SampleLevel(smp, uv - dir * 0.5, 0).rgb + cur.SampleLevel(smp, uv + dir * 0.5, 0).rgb);\n"
    "  float lB = L(b); o.c = float4((lB < lMin || lB > lMax) ? a : b, cM.a); return o;\n"
    "}\n",
    // mild TAA: reprojected history (motion vectors), clamped to the current 3x3 neighbourhood, blended with
    // weight TAA_W (much lower than the game's) and no history sharpening filter
    "Texture2D<float4> cur : register(t3); Texture2D<float4> hist : register(t4); Texture2D<float4> vel : register(t23); SamplerState smp : register(s3);\n"
    "struct O { float4 h : SV_Target0; float4 c : SV_Target1; };\n"
    "O main(float4 pos : SV_Position, float2 uv : TEXCOORD0) {\n"
    "  int3 p = int3(pos.xy, 0); float4 c = cur.Load(p);\n"
    "  float3 mn = c.rgb, mx = c.rgb;\n"
    "  [unroll] for (int y = -1; y <= 1; ++y) [unroll] for (int x = -1; x <= 1; ++x) { float3 n = cur.Load(p + int3(x, y, 0)).rgb; mn = min(mn, n); mx = max(mx, n); }\n"
    "  float2 puv = uv - vel.Load(p).xy;\n"
    "  float3 r = c.rgb;\n"
    "  if (all(puv > 0.0) && all(puv < 1.0)) { float3 hc = clamp(hist.SampleLevel(smp, puv, 0).rgb, mn, mx); r = lerp(c.rgb, hc, TAA_W); }\n"
    "  O o; o.h = float4(r, c.a); o.c = float4(r, c.a); return o;\n"
    "}\n",
};
ID3D11PixelShader* g_aaRepl[AA_COUNT] = {}; bool g_aaFailed[AA_COUNT] = {};

bool Enabled(int fx) { return fx == FX_TAA ? AaMode_() != AA_GAME : fx == FX_FOG ? g_cfg.disableFog : false; }

ID3D11PixelShader* CompilePS(ID3D11DeviceContext* c, const char* src, const D3D_SHADER_MACRO* defs, const char* what) {
    HMODULE comp = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = comp ? (pD3DCompile)GetProcAddress(comp, "D3DCompile") : nullptr;
    ID3DBlob* code = nullptr; ID3DBlob* err = nullptr; ID3D11PixelShader* ps = nullptr;
    ID3D11Device* dev = nullptr; c->GetDevice(&dev);
    if (compile && dev && SUCCEEDED(compile(src, strlen(src), nullptr, defs, nullptr, "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err)))
        dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps);
    if (!ps) LOGE("PostFx: %s shader failed: %s", what, err ? (const char*)err->GetBufferPointer() : "no compiler");
    else LOGI("PostFx: %s shader ready", what);
    if (code) code->Release(); if (err) err->Release(); if (dev) dev->Release();
    return ps;
}
ID3D11PixelShader* AaReplacement(ID3D11DeviceContext* c, int m) {
    if (g_aaRepl[m] || g_aaFailed[m]) return g_aaRepl[m];
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_aaRepl[m] || g_aaFailed[m]) return g_aaRepl[m];
    char w[32]; snprintf(w, sizeof(w), "%.3f", (std::min)((std::max)(g_cfg.taaStrength, 0.f), 1.f) * 0.95f);
    D3D_SHADER_MACRO defs[] = { { "TAA_W", w }, { nullptr, nullptr } };
    g_aaRepl[m] = CompilePS(c, kAaSrc[m], defs, kAaName[m]);
    if (!g_aaRepl[m]) g_aaFailed[m] = true;
    return g_aaRepl[m];
}

ID3D11PixelShader* Replacement(ID3D11DeviceContext* c, int fx) {
    if (g_repl[fx] || g_failed[fx]) return g_repl[fx];
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_repl[fx] || g_failed[fx]) return g_repl[fx];
    HMODULE comp = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = comp ? (pD3DCompile)GetProcAddress(comp, "D3DCompile") : nullptr;
    ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
    ID3D11Device* dev = nullptr; c->GetDevice(&dev);
    if (compile && dev && SUCCEEDED(compile(kSrc[fx], strlen(kSrc[fx]), nullptr, nullptr, nullptr, "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err)))
        dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_repl[fx]);
    if (!g_repl[fx]) { g_failed[fx] = true; LOGE("PostFx: %s replacement shader failed: %s", kName[fx], err ? (const char*)err->GetBufferPointer() : "no compiler"); }
    else LOGI("PostFx: %s disabled (shader replaced)", kName[fx]);
    if (code) code->Release(); if (err) err->Release(); if (dev) dev->Release();
    return g_repl[fx];
}
} // namespace

// shader creation (hooks.cpp, any thread)
void PostFx_SkipCheckCreated(void* shader, uint64_t hash);
void PostFx_OnShaderCreated(void* shader, uint64_t hash) {
    PostFx_SkipCheckCreated(shader, hash);
    for (int fx = 0; fx < FX_COUNT; ++fx) if (hash == kHash[fx]) {
        for (auto& slot : g_game[fx]) if (!slot) { slot = shader; LOGI("PostFx: found the game's %s shader", kName[fx]); break; }
    }
}
// ---- skipping draws by pixel shader (debug_skip_ps = hash,hash,...; and effects that are removed by skipping)
namespace {
struct SkipCtx { ID3D11DeviceContext* c; bool skip; };
SkipCtx g_skipCtx[64]; volatile LONG g_nSkipCtx = 0;
std::mutex g_skipMx;
std::vector<uint64_t> g_skipHashes; bool g_skipParsed = false;
std::unordered_map<void*, bool> g_skipShader;   // shader -> skip (filled at creation)
volatile LONG g_skipped = 0;
volatile bool g_anySkip = false;
void ParseSkip() {
    if (g_skipParsed) return;
    g_skipParsed = true;
    const char* p = g_cfg.debugSkipPs.c_str();
    while (*p) { char* e = nullptr; unsigned long long h = _strtoui64(p, &e, 16); if (e == p) { ++p; continue; } if (h) g_skipHashes.push_back(h); p = e; }
    if (!g_skipHashes.empty()) LOGI("PostFx: skipping draws of %zu pixel shaders (debug_skip_ps)", g_skipHashes.size());
}
SkipCtx* SkipSlot(ID3D11DeviceContext* c) {
    LONG n = g_nSkipCtx;
    for (LONG i = 0; i < n; ++i) if (g_skipCtx[i].c == c) return &g_skipCtx[i];
    std::lock_guard<std::mutex> lk(g_skipMx);
    n = g_nSkipCtx;
    for (LONG i = 0; i < n; ++i) if (g_skipCtx[i].c == c) return &g_skipCtx[i];
    if (n >= 64) return nullptr;
    g_skipCtx[n] = { c, false }; MemoryBarrier(); g_nSkipCtx = n + 1;
    return &g_skipCtx[n];
}
}
void PostFx_SkipCheckCreated(void* shader, uint64_t hash) {
    std::lock_guard<std::mutex> lk(g_skipMx);
    ParseSkip();
    for (auto h : g_skipHashes) if (h == hash) { g_skipShader[shader] = true; g_anySkip = true; LOGI("PostFx: found shader %016llx to skip", (unsigned long long)hash); }
}
// deferred-context draw: true = drop it
volatile LONG g_drawCount = 0;   // deferred-context draws (diagnostics: culling experiments)
bool PostFx_SkipDraw(ID3D11DeviceContext* c) {
    InterlockedIncrement(&g_drawCount);
    if (!g_anySkip) return false;
    SkipCtx* s = SkipSlot(c);
    if (s && s->skip) { InterlockedIncrement(&g_skipped); return true; }
    return false;
}
// deferred-context PSSetShader
ID3D11PixelShader* PostFx_ReplacePS(ID3D11DeviceContext* c, ID3D11PixelShader* p) {
    if (g_anySkip) {
        bool sk = false;
        { std::lock_guard<std::mutex> lk(g_skipMx); auto it = g_skipShader.find(p); sk = it != g_skipShader.end(); }
        if (SkipCtx* s = SkipSlot(c)) s->skip = sk;
    }
    if (!p) return p;
    for (int fx = 0; fx < FX_COUNT; ++fx) {
        for (auto slot : g_game[fx]) if (slot == p) {
            if (!Enabled(fx)) return p;
            ID3D11PixelShader* r = fx == FX_TAA ? AaReplacement(c, AaMode_()) : Replacement(c, fx);
            if (r) { InterlockedIncrement(&g_swaps[fx]); return r; }
            return p;
        }
    }
    return p;
}
ID3D11ComputeShader* PostFx_ReplaceCS(ID3D11DeviceContext*, ID3D11ComputeShader* p) { return p; }
// camera jitter scale for the current AA mode (the game's TAA needs its jitter; off/FXAA need none)
float PostFx_JitterScale() {
    int m = AaMode_();
    return m == AA_GAME ? 1.f : m == AA_TAA ? (std::min)((std::max)(g_cfg.taaJitter, 0.f), 1.f) : 0.f;
}
const char* PostFx_AaName() { return kAaName[AaMode_()]; }
void Temporal_Schedule(int frames);
// F1: next AA mode. The history textures the per-eye history must keep depend on the mode (mild/game TAA
// read their history, off/FXAA don't), so detect them again.
void PostFx_CycleAa() { int m = (AaMode_() + 1) % AA_COUNT; g_aa = m; LOGI("Anti-aliasing: %s", kAaName[m]); if (g_cfg.perEyeHistory) Temporal_Schedule(30); }
LONG PostFx_TakeDrawCount() { return InterlockedExchange(&g_drawCount, 0); }
void PostFx_LogStats() {
    if (g_anySkip) LOGI("PostFx: %ld draws skipped in the last interval", InterlockedExchange(&g_skipped, 0));
    for (int fx = 0; fx < FX_COUNT; ++fx) if (Enabled(fx)) {
        LONG n = InterlockedExchange(&g_swaps[fx], 0);
        LOGI("PostFx: %s pass replaced %ld times in the last interval%s", kName[fx], n, g_game[fx][0] ? "" : " (game shader not seen yet)");
    }
}

// ---- settings menu hooks
static const char* const kAaValue[AA_COUNT] = { "game", "off", "fxaa", "taa" };
const char* PostFx_AaValue() { return kAaValue[AaMode_()]; }
// aa_mode changed in g_cfg: re-read it (and rebuild per-eye history, like the hotkey does)
void PostFx_AaFromConfig() { g_aa = -1; LOGI("Anti-aliasing: %s", kAaName[AaMode_()]); if (g_cfg.perEyeHistory) Temporal_Schedule(30); }
// taa_strength changed: recompile the mild TAA shader on next use. The old shader object is leaked on
// purpose (a deferred command list recorded this frame may still reference it); it is small.
void PostFx_TaaChanged() { g_aaRepl[AA_TAA] = nullptr; g_aaFailed[AA_TAA] = false; }
