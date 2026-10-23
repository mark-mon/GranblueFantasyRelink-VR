// Performance statistics for the stats overlay and the log.
//  - frame interval (Present to Present) -> fps
//  - main thread CPU time inside render-frame, in our Present work (eye-1 replay submission), and
//    blocked in xrWaitFrame (time the runtime holds the game back)
//  - GPU time, from D3D11 timestamp queries on the immediate context: eye 0 from the frame's first
//    ExecuteCommandList to the start of our Present work, eye 1 for the replay.
#include <windows.h>
#include <d3d11.h>
#include <mutex>
#include <cstdio>
#include <algorithm>
#include "log.h"

namespace {
LARGE_INTEGER g_qpf;
double Now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (double)t.QuadPart * 1000.0 / (double)g_qpf.QuadPart; }

struct Acc { double sum = 0; int n = 0; void Add(double v) { sum += v; ++n; } double Avg() const { return n ? sum / n : 0; } };
std::mutex g_mx;
Acc a_frame, a_render, a_present, a_xrwait, a_gpu0, a_gpu1;
double g_lastPresent = 0, g_windowStart = 0;
// published averages (last window)
double p_fps = 0, p_frame = 0, p_render = 0, p_present = 0, p_xrwait = 0, p_gpu0 = 0, p_gpu1 = 0;
bool p_valid = false, p_gpuValid = false;

// GPU timestamps: ring of query sets, read back 3 frames later
struct QSet { ID3D11Query* disjoint = nullptr; ID3D11Query* t[3] = {}; bool began = false, have[3] = {}, pending = false; };
const int kRing = 5;
QSet g_q[kRing];
int g_cur = 0;
ID3D11DeviceContext* g_ctx = nullptr;
bool g_qOk = false;
volatile LONG g_firstExecDone = 0;

bool EnsureQueries(ID3D11DeviceContext* c) {
    if (g_qOk) return true;
    if (g_ctx) return false;   // creation failed before
    g_ctx = c;
    ID3D11Device* dev = nullptr; c->GetDevice(&dev);
    if (!dev) return false;
    D3D11_QUERY_DESC dd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }, td = { D3D11_QUERY_TIMESTAMP, 0 };
    bool ok = true;
    for (auto& q : g_q) {
        ok = ok && SUCCEEDED(dev->CreateQuery(&dd, &q.disjoint));
        for (auto& t : q.t) ok = ok && SUCCEEDED(dev->CreateQuery(&td, &t));
    }
    dev->Release();
    if (!ok) LOGE("Perf: timestamp queries unavailable");
    g_qOk = ok;
    return ok;
}
void Stamp(int i) {
    QSet& q = g_q[g_cur];
    if (!q.began) return;
    g_ctx->End(q.t[i]); q.have[i] = true;
}
void Collect() {
    // read the oldest pending set (non-blocking)
    for (int k = 1; k < kRing; ++k) {
        QSet& q = g_q[(g_cur + k) % kRing];
        if (!q.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        if (g_ctx->GetData(q.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        UINT64 t[3] = {};
        bool ok = !dj.Disjoint && dj.Frequency;
        for (int i = 0; i < 3 && ok; ++i) ok = q.have[i] && g_ctx->GetData(q.t[i], &t[i], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
        if (ok && t[1] >= t[0] && t[2] >= t[1]) {
            std::lock_guard<std::mutex> lk(g_mx);
            a_gpu0.Add((double)(t[1] - t[0]) * 1000.0 / dj.Frequency);
            a_gpu1.Add((double)(t[2] - t[1]) * 1000.0 / dj.Frequency);
        }
        q.pending = false;
    }
}
} // namespace

void Perf_Init() { QueryPerformanceFrequency(&g_qpf); }
double Perf_Now() { return Now(); }

// engineDraw thread: the frame's first ExecuteCommandList (immediate context)
void Perf_OnExec(ID3D11DeviceContext* c) {
    if (InterlockedCompareExchange(&g_firstExecDone, 1, 0) != 0) return;
    if (!EnsureQueries(c)) return;
    QSet& q = g_q[g_cur];
    if (q.began) { c->End(q.disjoint); q.began = false; }   // previous frame never reached the replay: drop it
    if (q.pending) return;   // ring full, skip this frame
    c->Begin(q.disjoint); q.began = true;
    q.have[0] = q.have[1] = q.have[2] = false;
    Stamp(0);
}
// main thread, Present: before / after our replay of the frame for eye 1
void Perf_BeforeReplay() { if (g_qOk) Stamp(1); }
void Perf_AfterReplay() {
    if (!g_qOk) return;
    QSet& q = g_q[g_cur];
    if (q.began) {
        Stamp(2);
        g_ctx->End(q.disjoint); q.began = false; q.pending = true;
        g_cur = (g_cur + 1) % kRing;
    }
}
void Perf_AddRender(double ms) { std::lock_guard<std::mutex> lk(g_mx); a_render.Add(ms); }
void Perf_AddPresentWork(double ms) { std::lock_guard<std::mutex> lk(g_mx); a_present.Add(ms); }
void Perf_AddXrWait(double ms) { std::lock_guard<std::mutex> lk(g_mx); a_xrwait.Add(ms); }

// main thread, every Present
void Perf_OnPresent() {
    double now = Now();
    if (g_qOk) Collect();
    InterlockedExchange(&g_firstExecDone, 0);
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_lastPresent) a_frame.Add(now - g_lastPresent);
    g_lastPresent = now;
    if (!g_windowStart) g_windowStart = now;
    if (now - g_windowStart >= 500.0 && a_frame.n) {
        p_frame = a_frame.Avg(); p_fps = p_frame > 0 ? 1000.0 / p_frame : 0;
        p_render = a_render.Avg(); p_present = a_present.Avg(); p_xrwait = a_xrwait.Avg();
        p_gpuValid = a_gpu0.n > 0;
        if (p_gpuValid) { p_gpu0 = a_gpu0.Avg(); p_gpu1 = a_gpu1.Avg(); }
        p_valid = true;
        a_frame = a_render = a_present = a_xrwait = a_gpu0 = a_gpu1 = Acc();
        g_windowStart = now;
    }
}
// text for the overlay / log; returns false until the first window is complete
bool Perf_Text(char* out, size_t n) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!p_valid) return false;
    // main thread busy = frame time minus the time the VR runtime held it in xrWaitFrame
    int k = snprintf(out, n, "%.1f fps  %.1f ms/frame\nCPU main %.1f  VR wait %.1f  submit %.1f ms\n",
                     p_fps, p_frame, (std::max)(0.0, p_frame - p_xrwait), p_xrwait, p_present);
    if (k > 0 && (size_t)k < n) {
        if (p_gpuValid) snprintf(out + k, n - k, "GPU eye1 %.1f  eye2 %.1f  total %.1f ms", p_gpu0, p_gpu1, p_gpu0 + p_gpu1);
        else snprintf(out + k, n - k, "GPU (no data)");
    }
    return true;
}
