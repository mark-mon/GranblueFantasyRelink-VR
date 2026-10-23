// Command-list replay stereo.
//
// Eye 0 is rendered by the game normally. Eye 1's own command lists are not usable (per-frame data such
// as the instance ring is produced once per game frame, so lists recorded by a second render-frame
// reference stale/unflushed data). Instead, when the engineDraw thread submits eye 1, we copy eye 1's
// per-view constant blocks (uploaded by eye 1's render-frame) over eye 0's blocks on the GPU timeline
// and execute eye 0's command lists a second time.
#include <windows.h>
#include <d3d11_4.h>
#include <vector>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include "log.h"
#include "globals.h"

namespace {
struct Desc { ID3D11Buffer* buf; uint64_t off; uint64_t size; uint64_t token; };
std::mutex g_mx;
std::vector<ID3D11CommandList*> g_lists;      // eye 0's lists for the current frame
volatile LONG g_phase = 0;                    // 0 idle, 1 eye-0 submission, 2 eye-1 submission
bool g_replayed = false;
std::vector<Desc> g_up[2];                  // every constant upload render-frame made on the main thread, per eye
ID3D11Buffer* g_tmp = nullptr;
uint64_t g_frames = 0, g_skipped = 0, g_copies = 0;
int g_log = 12;

void CopyBlock(ID3D11DeviceContext* c, const Desc& from, const Desc& to) {
    if (!from.buf || !to.buf || !from.size || from.size != to.size || from.size > 0x10000) return;
    if (!g_tmp) {
        ID3D11Device* dev = nullptr; c->GetDevice(&dev);
        D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = 0x10000; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (dev) { dev->CreateBuffer(&bd, nullptr, &g_tmp); dev->Release(); }
        if (!g_tmp) { LOGE("Replay: temp buffer creation failed"); return; }
    }
    D3D11_BOX b = { (UINT)from.off, 0, 0, (UINT)(from.off + from.size), 1, 1 };
    c->CopySubresourceRegion(g_tmp, 0, 0, 0, 0, from.buf, 0, &b);
    D3D11_BOX b2 = { 0, 0, 0, (UINT)from.size, 1, 1 };
    c->CopySubresourceRegion(to.buf, 0, (UINT)to.off, 0, 0, g_tmp, 0, &b2);
    ++g_copies;
}
} // namespace

// camera.cpp: main-view descriptors (view element +0x950, +0x970) after each eye's render-frame
void Replay_AddUpload(int eye, const void* d) {
    std::lock_guard<std::mutex> lk(g_mx);
    Desc x; memcpy(&x, d, sizeof(Desc));
    if (eye >= 0 && eye < 2 && g_up[eye].size() < 4096) g_up[eye].push_back(x);
}
void Replay_ClearUploads() { std::lock_guard<std::mutex> lk(g_mx); g_up[0].clear(); g_up[1].clear(); }
void Replay_BeginEye0Submit() {
    std::lock_guard<std::mutex> lk(g_mx);
    for (auto* l : g_lists) l->Release();
    g_lists.clear(); g_replayed = false;
    InterlockedExchange(&g_phase, 1);
}
void Replay_EndEye0Submit() { InterlockedExchange(&g_phase, 0); }
void Replay_BeginEye1Submit() { InterlockedExchange(&g_phase, 2); }
void Replay_EndFrame() {
    InterlockedExchange(&g_phase, 0);
    std::lock_guard<std::mutex> lk(g_mx);
    for (auto* l : g_lists) l->Release();
    g_lists.clear();
}

void Discovery_ExecRaw(ID3D11DeviceContext* c, ID3D11CommandList* cl, BOOL restore);
static void ReplayNow(ID3D11DeviceContext* c, const char* where) {
    g_replayed = true; ++g_frames;
    uint64_t c0 = g_copies;
    size_t n = g_up[0].size() < g_up[1].size() ? g_up[0].size() : g_up[1].size();
    size_t mism = 0;
    for (size_t i = 0; i < n; ++i) {
        const Desc& a = g_up[0][i]; const Desc& b = g_up[1][i];
        if (a.size != b.size) { ++mism; continue; }
        if (a.buf == b.buf && a.off == b.off) continue;
        CopyBlock(c, b, a);
    }
    for (auto* l : g_lists) Discovery_ExecRaw(c, l, FALSE);
    if (g_log > 0) {
        --g_log;
        LOGD("Replay (%s): frame %llu: %zu eye-0 lists re-executed, uploads eye0 %zu eye1 %zu, %llu block copies, %zu size mismatches",
             where, g_frames, g_lists.size(), g_up[0].size(), g_up[1].size(), g_copies - c0, mism);
    }
}
// vr.cpp: engineDraw's frame-latency wait (before it executes its lists) for eye 1's submission
void Replay_AtLatencyWait(ID3D11DeviceContext* c) {
    if (g_phase != 2) return;
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_replayed) ReplayNow(c, "latency");
}

// discovery.cpp ExecuteCommandList hook: returns true if the call was handled (skipped/replaced)
bool Replay_RecordAll(); void Replay_RecordList(ID3D11CommandList* cl);
bool Replay_OnExec(ID3D11DeviceContext* c, ID3D11CommandList* cl, BOOL restore,
                   void (STDMETHODCALLTYPE* exec)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL)) {
    if (Replay_RecordAll()) { Replay_RecordList(cl); return false; }
    LONG ph = g_phase;
    if (ph == 1) {
        std::lock_guard<std::mutex> lk(g_mx);
        cl->AddRef(); g_lists.push_back(cl);
        return false;
    }
    if (ph == 2) {
        std::lock_guard<std::mutex> lk(g_mx);
        ++g_skipped;
        if (!g_replayed) ReplayNow(c, "exec");
        return true;   // eye 1's own list is dropped
    }
    return false;
}

// ---- single-pass mode
// Eye 0 is drawn by the game. While its command lists are recorded, binds of the main view's two
// constant blocks (in the game's dynamic constant ring) are redirected to two buffers of ours, so
// at Present we can update those (UpdateSubresource, properly ordered on the GPU timeline) with eye
// 1's blocks and replay the lists. Writing into the game's dynamic ring from the GPU instead caused
// driver resets (TDR / VIDEO_MEMORY_MANAGEMENT_INTERNAL bugchecks).
namespace {
bool g_pjPending = false; float g_pjData[2][0x220 / 4];
const int kMaxExtra = 3;                    // replayed views: eye 1, and with quad views eye 0/1 inner
float g_pjViews[kMaxExtra][2][0x220 / 4]; int g_pjN = 0;
ID3D11Buffer* g_viewCB[2] = {};
ID3D11Buffer* volatile g_ringBuf = nullptr;
volatile UINT g_blkFirst[2] = { 0xffffffff, 0xffffffff };   // firstConstant of block A / B in the ring this frame
bool g_recordAll = false;
ID3D11DeviceContext* g_imm = nullptr;
int g_pjLog = 8;
std::atomic<uint64_t> g_subst{ 0 };
bool EnsureViewCBs() {
    if (g_viewCB[0] && g_viewCB[1]) return true;
    if (!g_imm) return false;
    ID3D11Device* dev = nullptr; g_imm->GetDevice(&dev);
    if (!dev) return false;
    for (int i = 0; i < 2; ++i) if (!g_viewCB[i]) {
        D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = 0x400; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        std::vector<uint8_t> z(0x400, 0); D3D11_SUBRESOURCE_DATA init = { z.data(), 0, 0 };
        dev->CreateBuffer(&bd, &init, &g_viewCB[i]);
    }
    dev->Release();
    return g_viewCB[0] && g_viewCB[1];
}
void Upload(const float* A, const float* B) {
    D3D11_BOX box = { 0, 0, 0, 0x220, 1, 1 };   // buffers (not CB-typed partial updates): full 0x220 bytes at 0
    // constant buffers must be updated whole on 11.0 drivers: use a full-size staging copy
    static uint8_t tmp[2][0x400];
    memcpy(tmp[0], A, 0x220); memcpy(tmp[1], B, 0x220);
    (void)box;
    g_imm->UpdateSubresource(g_viewCB[0], 0, nullptr, tmp[0], 0, 0);
    g_imm->UpdateSubresource(g_viewCB[1], 0, nullptr, tmp[1], 0, 0);
}
}
void Replay_SetContext(ID3D11DeviceContext* c) { g_imm = c; }
// camera.cpp hkCbUpload: render-frame uploaded the main view's block (0 = A, 1 = B)
void Replay_NoteMainViewUpload(int which, const void* desc) {
    Desc d; memcpy(&d, desc, sizeof(d));
    if (which < 0 || which > 1 || (d.off & 15)) return;
    g_ringBuf = d.buf;
    g_blkFirst[which] = (UINT)(d.off / 16);
}
void Replay_EnableRedirect(bool on) { g_recordAll = on; if (!on) { g_blkFirst[0] = g_blkFirst[1] = 0xffffffff; } }
// discovery.cpp deferred *SetConstantBuffers1 hooks: substitute our buffers for the main view blocks
bool Replay_SubstCB(UINT n, ID3D11Buffer* const* b, const UINT* f, const UINT* k, ID3D11Buffer** ob, UINT* of, UINT* ok) {
    if (!g_recordAll || !b || !f || n > 14 || !g_viewCB[0]) return false;
    ID3D11Buffer* ring = g_ringBuf;
    bool any = false;
    for (UINT i = 0; i < n; ++i) {
        ob[i] = b[i]; of[i] = f[i]; ok[i] = k ? k[i] : 4096;
        if (b[i] && b[i] == ring) {
            for (int w = 0; w < 2; ++w) if (f[i] == g_blkFirst[w]) { ob[i] = g_viewCB[w]; of[i] = 0; ok[i] = (std::min)(ok[i], 0x400u / 16); any = true; }
        }
    }
    if (any) ++g_subst;
    return any;
}
// camera.cpp after render-frame: eye 0's blocks (for the game's own submission) and eye 1's (for the replay)
void Replay_SetPresentJob(const void* eye0A, const void* eye0B, const float* A, const float* B) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!EnsureViewCBs()) return;
    Upload((const float*)eye0A, (const float*)eye0B);
    memcpy(g_pjData[0], A, 0x220); memcpy(g_pjData[1], B, 0x220);
    memcpy(g_pjViews[0][0], A, 0x220); memcpy(g_pjViews[0][1], B, 0x220);
    g_pjN = 1;
    g_pjPending = true;
}
// camera.cpp (quad views): blocks of a further replayed view (index 1 = eye 0 inner, 2 = eye 1 inner)
void Replay_AddView(int k, const float* A, const float* B) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (k < 1 || k >= kMaxExtra) return;
    memcpy(g_pjViews[k][0], A, 0x220); memcpy(g_pjViews[k][1], B, 0x220);
    if (g_pjN < k + 1) g_pjN = k + 1;
}
int Replay_ViewCount() { return g_pjPending ? 1 + g_pjN : 1; }
// Present: replay the frame's lists for replayed view k (0-based among replayed views); the lists are
// released after the last one.
void Replay_RunView(ID3D11DeviceContext* c, int k) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_pjPending || k < 0 || k >= g_pjN) return;
    if (!EnsureViewCBs()) return;
    Upload(g_pjViews[k][0], g_pjViews[k][1]);
    for (auto* l : g_lists) Discovery_ExecRaw(c, l, FALSE);
    if (k == g_pjN - 1) {
        size_t n = g_lists.size();
        for (auto* l : g_lists) l->Release();
        g_lists.clear();
        g_pjPending = false;
        if (g_pjLog > 0) { --g_pjLog; LOGD("Replay at Present: %zu lists re-executed for %d views (%llu redirected binds so far)", n, g_pjN, g_subst.load()); }
    }
}
bool Replay_RecordAll() { return g_recordAll; }
void Replay_RecordList(ID3D11CommandList* cl) { std::lock_guard<std::mutex> lk(g_mx); cl->AddRef(); g_lists.push_back(cl); }
bool Replay_PresentPending() { return g_pjPending; }
void Replay_AtPresent(ID3D11DeviceContext* c) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_pjPending) return;
    g_pjPending = false;
    if (!EnsureViewCBs()) return;
    Upload(g_pjData[0], g_pjData[1]);
    size_t n = g_lists.size();
    for (auto* l : g_lists) Discovery_ExecRaw(c, l, FALSE);
    for (auto* l : g_lists) l->Release();
    g_lists.clear();
    if (g_pjLog > 0) { --g_pjLog; LOGD("Replay at Present: %zu lists re-executed with eye-1 view blocks (%llu redirected binds so far)", n, g_subst.load()); }
}
void Replay_DropRecorded() { std::lock_guard<std::mutex> lk(g_mx); for (auto* l : g_lists) l->Release(); g_lists.clear(); }
