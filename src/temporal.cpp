// Per-eye temporal state.
//
// Eye 1 is produced by replaying eye 0's command lists, so any texture that carries state from one
// frame to the next (TAA history, previous-frame depth, exposure...) would be shared by the eyes: the
// left eye would read the right eye's history and vice versa. We detect such textures automatically
// (first access in the frame is a read, and the frame also writes them), then keep one copy per eye:
//   - before the game's own (eye 0) lists run: restore eye 0's copy
//   - at Present: save eye 0's result, restore eye 1's copy, replay, save eye 1's result.
// Textures written on the immediate context outside the lists (engineDraw's own compute work) are
// excluded, as restoring them would undo that work.
#include <windows.h>
#include <d3d11_4.h>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include "log.h"

namespace {
struct Access { ID3D11Resource* r; bool write; };
std::mutex g_mx;
bool g_detect = false;            // recording accesses (detection phase)
int g_detectFrames = 0, g_detectTotal = 0;
int g_redetectIn = 0;
int g_logRejected = 24;             // frames until (re)detection starts; 0 = none scheduled
std::unordered_map<ID3D11DeviceContext*, std::vector<Access>> g_ctxLog;
std::unordered_map<ID3D11CommandList*, std::vector<Access>> g_listLog;
std::vector<Access> g_frameSeq;
std::unordered_map<ID3D11Resource*, int> g_tempCount;   // frames in which a texture behaved temporally
std::unordered_set<ID3D11Resource*> g_immWritten;       // written on the immediate context
std::unordered_set<ID3D11Resource*> g_listWritten;      // written by the game's command lists during detection
std::unordered_map<ID3D11View*, ID3D11Resource*> g_viewRes;

const int kMaxViews = 4;   // eye 0/1 (outer) and, with quad views, eye 0/1 inner
struct Tracked { ID3D11Texture2D* tex; ID3D11Texture2D* copy[kMaxViews]; bool have[kMaxViews]; };
std::vector<Tracked> g_tracked;
bool g_active = false;            // swapping enabled
bool g_frameRestored = false;     // eye-0 restore done for this frame

ID3D11Resource* ResOf(ID3D11View* v) {
    if (!v) return nullptr;
    auto it = g_viewRes.find(v);
    if (it != g_viewRes.end()) return it->second;
    ID3D11Resource* r = nullptr; v->GetResource(&r);
    if (r) r->Release();   // the view keeps it alive
    D3D11_RESOURCE_DIMENSION d = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    if (r) r->GetType(&d);
    if (d != D3D11_RESOURCE_DIMENSION_TEXTURE2D) r = nullptr;
    if (g_viewRes.size() < 200000) g_viewRes[v] = r;
    return r;
}
void Note(ID3D11DeviceContext* c, ID3D11Resource* r, bool w) {
    if (!r) return;
    auto& v = g_ctxLog[c];
    if (v.size() < 100000) v.push_back({ r, w });
}
} // namespace

// ---- hooks (deferred contexts unless noted)
void Temporal_OnSRVs(ID3D11DeviceContext* c, UINT n, ID3D11ShaderResourceView* const* v) {
    if (!g_detect || !v) return;
    std::lock_guard<std::mutex> lk(g_mx);
    for (UINT i = 0; i < n; ++i) Note(c, ResOf(v[i]), false);
}
void Temporal_OnRTVs(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* v) {
    if (!g_detect || !v) return;
    std::lock_guard<std::mutex> lk(g_mx);
    for (UINT i = 0; i < n; ++i) Note(c, ResOf(v[i]), true);
}
void Temporal_OnUAVs(ID3D11DeviceContext* c, UINT n, ID3D11UnorderedAccessView* const* v) {
    if (!g_detect || !v) return;
    std::lock_guard<std::mutex> lk(g_mx);
    for (UINT i = 0; i < n; ++i) Note(c, ResOf(v[i]), true);
}
void Temporal_OnDSV(ID3D11DeviceContext* c, ID3D11DepthStencilView* d) {
    if (!g_detect || !d) return;
    std::lock_guard<std::mutex> lk(g_mx);
    Note(c, ResOf(d), true);
}
void Temporal_OnClearView(ID3D11DeviceContext* c, ID3D11View* v) {
    if (!g_detect || !v) return;
    std::lock_guard<std::mutex> lk(g_mx);
    Note(c, ResOf(v), true);
}
void Temporal_OnCopy(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src) {
    if (!g_detect) return;
    std::lock_guard<std::mutex> lk(g_mx);
    D3D11_RESOURCE_DIMENSION d;
    if (src) { src->GetType(&d); if (d == D3D11_RESOURCE_DIMENSION_TEXTURE2D) Note(c, src, false); }
    if (dst) { dst->GetType(&d); if (d == D3D11_RESOURCE_DIMENSION_TEXTURE2D) Note(c, dst, true); }
}
// immediate context writes (UAV/RTV binds, copies) outside the command lists
void Temporal_OnImmediateWrite(ID3D11Resource* r) {
    if (!g_detect || !r) return;
    std::lock_guard<std::mutex> lk(g_mx);
    g_immWritten.insert(r);
}
void Temporal_OnImmediateViewsWrite(UINT n, ID3D11View* const* v) {
    if (!g_detect || !v) return;
    std::lock_guard<std::mutex> lk(g_mx);
    for (UINT i = 0; i < n; ++i) { ID3D11Resource* r = ResOf(v[i]); if (r) g_immWritten.insert(r); }
}
void Temporal_OnFinish(ID3D11DeviceContext* c, ID3D11CommandList* cl) {
    if (!g_detect || !cl) return;
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_ctxLog.find(c);
    if (it != g_ctxLog.end()) { g_listLog[cl] = std::move(it->second); g_ctxLog.erase(it); }
}

// ExecuteCommandList of the game's own (eye 0) lists, on the engineDraw thread
void Temporal_OnExec(ID3D11DeviceContext* imm, ID3D11CommandList* cl) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_detect) {
        auto it = g_listLog.find(cl);
        if (it != g_listLog.end()) { g_frameSeq.insert(g_frameSeq.end(), it->second.begin(), it->second.end()); g_listLog.erase(it); }
    }
    if (g_active && !g_frameRestored) {
        g_frameRestored = true;
        for (auto& t : g_tracked) if (t.have[0]) imm->CopyResource(t.tex, t.copy[0]);
    }
}

void Temporal_StartDetect(int frames) {
    std::lock_guard<std::mutex> lk(g_mx);
    g_detect = true; g_detectFrames = g_detectTotal = frames;
    g_ctxLog.clear(); g_listLog.clear(); g_frameSeq.clear(); g_tempCount.clear(); g_immWritten.clear(); g_listWritten.clear();
}

static void ReleaseTracked() {
    for (auto& t : g_tracked) { t.tex->Release(); for (auto* c : t.copy) if (c) c->Release(); }
    g_tracked.clear(); g_active = false;
}
static void FinishDetect(ID3D11Device* dev) {
    g_detect = false;
    ReleaseTracked();
    std::vector<std::pair<ID3D11Resource*, int>> c(g_tempCount.begin(), g_tempCount.end());
    std::sort(c.begin(), c.end(), [](auto& a, auto& b) { return a.second > b.second; });
    for (auto& p : c) {
        ID3D11Texture2D* t = (ID3D11Texture2D*)p.first;
        bool imm = g_immWritten.count(p.first) > 0;
        if (!g_listWritten.count(p.first)) {   // never written by the lists: static content (or written elsewhere)
            if (p.second * 5 >= g_detectTotal * 2 && g_logRejected > 0) {
                D3D11_TEXTURE2D_DESC d; t->GetDesc(&d);
                if (d.Width >= 64 && d.Height >= 64) { --g_logRejected; LOGD("Temporal: read-first but not written by the lists: %p %ux%u fmt %d bind 0x%x (%d frames)", (void*)t, d.Width, d.Height, (int)d.Format, d.BindFlags, p.second); }
            }
            continue;
        }
        D3D11_TEXTURE2D_DESC d; t->GetDesc(&d);
        bool take = p.second * 5 >= g_detectTotal * 2 && !imm && d.SampleDesc.Count == 1 && g_tracked.size() < 24;
        LOGI("Temporal: candidate %p %ux%u fmt %d mips %u array %u bind 0x%x: temporal in %d frames%s -> %s",
             (void*)t, d.Width, d.Height, (int)d.Format, d.MipLevels, d.ArraySize, d.BindFlags, p.second,
             imm ? " (written on immediate context)" : "", take ? "per-eye" : "skipped");
        if (!take) continue;
        Tracked tr = {}; tr.tex = t;
        D3D11_TEXTURE2D_DESC cd = d; cd.BindFlags = 0; cd.CPUAccessFlags = 0; cd.Usage = D3D11_USAGE_DEFAULT; cd.MiscFlags &= D3D11_RESOURCE_MISC_TEXTURECUBE;
        bool ok = SUCCEEDED(dev->CreateTexture2D(&cd, nullptr, &tr.copy[0])) && SUCCEEDED(dev->CreateTexture2D(&cd, nullptr, &tr.copy[1]));
        if (!ok) { LOGE("Temporal: copy creation failed"); if (tr.copy[0]) tr.copy[0]->Release(); if (tr.copy[1]) tr.copy[1]->Release(); continue; }
        t->AddRef();
        g_tracked.push_back(tr);
    }
    g_active = !g_tracked.empty();
    LOGI("Temporal: %zu textures kept per eye", g_tracked.size());
    g_ctxLog.clear(); g_listLog.clear(); g_frameSeq.clear();
}

// Present (main thread, before the replay): analyse detection data; save eye 0, restore eye 1
void Temporal_BeforeReplay(ID3D11DeviceContext* imm) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_redetectIn > 0 && --g_redetectIn == 0) {
        g_detect = true; g_detectFrames = g_detectTotal = 60;
        g_ctxLog.clear(); g_listLog.clear(); g_frameSeq.clear(); g_tempCount.clear(); g_immWritten.clear(); g_listWritten.clear(); g_viewRes.clear();
        LOGI("Temporal: detecting per-frame history textures");
        return;   // start with a clean frame
    }
    if (g_detect) {
        std::unordered_map<ID3D11Resource*, int> first;   // 1 read first, 2 write first
        std::unordered_set<ID3D11Resource*> written;
        for (auto& a : g_frameSeq) {
            if (!first.count(a.r)) first[a.r] = a.write ? 2 : 1;
            if (a.write) written.insert(a.r);
        }
        // read before written this frame -> its content came from an earlier frame. (Ping-pong history
        // pairs are written only every other frame, so "written by the lists" is checked over the window.)
        if (!g_frameSeq.empty()) for (auto& f : first) if (f.second == 1) ++g_tempCount[f.first];
        for (auto* r : written) g_listWritten.insert(r);
        g_frameSeq.clear();
        if (--g_detectFrames <= 0) {
            ID3D11Device* dev = nullptr; imm->GetDevice(&dev);
            if (dev) { FinishDetect(dev); dev->Release(); }
        }
    }
}
// copy k of a tracked texture (views 2/3 are created on first use)
static ID3D11Texture2D* CopyOf(ID3D11DeviceContext* imm, Tracked& t, int k) {
    if (t.copy[k]) return t.copy[k];
    ID3D11Device* dev = nullptr; imm->GetDevice(&dev);
    if (!dev) return nullptr;
    D3D11_TEXTURE2D_DESC cd; t.tex->GetDesc(&cd); cd.BindFlags = 0; cd.CPUAccessFlags = 0; cd.Usage = D3D11_USAGE_DEFAULT; cd.MiscFlags &= D3D11_RESOURCE_MISC_TEXTURECUBE;
    if (FAILED(dev->CreateTexture2D(&cd, nullptr, &t.copy[k]))) t.copy[k] = nullptr;
    dev->Release();
    return t.copy[k];
}
// Present, before replaying view k (1..): save view k-1's results, restore view k's history
void Temporal_BeforeView(ID3D11DeviceContext* imm, int k) {
    if (k <= 1) { Temporal_BeforeReplay(imm); }
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_active || k < 1 || k >= kMaxViews) return;
    for (auto& t : g_tracked) {
        ID3D11Texture2D* prev = CopyOf(imm, t, k - 1); ID3D11Texture2D* cur = CopyOf(imm, t, k);
        if (prev) { imm->CopyResource(prev, t.tex); t.have[k - 1] = true; }
        if (cur && t.have[k]) imm->CopyResource(t.tex, cur);
    }
}
// Present, after the last view (n views in total)
void Temporal_AfterViews(ID3D11DeviceContext* imm, int n) {
    std::lock_guard<std::mutex> lk(g_mx);
    g_frameRestored = false;
    if (!g_active || n < 2 || n > kMaxViews) return;
    for (auto& t : g_tracked) { ID3D11Texture2D* c = CopyOf(imm, t, n - 1); if (c) { imm->CopyResource(c, t.tex); t.have[n - 1] = true; } }
}
void Temporal_AfterReplay(ID3D11DeviceContext* imm) { Temporal_AfterViews(imm, 2); }
// stereo stopped (menus, loading): leave the game's own state alone and forget the per-eye copies' validity
void Temporal_Reset() {
    std::lock_guard<std::mutex> lk(g_mx);
    for (auto& t : g_tracked) for (auto& h : t.have) h = false;
    g_frameRestored = false;
}

// (re)detect after this many stereo frames (startup, render-size change)
void Temporal_Schedule(int frames) {
    std::lock_guard<std::mutex> lk(g_mx);
    g_detect = false;
    ReleaseTracked();
    g_viewRes.clear();
    g_frameRestored = false;
    g_redetectIn = frames;
}
