#include <intrin.h>
#include "camera.h"
// Camera discovery (diagnostic). Press F9 during gameplay with the camera still:
//  1. Scans every CPU buffer update for 2 frames, looking for projection / view matrices.
//  2. Searches process memory for the CPU-side copies of those matrices and names the
//     owning object via RTTI.
//  3. Puts hardware write-watchpoints on those copies for ~3 s and logs the code
//     (exe+RVA, with a short call stack) that writes them.
#include <windows.h>
#include <d3d11_4.h>
#include <tlhelp32.h>
#include <cmath>
#include <cstring>
#include <vector>
#include <string>
#include <mutex>
#include <atomic>
#include <algorithm>
#include "MinHook.h"
#include "globals.h"

bool Replay_OnExec(ID3D11DeviceContext* c, ID3D11CommandList* cl, BOOL restore, void (STDMETHODCALLTYPE* exec)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL));
ID3D11RenderTargetView* Hud_OnSetRenderTargets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv);
bool Replay_SubstCB(UINT n, ID3D11Buffer* const* b, const UINT* f, const UINT* k, ID3D11Buffer** ob, UINT* of, UINT* ok);
void Temporal_OnSRVs(ID3D11DeviceContext*, UINT, ID3D11ShaderResourceView* const*); void Temporal_OnRTVs(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*);
void Temporal_OnUAVs(ID3D11DeviceContext*, UINT, ID3D11UnorderedAccessView* const*); void Temporal_OnCopy(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
void Temporal_OnImmediateWrite(ID3D11Resource*); void Temporal_OnFinish(ID3D11DeviceContext*, ID3D11CommandList*); void Temporal_OnExec(ID3D11DeviceContext*, ID3D11CommandList*);
void Temporal_OnDSV(ID3D11DeviceContext*, ID3D11DepthStencilView*); void Temporal_OnClearView(ID3D11DeviceContext*, ID3D11View*);
void Perf_OnExec(ID3D11DeviceContext*);
ID3D11PixelShader* PostFx_ReplacePS(ID3D11DeviceContext*, ID3D11PixelShader*);
ID3D11ComputeShader* PostFx_ReplaceCS(ID3D11DeviceContext*, ID3D11ComputeShader*);
bool PostFx_SkipDraw(ID3D11DeviceContext*);
void Vrs_OnSetRenderTargets(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
bool Passes_Capturing(); void Passes_SetShader(ID3D11DeviceContext*, char, void*); void Passes_SetSRVs(ID3D11DeviceContext*, char, UINT, UINT, ID3D11ShaderResourceView* const*);
void Passes_SetRTVs(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*); void Passes_SetUAVs(ID3D11DeviceContext*, UINT, UINT, ID3D11UnorderedAccessView* const*);
void Passes_OnWork(ID3D11DeviceContext*, const char*, UINT, UINT); void Passes_OnFinish(ID3D11DeviceContext*);
bool Hud_InUiPass(ID3D11DeviceContext* c); bool Hud_PendingAny();
bool Hud_BeforeDraw(ID3D11DeviceContext*, bool, ID3D11RenderTargetView**, ID3D11BlendState**, FLOAT[4], UINT*);
ID3D11BlendState* Hud_OnBlend(ID3D11DeviceContext*, ID3D11BlendState*, const FLOAT[4], UINT); ID3D11BlendState* Hud_BlendFor(ID3D11DeviceContext* c, ID3D11BlendState* s); void Hud_OnFinishList(ID3D11DeviceContext* c);
namespace {

// ------------------------------------------------------------------ matrix tests
// m is 16 floats in memory order; "rm" = interpret as row-major m[r*4+c]
inline float At(const float* m, bool transposed, int r, int c) { return transposed ? m[c * 4 + r] : m[r * 4 + c]; }
inline bool Z(float v, float e = 1e-4f) { return fabsf(v) < e; }
float g_screenAspect = 0.f;   // height/width of the game's back buffer (ys/xs of its projection)
// True if the matrix has at least 'need' values that are not 0, +-1 or simple fractions.
bool HasRealValues(const float* m, int n, int need) {
    int c = 0;
    for (int i = 0; i < n; ++i) {
        float a = fabsf(m[i]);
        if (a < 1e-6f) continue;
        float x4 = a * 4.f;
        if (fabsf(x4 - roundf(x4)) < 1e-5f) continue;   // 0.25 multiples
        ++c;
    }
    return c >= need;
}
bool AspectOk(float a) { return g_screenAspect <= 0.f || fabsf(a / g_screenAspect - 1.f) < 0.15f; }
inline bool Fin(const float* m) { for (int i = 0; i < 16; ++i) if (!std::isfinite(m[i])) return false; return true; }

// Perspective projection (any handedness, reverse/forward Z, optional jitter/off-centre).
bool IsProj(const float* m, bool t, float* fovYDeg, float* aspect) {
    float xs = At(m, t, 0, 0), ys = At(m, t, 1, 1);
    if (!(xs > 0.1f && xs < 20.f && ys > 0.1f && ys < 20.f)) return false;
    if (!Z(At(m, t, 0, 1)) || !Z(At(m, t, 0, 2)) || !Z(At(m, t, 0, 3))) return false;
    if (!Z(At(m, t, 1, 0)) || !Z(At(m, t, 1, 2)) || !Z(At(m, t, 1, 3))) return false;
    if (!Z(At(m, t, 3, 0)) || !Z(At(m, t, 3, 1)) || !Z(At(m, t, 3, 3))) return false;
    if (fabsf(fabsf(At(m, t, 2, 3)) - 1.f) > 1e-3f) return false;
    if (fabsf(At(m, t, 2, 0)) > 0.2f || fabsf(At(m, t, 2, 1)) > 0.2f) return false;
    if (Z(At(m, t, 3, 2), 1e-7f) && Z(At(m, t, 2, 2), 1e-7f)) return false;
    float a = ys / xs;
    if (a < 0.3f || a > 5.f || !AspectOk(a)) return false;
    if (!HasRealValues(m, 16, 2)) return false;
    *fovYDeg = 2.f * atanf(1.f / ys) * 57.29578f;
    *aspect = a;
    return true;
}

// Rigid transform: orthonormal 3x3 + translation, last column (0,0,0,1) in row-major sense.
bool IsRigid(const float* m, bool t) {
    if (!Z(At(m, t, 0, 3)) || !Z(At(m, t, 1, 3)) || !Z(At(m, t, 2, 3)) || fabsf(At(m, t, 3, 3) - 1.f) > 1e-4f) return false;
    float r[3][3];
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) r[i][j] = At(m, t, i, j);
    for (int i = 0; i < 3; ++i) {
        float l = r[i][0] * r[i][0] + r[i][1] * r[i][1] + r[i][2] * r[i][2];
        if (fabsf(l - 1.f) > 2e-3f) return false;
        for (int j = i + 1; j < 3; ++j)
            if (fabsf(r[i][0] * r[j][0] + r[i][1] * r[j][1] + r[i][2] * r[j][2]) > 2e-3f) return false;
    }
    // reject identity-ish rotation with zero translation (common placeholder)
    bool ident = Z(r[0][0] - 1) && Z(r[1][1] - 1) && Z(r[2][2] - 1) && Z(At(m, t, 3, 0)) && Z(At(m, t, 3, 1)) && Z(At(m, t, 3, 2));
    if (ident) return false;
    for (int c = 0; c < 3; ++c) if (fabsf(At(m, t, 3, c)) > 1e6f) return false;
    if (!HasRealValues(m, 16, 3)) return false;
    return true;
}

inline float Dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline float Len3(const float* a) { return sqrtf(Dot3(a, a)); }

// Combined view*projection: one axis (w) is a unit vector (camera forward), x/y axes are
// perpendicular to it and to each other with lengths xs/ys, z axis parallel to w.
bool IsViewProj(const float* m, bool t, float* fovYDeg, float* aspect, float fwd[3], float right[3]) {
    float r[4][4];
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) r[i][j] = At(m, t, j, i);   // r[i] = column i of row-major = output component i
    float lw = Len3(r[3]);
    if (fabsf(lw - 1.f) > 2e-3f) return false;
    float lx = Len3(r[0]), ly = Len3(r[1]), lz = Len3(r[2]);
    if (!(lx > 0.1f && lx < 20.f && ly > 0.1f && ly < 20.f && lz > 1e-6f)) return false;
    if (fabsf(Dot3(r[0], r[3])) > 0.02f * lx || fabsf(Dot3(r[1], r[3])) > 0.02f * ly || fabsf(Dot3(r[0], r[1])) > 1e-3f * lx * ly) return false;
    float cz = fabsf(Dot3(r[2], r[3])) / lz;
    if (cz < 0.999f) return false;
    float a = ly / lx;
    if (a < 0.3f || a > 5.f || !AspectOk(a)) return false;
    if (!HasRealValues(m, 16, 4)) return false;
    // reject pure projections (axes aligned with basis) - handled by IsProj
    if (fabsf(fabsf(r[3][2]) - 1.f) < 1e-6f && fabsf(r[0][0] - lx) < 1e-6f) return false;
    *fovYDeg = 2.f * atanf(1.f / ly) * 57.29578f;
    *aspect = a;
    for (int k = 0; k < 3; ++k) { fwd[k] = r[3][k]; right[k] = r[0][k] / lx; }
    return true;
}

// 3x4 rigid transform: rows-of-4 [R|t] or rows-of-3 [R; t]
bool IsRigid34(const float* m, bool rowsOf3) {
    float r[3][3];
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) r[i][j] = rowsOf3 ? m[i * 3 + j] : m[i * 4 + j];
    for (int i = 0; i < 3; ++i) {
        float l = Dot3(r[i], r[i]);
        if (fabsf(l - 1.f) > 2e-3f) return false;
        for (int j = i + 1; j < 3; ++j) if (fabsf(Dot3(r[i], r[j])) > 2e-3f) return false;
    }
    if (Z(r[0][0] - 1) && Z(r[1][1] - 1) && Z(r[2][2] - 1)) return false;
    float tx = rowsOf3 ? m[9] : m[3], ty = rowsOf3 ? m[10] : m[7], tz = rowsOf3 ? m[11] : m[11];
    if (fabsf(tx) > 1e6f || fabsf(ty) > 1e6f || fabsf(tz) > 1e6f) return false;
    if (Z(tx) && Z(ty) && Z(tz)) return false;
    if (!HasRealValues(m, 12, 3)) return false;
    return true;
}

// ------------------------------------------------------------------ capture state
struct ScanHit { int kind; bool t; uint32_t off; float m[16]; float fov, aspect; float fwd[3], right[3]; };
struct BufferCapture {
    uint32_t byteWidth; uint32_t bind; int via;   // via: 0=Map 1=UpdateSubresource
    std::vector<ScanHit> hits;
    std::vector<float> data;                       // copy of contents (window)
    uint32_t dataStart = 0;                        // byte offset of data[0] in the buffer
    uint32_t count = 1;
};
enum { K_PROJ = 1, K_RIGID = 2, K_VIEWPROJ = 3, K_RIGID34 = 4 };
const char* KindName(int k) { return k == K_PROJ ? "PROJECTION" : k == K_RIGID ? "RIGID" : k == K_VIEWPROJ ? "VIEWPROJ" : "RIGID3x4"; }

std::mutex g_capMx;
struct RawDump { std::string name; std::vector<uint8_t> bytes; };
std::vector<RawDump> g_dumps;          // raw data captured during F9 (written to disk by analysis thread)
size_t g_dumpBytes = 0;
std::vector<ID3D11Buffer*> g_boundCBs; // unique CBs bound during capture (AddRef'd)
struct CopyInfo { uint32_t srcBw, srcBind, srcUsage, dstBw, dstBind, dstUsage; int region; uint32_t count; };
std::vector<CopyInfo> g_copies;
std::atomic<uint64_t> g_draws{ 0 }, g_dispatches{ 0 }, g_cbBinds{ 0 };
std::atomic<int> g_dumpSeq{ 0 };
std::vector<float> g_bigCB;            // copy of the largest bound constant buffer (per-view constants ring)
bool g_writeDumps = false;
ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;

void AddDump(const char* prefix, uint32_t bw, uint32_t bind, const void* data, size_t n) {
    if (!g_writeDumps) return;
    std::lock_guard<std::mutex> lk(g_capMx);
    if (g_dumpBytes + n > 256u * 1024 * 1024) return;
    char nm[128];
    snprintf(nm, sizeof(nm), "%03d_%s_%u_bind%x.bin", g_dumpSeq.fetch_add(1), prefix, bw, bind);
    RawDump d; d.name = nm; d.bytes.assign((const uint8_t*)data, (const uint8_t*)data + n);
    g_dumpBytes += n;
    g_dumps.push_back(std::move(d));
}
std::vector<BufferCapture> g_caps;                 // only buffers that had a projection
std::atomic<int> g_captureFrames{ 0 };
std::atomic<uint64_t> g_mapCount{ 0 }, g_scanBytes{ 0 }, g_bigSkipped{ 0 }, g_deferredCreated{ 0 };

struct BufInfo { uint32_t byteWidth, bind, usage, cpu, misc; int via; uint32_t count; };
std::vector<BufInfo> g_bufInfos;   // every buffer written during capture (deduped by desc)

void NoteBuffer(uint32_t bw, uint32_t bind, uint32_t usage, uint32_t cpu, uint32_t misc, int via) {
    std::lock_guard<std::mutex> lk(g_capMx);
    for (auto& b : g_bufInfos) if (b.byteWidth == bw && b.bind == bind && b.usage == usage && b.misc == misc && b.via == via) { ++b.count; return; }
    if (g_bufInfos.size() < 64) g_bufInfos.push_back({ bw, bind, usage, cpu, misc, via, 1 });
}

void ScanBuffer(const void* data, uint32_t size, uint32_t byteWidth, uint32_t bind, int via) {
    if (size > 64u * 1024 * 1024) { ++g_bigSkipped; return; }
    g_scanBytes += size;
    // copy out first: mapped memory is write-combined, read it once
    std::vector<float> f(size / 4);
    memcpy(f.data(), data, (size / 4) * 4);
    std::vector<ScanHit> hits;
    bool hasProj = false;
    uint32_t skipUntil = 0;
    for (uint32_t o = 0; o + 16 <= f.size(); ++o) {   // any 4-byte alignment (structured buffers are packed)
        if (o < skipUntil) continue;
        const float* m = &f[o];
        if (!Fin(m)) continue;
        bool found = false;
        for (int t = 0; t < 2 && !found; ++t) {
            ScanHit h{}; float fov, asp;
            if (IsProj(m, t, &fov, &asp)) { h.kind = K_PROJ; h.fov = fov; h.aspect = asp; hasProj = true; }
            else if (IsViewProj(m, t, &fov, &asp, h.fwd, h.right)) { h.kind = K_VIEWPROJ; h.fov = fov; h.aspect = asp; hasProj = true; }
            else if (IsRigid(m, t)) h.kind = K_RIGID;
            else if (t == 0 && IsRigid34(m, false)) h.kind = K_RIGID34;
            else if (t == 1 && IsRigid34(m, true)) { h.kind = K_RIGID34; }
            else continue;
            h.t = t; h.off = o * 4; memcpy(h.m, m, 64);
            hits.push_back(h);
            found = true;
            skipUntil = o + (h.kind == K_RIGID34 ? 12 : 16);
        }
    }
    if (!hasProj) return;
    std::lock_guard<std::mutex> lk(g_capMx);
    for (auto& c : g_caps) {
        if (c.byteWidth == byteWidth && c.hits.size() == hits.size()) {
            bool same = true;
            for (size_t i = 0; i < hits.size() && same; ++i) same = hits[i].off == c.hits[i].off && hits[i].kind == c.hits[i].kind;
            if (same) { ++c.count; return; }
        }
    }
    if (g_caps.size() >= 24) return;
    BufferCapture c; c.byteWidth = byteWidth; c.bind = bind; c.via = via;
    // keep a 4 KB window starting 1 KB before the first projection
    uint32_t firstProj = 0;
    for (auto& h : hits) if (h.kind == K_PROJ || h.kind == K_VIEWPROJ) { firstProj = h.off; break; }
    uint32_t start = firstProj > 1024 ? (firstProj - 1024) / 4 : 0;
    size_t end = std::min<size_t>(f.size(), start + 1024);
    c.dataStart = start * 4;
    c.data.assign(f.begin() + start, f.begin() + end);
    // keep only matrices inside a 16 KB neighbourhood of the projection (ring buffers hold everything)
    std::vector<ScanHit> nearHits;
    for (auto& h : hits) if ((h.kind == K_PROJ || h.kind == K_VIEWPROJ || h.off + 8192 >= firstProj) && h.off <= firstProj + 8192 && nearHits.size() < 48) nearHits.push_back(h);
    c.hits = std::move(nearHits);
    g_caps.push_back(std::move(c));
}

// ------------------------------------------------------------------ context hooks
using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using UnmapFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using UpdSubFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
using UpdSub1Fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT, UINT);
using ExecFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
using CreateDefFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, UINT, ID3D11DeviceContext**);

struct CtxHooks { void* map = nullptr; MapFn oMap = nullptr; UnmapFn oUnmap = nullptr; UpdSubFn oUpd = nullptr; UpdSub1Fn oUpd1 = nullptr; };
ExecFn oExec = nullptr;
std::atomic<uint64_t> g_execCount{ 0 }, g_upd1Count{ 0 }, g_updCount{ 0 };
CtxHooks g_imm, g_def;
CreateDefFn oCreateDef = nullptr;

struct Pending { ID3D11Resource* res; void* p; uint32_t size, bw, bind; };
thread_local Pending t_pending[32];
thread_local int t_pendingN = 0;

HRESULT MapCommon(MapFn o, ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP type, UINT fl, D3D11_MAPPED_SUBRESOURCE* m) {
    HRESULT hr = o(c, r, sub, type, fl, m);
    if (g_captureFrames.load(std::memory_order_relaxed) > 0 && SUCCEEDED(hr) && m && m->pData && type != D3D11_MAP_READ) {
        ++g_mapCount;
        D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_BUFFER && t_pendingN < 32) {
            D3D11_BUFFER_DESC bd; ((ID3D11Buffer*)r)->GetDesc(&bd);
            NoteBuffer(bd.ByteWidth, bd.BindFlags, bd.Usage, bd.CPUAccessFlags, bd.MiscFlags, 10 + (int)type);
            t_pending[t_pendingN++] = { r, m->pData, bd.ByteWidth, bd.ByteWidth, bd.BindFlags };
        }
    }
    return hr;
}
void UnmapCommon(UnmapFn o, ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub) {
    for (int i = 0; i < t_pendingN; ++i) {
        if (t_pending[i].res == r) {
            Pending p = t_pending[i];
            t_pending[i] = t_pending[--t_pendingN];
            AddDump("map", p.bw, p.bind, p.p, p.size);
            ScanBuffer(p.p, p.size, p.bw, p.bind, 0);
            break;
        }
    }
    o(c, r, sub);
}
void ScanUpd(ID3D11Resource* r, const D3D11_BOX* box, const void* src) {
    if (g_captureFrames.load(std::memory_order_relaxed) <= 0 || !src) return;
    D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return;
    D3D11_BUFFER_DESC bd; ((ID3D11Buffer*)r)->GetDesc(&bd);
    NoteBuffer(bd.ByteWidth, bd.BindFlags, bd.Usage, bd.CPUAccessFlags, bd.MiscFlags, 1);
    uint32_t size = box ? (box->right - box->left) : bd.ByteWidth;
    if (size > bd.ByteWidth) return;
    if (bd.ByteWidth != 16 || bd.BindFlags != D3D11_BIND_CONSTANT_BUFFER) AddDump("upd", bd.ByteWidth, bd.BindFlags, src, size);
    ScanBuffer(src, size, bd.ByteWidth, bd.BindFlags, 1);
}
void UpdCommon(UpdSubFn o, ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* src, UINT rp, UINT dp) {
    if (g_captureFrames.load(std::memory_order_relaxed) > 0) ++g_updCount;
    ScanUpd(r, box, src);
    o(c, r, sub, box, src, rp, dp);
}
void STDMETHODCALLTYPE hkUpd1I(ID3D11DeviceContext* c, ID3D11Resource* r, UINT s, const D3D11_BOX* b, const void* p, UINT rp, UINT dp, UINT fl);
void STDMETHODCALLTYPE hkUpd1D(ID3D11DeviceContext* c, ID3D11Resource* r, UINT s, const D3D11_BOX* b, const void* p, UINT rp, UINT dp, UINT fl);
// ---- map logger ("maplog" trigger): every non-constant-buffer Map/Unmap + ExecuteCommandList for a few frames
struct MapLogE { uint64_t t; uint32_t tid; char kind; char ctx; uint32_t type; void* res; uint32_t bw, bind; uintptr_t ra; void* p; void* c; };
static MapLogE g_ml[8192]; static std::atomic<int> g_mlN{ 0 }; std::atomic<int> g_mapLogFrames{ 0 };
static std::atomic<uint64_t> g_mlCB{ 0 };
static void MapLogAdd(char kind, char ctx, ID3D11Resource* r, uint32_t type, uintptr_t ra, void* p, void* cptr = nullptr) {
    if (g_mapLogFrames.load(std::memory_order_relaxed) <= 0) return;
    uint32_t bw = 0, bind = 0;
    if (r) { D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) { D3D11_BUFFER_DESC bd; ((ID3D11Buffer*)r)->GetDesc(&bd); bw = bd.ByteWidth; bind = bd.BindFlags; }
        else bind = 0x80000000u | (uint32_t)dim; }
    if ((bind & D3D11_BIND_CONSTANT_BUFFER) && !(bind & 0x80000000u)) { ++g_mlCB; return; }
    int i = g_mlN.fetch_add(1); if (i >= 8192) return;
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    g_ml[i] = { (uint64_t)q.QuadPart, GetCurrentThreadId(), kind, ctx, type, r, bw, bind, ra, p, cptr };
}
} // namespace
void MapLog_Marker(char kind, uintptr_t v) { MapLogAdd(kind, '-', nullptr, (uint32_t)v, 0, nullptr); }
void Discovery_Rehook();
void MapLog_Start(int frames) { Discovery_Rehook(); g_mlN = 0; g_mlCB = 0; g_mapLogFrames = frames; LOGI("maplog: started for %d presents", frames); }
void MapLog_OnPresent() {
    int f = g_mapLogFrames.load(); if (f <= 0) return;
    MapLog_Marker('P', Camera_LastStereoEye());
    if (--g_mapLogFrames > 0) return;
    int n = (std::min)(g_mlN.load(), 8192);
    uintptr_t b = (uintptr_t)GetModuleHandleW(nullptr);
    LOGI("maplog: %d events, %llu constant-buffer maps skipped", n, g_mlCB.load());
    for (int i = 0; i < n; ++i) { auto& e = g_ml[i];
        if (e.kind == 'X' || e.kind == 'P' || e.kind == 'E' || e.kind == 'B' || e.kind == 'R') LOGI("ML %c t%05u val=%u", e.kind, e.tid, e.type);
        else LOGI("ML %c%c t%05u ctx=%p res=%p bw=%u bind=0x%x type=%u ra=exe+0x%llx", e.kind, e.ctx, e.tid, e.c, e.res, e.bw, e.bind, e.type, (unsigned long long)(e.ra - b)); }
}
namespace {
HRESULT STDMETHODCALLTYPE hkMapI(ID3D11DeviceContext* c, ID3D11Resource* r, UINT s, D3D11_MAP t, UINT f, D3D11_MAPPED_SUBRESOURCE* m) { HRESULT hr = MapCommon(g_imm.oMap, c, r, s, t, f, m); MapLogAdd('M', 'I', r, t, (uintptr_t)_ReturnAddress(), m ? m->pData : nullptr, c); return hr; }
void STDMETHODCALLTYPE hkUnmapI(ID3D11DeviceContext* c, ID3D11Resource* r, UINT s) { MapLogAdd('U', 'I', r, 0, (uintptr_t)_ReturnAddress(), nullptr, c); UnmapCommon(g_imm.oUnmap, c, r, s); }
void STDMETHODCALLTYPE hkUpdI(ID3D11DeviceContext* c, ID3D11Resource* r, UINT s, const D3D11_BOX* b, const void* p, UINT rp, UINT dp) { UpdCommon(g_imm.oUpd, c, r, s, b, p, rp, dp); }
HRESULT STDMETHODCALLTYPE hkMapD(ID3D11DeviceContext* c, ID3D11Resource* r, UINT s, D3D11_MAP t, UINT f, D3D11_MAPPED_SUBRESOURCE* m) { HRESULT hr = MapCommon(g_def.oMap, c, r, s, t, f, m); MapLogAdd('M', 'D', r, t, (uintptr_t)_ReturnAddress(), m ? m->pData : nullptr, c); return hr; }
void STDMETHODCALLTYPE hkUnmapD(ID3D11DeviceContext* c, ID3D11Resource* r, UINT s) { MapLogAdd('U', 'D', r, 0, (uintptr_t)_ReturnAddress(), nullptr, c); UnmapCommon(g_def.oUnmap, c, r, s); }
void STDMETHODCALLTYPE hkUpdD(ID3D11DeviceContext* c, ID3D11Resource* r, UINT s, const D3D11_BOX* b, const void* p, UINT rp, UINT dp) { UpdCommon(g_def.oUpd, c, r, s, b, p, rp, dp); }

void STDMETHODCALLTYPE hkUpd1I(ID3D11DeviceContext* c, ID3D11Resource* r, UINT s, const D3D11_BOX* b, const void* p, UINT rp, UINT dp, UINT fl) {
    if (g_captureFrames.load(std::memory_order_relaxed) > 0) ++g_upd1Count;
    ScanUpd(r, b, p); g_imm.oUpd1(c, r, s, b, p, rp, dp, fl);
}
void STDMETHODCALLTYPE hkUpd1D(ID3D11DeviceContext* c, ID3D11Resource* r, UINT s, const D3D11_BOX* b, const void* p, UINT rp, UINT dp, UINT fl) {
    if (g_captureFrames.load(std::memory_order_relaxed) > 0) ++g_upd1Count;
    ScanUpd(r, b, p); g_def.oUpd1(c, r, s, b, p, rp, dp, fl);
}
std::atomic<uint64_t> g_execSeq{ 0 };
std::atomic<int> g_execLog{ 0 };
void STDMETHODCALLTYPE hkExec(ID3D11DeviceContext* c, ID3D11CommandList* cl, BOOL restore) {
    if (g_captureFrames.load(std::memory_order_relaxed) > 0) ++g_execCount;
    uint64_t n = ++g_execSeq;
    MapLog_Marker('X', (uintptr_t)_ReturnAddress() - (uintptr_t)GetModuleHandleW(nullptr));
    if (Replay_OnExec(c, cl, restore, oExec)) return;
    if (g_execLog > 0) { --g_execLog; LOGD("ExecuteCommandList #%llu list %p", n, (void*)cl); }
    oExec(c, cl, restore);
}

// ---- draw / constant buffer / copy instrumentation (shared by immediate+deferred via per-target trampolines)
using SetCBFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
using DrawIdxFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIdxInstFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DrawInstFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using DispatchFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
using SetCB1Fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*);
using CreateBufFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);
using CopyResFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
using CopyRegFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);

struct InstrHooks {
    SetCBFn vsCB = nullptr, psCB = nullptr, csCB = nullptr;
    DrawIdxFn drawIdx = nullptr; DrawFn draw = nullptr; DrawIdxInstFn drawIdxInst = nullptr; DrawInstFn drawInst = nullptr;
    DispatchFn dispatch = nullptr; CopyResFn copyRes = nullptr; CopyRegFn copyReg = nullptr;
    SetCB1Fn vsCB1 = nullptr, psCB1 = nullptr, csCB1 = nullptr;
};
std::atomic<uint64_t> g_cbBindsNonNull{ 0 }, g_cb1Binds{ 0 }, g_bufCreates{ 0 }, g_cbCreates{ 0 };
CreateBufFn oCreateBuf = nullptr;
InstrHooks g_ii, g_id;

inline bool Capturing() { return g_captureFrames.load(std::memory_order_relaxed) > 0; }

void NoteCBs(UINT n, ID3D11Buffer* const* b) {
    if (!Capturing() || !b) return;
    ++g_cbBinds;
    std::lock_guard<std::mutex> lk(g_capMx);
    for (UINT i = 0; i < n; ++i) {
        if (!b[i]) continue;
        ++g_cbBindsNonNull;
        bool have = false;
        for (auto* x : g_boundCBs) if (x == b[i]) { have = true; break; }
        if (!have && g_boundCBs.size() < 256) { b[i]->AddRef(); g_boundCBs.push_back(b[i]); }
    }
}
void NoteCopy(ID3D11Resource* dst, ID3D11Resource* src, int region) {
    if (!Capturing() || !dst || !src) return;
    D3D11_RESOURCE_DIMENSION dd, sd; dst->GetType(&dd); src->GetType(&sd);
    if (dd != D3D11_RESOURCE_DIMENSION_BUFFER || sd != D3D11_RESOURCE_DIMENSION_BUFFER) return;
    D3D11_BUFFER_DESC a, b; ((ID3D11Buffer*)src)->GetDesc(&a); ((ID3D11Buffer*)dst)->GetDesc(&b);
    std::lock_guard<std::mutex> lk(g_capMx);
    for (auto& c : g_copies) if (c.srcBw == a.ByteWidth && c.srcBind == a.BindFlags && c.dstBw == b.ByteWidth && c.dstBind == b.BindFlags && c.region == region) { ++c.count; return; }
    if (g_copies.size() < 64) g_copies.push_back({ a.ByteWidth, a.BindFlags, (uint32_t)a.Usage, b.ByteWidth, b.BindFlags, (uint32_t)b.Usage, region, 1 });
}

struct DrawSrc { DWORD tid; void* ctx; char kind; std::atomic<uint64_t> n; };
DrawSrc g_drawSrc[24];
std::atomic<int> g_drawSrcN{ 0 };
volatile LONG g_drawSrcLock = 0;
thread_local void* t_lastCtx = nullptr; thread_local DrawSrc* t_lastSrc = nullptr;
inline void NoteDraw(ID3D11DeviceContext* c, char kind) {
    if (t_lastCtx == c && t_lastSrc) { ++t_lastSrc->n; return; }
    DWORD tid = GetCurrentThreadId();
    int n = g_drawSrcN.load();
    for (int i = 0; i < n; ++i) if (g_drawSrc[i].tid == tid && g_drawSrc[i].ctx == c) { t_lastCtx = c; t_lastSrc = &g_drawSrc[i]; ++g_drawSrc[i].n; return; }
    while (InterlockedCompareExchange(&g_drawSrcLock, 1, 0) != 0) YieldProcessor();
    n = g_drawSrcN.load();
    if (n < 24) {
        g_drawSrc[n].tid = tid; g_drawSrc[n].ctx = c; g_drawSrc[n].kind = kind; g_drawSrc[n].n = 1;
        t_lastCtx = c; t_lastSrc = &g_drawSrc[n];
        g_drawSrcN = n + 1;
        LOGD("Draw source %d: thread t%05lu %s context %p (vtable %p, type %u)", n, tid, kind == 'I' ? "immediate" : "deferred", (void*)c, *(void**)c, (unsigned)c->GetType());
    }
    InterlockedExchange(&g_drawSrcLock, 0);
}

std::atomic<uint64_t> g_diN{0}, g_diMin{~0ull}, g_diMax{0}, g_diSum{0};
inline void NoteInst(UINT start, UINT count) {
    ++g_diN; g_diSum += start;
    uint64_t m = g_diMin.load(); while (start < m && !g_diMin.compare_exchange_weak(m, start)) {}
    m = g_diMax.load(); while (start + count > m && !g_diMax.compare_exchange_weak(m, start + count)) {}
}
using OMSetRTFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using OMSetBlendFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11BlendState*, const FLOAT[4], UINT);
OMSetRTFn oOMSetRTD = nullptr; OMSetBlendFn oOMSetBlendD = nullptr;
// UI recorded apart from the post pass: bind our UI target (and alpha-accumulating blend) before its first draw
inline void HudDrawCheck(ID3D11DeviceContext* c, bool indexed) {
    if (!Hud_PendingAny()) return;
    ID3D11RenderTargetView* r = nullptr; ID3D11BlendState* bs = nullptr; FLOAT bf[4]; UINT m = 0xffffffff;
    if (Hud_BeforeDraw(c, indexed, &r, &bs, bf, &m)) { oOMSetRTD(c, 1, &r, nullptr); oOMSetBlendD(c, bs, bf, m); }
}
#define DEF_INSTR(SUFFIX, H)                                                                                         \
void STDMETHODCALLTYPE hkVSCB##SUFFIX(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11Buffer* const* b) { NoteCBs(n, b); H.vsCB(c, s, n, b); } \
void STDMETHODCALLTYPE hkPSCB##SUFFIX(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11Buffer* const* b) { NoteCBs(n, b); H.psCB(c, s, n, b); } \
void STDMETHODCALLTYPE hkCSCB##SUFFIX(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11Buffer* const* b) { NoteCBs(n, b); H.csCB(c, s, n, b); } \
void STDMETHODCALLTYPE hkVSCB1##SUFFIX(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11Buffer* const* b, const UINT* f, const UINT* k) { if (Capturing()) ++g_cb1Binds; NoteCBs(n, b); \
    ID3D11Buffer* ob[14]; UINT of[14], ok[14]; if (Replay_SubstCB(n, b, f, k, ob, of, ok)) H.vsCB1(c, s, n, ob, of, ok); else H.vsCB1(c, s, n, b, f, k); } \
void STDMETHODCALLTYPE hkPSCB1##SUFFIX(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11Buffer* const* b, const UINT* f, const UINT* k) { if (Capturing()) ++g_cb1Binds; NoteCBs(n, b); \
    ID3D11Buffer* ob[14]; UINT of[14], ok[14]; if (Replay_SubstCB(n, b, f, k, ob, of, ok)) H.psCB1(c, s, n, ob, of, ok); else H.psCB1(c, s, n, b, f, k); } \
void STDMETHODCALLTYPE hkCSCB1##SUFFIX(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11Buffer* const* b, const UINT* f, const UINT* k) { if (Capturing()) ++g_cb1Binds; NoteCBs(n, b); \
    ID3D11Buffer* ob[14]; UINT of[14], ok[14]; if (Replay_SubstCB(n, b, f, k, ob, of, ok)) H.csCB1(c, s, n, ob, of, ok); else H.csCB1(c, s, n, b, f, k); } \
void STDMETHODCALLTYPE hkDrawIdx##SUFFIX(ID3D11DeviceContext* c, UINT a, UINT b, INT d) { if (#SUFFIX[0] == 'D') { if (PostFx_SkipDraw(c)) return; HudDrawCheck(c, true); Passes_OnWork(c, "DrawIndexed", a, 1); } NoteDraw(c, #SUFFIX[0]); if (Capturing()) ++g_draws; H.drawIdx(c, a, b, d); } \
void STDMETHODCALLTYPE hkDraw##SUFFIX(ID3D11DeviceContext* c, UINT a, UINT b) { if (#SUFFIX[0] == 'D') { if (PostFx_SkipDraw(c)) return; HudDrawCheck(c, false); Passes_OnWork(c, "Draw", a, 1); } NoteDraw(c, #SUFFIX[0]); if (Capturing()) ++g_draws; H.draw(c, a, b); } \
void STDMETHODCALLTYPE hkDrawIdxInst##SUFFIX(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, INT e, UINT f) { if (#SUFFIX[0] == 'D') { if (PostFx_SkipDraw(c)) return; HudDrawCheck(c, true); Passes_OnWork(c, "DrawIndexedInst", a, b); } NoteDraw(c, #SUFFIX[0]); NoteInst(f, b); if (Capturing()) ++g_draws; H.drawIdxInst(c, a, b, d, e, f); } \
void STDMETHODCALLTYPE hkDrawInst##SUFFIX(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, UINT e) { if (#SUFFIX[0] == 'D') { if (PostFx_SkipDraw(c)) return; HudDrawCheck(c, false); Passes_OnWork(c, "DrawInst", a, b); } NoteDraw(c, #SUFFIX[0]); if (Capturing()) ++g_draws; H.drawInst(c, a, b, d, e); } \
void STDMETHODCALLTYPE hkDispatch##SUFFIX(ID3D11DeviceContext* c, UINT x, UINT y, UINT z) { if (#SUFFIX[0] == 'D') Passes_OnWork(c, "X Dispatch", x, y); if (Capturing()) ++g_dispatches; H.dispatch(c, x, y, z); } \
void STDMETHODCALLTYPE hkCopyRes##SUFFIX(ID3D11DeviceContext* c, ID3D11Resource* d, ID3D11Resource* s) { NoteCopy(d, s, 0); if (#SUFFIX[0] == 'D') Temporal_OnCopy(c, d, s); else Temporal_OnImmediateWrite(d); H.copyRes(c, d, s); } \
void STDMETHODCALLTYPE hkCopyReg##SUFFIX(ID3D11DeviceContext* c, ID3D11Resource* d, UINT ds, UINT x, UINT y, UINT z, ID3D11Resource* s, UINT ss, const D3D11_BOX* bx) { NoteCopy(d, s, 1); if (#SUFFIX[0] == 'D') Temporal_OnCopy(c, d, s); else Temporal_OnImmediateWrite(d); H.copyReg(c, d, ds, x, y, z, s, ss, bx); }

DEF_INSTR(I, g_ii)
DEF_INSTR(D, g_id)

// ---- HUD separation hooks (deferred contexts only)
using FinishFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, BOOL, ID3D11CommandList**);
FinishFn oFinishD = nullptr;
// ---- temporal-resource tracking hooks (deferred contexts only)
using SetSRVFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
using SetUAVFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
using OMSetRTUAVFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
using ClearRTVFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]);
using ClearUAVUFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11UnorderedAccessView*, const UINT[4]);
using ClearUAVFFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11UnorderedAccessView*, const FLOAT[4]);
using ClearDSVFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
SetSRVFn oPSSRV = nullptr, oVSSRV = nullptr, oCSSRV = nullptr; SetUAVFn oCSUAV = nullptr; OMSetRTUAVFn oOMRTUAV = nullptr;
ClearRTVFn oClrRTV = nullptr; ClearUAVUFn oClrUAVU = nullptr; ClearUAVFFn oClrUAVF = nullptr; ClearDSVFn oClrDSV = nullptr;
void STDMETHODCALLTYPE hkPSSRV(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11ShaderResourceView* const* v) { Temporal_OnSRVs(c, n, v); Passes_SetSRVs(c, 'P', s, n, v); oPSSRV(c, s, n, v); }
void STDMETHODCALLTYPE hkVSSRV(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11ShaderResourceView* const* v) { Temporal_OnSRVs(c, n, v); oVSSRV(c, s, n, v); }
void STDMETHODCALLTYPE hkCSSRV(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11ShaderResourceView* const* v) { Temporal_OnSRVs(c, n, v); Passes_SetSRVs(c, 'C', s, n, v); oCSSRV(c, s, n, v); }
void STDMETHODCALLTYPE hkCSUAV(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11UnorderedAccessView* const* v, const UINT* k) { Temporal_OnUAVs(c, n, v); Passes_SetUAVs(c, s, n, v); oCSUAV(c, s, n, v, k); }
void STDMETHODCALLTYPE hkOMRTUAV(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* r, ID3D11DepthStencilView* d, UINT us, UINT un, ID3D11UnorderedAccessView* const* u, const UINT* k) {
    if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) { Temporal_OnRTVs(c, n, r); Temporal_OnDSV(c, d); Passes_SetRTVs(c, n, r, d); Vrs_OnSetRenderTargets(c, n, r, d); }
    if (un != D3D11_KEEP_UNORDERED_ACCESS_VIEWS) Temporal_OnUAVs(c, un, u);
    oOMRTUAV(c, n, r, d, us, un, u, k);
}
using SetPSFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);
using SetCSFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11ComputeShader*, ID3D11ClassInstance* const*, UINT);
SetPSFn oSetPSD = nullptr; SetCSFn oSetCSD = nullptr;
void STDMETHODCALLTYPE hkSetPSD(ID3D11DeviceContext* c, ID3D11PixelShader* p, ID3D11ClassInstance* const* ci, UINT n) {
    Passes_SetShader(c, 'P', p);
    oSetPSD(c, PostFx_ReplacePS(c, p), ci, n);
}
void STDMETHODCALLTYPE hkSetCSD(ID3D11DeviceContext* c, ID3D11ComputeShader* p, ID3D11ClassInstance* const* ci, UINT n) {
    Passes_SetShader(c, 'C', p);
    oSetCSD(c, PostFx_ReplaceCS(c, p), ci, n);
}
void STDMETHODCALLTYPE hkClrRTV(ID3D11DeviceContext* c, ID3D11RenderTargetView* v, const FLOAT f[4]) { Temporal_OnClearView(c, v); oClrRTV(c, v, f); }
void STDMETHODCALLTYPE hkClrUAVU(ID3D11DeviceContext* c, ID3D11UnorderedAccessView* v, const UINT f[4]) { Temporal_OnClearView(c, v); oClrUAVU(c, v, f); }
void STDMETHODCALLTYPE hkClrUAVF(ID3D11DeviceContext* c, ID3D11UnorderedAccessView* v, const FLOAT f[4]) { Temporal_OnClearView(c, v); oClrUAVF(c, v, f); }
void STDMETHODCALLTYPE hkClrDSV(ID3D11DeviceContext* c, ID3D11DepthStencilView* v, UINT fl, FLOAT d, UINT8 st) { Temporal_OnClearView(c, v); oClrDSV(c, v, fl, d, st); }

void STDMETHODCALLTYPE hkOMSetRTD(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* r, ID3D11DepthStencilView* d) {
    Temporal_OnRTVs(c, n, r); Temporal_OnDSV(c, d); Passes_SetRTVs(c, n, r, d);
    Vrs_OnSetRenderTargets(c, n, r, d);
    ID3D11RenderTargetView* repl = Hud_OnSetRenderTargets(c, n, r, d);
    if (repl) {
        ID3D11RenderTargetView* a[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        for (UINT i = 0; i < n && i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) a[i] = r[i];
        a[0] = repl;
        oOMSetRTD(c, n, a, d);
        return;
    }
    oOMSetRTD(c, n, r, d);
}
void STDMETHODCALLTYPE hkOMSetBlendD(ID3D11DeviceContext* c, ID3D11BlendState* s, const FLOAT f[4], UINT m) {
    oOMSetBlendD(c, Hud_OnBlend(c, s, f, m), f, m);
}
HRESULT STDMETHODCALLTYPE hkFinishD(ID3D11DeviceContext* c, BOOL restore, ID3D11CommandList** cl) {
    Hud_OnFinishList(c);
    HRESULT hr = oFinishD(c, restore, cl);
    if (!restore) Passes_OnFinish(c);
    if (SUCCEEDED(hr) && cl) Temporal_OnFinish(c, *cl);
    return hr;
}

void HookInstr(void** vt, InstrHooks& h, bool deferred) {
    if (deferred) {
        int ok = 0;
        if (MH_CreateHook(vt[33], (void*)hkOMSetRTD, (void**)&oOMSetRTD) == MH_OK && MH_EnableHook(vt[33]) == MH_OK) ++ok;
        if (MH_CreateHook(vt[35], (void*)hkOMSetBlendD, (void**)&oOMSetBlendD) == MH_OK && MH_EnableHook(vt[35]) == MH_OK) ++ok;
        if (MH_CreateHook(vt[114], (void*)hkFinishD, (void**)&oFinishD) == MH_OK && MH_EnableHook(vt[114]) == MH_OK) ++ok;
        LOGI("Discovery: HUD hooks %d/3", ok);
        struct T { int idx; void* fn; void** orig; } t[] = {
            { 8, (void*)hkPSSRV, (void**)&oPSSRV }, { 25, (void*)hkVSSRV, (void**)&oVSSRV }, { 67, (void*)hkCSSRV, (void**)&oCSSRV },
            { 68, (void*)hkCSUAV, (void**)&oCSUAV }, { 34, (void*)hkOMRTUAV, (void**)&oOMRTUAV }, { 50, (void*)hkClrRTV, (void**)&oClrRTV },
            { 51, (void*)hkClrUAVU, (void**)&oClrUAVU }, { 52, (void*)hkClrUAVF, (void**)&oClrUAVF }, { 53, (void*)hkClrDSV, (void**)&oClrDSV },
            { 9, (void*)hkSetPSD, (void**)&oSetPSD }, { 69, (void*)hkSetCSD, (void**)&oSetCSD } };
        int tk = 0;
        for (auto& x : t) if (MH_CreateHook(vt[x.idx], x.fn, x.orig) == MH_OK && MH_EnableHook(vt[x.idx]) == MH_OK) ++tk;
        LOGI("Discovery: temporal-tracking/shader hooks %d/11", tk);
    }
    struct E { int idx; void* fn; void** orig; };
    E e[] = {
        { 7,  deferred ? (void*)hkVSCBD : (void*)hkVSCBI, (void**)&h.vsCB },
        { 16, deferred ? (void*)hkPSCBD : (void*)hkPSCBI, (void**)&h.psCB },
        { 71, deferred ? (void*)hkCSCBD : (void*)hkCSCBI, (void**)&h.csCB },
        { 12, deferred ? (void*)hkDrawIdxD : (void*)hkDrawIdxI, (void**)&h.drawIdx },
        { 13, deferred ? (void*)hkDrawD : (void*)hkDrawI, (void**)&h.draw },
        { 20, deferred ? (void*)hkDrawIdxInstD : (void*)hkDrawIdxInstI, (void**)&h.drawIdxInst },
        { 21, deferred ? (void*)hkDrawInstD : (void*)hkDrawInstI, (void**)&h.drawInst },
        { 41, deferred ? (void*)hkDispatchD : (void*)hkDispatchI, (void**)&h.dispatch },
        { 47, deferred ? (void*)hkCopyResD : (void*)hkCopyResI, (void**)&h.copyRes },
        { 46, deferred ? (void*)hkCopyRegD : (void*)hkCopyRegI, (void**)&h.copyReg },
    };
    int ok = 0;
    for (auto& x : e) if (MH_CreateHook(vt[x.idx], x.fn, x.orig) == MH_OK && MH_EnableHook(vt[x.idx]) == MH_OK) ++ok;
    E e1[] = {
        { 119, deferred ? (void*)hkVSCB1D : (void*)hkVSCB1I, (void**)&h.vsCB1 },
        { 123, deferred ? (void*)hkPSCB1D : (void*)hkPSCB1I, (void**)&h.psCB1 },
        { 124, deferred ? (void*)hkCSCB1D : (void*)hkCSCB1I, (void**)&h.csCB1 },
    };
    for (auto& x : e1) if (MH_CreateHook(vt[x.idx], x.fn, x.orig) == MH_OK && MH_EnableHook(vt[x.idx]) == MH_OK) ++ok;
    LOGI("Discovery: %s instrumentation hooks %d/13", deferred ? "deferred" : "immediate", ok);
}

HRESULT STDMETHODCALLTYPE hkCreateBuf(ID3D11Device* d, const D3D11_BUFFER_DESC* desc, const D3D11_SUBRESOURCE_DATA* init, ID3D11Buffer** out) {
    HRESULT hr = oCreateBuf(d, desc, init, out);
    if (Capturing() && desc) {
        ++g_bufCreates;
        if (desc->BindFlags & D3D11_BIND_CONSTANT_BUFFER) ++g_cbCreates;
        if (init && init->pSysMem && desc->ByteWidth <= 1024 * 1024) {
            AddDump("create", desc->ByteWidth, desc->BindFlags, init->pSysMem, desc->ByteWidth);
            ScanBuffer(init->pSysMem, desc->ByteWidth, desc->ByteWidth, desc->BindFlags, 3);
        }
    }
    return hr;
}

bool HookCtx(ID3D11DeviceContext* ctx, CtxHooks& h, void* m, void* u, void* up, const char* what) {
    void** vt = *(void***)ctx;
    h.map = vt[14];
    bool deferred = (&h == &g_def);
    void* up1 = deferred ? (void*)hkUpd1D : (void*)hkUpd1I;
    bool ok = MH_CreateHook(vt[14], m, (void**)&h.oMap) == MH_OK &&
              MH_CreateHook(vt[15], u, (void**)&h.oUnmap) == MH_OK &&
              MH_CreateHook(vt[48], up, (void**)&h.oUpd) == MH_OK;
    ID3D11DeviceContext1* c1 = nullptr;
    if (ok && SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&c1))) {
        void** vt1 = *(void***)c1;
        if (MH_CreateHook(vt1[116], up1, (void**)&h.oUpd1) == MH_OK) MH_EnableHook(vt1[116]);
        c1->Release();
    }
    if (ok) ok = MH_EnableHook(vt[14]) == MH_OK && MH_EnableHook(vt[15]) == MH_OK && MH_EnableHook(vt[48]) == MH_OK;
    LOGI("Discovery: %s context hooks %s (Map=%p)", what, ok ? "installed" : "FAILED", vt[14]);
    HookInstr(vt, deferred ? g_id : g_ii, deferred);
    return ok;
}

HRESULT STDMETHODCALLTYPE hkCreateDef(ID3D11Device* d, UINT f, ID3D11DeviceContext** out) {
    HRESULT hr = oCreateDef(d, f, out);
    if (SUCCEEDED(hr) && out && *out) {
        ++g_deferredCreated;
        void* map = (*(void***)*out)[14];
        if (map != g_imm.map && map != g_def.map && !g_def.map) HookCtx(*out, g_def, (void*)hkMapD, (void*)hkUnmapD, (void*)hkUpdD, "deferred");
    }
    return hr;
}

// ------------------------------------------------------------------ RTTI / modules
uintptr_t g_exeBase = 0, g_exeEnd = 0;

std::string AddrName(uintptr_t a) {
    char buf[300];
    HMODULE mod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)a, &mod) && mod) {
        wchar_t w[MAX_PATH]; GetModuleFileNameW(mod, w, MAX_PATH);
        const wchar_t* n = wcsrchr(w, L'\\'); n = n ? n + 1 : w;
        char nn[MAX_PATH]; WideCharToMultiByte(CP_UTF8, 0, n, -1, nn, sizeof(nn), nullptr, nullptr);
        snprintf(buf, sizeof(buf), "%s+0x%llx", nn, (unsigned long long)(a - (uintptr_t)mod));
    } else snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)a);
    return buf;
}

bool InExe(uintptr_t p) { return p >= g_exeBase && p < g_exeEnd; }

// Returns RTTI class name if 'vt' is a vtable in the exe.
const char* VtableClass(uintptr_t vt) {
    __try {
        if (!InExe(vt) || (vt & 7)) return nullptr;
        uintptr_t col = *(uintptr_t*)(vt - 8);
        if (!InExe(col)) return nullptr;
        uint32_t sig = *(uint32_t*)col;
        if (sig != 1) return nullptr;
        uint32_t tdRva = *(uint32_t*)(col + 12);
        uintptr_t td = g_exeBase + tdRva;
        if (!InExe(td)) return nullptr;
        const char* name = (const char*)(td + 16);
        if (name[0] != '.' || name[1] != '?' || name[2] != 'A') return nullptr;
        return name;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Walk backwards from a field to find the owning object's vtable.
bool FindOwner(uintptr_t addr, uintptr_t* obj, const char** cls) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void*)addr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    uintptr_t lo = (uintptr_t)mbi.BaseAddress;
    __try {
        for (uintptr_t p = addr & ~(uintptr_t)7; p + 0x1000 > addr && p >= lo; p -= 8) {
            const char* n = VtableClass(*(uintptr_t*)p);
            if (n) { *obj = p; *cls = n; return true; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

// ------------------------------------------------------------------ memory search
struct Pattern { std::string desc; float v[16]; uint8_t mask[16]; int n; int kind; float tol = 0; int align = 16; };
struct MemHit { uintptr_t addr; int pat; uintptr_t obj; const char* cls; };
uint32_t g_patHits[32];
uintptr_t g_curAlloc = 0;            // allocation base of the region being searched
uint32_t g_allocPatHits[32];         // hits for each pattern within the current allocation (reset per allocation)

bool MatchAt(const float* p, const Pattern& pt) {
    if (pt.tol == 0) { for (int i = 0; i < pt.n; ++i) if (pt.mask[i] && p[i] != pt.v[i]) return false; }
    else { for (int i = 0; i < pt.n; ++i) if (pt.mask[i] && !(fabsf(p[i] - pt.v[i]) <= pt.tol)) return false; }
    return true;
}

void SearchRegion(const uint8_t* base, size_t size, const std::vector<Pattern>& pats, std::vector<MemHit>& hits) {
    __try {
        const float* f = (const float*)base;
        size_t n = size / 4;
        for (size_t i = 0; i + 16 <= n; ++i) {
            float x = f[i];
            if (x == 0.f || x != x) continue;
            for (size_t k = 0; k < pats.size(); ++k) {
                const Pattern& pt = pats[k];
                if (pt.tol == 0 ? (x != pt.v[0]) : !(fabsf(x - pt.v[0]) <= pt.tol)) continue;
                if ((i * 4) % pt.align) continue;
                if (MatchAt(f + i, pt)) {
                    if (hits.size() < 4000 && g_allocPatHits[k] < 6) hits.push_back({ (uintptr_t)(f + i), (int)k, 0, nullptr });
                    ++g_patHits[k]; ++g_allocPatHits[k];
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ------------------------------------------------------------------ hardware watchpoints
struct Watch { uintptr_t addr; std::string desc; };
std::vector<Watch> g_watches;
PVOID g_veh = nullptr;
std::atomic<bool> g_watching{ false };
DWORD g_watchStart = 0;

struct WriterRec { volatile LONG used; int watch; DWORD tid; uintptr_t frames[8]; int nframes; volatile LONG count; };
WriterRec g_writers[48];
volatile LONG g_writerLock = 0;
volatile LONG g_overflow = 0;

LONG CALLBACK Veh(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    DWORD64 dr6 = c->Dr6;
    int w = -1;
    for (int i = 0; i < 4; ++i) if (dr6 & (1ull << i)) { w = i; break; }
    if (w < 0) return EXCEPTION_CONTINUE_SEARCH;
    c->Dr6 = 0;
    if (!g_watching) {
        // Stale watchpoint left on a thread we could not clear (e.g. the thread that ended the
        // watch): disarm it in this thread's context and carry on.
        c->Dr0 = c->Dr1 = c->Dr2 = c->Dr3 = 0; c->Dr7 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Unwind a few frames from the faulting context (no allocation).
    uintptr_t frames[8]; int nf = 0;
    CONTEXT ctx = *c;
    __try {
    for (; nf < 8; ++nf) {
        frames[nf] = (uintptr_t)ctx.Rip;
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (!fe) { if (!ctx.Rsp) break; ctx.Rip = *(DWORD64*)ctx.Rsp; ctx.Rsp += 8; }
        else { PVOID hd; DWORD64 ef; RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fe, &ctx, &hd, &ef, nullptr); }
        if (!ctx.Rip) { ++nf; break; }
    }
    } __except (EXCEPTION_EXECUTE_HANDLER) { if (nf == 0) { frames[0] = (uintptr_t)c->Rip; nf = 1; } }

    while (InterlockedCompareExchange(&g_writerLock, 1, 0) != 0) YieldProcessor();
    bool found = false;
    for (auto& r : g_writers) {
        if (!r.used) break;
        if (r.watch == w && r.tid == GetCurrentThreadId() && r.nframes == nf && !memcmp(r.frames, frames, nf * sizeof(uintptr_t))) { ++r.count; found = true; break; }
    }
    if (!found) {
        bool stored = false;
        for (auto& r : g_writers) if (!r.used) {
            r.used = 1; r.watch = w; r.tid = GetCurrentThreadId(); r.nframes = nf; memcpy(r.frames, frames, nf * sizeof(uintptr_t)); r.count = 1; stored = true; break;
        }
        if (!stored) ++g_overflow;
    }
    InterlockedExchange(&g_writerLock, 0);
    return EXCEPTION_CONTINUE_EXECUTION;
}

void SetDebugRegsAllThreads(bool enable) {
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te = { sizeof(te) };
    int n = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!th) continue;
        if (SuspendThread(th) != (DWORD)-1) {
            CONTEXT c = {}; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(th, &c)) {
                c.Dr0 = c.Dr1 = c.Dr2 = c.Dr3 = 0; c.Dr6 = 0; c.Dr7 = 0;
                if (enable) {
                    for (size_t i = 0; i < g_watches.size() && i < 4; ++i) {
                        (&c.Dr0)[i] = g_watches[i].addr;
                        c.Dr7 |= (1ull << (2 * i));                 // local enable
                        c.Dr7 |= (1ull << (16 + 4 * i));            // RW=01 write
                        c.Dr7 |= (3ull << (18 + 4 * i));            // LEN=11 4 bytes
                    }
                }
                if (SetThreadContext(th, &c)) ++n;
            }
            ResumeThread(th);
        }
        CloseHandle(th);
    }
    CloseHandle(snap);
    LOGD("Discovery: debug registers %s on %d threads", enable ? "armed" : "cleared", n);
}

// ------------------------------------------------------------------ analysis thread
std::atomic<int> g_phase{ 0 };   // 0 idle, 1 capturing, 2 analysing, 3 watching

void LogMatrix(const char* label, const float* m) {
    LOGI("    %s", label);
    for (int r = 0; r < 4; ++r) LOGI("      [%12.6f %12.6f %12.6f %12.6f]", m[r * 4], m[r * 4 + 1], m[r * 4 + 2], m[r * 4 + 3]);
}

// Camera block layout found in the per-view constant ring (offsets from the projection):
//   +0x00 projection rows [xs 0 jx 0; 0 ys jy 0; 0 0 a b; 0 0 -1 0]  (column-vector convention, RH)
//   +0x40 view*projection,  +0x80 view rows [R | t; 0 0 0 1]
struct CamBlock { uint32_t off; float P[16]; float V[16]; uint32_t count; };

bool IsRigidRows(const float* v) {
    if (fabsf(v[12]) > 1e-5f || fabsf(v[13]) > 1e-5f || fabsf(v[14]) > 1e-5f || fabsf(v[15] - 1.f) > 1e-5f) return false;
    for (int i = 0; i < 3; ++i) {
        const float* r = v + i * 4;
        if (fabsf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] - 1.f) > 2e-3f) return false;
        for (int j = i + 1; j < 3; ++j) {
            const float* q = v + j * 4;
            if (fabsf(r[0] * q[0] + r[1] * q[1] + r[2] * q[2]) > 2e-3f) return false;
        }
    }
    return true;
}

std::vector<CamBlock> FindCameraBlocks(const std::vector<float>& f) {
    std::vector<CamBlock> out;
    for (size_t i = 0; i + 48 <= f.size(); i += 4) {
        const float* p = &f[i];
        if (p[1] != 0 || p[3] != 0 || p[4] != 0 || p[7] != 0 || p[8] != 0 || p[9] != 0 || p[12] != 0 || p[13] != 0 || p[15] != 0) continue;
        if (fabsf(fabsf(p[14]) - 1.f) > 1e-4f || !(p[0] > 0.1f && p[0] < 20.f && p[5] > 0.1f && p[5] < 20.f)) continue;
        if (!AspectOk(p[5] / p[0])) continue;
        const float* v = p + 32;
        if (!IsRigidRows(v) || !HasRealValues(v, 16, 3)) continue;
        bool dup = false;
        for (auto& c : out) if (!memcmp(c.V, v, 48)) { ++c.count; if (p[2] != 0.f || p[6] != 0.f) memcpy(c.P, p, 64); dup = true; break; }
        if (dup) continue;
        if (out.size() >= 64) break;
        CamBlock c; c.off = (uint32_t)(i * 4); memcpy(c.P, p, 64); memcpy(c.V, v, 64); c.count = 1;
        out.push_back(c);
    }
    return out;
}

void CamPose(const float* V, float pos[3], float fwd[3]) {
    // V = [R | t]: world->view. Camera position = -R^T t; camera looks down -Z (RH): forward = -(row 2 of R)
    for (int k = 0; k < 3; ++k) pos[k] = -(V[0 * 4 + k] * V[3] + V[1 * 4 + k] * V[7] + V[2 * 4 + k] * V[11]);
    for (int k = 0; k < 3; ++k) fwd[k] = -V[2 * 4 + k];
}

DWORD WINAPI SearchAndWatch(std::vector<Pattern> pats, std::vector<std::pair<uintptr_t, uintptr_t>> own);
float g_camV[16], g_camP[16];
bool g_haveCam = false;
uint32_t g_camOff = 0;               // byte offset of the main camera block (projection) in the GPU ring
std::vector<uintptr_t> g_floodAllocs;

bool CameraAnalyse() {
    std::vector<float> big;
    { std::lock_guard<std::mutex> lk(g_capMx); big.swap(g_bigCB); }
    if (big.empty()) { LOGI("Discovery: no large constant buffer captured"); return false; }
    auto cams = FindCameraBlocks(big);
    LOGI("Discovery: per-view constant ring %zu KB: %zu distinct camera blocks", big.size() * 4 / 1024, cams.size());
    if (cams.empty()) return false;
    // The main gameplay camera is the one with TAA jitter; break ties by how often it appears.
    std::sort(cams.begin(), cams.end(), [](const CamBlock& a, const CamBlock& b) {
        bool ja = a.P[2] != 0.f || a.P[6] != 0.f, jb = b.P[2] != 0.f || b.P[6] != 0.f;
        if (ja != jb) return ja;
        return a.count > b.count;
    });
    for (size_t i = 0; i < cams.size() && i < 8; ++i) {
        float pos[3], fwd[3]; CamPose(cams[i].V, pos, fwd);
        const float* P = cams[i].P;
        LOGI("  camera %zu @+0x%x x%u: fovY %.2f aspect %.4f near-ish %.4f jitter (%.6f, %.6f) pos (%.3f %.3f %.3f) fwd (%.4f %.4f %.4f)",
             i, cams[i].off, cams[i].count, 2.f * atanf(1.f / P[5]) * 57.29578f, P[5] / P[0], P[11], P[2], P[6],
             pos[0], pos[1], pos[2], fwd[0], fwd[1], fwd[2]);
    }
    const CamBlock& c = cams[0];
    LogMatrix("main camera projection (rows)", c.P);
    LogMatrix("main camera view (rows)", c.V);
    float pos[3], fwd[3]; CamPose(c.V, pos, fwd);
    memcpy(g_camV, c.V, 64); memcpy(g_camP, c.P, 64); g_haveCam = true; g_camOff = c.off;

    std::vector<Pattern> pats;
    auto add = [&](const char* d, std::initializer_list<float> v, std::initializer_list<int> mask, float tol, int align) {
        Pattern p; p.desc = d; p.n = (int)v.size(); p.kind = K_RIGID; p.tol = tol; p.align = align;
        int i = 0; for (float x : v) p.v[i++] = x;
        i = 0; for (int m : mask) p.mask[i++] = (uint8_t)m;
        for (; i < p.n; ++i) p.mask[i] = 1;
        if (fabsf(p.v[0]) < 1e-3f) { LOGD("  pattern '%s' skipped (first value ~0)", d); return; }
        pats.push_back(p);
    };
    const float* V = c.V;
    add("view rows exact", { V[0], V[1], V[2], V[3], V[4], V[5], V[6], V[7], V[8], V[9], V[10], V[11] }, {}, 0, 4);
    add("view transposed exact", { V[0], V[4], V[8], V[12], V[1], V[5], V[9], V[13], V[2], V[6], V[10], V[14] }, {}, 0, 4);
    add("view R^T rows (vec4 padded)", { V[0], V[4], V[8], 0, V[1], V[5], V[9], 0, V[2], V[6], V[10] }, { 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1 }, 0, 4);
    add("view R^T rows (packed)", { V[0], V[4], V[8], V[1], V[5], V[9], V[2], V[6], V[10] }, {}, 0, 4);
    add("view R rows (vec4 padded)", { V[0], V[1], V[2], 0, V[4], V[5], V[6], 0, V[8], V[9], V[10] }, { 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1 }, 0, 4);
    add("camera position ~", { pos[0], pos[1], pos[2] }, {}, 1e-3f, 4);
    add("camera forward ~", { fwd[0], fwd[1], fwd[2] }, {}, 2e-4f, 4);
    add("camera -forward ~", { -fwd[0], -fwd[1], -fwd[2] }, {}, 2e-4f, 4);
    add("projection xs/ys unjittered", { c.P[0], 0, 0, 0, 0, c.P[5] }, { 1, 1, 0, 1, 1, 1 }, 0, 4);
    std::vector<std::pair<uintptr_t, uintptr_t>> own;
    own.push_back({ (uintptr_t)big.data(), (uintptr_t)(big.data() + big.size()) });
    SearchAndWatch(std::move(pats), std::move(own));
    return true;
}

// Engine render-view array (found by disassembly of exe+0x1cdb65):
//   exe+0x7038130: pointer to view array, exe+0x7038140: int count, exe+0x70207c4: int current view
//   element stride 0x9F0; CPU-side matrices (row-vector form, transposed vs GPU):
//   +0x40 projection, +0x80 view*proj, +0xC0 view, +0x150.. more; flags at +0x9dd/+0x9e0/+0x9e1
const uintptr_t RVA_VIEWS = 0x7038130, RVA_VIEWCOUNT = 0x7038140, RVA_CURVIEW = 0x70207c4, VIEW_STRIDE = 0x9F0;

bool ViewArrayAnalyse() {
    __try {
        uintptr_t arr = *(uintptr_t*)(g_exeBase + RVA_VIEWS);
        int count = *(int*)(g_exeBase + RVA_VIEWCOUNT);
        int cur = *(int*)(g_exeBase + RVA_CURVIEW);
        LOGI("Discovery: view array %p, count %d, current view %d", (void*)arr, count, cur);
        if (!arr || count <= 0 || count > 64) return false;
        int main = -1;
        for (int i = 0; i < count; ++i) {
            uintptr_t e = arr + (uintptr_t)i * VIEW_STRIDE;
            const float* P = (const float*)(e + 0x40);
            const float* Vc = (const float*)(e + 0xC0);
            float V[16];
            for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) V[r * 4 + c] = Vc[c * 4 + r];   // to GPU rows
            float pos[3], fwd[3]; CamPose(V, pos, fwd);
            uint8_t f9dd = *(uint8_t*)(e + 0x9dd), f9e0 = *(uint8_t*)(e + 0x9e0), f9e1 = *(uint8_t*)(e + 0x9e1);
            float fov = P[5] > 0 ? 2.f * atanf(1.f / P[5]) * 57.29578f : 0.f;
            LOGI("  view %2d @%p flags %d/%d/%d P xs %.4f ys %.4f (fovY %.2f) jitter (%.6f %.6f) a %.6f b %.6f | pos (%.3f %.3f %.3f) fwd (%.4f %.4f %.4f)",
                 i, (void*)e, f9dd, f9e0, f9e1, P[0], P[5], fov, P[8], P[9], P[10], P[14], pos[0], pos[1], pos[2], fwd[0], fwd[1], fwd[2]);
            bool jit = P[8] != 0.f || P[9] != 0.f;
            if (main < 0 && jit && P[0] > 0.1f && AspectOk(P[5] / P[0])) main = i;
        }
        if (cur >= 0 && cur < count) LOGI("  current-view global says %d; jittered main candidate %d", cur, main);
        if (main < 0) main = (cur >= 0 && cur < count) ? cur : 0;
        uintptr_t e = arr + (uintptr_t)main * VIEW_STRIDE;
        LogMatrix("main view +0x40 (projection, CPU form)", (const float*)(e + 0x40));
        LogMatrix("main view +0xC0 (view, CPU form)", (const float*)(e + 0xC0));
        LogMatrix("main view +0x00", (const float*)(e + 0x00));
        LogMatrix("main view +0x80 (view*proj)", (const float*)(e + 0x80));
        LogMatrix("main view +0x100", (const float*)(e + 0x100));
        LogMatrix("main view +0x150", (const float*)(e + 0x150));
        LogMatrix("main view +0x190", (const float*)(e + 0x190));
        LOGI("  main view +0x940..+0x9ef raw:");
        for (uintptr_t o = 0x900; o < 0x9f0; o += 16) {
            const float* q = (const float*)(e + o); const uint32_t* u = (const uint32_t*)q;
            LOGI("    +0x%03llx: %12.5f %12.5f %12.5f %12.5f | %08x %08x %08x %08x", (unsigned long long)o, q[0], q[1], q[2], q[3], u[0], u[1], u[2], u[3]);
        }
        g_watches.clear();
        g_watches.push_back({ e + 0xF0, "main view: view translation (+0xF0)" });
        g_watches.push_back({ e + 0xC0, "main view: view m00 (+0xC0)" });
        g_watches.push_back({ e + 0x40, "main view: projection xs (+0x40)" });
        g_watches.push_back({ e + 0x150, "main view: +0x150 matrix" });
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOGE("Discovery: exception while reading the view array");
        return false;
    }
    for (size_t i = 0; i < g_watches.size(); ++i) LOGI("Discovery: watch %zu -> %p %s", i, (void*)g_watches[i].addr, g_watches[i].desc.c_str());
    memset((void*)g_writers, 0, sizeof(g_writers));
    g_overflow = 0;
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, Veh);
    g_watching = true;
    SetDebugRegsAllThreads(true);
    g_watchStart = GetTickCount();
    g_phase = 3;
    LOGI("Discovery: watching the main view for 5 seconds - MOVE THE CAMERA NOW");
    return true;
}

DWORD WINAPI AnalyseThread(LPVOID) {
    if (ViewArrayAnalyse()) return 0;
    LOGI("Discovery: capture done. %llu draws, %llu dispatches, %llu CB bind calls (%llu via *SetConstantBuffers1)",
         g_draws.load(), g_dispatches.load(), g_cbBinds.load(), g_cb1Binds.load());
    if (CameraAnalyse()) return 0;
    LOGI("Discovery: targeted camera analysis failed; falling back to generic scan");
    std::vector<BufferCapture> caps;
    { std::lock_guard<std::mutex> lk(g_capMx); caps = g_caps; }
    LOGD("Discovery: generic: buffer maps %llu, UpdateSubresource %llu, UpdateSubresource1 %llu, ExecuteCommandList %llu, bytes scanned %llu, buffers >64MB skipped %llu, deferred contexts created %llu",
         g_mapCount.load(), g_updCount.load(), g_upd1Count.load(), g_execCount.load(), g_scanBytes.load(), g_bigSkipped.load(), g_deferredCreated.load());
    LOGD("Discovery: during capture: %llu draws, %llu dispatches, %llu CB bind calls (%llu via *SetConstantBuffers1), %llu non-null CB slots, %llu buffers created (%llu CBs)",
         g_draws.load(), g_dispatches.load(), g_cbBinds.load(), g_cb1Binds.load(), g_cbBindsNonNull.load(), g_bufCreates.load(), g_cbCreates.load());
    {
        std::lock_guard<std::mutex> lk(g_capMx);
        for (auto& c : g_copies)
            LOGI("  buffer copy (%s): src %u bytes bind 0x%x usage %u -> dst %u bytes bind 0x%x usage %u, %u times", c.region ? "region" : "full",
                 c.srcBw, c.srcBind, c.srcUsage, c.dstBw, c.dstBind, c.dstUsage, c.count);
        std::wstring dir = g_modDir + L"GranblueVR_dump\\";
        CreateDirectoryW(dir.c_str(), nullptr);
        WIN32_FIND_DATAW fd;
        HANDLE fh = FindFirstFileW((dir + L"*.bin").c_str(), &fd);
        if (fh != INVALID_HANDLE_VALUE) { do { DeleteFileW((dir + fd.cFileName).c_str()); } while (FindNextFileW(fh, &fd)); FindClose(fh); }
        size_t total = 0;
        for (auto& d : g_dumps) {
            std::wstring path = dir + std::wstring(d.name.begin(), d.name.end());
            FILE* f = _wfopen(path.c_str(), L"wb");
            if (f) { fwrite(d.bytes.data(), 1, d.bytes.size(), f); fclose(f); total += d.bytes.size(); }
        }
        LOGI("Discovery: wrote %zu raw dumps (%.1f MB) to GranblueVR_dump", g_dumps.size(), total / 1048576.0);
        g_dumps.clear(); g_dumpBytes = 0;
    }
    {
        std::lock_guard<std::mutex> lk(g_capMx);
        for (auto& b : g_bufInfos)
            LOGI("  buffer written: %u bytes, bind 0x%x, usage %u, cpu 0x%x, misc 0x%x, via %s%s, %u times", b.byteWidth, b.bind, b.usage, b.cpu, b.misc,
                 b.via == 1 ? "UpdateSubresource" : "Map type ", b.via == 1 ? "" : (b.via == 14 ? "WRITE_DISCARD" : b.via == 15 ? "WRITE_NO_OVERWRITE" : b.via == 12 ? "WRITE" : b.via == 13 ? "READ_WRITE" : "?"), b.count);
    }
    if (caps.empty()) {
        LOGE("Discovery: no projection matrix found in any CPU-updated buffer. Try again during gameplay (not a menu).");
        g_phase = 0; return 0;
    }

    std::vector<Pattern> pats;
    auto addPat = [&](const std::string& d, const float* m, int n, int kind, bool transpose, const uint8_t* mask, float tol = 0.f, int align = 16) {
        Pattern p; p.desc = d; p.n = n; p.kind = kind; p.tol = tol; p.align = align;
        for (int i = 0; i < n; ++i) { p.v[i] = transpose ? m[(i % 4) * 4 + i / 4] : m[i]; p.mask[i] = mask ? mask[i] : 1; }
        if (fabsf(p.v[0]) < 1e-3f) { LOGD("  pattern '%s' skipped (first value ~0)", d.c_str()); return; }   // search keys on first float
        pats.push_back(p);
    };

    int ci = 0;
    for (auto& c : caps) {
        LOGI("Discovery: buffer #%d: %u bytes, bind 0x%x, via %s, seen %u times, %zu matrices", ci, c.byteWidth, c.bind,
             c.via == 3 ? "CreateBuffer initial data" : c.via == 2 ? "GPU readback of bound CB" : c.via ? "UpdateSubresource" : "Map", c.count, c.hits.size());
        for (auto& h : c.hits) {
            char lab[160];
            if (h.kind == K_PROJ || h.kind == K_VIEWPROJ)
                snprintf(lab, sizeof(lab), "+0x%x %s (%s) fovY %.2f deg aspect %.4f", h.off, KindName(h.kind), h.t ? "column-major" : "row-major", h.fov, h.aspect);
            else snprintf(lab, sizeof(lab), "+0x%x %s (%s)", h.off, KindName(h.kind), h.t ? "transposed" : "as-stored");
            if (h.kind == K_VIEWPROJ) LOGI("    forward (%.5f %.5f %.5f) right (%.5f %.5f %.5f)", h.fwd[0], h.fwd[1], h.fwd[2], h.right[0], h.right[1], h.right[2]);
            LogMatrix(lab, h.m);
            if (ci < 2 && pats.size() < 24) {
                char d[200];
                snprintf(d, sizeof(d), "buf#%d+0x%x %s", ci, h.off, KindName(h.kind));
                if (h.kind == K_PROJ) {
                    // CPU copy may be unjittered: match xs and ys only, via 6-float masked pattern.
                    static const uint8_t mrm[6] = { 1, 1, 1, 1, 1, 1 };
                    float rm[16]; for (int i = 0; i < 16; ++i) rm[i] = h.t ? h.m[(i % 4) * 4 + i / 4] : h.m[i];
                    float p6[16] = { rm[0], 0, 0, 0, 0, rm[5] };
                    addPat(std::string(d) + " xs..ys row-major", p6, 6, K_PROJ, false, mrm);
                    uint8_t mcm[6] = { 1, 1, 0, 1, 1, 1 };
                    float c6[16] = { rm[0], 0, 0, 0, 0, rm[5] };
                    addPat(std::string(d) + " xs..ys column-major", c6, 6, K_PROJ, false, mcm);
                } else if (h.kind == K_VIEWPROJ) {
                    addPat(std::string(d) + " exact first 8", h.m, 8, K_VIEWPROJ, false, nullptr);
                    addPat(std::string(d) + " exact transposed first 8", h.m, 8, K_VIEWPROJ, true, nullptr);
                    float fw[16] = { h.fwd[0], h.fwd[1], h.fwd[2] }, nf[16] = { -h.fwd[0], -h.fwd[1], -h.fwd[2] };
                    float rt[16] = { h.right[0], h.right[1], h.right[2] }, nr[16] = { -h.right[0], -h.right[1], -h.right[2] };
                    addPat(std::string(d) + " ~forward", fw, 3, K_VIEWPROJ, false, nullptr, 3e-4f, 4);
                    addPat(std::string(d) + " ~-forward", nf, 3, K_VIEWPROJ, false, nullptr, 3e-4f, 4);
                    addPat(std::string(d) + " ~right", rt, 3, K_VIEWPROJ, false, nullptr, 3e-4f, 4);
                    addPat(std::string(d) + " ~-right", nr, 3, K_VIEWPROJ, false, nullptr, 3e-4f, 4);
                } else if (h.kind == K_RIGID) {
                    addPat(std::string(d) + " as-stored 3 rows", h.m, 12, K_RIGID, false, nullptr);
                    addPat(std::string(d) + " transposed 3 rows", h.m, 12, K_RIGID, true, nullptr);
                } else {
                    addPat(std::string(d) + " 3x4 exact", h.m, 12, K_RIGID, false, nullptr);
                }
            }
        }
        ++ci;
    }
    // Raw dump of the first two buffers for offline layout analysis.
    for (size_t b = 0; b < caps.size() && b < 2; ++b) {
        LOGD("Discovery: raw dump buffer #%zu (%zu floats)", b, caps[b].data.size());
        auto& d = caps[b].data;
        for (size_t i = 0; i + 4 <= d.size(); i += 4)
            LOGD("  +0x%06zx: %14.6f %14.6f %14.6f %14.6f", caps[b].dataStart + i * 4, d[i], d[i + 1], d[i + 2], d[i + 3]);
    }

    std::vector<std::pair<uintptr_t, uintptr_t>> own;
    for (auto& c : caps) {
        own.push_back({ (uintptr_t)c.data.data(), (uintptr_t)(c.data.data() + c.data.size()) });
        own.push_back({ (uintptr_t)c.hits.data(), (uintptr_t)(c.hits.data() + c.hits.size()) });
    }
    SearchAndWatch(std::move(pats), std::move(own));
    return 0;
}

DWORD WINAPI SearchAndWatch(std::vector<Pattern> pats, std::vector<std::pair<uintptr_t, uintptr_t>> own) {
    if (pats.empty()) { LOGE("Discovery: no usable search patterns (all candidate values ~0)"); g_phase = 0; return 0; }
    memset(g_patHits, 0, sizeof(g_patHits));
    if (pats.size() > 32) pats.resize(32);
    LOGI("Discovery: searching process memory for %zu patterns (keep the camera still ~30 s)...", pats.size());
    DWORD t0 = GetTickCount();
    std::vector<MemHit> hits;
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t self = (uintptr_t)&AnalyseThread;
    HMODULE selfMod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)self, &selfMod);
    uint64_t scanned = 0;
    for (uintptr_t a = 0x10000; a < 0x7FFFFFFF0000ull && VirtualQuery((void*)a, &mbi, sizeof(mbi)); a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT) continue;
        DWORD pr = mbi.Protect;
        if (pr & (PAGE_GUARD | PAGE_NOACCESS | PAGE_WRITECOMBINE | PAGE_NOCACHE)) continue;
        if (!(pr & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE))) continue;
        if (mbi.AllocationBase == (void*)selfMod) continue;
        if ((uintptr_t)mbi.AllocationBase != g_curAlloc) { g_curAlloc = (uintptr_t)mbi.AllocationBase; memset(g_allocPatHits, 0, sizeof(g_allocPatHits)); }
        SearchRegion((const uint8_t*)mbi.BaseAddress, mbi.RegionSize, pats, hits);
        scanned += mbi.RegionSize;
    }
    LOGI("Discovery: scanned %.1f MB in %lu ms, %zu hits", scanned / 1048576.0, GetTickCount() - t0, hits.size());
    for (size_t k = 0; k < pats.size(); ++k) LOGI("  pattern %-40s total matches %u", pats[k].desc.c_str(), g_patHits[k]);

    // Drop hits inside our own copies of the data.
    {
        own.push_back({ (uintptr_t)pats.data(), (uintptr_t)(pats.data() + pats.size()) });
        {
            std::lock_guard<std::mutex> lk(g_capMx);
            for (auto& c : g_caps) {
                own.push_back({ (uintptr_t)c.data.data(), (uintptr_t)(c.data.data() + c.data.size()) });
                own.push_back({ (uintptr_t)c.hits.data(), (uintptr_t)(c.hits.data() + c.hits.size()) });
            }
        }
        std::vector<MemHit> kept;
        for (auto& h : hits) {
            bool mine = false;
            for (auto& r : own) if (h.addr >= r.first && h.addr < r.second) mine = true;
            if (!mine) kept.push_back(h);
        }
        LOGI("Discovery: %zu hits after removing our own copies", kept.size());
        hits.swap(kept);
    }
    // Allocations holding many exact view copies are the GPU upload ring / driver copies: drop them.
    {
        std::vector<uintptr_t> hAlloc(hits.size());
        for (size_t i = 0; i < hits.size(); ++i) {
            MEMORY_BASIC_INFORMATION mi = {};
            VirtualQuery((void*)hits[i].addr, &mi, sizeof(mi));
            hAlloc[i] = (uintptr_t)mi.AllocationBase;
        }
        struct AC { uintptr_t base; int perPat[32]; };
        std::vector<AC> allocs;
        for (size_t i = 0; i < hits.size(); ++i) {
            AC* a = nullptr;
            for (auto& x : allocs) if (x.base == hAlloc[i]) { a = &x; break; }
            if (!a) { allocs.push_back({ hAlloc[i], {} }); a = &allocs.back(); }
            if (hits[i].pat < 32) ++a->perPat[hits[i].pat];
        }
        std::vector<uintptr_t> flood;
        // flood = allocation with many exact view-matrix copies (pattern 0); the game heap holds none
        for (auto& x : allocs) if (x.perPat[0] >= 6) flood.push_back(x.base);
        std::vector<MemHit> kept;
        for (size_t i = 0; i < hits.size(); ++i) {
            bool fl = false;
            for (auto f : flood) if (f == hAlloc[i]) { fl = true; break; }
            if (!fl) kept.push_back(hits[i]);
        }
        for (auto f : flood) LOGI("Discovery: ignoring allocation %p (GPU upload ring / driver copies)", (void*)f);
        g_floodAllocs = flood;
        LOGI("Discovery: %zu hits outside flood allocations (%zu allocations had hits)", kept.size(), allocs.size());
        hits.swap(kept);
    }
    // Name owners. Then choose watch targets from the RAREST patterns first: the CPU-side camera
    // objects hold transposed/basis copies, while exact row copies flood in from the GPU upload ring.
    g_watches.clear();
    for (auto& h : hits) FindOwner(h.addr, &h.obj, &h.cls);
    std::vector<int> order(pats.size());
    for (size_t k = 0; k < pats.size(); ++k) order[k] = (int)k;
    std::sort(order.begin(), order.end(), [&](int x, int y) { return g_patHits[x] < g_patHits[y]; });
    for (int k : order) {
        int shown = 0;
        LOGI("  pattern '%s': %u matches", pats[k].desc.c_str(), g_patHits[k]);
        for (auto& h : hits) {
            if (h.pat != k) continue;
            if (++shown > 12) break;
            MEMORY_BASIC_INFORMATION mi = {};
            VirtualQuery((void*)h.addr, &mi, sizeof(mi));
            if (h.cls) LOGI("    %p owner %p %s +0x%llx (alloc %p)", (void*)h.addr, (void*)h.obj, h.cls, (unsigned long long)(h.addr - h.obj), mi.AllocationBase);
            else LOGI("    %p (no RTTI owner) alloc %p type 0x%lx %s", (void*)h.addr, mi.AllocationBase, mi.Type, AddrName(h.addr).c_str());
        }
    }
    {
        int dumped = 0;
        for (auto& h : hits) {
            if (dumped >= 24) break;
            ++dumped;
            LOGI("  context around %p (%s):", (void*)h.addr, pats[h.pat].desc.c_str());
            __try {
                const float* f = (const float*)((h.addr - 0x40) & ~0xFull);
                MEMORY_BASIC_INFORMATION mi = {};
                VirtualQuery(f, &mi, sizeof(mi));
                if ((uintptr_t)f < (uintptr_t)mi.BaseAddress) f = (const float*)mi.BaseAddress;
                for (int r = 0; r < 12; ++r) {
                    const float* q = f + r * 4;
                    if ((uintptr_t)(q + 4) > (uintptr_t)mi.BaseAddress + mi.RegionSize) break;
                    const uint32_t* u = (const uint32_t*)q;
                    LOGI("    %p: %12.5f %12.5f %12.5f %12.5f   | %08x %08x %08x %08x", (void*)q, q[0], q[1], q[2], q[3], u[0], u[1], u[2], u[3]);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { LOGI("    (unreadable)"); }
        }
    }
    // Structural candidates first:
    //  (a) camera pose struct: position vec4 followed by the up vector (view row 1) as vec4
    //  (b) per-view record: projection xs at -0x40 from an exact view copy (CPU record list)
    // Ring slot watches: the CPU-side upload ring mirrors the GPU ring 1:1 from its allocation base,
    // with 256 KB per-frame slices, so the main camera's view lands at the same offset every slice.
    if (g_haveCam) {
        for (auto base : g_floodAllocs) {
            if (!base || g_watches.size() >= 2) continue;
            __try {
                const float* q = (const float*)(base + g_camOff + 0x80);
                MEMORY_BASIC_INFORMATION mi = {};
                VirtualQuery(q, &mi, sizeof(mi));
                if (mi.State != MEM_COMMIT) continue;
                bool match = fabsf(q[0] - g_camV[0]) < 1e-3f && fabsf(q[5] - g_camV[5]) < 1e-3f;
                LOGI("Discovery: ring candidate %p: view slot %s (%.5f %.5f vs %.5f %.5f)", (void*)base, match ? "MATCHES" : "differs", q[0], q[5], g_camV[0], g_camV[5]);
                if (!match) continue;
                uint32_t slice = 0x40000;
                uint32_t inSlice = (g_camOff + 0x80) % slice;
                uint32_t thisSlice = (g_camOff + 0x80) / slice;
                // watch the same slot in the next two slices (written within ~1-2 frames)
                for (uint32_t k = 1; k <= 2 && g_watches.size() < 2; ++k) {
                    uint32_t off = ((thisSlice + k) * slice + inSlice) % (32u * 1024 * 1024);
                    g_watches.push_back({ base + off + 12, "GPU upload ring: main camera view.tx" });
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
    if (g_haveCam) {
        auto approxEq = [](float a, float b, float t) { return fabsf(a - b) <= t; };
        for (auto& h : hits) {
            if (g_watches.size() >= 2) break;
            if (pats[h.pat].desc.find("camera position") == std::string::npos) continue;
            __try {
                const float* q = (const float*)h.addr;
                if (approxEq(q[4], g_camV[4], 1e-4f) && approxEq(q[5], g_camV[5], 1e-4f) && approxEq(q[6], g_camV[6], 1e-4f)) {
                    g_watches.push_back({ h.addr, "camera pose struct (pos, up, back)" });
                    LOGI("Discovery: camera pose struct at %p", (void*)h.addr);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        for (auto& h : hits) {
            if (g_watches.size() >= 4) break;
            if (pats[h.pat].desc != "view rows exact") continue;
            __try {
                const float* q = (const float*)h.addr;
                if (q[-16] == g_camP[0]) {
                    // watch the translation-x of the view (changes whenever the camera moves)
                    g_watches.push_back({ h.addr + 12, "per-view record (proj+view) view.tx" });
                    LOGI("Discovery: per-view record at %p (record starts %p)", (void*)h.addr, (void*)(h.addr - 0x44));
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
    for (int pass = 0; pass < 2 && g_watches.size() < 4; ++pass) {
        for (int k : order) {
            if (g_patHits[k] == 0 || g_patHits[k] > 50) continue;   // skip floods
            for (auto& h : hits) {
                if (g_watches.size() >= 4) break;
                if (h.pat != k || (pass == 0 && !h.cls)) continue;
                bool dup = false;
                for (auto& w : g_watches) if ((w.addr & ~0x3Full) == (h.addr & ~0x3Full)) dup = true;
                if (dup) continue;
                g_watches.push_back({ h.addr, pats[k].desc + (h.cls ? std::string(" in ") + h.cls : std::string(" (no RTTI owner)")) });
            }
        }
    }
    if (g_watches.empty()) { LOGE("Discovery: nothing to watch"); g_phase = 0; return 0; }
    LOGI("Discovery: arming hardware watchpoints (if the game closes right after this line, the DRM dislikes debug registers)");

    for (size_t i = 0; i < g_watches.size(); ++i) LOGI("Discovery: watch %zu -> %p %s", i, (void*)g_watches[i].addr, g_watches[i].desc.c_str());
    memset((void*)g_writers, 0, sizeof(g_writers));
    g_overflow = 0;
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, Veh);
    g_watching = true;
    SetDebugRegsAllThreads(true);
    g_watchStart = GetTickCount();
    g_phase = 3;
    LOGI("Discovery: watching writes for 5 seconds - MOVE THE CAMERA NOW");
    return 0;
}

// Called on the render thread when the capture window closes: read back every bound CB from the GPU.
void ReadbackBoundCBs() {
    std::vector<ID3D11Buffer*> bufs;
    { std::lock_guard<std::mutex> lk(g_capMx); bufs.swap(g_boundCBs); }
    int n = 0;
    for (auto* b : bufs) {
        D3D11_BUFFER_DESC bd; b->GetDesc(&bd);
        D3D11_BUFFER_DESC sd = {};
        sd.ByteWidth = bd.ByteWidth; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Buffer* st = nullptr;
        if (SUCCEEDED(g_dev->CreateBuffer(&sd, nullptr, &st))) {
            g_ctx->CopyResource(st, b);
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(g_imm.oMap(g_ctx, st, 0, D3D11_MAP_READ, 0, &m))) {
                char pre[32]; snprintf(pre, sizeof(pre), "cbread_u%u", (unsigned)bd.Usage);
                AddDump(pre, bd.ByteWidth, bd.BindFlags, m.pData, bd.ByteWidth);
                if (bd.ByteWidth >= 1024 * 1024 && bd.ByteWidth / 4 > g_bigCB.size()) {
                    g_bigCB.resize(bd.ByteWidth / 4);
                    memcpy(g_bigCB.data(), m.pData, bd.ByteWidth);
                }
                g_imm.oUnmap(g_ctx, st, 0);
                ++n;
            }
            st->Release();
        }
        b->Release();
    }
    LOGI("Discovery: read back %d of %zu bound constant buffers from the GPU", n, bufs.size());
}


// Immediate context reached through ID3D11DeviceContext4 (what the game's engineDraw uses) may have its
// own vtable/entry points; hook those separately.
MapFn o4Map = nullptr; UnmapFn o4Unmap = nullptr; ExecFn o4Exec = nullptr;
HRESULT STDMETHODCALLTYPE hk4Map(ID3D11DeviceContext* c, ID3D11Resource* r, UINT s, D3D11_MAP t, UINT f, D3D11_MAPPED_SUBRESOURCE* m) { HRESULT hr = o4Map(c, r, s, t, f, m); MapLogAdd('M', '4', r, t, (uintptr_t)_ReturnAddress(), m ? m->pData : nullptr, c); return hr; }
void STDMETHODCALLTYPE hk4Unmap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT s) { MapLogAdd('U', '4', r, 0, (uintptr_t)_ReturnAddress(), nullptr, c); o4Unmap(c, r, s); }
void STDMETHODCALLTYPE hk4Exec(ID3D11DeviceContext* c, ID3D11CommandList* cl, BOOL restore) {
    ++g_execSeq; MapLog_Marker('X', (uintptr_t)_ReturnAddress() - (uintptr_t)GetModuleHandleW(nullptr));
    if (Replay_OnExec(c, cl, restore, o4Exec)) return;
    Perf_OnExec(c);
    Temporal_OnExec(c, cl);
    o4Exec(c, cl, restore);
}
void* g_4MapAt = nullptr; void* g_4UnmapAt = nullptr; void* g_4ExecAt = nullptr;
// The immediate context keeps its vtable inline and the runtime swaps entries (e.g. when the game toggles
// multithread protection), so re-check the live entries and hook any new targets.
void* g_execOrigAt = nullptr;
void RehookImmediate() {
    if (!g_ctx) return;
    void** v = *(void***)g_ctx;
    bool mapNew = v[14] != g_imm.map && v[14] != g_4MapAt;
    bool execNew = v[58] != g_execOrigAt && v[58] != g_4ExecAt;
    if (!mapNew && !execNew) return;
    LOGI("Discovery: immediate live Map %p (hooked %p/%p) Unmap %p Exec %p (hooked %p/%p)", v[14], g_imm.map, g_4MapAt, v[15], v[58], g_execOrigAt, g_4ExecAt);
    if (mapNew && !g_4MapAt) {
        g_4MapAt = v[14]; if (MH_CreateHook(v[14], (void*)hk4Map, (void**)&o4Map) == MH_OK) LOGI("  rehook Map %d", (int)MH_EnableHook(v[14]));
        g_4UnmapAt = v[15]; if (MH_CreateHook(v[15], (void*)hk4Unmap, (void**)&o4Unmap) == MH_OK) LOGI("  rehook Unmap %d", (int)MH_EnableHook(v[15]));
    }
    if (execNew && !g_4ExecAt) {
        MH_STATUS st = MH_CreateHook(v[58], (void*)hk4Exec, (void**)&o4Exec);
        if (st == MH_OK) { g_4ExecAt = v[58]; LOGI("  rehook Exec %d", (int)MH_EnableHook(v[58])); } else LOGI("  rehook Exec create %d", (int)st);
    }
}
void HookCtx4(ID3D11DeviceContext* ctx) {
    ID3D11DeviceContext4* c4 = nullptr;
    if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void**)&c4)) || !c4) { LOGI("Discovery: no ID3D11DeviceContext4"); return; }
    void** v4 = *(void***)c4; void** v = *(void***)ctx;
    LOGI("Discovery: context4 %p vtable %p (context %p vtable %p) Map %p/%p Exec %p/%p", (void*)c4, (void*)v4, (void*)ctx, (void*)v, v4[14], v[14], v4[58], v[58]);
    if (v4[14] != v[14]) { if (MH_CreateHook(v4[14], (void*)hk4Map, (void**)&o4Map) == MH_OK) MH_EnableHook(v4[14]);
                           if (MH_CreateHook(v4[15], (void*)hk4Unmap, (void**)&o4Unmap) == MH_OK) MH_EnableHook(v4[15]); }
    if (v4[58] != v[58]) { if (MH_CreateHook(v4[58], (void*)hk4Exec, (void**)&o4Exec) == MH_OK) MH_EnableHook(v4[58]); }
    c4->Release();
}

void FinishWatch() {
    SetDebugRegsAllThreads(false);
    g_watching = false;
    LOGI("Discovery: writer report (overflow %ld)", g_overflow);
    for (auto& r : g_writers) {
        if (!r.used) break;
        LOGI("  watch %d (%s): %ld writes by thread t%05lu", r.watch, g_watches[r.watch].desc.c_str(), r.count, r.tid);
        for (int i = 0; i < r.nframes; ++i) LOGI("      #%d %s", i, AddrName(r.frames[i]).c_str());
    }
    LOGI("Discovery: finished. Press F9 again to repeat.");
    g_phase = 0;
}

} // namespace

void Discovery_Init(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    HMODULE exe = GetModuleHandleW(nullptr);
    g_exeBase = (uintptr_t)exe;
    auto nt = (IMAGE_NT_HEADERS*)(g_exeBase + ((IMAGE_DOS_HEADER*)exe)->e_lfanew);
    g_exeEnd = g_exeBase + nt->OptionalHeader.SizeOfImage;
    LOGI("Discovery: exe base %p size 0x%x. Press F9 in gameplay to run camera discovery.", (void*)g_exeBase, nt->OptionalHeader.SizeOfImage);
    g_dev = dev; g_ctx = ctx;
    LOGI("Discovery: immediate context %p vtable %p", (void*)ctx, *(void**)ctx);
    HookCtx(ctx, g_imm, (void*)hkMapI, (void*)hkUnmapI, (void*)hkUpdI, "immediate");
    {
        void** ivt = *(void***)ctx;
        MH_STATUS a = MH_CreateHook(ivt[58], (void*)hkExec, (void**)&oExec), b = MH_EnableHook(ivt[58]);
        g_execOrigAt = ivt[58];
        LOGI("Discovery: ExecuteCommandList hook at %p: create %d enable %d", ivt[58], (int)a, (int)b);
    }
    HookCtx4(ctx);
    // The game records on deferred contexts created at startup (before we were hooked);
    // make one ourselves to reach the deferred-context implementation.
    ID3D11DeviceContext* dc = nullptr;
    HRESULT hr = dev->CreateDeferredContext(0, &dc);
    if (SUCCEEDED(hr) && dc) {
        if ((*(void***)dc)[14] != g_imm.map) HookCtx(dc, g_def, (void*)hkMapD, (void*)hkUnmapD, (void*)hkUpdD, "deferred");
        else LOGI("Discovery: deferred context shares the immediate implementation");
        dc->Release();
    } else LOGE("Discovery: CreateDeferredContext failed 0x%08lx", hr);
    void** dvt = *(void***)dev;
    if (MH_CreateHook(dvt[27], (void*)hkCreateDef, (void**)&oCreateDef) == MH_OK && MH_EnableHook(dvt[27]) == MH_OK)
        LOGD("Discovery: CreateDeferredContext hooked");
    if (MH_CreateHook(dvt[3], (void*)hkCreateBuf, (void**)&oCreateBuf) == MH_OK && MH_EnableHook(dvt[3]) == MH_OK)
        LOGD("Discovery: CreateBuffer hooked");
}

void Discovery_SetScreen(unsigned w, unsigned h) { if (w && h) g_screenAspect = (float)w / (float)h; }

// Watch writes to a few fixed addresses for 5 seconds (diagnostics for the stereo work).
void Discovery_WatchAddresses(const uintptr_t* addrs, const char* const* descs, int n) {
    if (g_phase != 0) { LOGE("Discovery: busy, watch request ignored"); return; }
    g_watches.clear();
    for (int i = 0; i < n && i < 4; ++i) { g_watches.push_back({ addrs[i], descs[i] }); LOGI("Discovery: watch %d -> %p %s", i, (void*)addrs[i], descs[i]); }
    memset((void*)g_writers, 0, sizeof(g_writers));
    g_overflow = 0;
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, Veh);
    g_watching = true;
    SetDebugRegsAllThreads(true);
    g_watchStart = GetTickCount();
    g_phase = 3;
    LOGI("Discovery: watching writes for 5 seconds (render thread t%05lu not covered)", GetCurrentThreadId());
}

void Discovery_LogExecs(int n) { g_execLog = n; }
void Discovery_LogDrawSources() {
    LOGD("  ExecuteCommandList calls so far: %llu", g_execSeq.load());
    int n = g_drawSrcN.load();
    for (int i = 0; i < n; ++i) LOGD("  draw source %d: t%05lu %c %p draws %llu", i, g_drawSrc[i].tid, g_drawSrc[i].kind, g_drawSrc[i].ctx, g_drawSrc[i].n.load());
}
uint64_t Discovery_ExecCount() { return g_execSeq.load(); }

bool g_triggerDiscovery = false;
void Discovery_Trigger() { g_triggerDiscovery = true; }

void Discovery_OnPresent() {
    static bool prevKey = false;
    bool key = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    bool pressed = (key && !prevKey) || g_triggerDiscovery;
    g_triggerDiscovery = false;
    prevKey = key;

    int phase = g_phase.load();
    if (phase == 0 && pressed) {
        LOGI("Discovery: F9 pressed");
        {
            std::lock_guard<std::mutex> lk(g_capMx);
            g_caps.clear(); g_bufInfos.clear(); g_dumps.clear(); g_dumpBytes = 0; g_copies.clear(); g_bigCB.clear();
            for (auto* b : g_boundCBs) b->Release();
            g_boundCBs.clear();
        }
        g_draws = 0; g_dispatches = 0; g_cbBinds = 0; g_dumpSeq = 0; g_cbBindsNonNull = 0; g_cb1Binds = 0; g_bufCreates = 0; g_cbCreates = 0;
        g_mapCount = 0; g_scanBytes = 0; g_bigSkipped = 0; g_updCount = 0; g_upd1Count = 0; g_execCount = 0;
        g_captureFrames = 3;   // decremented each Present; >0 means capturing
        g_phase = 1;
    } else if (phase == 1) {
        if (--g_captureFrames <= 0) {
            g_captureFrames = 0;
            ReadbackBoundCBs();
            g_phase = 2;
            HANDLE t = CreateThread(nullptr, 0, AnalyseThread, nullptr, 0, nullptr);
            if (t) CloseHandle(t);
        }
    } else if (phase == 3 && GetTickCount() - g_watchStart > 5000) {
        FinishWatch();
    }
}
void Discovery_Rehook() { RehookImmediate(); }

void Discovery_InstDraws(uint64_t* n, uint64_t* mn, uint64_t* mx, uint64_t* avg) {
    *n = g_diN.exchange(0); *mn = g_diMin.exchange(~0ull); *mx = g_diMax.exchange(0); uint64_t sm = g_diSum.exchange(0); *avg = *n ? sm / *n : 0;
}

void Discovery_ExecRaw(ID3D11DeviceContext* c, ID3D11CommandList* cl, BOOL restore) {
    if (o4Exec) o4Exec(c, cl, restore); else if (oExec) oExec(c, cl, restore); else c->ExecuteCommandList(cl, restore);
}
