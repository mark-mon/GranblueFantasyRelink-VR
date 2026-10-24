// Head tracking: hooks the engine's look-at view builder (exe+0x74ad10) for the main view and
// applies the HMD pose (relative to the recentred reference) in the game camera's own basis.
// Also instruments the render-frame function (exe+0x1cbcd0) for the stereo work.
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <atomic>
#include <algorithm>
#include <cstring>
#include "MinHook.h"
#include "globals.h"
#include "camera.h"
void MapLog_Marker(char, uintptr_t);
static void InstStats(const char* tag);
extern volatile int g_recEye; extern volatile bool g_spActive;
extern int g_upN[2]; static void LogUploadDiffs();
// engineDraw (exe+0x1a97d0) flips its double-buffer index for the per-object lists A/B (exe+0x718c908 /
// 0x718c8f8) and then uploads list[index]; the producer (exe+0x23fe2c0, once per game frame) fills
// list[exe+0x7ba4d24] / list[exe+0x7ba4d20]. Make the next engineDraw run consume the producer's list.
static void SyncListParity();
void Replay_AddUpload(int eye, const void* d); void Replay_ClearUploads(); void Replay_BeginEye0Submit(); void Replay_EndEye0Submit();
void Replay_BeginEye1Submit(); void Replay_EndFrame();
void Replay_SetPresentJob(const void* descA, const void* descB, const float* A, const float* B);
void Hud_SetEnabled(bool on); void Replay_NoteMainViewUpload(int which, const void* desc); void Replay_EnableRedirect(bool on);
void Discovery_InstDraws(uint64_t*, uint64_t*, uint64_t*, uint64_t*);
std::atomic<int> g_instLog{0};
void Camera_InstLog(int n) { g_instLog = n; }

bool VR_RenderSize(UINT* w, UINT* h);
void Temporal_Schedule(int frames);
double Perf_Now(); void Perf_AddRender(double);
float PostFx_JitterScale();
int VR_ViewCount();
void Replay_AddView(int k, const float* A, const float* B);
namespace {
const uintptr_t RVA_UPDATEVIEW = 0x74ad10;
const uintptr_t RVA_RENDERFRAME = 0x1cbcd0;
const uintptr_t RVA_CAMUPDATE = 0x68cb00;    // game camera update: copies camera+0x138 (fov) into view+0x9d4
const uintptr_t RVA_VIEWS = 0x7038130;
const uintptr_t RVA_RENDERJOB = 0x1dd4c0;     // render-worker job posted by render-frame; reads the frame payload
const uintptr_t RVA_PAYLOAD = 0x703ef78;       // current frame payload (swapped in by the main loop)
const uintptr_t RVA_PENDING = 0x703ebd0;       // pending payload from the simulation
const uintptr_t RVA_SEM_FLUSH = 0x701ce08;     // render-worker flush semaphore (count at +0)
const uintptr_t RVA_SEM_FRAME = 0x701cca8;     // main-loop frame semaphore
const uintptr_t RVA_QUEUEPUSH = 0x7b61d0;      // ring-queue push(queue, item&&); render-frame uses it to hand the payload back
const uintptr_t RVA_PAYLOAD_HOLDER = 0x703ef60; // {functor..., payload at +0x18}; moved into the return queue at frame end
const uintptr_t RVA_KICK_SITE = 0x1d3691;      // mov [rip->0x703efa8], r14 : hands the next-frame builder to the sim thread
const uintptr_t RVA_SIM_BUILDER = 0x703efa8;
const uintptr_t RVA_SEM_RELEASE = 0x1a3320;     // semaphore release(sem*)

uintptr_t g_base = 0;

using UpdateViewFn = void(__fastcall*)(void** h, const float* eye, const float* target, const float* up, float a5, uint8_t a6, uint8_t a7);
using RenderFrameFn = void(__fastcall*)(void* renderer);
UpdateViewFn oUpdateView = nullptr;
RenderFrameFn oRenderFrame = nullptr;
using JobFn = void(__fastcall*)(void* self, void* a2, void* a3, void* a4);
JobFn oRenderJob = nullptr;
std::atomic<int> g_jobLog{ 0 }, g_frameLog{ 0 }, g_jobSkipped{ 0 };
std::atomic<uint64_t> g_jobSeq{ 0 };
std::atomic<bool> g_logJobs{ true };
using QueuePushFn = void(__fastcall*)(void* queue, void* item);
QueuePushFn oQueuePush = nullptr;
thread_local bool t_holdPayload = false;     // set while rendering every eye but the last
volatile uint8_t g_skipKick = 0;              // read by the patched kick site (main thread only)
std::atomic<uint64_t> g_kickSkipped{ 0 };

// Allocate executable memory within +-2GB of the target (for a rel32 jump).
void* AllocNear(uintptr_t target, size_t size) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity;
    for (uintptr_t d = gran; d < 0x70000000; d += gran) {
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            uintptr_t a = (sgn < 0 ? target - d : target + d) & ~(gran - 1);
            void* p = VirtualAlloc((void*)a, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p) return p;
        }
    }
    return nullptr;
}

// Replace the sim kick with: if (!g_skipKick) builder = r14.  Eye 0 of a stereo frame must not
// start the next simulation step; only the last eye does.
bool InstallKickPatch() {
    uint8_t* site = (uint8_t*)(g_base + RVA_KICK_SITE);
    const uint8_t expect[3] = { 0x4C, 0x89, 0x35 };
    int32_t disp; memcpy(&disp, site + 3, 4);
    if (memcmp(site, expect, 3) != 0 || (uintptr_t)(site + 7 + disp) != g_base + RVA_SIM_BUILDER) {
        LOGE("Camera: sim kick site mismatch - stereo disabled");
        return false;
    }
    uint8_t* st = (uint8_t*)AllocNear((uintptr_t)site, 64);
    if (!st) { LOGE("Camera: no memory near the exe for the kick stub"); return false; }
    uint8_t* q = st;
    auto imm64 = [&](uint64_t v) { memcpy(q, &v, 8); q += 8; };
    *q++ = 0x48; *q++ = 0xB8; imm64((uint64_t)&g_skipKick);          // mov rax, &g_skipKick
    *q++ = 0x80; *q++ = 0x38; *q++ = 0x00;                             // cmp byte [rax], 0
    *q++ = 0x75; *q++ = 13;                                            // jne skip
    *q++ = 0x48; *q++ = 0xB8; imm64(g_base + RVA_SIM_BUILDER);         // mov rax, &builder
    *q++ = 0x4C; *q++ = 0x89; *q++ = 0x30;                             // mov [rax], r14
    *q++ = 0x48; *q++ = 0xB8; imm64((uint64_t)(site + 7));             // skip: mov rax, resume
    *q++ = 0xFF; *q++ = 0xE0;                                          // jmp rax
    FlushInstructionCache(GetCurrentProcess(), st, q - st);
    DWORD old;
    if (!VirtualProtect(site, 7, PAGE_EXECUTE_READWRITE, &old)) return false;
    int32_t rel = (int32_t)((intptr_t)st - (intptr_t)(site + 5));
    uint8_t patch[7] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90 };
    memcpy(patch + 1, &rel, 4);
    memcpy(site, patch, 7);
    VirtualProtect(site, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 7);
    LOGI("Camera: sim kick patch installed (stub %p)", (void*)st);
    return true;
}
bool g_kickPatched = false;
using SemFn = void(__fastcall*)(void*);
using MtxFn = int(__cdecl*)(void*);

// engineDraw waits until the simulation has consumed the next-frame builder (flag set by the sim at
// exe+0x7b64da). Raise it the same way for submissions that have no simulation step of their own.
void RaiseSimDoneFlag() {
    MtxFn mtxLock = (MtxFn)(g_base + 0x47142a8), mtxUnlock = (MtxFn)(g_base + 0x47142d0), cndBroadcast = (MtxFn)(g_base + 0x4714278);
    mtxLock((void*)(g_base + 0x703ed88));
    *(volatile uint8_t*)(g_base + 0x703efa0) = 1;
    void* m = *(void**)(g_base + 0x703edd8);
    mtxLock(m);
    cndBroadcast((void*)(g_base + 0x703ede8));
    mtxUnlock(m);
    mtxUnlock((void*)(g_base + 0x703ed88));
}

double SubmitAndWait() {
    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    SemFn semRelease = (SemFn)(g_base + 0x1a3320), semAcquire = (SemFn)(g_base + 0x1a3260);
    MtxFn mtxLock = (MtxFn)(g_base + 0x47142a8), mtxUnlock = (MtxFn)(g_base + 0x47142d0), cndSignal = (MtxFn)(g_base + 0x4714290);
    semRelease((void*)(g_base + 0x701cca8));
    mtxLock((void*)(g_base + 0x701cf90));
    *(volatile uint8_t*)(g_base + 0x701cf8b) = 1;          // engineDraw: work pending
    mtxUnlock((void*)(g_base + 0x701cf90));
    void* m = *(void**)(g_base + 0x701cfe0);
    mtxLock(m);
    cndSignal((void*)(g_base + 0x701cff0));
    mtxUnlock(m);
    semAcquire((void*)(g_base + 0x701cca8));               // engineDraw has taken the frame
    semAcquire((void*)(g_base + 0x701cd58));               // engineDraw finished submitting
    QueryPerformanceCounter(&t1);
    return (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
}
volatile uint8_t g_skipHandshake = 0;
volatile LONG g_workerExtra = 0;              // frame worker: serve this many extra render requests this frame

// Frame worker (exe+0x7d5dd0): after acknowledging render-frame's request (exe+0x7d73be) it jumps
// back to the top and waits for the next main-loop frame. Let it instead go back to serving
// tasks once more when we render another eye in the same frame.
volatile LONG g_skipStages = 0;               // frame worker: skip the payload stages on its next pass

// Frame worker (exe+0x7d5dd0) processes the frame payload in stages (exe+0x7d5eca onwards) once per
// main-loop pass. Those stages consume the payload, so for the second pass over the same payload
// (eye 1) jump straight to serving render-frame's tasks (exe+0x7d6ec0) instead.
bool InstallStageSkipPatch() {
    uint8_t* site = (uint8_t*)(g_base + 0x7d5eca);
    int32_t disp; memcpy(&disp, site + 3, 4);
    if (site[0] != 0x48 || site[1] != 0x8B || site[2] != 0x1D || (uintptr_t)(site + 7 + disp) != g_base + RVA_PAYLOAD) {
        LOGE("Camera: frame-worker stage site mismatch - stereo disabled"); return false;
    }
    uint8_t* st = (uint8_t*)AllocNear((uintptr_t)site, 96);
    if (!st) return false;
    uint8_t* q = st;
    auto imm64 = [&](uint64_t v) { memcpy(q, &v, 8); q += 8; };
    auto jmpAbs = [&](uint64_t a) { *q++ = 0xFF; *q++ = 0x25; *q++ = 0; *q++ = 0; *q++ = 0; *q++ = 0; imm64(a); };
    *q++ = 0x48; *q++ = 0xB8; imm64((uint64_t)&g_skipStages);     // mov rax, &skip
    *q++ = 0x83; *q++ = 0x38; *q++ = 0x00;                         // cmp dword [rax], 0
    *q++ = 0x74; *q++ = 6 + 2 + 14;                                // je NORMAL
    *q++ = 0xC7; *q++ = 0x00; *q++ = 0; *q++ = 0; *q++ = 0; *q++ = 0;   // mov dword [rax], 0
    *q++ = 0x31; *q++ = 0xFF;                                      // xor edi, edi
    jmpAbs(g_base + 0x7d6ec0);                                     // task-serving loop
    *q++ = 0x48; *q++ = 0xB8; imm64(g_base + RVA_PAYLOAD);         // NORMAL: mov rax, &payload
    *q++ = 0x48; *q++ = 0x8B; *q++ = 0x18;                         // mov rbx, [rax]
    jmpAbs(g_base + 0x7d5ed1);
    FlushInstructionCache(GetCurrentProcess(), st, q - st);
    DWORD old;
    if (!VirtualProtect(site, 7, PAGE_EXECUTE_READWRITE, &old)) return false;
    int32_t nr = (int32_t)((intptr_t)st - (intptr_t)(site + 5));
    uint8_t patch[7] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90 };
    memcpy(patch + 1, &nr, 4);
    memcpy(site, patch, 7);
    VirtualProtect(site, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 7);
    LOGI("Camera: frame-worker stage-skip patch installed (stub %p)", (void*)st);
    return true;
}

bool InstallWorkerPatch() {
    uint8_t* site = (uint8_t*)(g_base + 0x7d73be);
    int32_t rel; memcpy(&rel, site + 1, 4);
    if (site[0] != 0xE9 || (uintptr_t)(site + 5 + rel) != g_base + 0x7d5e50) { LOGE("Camera: frame-worker site mismatch - stereo disabled"); return false; }
    uint8_t* st = (uint8_t*)AllocNear((uintptr_t)site, 96);
    if (!st) return false;
    uint8_t* q = st;
    auto imm64 = [&](uint64_t v) { memcpy(q, &v, 8); q += 8; };
    auto jmpAbs = [&](uint64_t a) { *q++ = 0xFF; *q++ = 0x25; *q++ = 0; *q++ = 0; *q++ = 0; *q++ = 0; imm64(a); };
    *q++ = 0x48; *q++ = 0xB8; imm64((uint64_t)&g_workerExtra);    // mov rax, &extra
    *q++ = 0x83; *q++ = 0x38; *q++ = 0x00;                         // cmp dword [rax], 0
    *q++ = 0x74; *q++ = 3 + 2 + 14;                                // je TOP
    *q++ = 0xF0; *q++ = 0xFF; *q++ = 0x08;                         // lock dec dword [rax]
    *q++ = 0x31; *q++ = 0xFF;                                      // xor edi, edi (no request pending)
    jmpAbs(g_base + 0x7d6ec0);                                     // back to the task loop
    jmpAbs(g_base + 0x7d5e50);                                     // TOP: original target
    FlushInstructionCache(GetCurrentProcess(), st, q - st);
    DWORD old;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    int32_t nr = (int32_t)((intptr_t)st - (intptr_t)(site + 5));
    site[0] = 0xE9; memcpy(site + 1, &nr, 4);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    LOGI("Camera: frame-worker extra-request patch installed (stub %p)", (void*)st);
    return true;
}         // eye 1+: don't ask the frame worker again (it serves one request per frame)

// render-frame exe+0x1d2e80: cmp byte [frameWorkerEnabled],0 ; je fatal  -> then request/wait handshake.
// Stub: if g_skipHandshake jump past the handshake (exe+0x1d2f7d), else run the original checks.
bool InstallHandshakePatch() {
    uint8_t* site = (uint8_t*)(g_base + 0x1d2e80);
    const uint8_t expect[13] = { 0x80, 0x3D, 0x89, 0xC5, 0xE6, 0x06, 0x00, 0x0F, 0x84, 0x90, 0x21, 0x00, 0x00 };
    if (memcmp(site, expect, 13) != 0) { LOGE("Camera: frame-worker handshake site mismatch - stereo disabled"); return false; }
    uint8_t* st = (uint8_t*)AllocNear((uintptr_t)site, 128);
    if (!st) return false;
    uint8_t* q = st;
    auto imm64 = [&](uint64_t v) { memcpy(q, &v, 8); q += 8; };
    auto jmpAbs = [&](uint64_t a) { *q++ = 0xFF; *q++ = 0x25; *q++ = 0; *q++ = 0; *q++ = 0; *q++ = 0; imm64(a); };
    *q++ = 0x50;                                                       // push rax
    *q++ = 0x48; *q++ = 0xB8; imm64((uint64_t)&g_skipHandshake);       // mov rax, &flag
    *q++ = 0x80; *q++ = 0x38; *q++ = 0x00;                             // cmp byte [rax], 0
    *q++ = 0x75; *q++ = 0x2C;                                          // jne SKIP
    *q++ = 0x48; *q++ = 0xB8; imm64(g_base + 0x703f410);               // mov rax, &workerEnabled
    *q++ = 0x80; *q++ = 0x38; *q++ = 0x00;                             // cmp byte [rax], 0
    *q++ = 0x58;                                                       // pop rax
    *q++ = 0x75; *q++ = 0x0E;                                          // jne CONT
    jmpAbs(g_base + 0x1d501d);                                         // original je target
    jmpAbs(g_base + 0x1d2e8d);                                         // CONT
    *q++ = 0x58;                                                       // SKIP: pop rax
    jmpAbs(g_base + 0x1d2f7d);
    FlushInstructionCache(GetCurrentProcess(), st, q - st);
    DWORD old;
    if (!VirtualProtect(site, 13, PAGE_EXECUTE_READWRITE, &old)) return false;
    int32_t rel = (int32_t)((intptr_t)st - (intptr_t)(site + 5));
    uint8_t patch[13] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    memcpy(patch + 1, &rel, 4);
    memcpy(site, patch, 13);
    VirtualProtect(site, 13, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 13);
    LOGI("Camera: frame-worker handshake patch installed (stub %p)", (void*)st);
    return true;
}
std::atomic<uint64_t> g_payloadHeld{ 0 };

// Render-frame returns the frame payload to the simulation at its end. When rendering the first
// eye we keep it so the second eye renders the same frame.
// Callable whose every operation is a no-op: used as the "return payload to the sim" deleter for
// the eye-0 copy of the payload handed to engineDraw (the real holder goes with the last eye).
uintptr_t __fastcall NoopInvoke(void*, void*, void*, void*) { return 0; }
void __fastcall NoopMove(void* dst, void* src) { if (dst && src) *(uint64_t*)dst = *(uint64_t*)src; }
void __fastcall NoopDestroy(void*, void*) {}
void* g_noopVtbl[8] = { (void*)NoopInvoke, (void*)NoopMove, (void*)NoopMove, (void*)NoopDestroy,
                        (void*)NoopDestroy, (void*)NoopDestroy, (void*)NoopDestroy, (void*)NoopDestroy };

// Eye 0 hands engineDraw the real payload holder (its deleter returns the payload to the sim once
// engineDraw is done), then we put the payload pointer back so eye 1 can render the same frame.
// Eye 1 hands engineDraw a copy whose deleter does nothing, so the payload is returned only once.
thread_local int t_pushMode = 0;
alignas(16) uint64_t g_parkedHolder[4];
volatile bool g_haveParked = false;
void* volatile g_parkReturnOf = nullptr;
std::atomic<uint64_t> g_fakeDropped{ 0 };   // 0 normal, 1 eye 0 (real + keep), 2 later eyes (fake)
void __fastcall hkQueuePush(void* queue, void* item) {
    Trace_Event("queuePush", (uintptr_t)queue - g_base, (uintptr_t)item - g_base);
    // engineDraw returns each holder it finished with to the simulation's free list
    // (exe+0x1aca13). Our eye-1 copy must not be returned: the real one already was.
    if ((uintptr_t)queue == g_base + 0x703eb78) {
        uint64_t* it = (uint64_t*)item;
        if (*(void**)item == (void*)g_noopVtbl) {
            // our copy: never returned itself; if the real holder was parked, return that now
            it[3] = 0;
            ++g_fakeDropped;
            if (g_haveParked) { g_haveParked = false; oQueuePush(queue, g_parkedHolder); }
            return;
        }
        if (g_parkReturnOf && it[3] == (uint64_t)g_parkReturnOf) {
            // eye 0 pass: engineDraw is done with the real holder, but eye 1 still needs the payload
            memcpy(g_parkedHolder, it, 32);
            it[3] = 0;
            g_haveParked = true;
            g_parkReturnOf = nullptr;
            return;
        }
    }
    if (t_pushMode && (uintptr_t)item == g_base + RVA_PAYLOAD_HOLDER) {
        uint64_t* real = (uint64_t*)item;
        if (t_pushMode == 1) {
            uint64_t payload = real[3];
            oQueuePush(queue, item);
            real[3] = payload;
        } else {
            ++g_payloadHeld;
            alignas(16) uint64_t fake[4];
            fake[0] = (uint64_t)g_noopVtbl; fake[1] = 0; fake[2] = real[2]; fake[3] = real[3];
            oQueuePush(queue, fake);
            if (t_pushMode == 2) real[3] = 0;     // as after a normal push: the main loop must not release it again
        }
        return;
    }
    oQueuePush(queue, item);
}
inline void* Payload() { return *(void**)(g_base + RVA_PAYLOAD); }
inline void* Pending() { return *(void**)(g_base + RVA_PENDING); }
inline int SemCount(uintptr_t rva) { return *(volatile int*)(g_base + rva); }

volatile LONG g_skipEyeJob = 0;
void __fastcall hkRenderJob(void* self, void* a2, void* a3, void* a4) {
    Trace_Event("renderJob");
    uint64_t n = ++g_jobSeq;
    void* pl = Payload();
    if (g_logJobs && g_jobLog < 80) { ++g_jobLog; LOGD("RenderJob #%llu begin: payload %p pending %p", n, pl, Pending()); }
    if (g_skipEyeJob && InterlockedExchange(&g_skipEyeJob, 0)) {
        if (g_logJobs && g_jobLog < 80) { ++g_jobLog; LOGD("RenderJob #%llu skipped (eye 1, replayed from eye 0)", n); }
        return;
    }
    if (!pl) {
        if (g_jobSkipped++ < 20) LOGE("RenderJob #%llu: frame payload is null - skipping job to avoid a crash", n);
        return;
    }
    oRenderJob(self, a2, a3, a4);
    if (g_logJobs && g_jobLog < 80) { ++g_jobLog; LOGD("RenderJob #%llu end", n); }
}

struct V3 { float x, y, z; };
inline V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline V3 operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline float Len(V3 a) { return sqrtf(Dot(a, a)); }
inline V3 Norm(V3 a) { float l = Len(a); return l > 1e-12f ? a * (1.f / l) : V3{ 0, 0, 0 }; }
inline V3 Rot(const Quat& q, V3 v) {
    V3 u{ q.x, q.y, q.z };
    V3 t = Cross(u, v) * 2.f;
    return v + t * q.w + Cross(u, t);
}

// Latest game camera inputs for the main view (before our modification), for re-rendering per eye.
CameraInputs g_last;
std::atomic<uint64_t> g_mainUpdates{ 0 }, g_renderFrames{ 0 };
uint64_t g_lastMainUpdateAt = 0;   // render-frame count when the game last updated the main camera
thread_local int t_renderDepth = 0;
std::atomic<int> g_presentsInRender{ 0 }, g_presentsOutside{ 0 };

bool IsMainView(void** h) {
    if (!h || !*h) return false;
    uintptr_t arr = *(uintptr_t*)(g_base + RVA_VIEWS);
    return arr && (uintptr_t)*h == arr;
}

// Build modified eye/target/up for a pose given in the game camera's local frame (OpenXR axes).
void ApplyPose(const float* eye, const float* target, const float* up, const Pose& p, float* outEye, float* outTarget, float* outUp) {
    V3 E{ eye[0], eye[1], eye[2] }, T{ target[0], target[1], target[2] }, U{ up[0], up[1], up[2] };
    V3 fd = T - E;
    float dist = Len(fd);
    V3 f = Norm(fd);
    if (g_cfg.decoupledPitch) {
        // Decoupled pitch: keep the game camera's position (it still rises and falls as the right stick orbits it)
        // and its heading, but drop its pitch and roll, so the horizon stays level and you look up / down with
        // your head only. The game's world is Y-up.
        static V3 lastFlat{ 0, 0, -1 };
        V3 W{ 0, 1, 0 };
        V3 fh = f - W * (f.x * W.x + f.y * W.y + f.z * W.z);
        if (Len(fh) > 1e-3f) lastFlat = Norm(fh);   // looking straight up / down: keep the last heading
        f = lastFlat; U = W;
    }
    V3 r = Norm(Cross(f, U));
    V3 u = Cross(r, f);
    float s = g_cfg.worldScale;
    V3 lp{ p.pos.x * s, p.pos.y * s, p.pos.z * s };
    // camera-local axes in world: +X -> r, +Y -> u, +Z (back) -> -f
    auto toWorld = [&](V3 v) { return r * v.x + u * v.y - f * v.z; };
    V3 ne = E + toWorld(lp);
    V3 nf = toWorld(Rot(p.ori, V3{ 0, 0, -1 }));
    V3 nu = toWorld(Rot(p.ori, V3{ 0, 1, 0 }));
    V3 nt = ne + nf * (dist > 1e-3f ? dist : 1.f);
    outEye[0] = ne.x; outEye[1] = ne.y; outEye[2] = ne.z; outEye[3] = eye[3];
    outTarget[0] = nt.x; outTarget[1] = nt.y; outTarget[2] = nt.z; outTarget[3] = target[3];
    outUp[0] = nu.x; outUp[1] = nu.y; outUp[2] = nu.z; outUp[3] = up[3];
}

volatile int g_viewLog = 0;
void ViewDumpStart() { g_viewLog = 60; LOGI("viewdump: main view element %p", (void*)*(uintptr_t*)(g_base + RVA_VIEWS)); }
void __fastcall hkUpdateView(void** h, const float* eye, const float* target, const float* up, float a5, uint8_t a6, uint8_t a7) {
    if (!IsMainView(h)) {
        if (g_viewLog > 0) {   // diagnostics (trigger "viewdump"): other views updated, and whether they share the main camera
            --g_viewLog;
            uintptr_t arr = *(uintptr_t*)(g_base + RVA_VIEWS);
            bool same = g_last.valid && fabsf(eye[0] - g_last.eye[0]) < 0.01f && fabsf(eye[1] - g_last.eye[1]) < 0.01f && fabsf(eye[2] - g_last.eye[2]) < 0.01f;
            LOGI("viewdump: view %p (index %lld) eye (%.2f %.2f %.2f) target (%.2f %.2f %.2f) up (%.2f %.2f %.2f) a5 %.3f a6 %d a7 %d%s",
                 h ? *h : nullptr, h && *h && arr ? ((long long)((uintptr_t)*h - arr)) / 0x9f0 : -1, eye[0], eye[1], eye[2], target[0], target[1], target[2],
                 up[0], up[1], up[2], a5, a6, a7, same ? "  << same eye as main" : "");
        }
        oUpdateView(h, eye, target, up, a5, a6, a7); return;
    }
    ++g_mainUpdates;
    g_lastMainUpdateAt = g_renderFrames.load();
    memcpy(g_last.eye, eye, 16); memcpy(g_last.target, target, 16); memcpy(g_last.up, up, 16);
    g_last.a5 = a5; g_last.a6 = a6; g_last.handle = h; g_last.valid = true;
    Pose head;
    if (!g_cfg.headTracking || !VR_GetHeadPose(&head)) { oUpdateView(h, eye, target, up, a5, a6, a7); return; }
    alignas(16) float e[4], t[4], u[4];
    ApplyPose(eye, target, up, head, e, t, u);
    oUpdateView(h, e, t, u, a5, a6, a7);
    static int logged = 0;
    if (logged < 3 || (g_mainUpdates % 600) == 0) {
        ++logged;
        LOGD("UpdateView(main): eye (%.3f %.3f %.3f) tgt (%.3f %.3f %.3f) -> eye (%.3f %.3f %.3f) tgt (%.3f %.3f %.3f) head pos (%.3f %.3f %.3f) a5 %.3f a6 %u a7 %u",
             eye[0], eye[1], eye[2], target[0], target[1], target[2], e[0], e[1], e[2], t[0], t[1], t[2],
             head.pos.x, head.pos.y, head.pos.z, a5, a6, a7);
    }
}

// ---- native stereo: render the frame once per eye ----
uint8_t g_prevSave[4][0x150];   // per-view "previous frame" view slots (+0x150..+0x29f): eye 0/1, eye 0/1 inner
bool g_havePrev[4] = { false, false, false, false };
std::atomic<uint64_t> g_stereoFrames{ 0 };

void FixProjection(uintptr_t view, const EyeSetup& es) {
    float* P = (float*)(view + 0x40);      // memory row-major, row-vector form: row2 = [ox+jx, oy+jy, a, -1]
    float jx = P[8], jy = P[9];             // the game's TAA jitter (projection is symmetric)
    { float js = PostFx_JitterScale(); jx *= js; jy *= js; }   // AA mode: no jitter without TAA, less for mild TAA
    float xs = 2.f / (es.tanR - es.tanL), ys = 2.f / (es.tanU - es.tanD);
    float ox = (es.tanR + es.tanL) / (es.tanR - es.tanL), oy = (es.tanU + es.tanD) / (es.tanU - es.tanD);
    P[0] = xs; P[5] = ys; P[8] = ox + jx; P[9] = oy + jy;
    float* Pn = (float*)(view + 0x100);    // projection without jitter
    Pn[0] = xs; Pn[5] = ys; Pn[8] = ox; Pn[9] = oy;
    float* J = (float*)(view + 0x140);
    J[0] = P[8]; J[1] = P[9];
    const float* M = (const float*)view;   // +0x80 = (+0x00) * P, as the game computes it
    float* R = (float*)(view + 0x80);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            R[i * 4 + j] = M[i * 4 + 0] * P[0 * 4 + j] + M[i * 4 + 1] * P[1 * 4 + j] + M[i * 4 + 2] * P[2 * 4 + j] + M[i * 4 + 3] * P[3 * 4 + j];
}

void SetupEye(int e, const EyeSetup& es) {
    if ((g_cfg.debugStereo & 0xF) == 1) return;
    alignas(16) float ee[4], tt[4], uu[4];
    ApplyPose(g_last.eye, g_last.target, g_last.up, es.pose, ee, tt, uu);
    uintptr_t view = (uintptr_t)*g_last.handle;
    // The view's aspect/vertical FOV (+0x9d0/+0x9d4) feed UpdateView's culling volume (exe+0x2166930 on
    // elem+0x948). Set them before UpdateView, widened by a margin so the culling volume of the eye we
    // render with also covers the other eye (offset by the IPD); the projection is set exactly afterwards.
    float tx = (std::max)(es.tanR, -es.tanL), ty = (std::max)(es.tanU, -es.tanD);
    if ((g_cfg.debugStereo & 0xF) != 2) {
        // Culling margin: the game decides what to draw with the head pose of its camera update, a frame or two before
        // the eyes are rendered; on fast head turns the newest view runs past that cone. edge_clipping widens it.
        float md = (std::max)(g_cfg.cullMarginDeg, g_cfg.edgeClipping == "experimental" ? 25.f : g_cfg.edgeClipping == "default" ? 15.f : 0.f);
        float m = md * 0.0174533f;
        float cx = tanf((std::min)(atanf(tx) + m, 1.48f)), cy = tanf((std::min)(atanf(ty) + m, 1.48f));
        *(float*)(view + 0x9d4) = 2.f * atanf(cy);
        *(float*)(view + 0x9d0) = cx / cy;
    }
    oUpdateView(g_last.handle, ee, tt, uu, g_last.a5, 0, 0);
    if ((g_cfg.debugStereo & 0xF) == 2) return;
    FixProjection(view, es);
    if (g_havePrev[e] && g_cfg.debugStereo != 3) memcpy((void*)(view + 0x150), g_prevSave[e], 0x150);
}


// ---- single render-frame stereo (flag 8192): eye 0 is rendered by the game as usual; eye 1's per-view
// constant blocks are computed here (port of render-frame exe+0x1cdb86..0x1ce436) and the frame's
// command lists are replayed at Present with them (replay.cpp).
static inline void Transp(float* dst, const float* m) { for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) dst[i * 4 + j] = m[j * 4 + i]; }
static inline void Mul(float* r, const float* a, const float* b) {
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j)
        r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] + a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
}
// elem: view element (cur matrices +0x00 V, +0x40 P, +0x80 VP, +0xc0 X, +0x100 P (no jitter), +0x140 jitter;
// previous-frame copies of those at +0x150..). Output: block A (0x220 bytes, elem+0x4c0) and B (elem+0x6e0).
static void BuildViewBlocks(uintptr_t e, float* A, float* B) {
    const float* F = (const float*)e;
    auto M = [&](int off) { return (const float*)(e + off); };
    float t[16];
    Transp(A + 0x00 / 4, M(0x00)); Transp(A + 0x40 / 4, M(0x40)); Transp(A + 0x80 / 4, M(0x80)); Transp(A + 0xc0 / 4, M(0xc0));
    Transp(A + 0x100 / 4, M(0x150)); Transp(A + 0x140 / 4, M(0x190)); Transp(A + 0x180 / 4, M(0x1d0)); Transp(A + 0x1c0 / 4, M(0x210));
    A[0x200 / 4 + 0] = F[0x140 / 4]; A[0x200 / 4 + 1] = F[0x144 / 4]; A[0x200 / 4 + 2] = F[0x290 / 4]; A[0x200 / 4 + 3] = F[0x294 / 4];
    uint32_t g = *(volatile uint32_t*)(g_base + 0x703e910);
    memcpy(&A[0x210 / 4], &g, 4); A[0x214 / 4] = 0; A[0x218 / 4] = 0; A[0x21c / 4] = 0;
    memcpy(B + 0x00 / 4, A + 0x00 / 4, 64);
    Transp(B + 0x40 / 4, M(0x100));
    Mul(t, M(0x00), M(0x100)); Transp(B + 0x80 / 4, t);
    memcpy(B + 0xc0 / 4, A + 0xc0 / 4, 64);
    memcpy(B + 0x100 / 4, A + 0x100 / 4, 64);
    Transp(B + 0x140 / 4, M(0x250));
    Mul(t, M(0x150), M(0x250)); Transp(B + 0x180 / 4, t);
    memcpy(B + 0x1c0 / 4, A + 0x1c0 / 4, 64);
    memcpy(B + 0x200 / 4, A + 0x200 / 4, 32);
}
alignas(64) uint8_t g_scr0[0xa00], g_scr1[0xa00];
alignas(16) float g_blkA[0x220 / 4], g_blkB[0x220 / 4], g_chkA[0x220 / 4], g_chkB[0x220 / 4];
int g_spLog = 0;
// The game's aspect/vertical FOV for the main view (+0x9d0/+0x9d4). We widen them for CPU culling while
// stereo runs. They must NOT be restored right after render-frame: its jobs keep running afterwards and
// would see the values change mid-frame (inconsistent culling/shadow setup -> crashes). Restore them
// only when stereo stops.
float g_gameAspect = 0, g_gameFov = 0, g_ourAspect = 0, g_ourFov = 0; bool g_fovOverridden = false; uintptr_t g_fovView = 0;
static void TrackGameFov(uintptr_t view) {
    float a = *(float*)(view + 0x9d0), f = *(float*)(view + 0x9d4);
    if (!g_fovOverridden || view != g_fovView || a != g_ourAspect || f != g_ourFov) {
        static int lg = 0;
        if (lg < 6 && (a != g_gameAspect || f != g_gameFov)) { ++lg; LOGI("Game view FOV: aspect %.4f, vertical fov %.4f rad (%.1f deg)", a, f, f * 57.2958f); }
        g_gameAspect = a; g_gameFov = f;
    }
    g_fovOverridden = true; g_fovView = view;
}
// The game camera's update (exe+0x68cb00) writes the camera's own vertical FOV into the view (+0x9d4) every
// game update. Objects culled during the update (props, NPCs, parts of buildings) then use the game's
// narrow FOV with our head-turned view - a cone narrower than the headset's that turns with the head, so
// things pop in and out near the edges of the view. While stereo runs, put our (wider) FOV straight back.
using CamUpdateFn = void(__fastcall*)(uintptr_t);
CamUpdateFn oCamUpdate = nullptr;
volatile LONG g_camFovFixes = 0;

// The camera object's own vertical FOV (cam+0x138, 50 deg) is used by a second visibility test besides the view
// fields fixed below: with the head turned, buildings / props near the edge of the headset view (e.g. a chimney 60 deg
// to the side) were not drawn even with fix_cull_fov. While stereo runs, the camera gets our FOV (incl. cull margin)
// before its update; the game's value is restored when stereo stops.
// edge_clipping widens it further (user testing: textures at the view edges dropping out while turning the head get
// much rarer at 165 deg, rarer still at 200). Trigger "camfov=<deg>": 0 = automatic (this), > 0 = that FOV,
// < 0 = leave the camera alone (comparison).
float g_camFovOverride = 0.f;
uintptr_t g_fovCam = 0; float g_camGameFov = 0.f, g_camWritten = 0.f; bool g_camFovSet = false;
uintptr_t g_lastCam = 0;
// Experiment (trigger "camhead=1"): after the main camera's update, also turn the camera object's own pose
// (eye +0x100, target +0x110, up +0x120) with the head, so systems that read the camera object see where the
// player looks; the game's values are put back before its next update.
int g_camHead = 0; uintptr_t g_mainCam = 0; bool g_camPoseSwapped = false; float g_camPoseSave[12];
static int g_camHeadLog = 0;
void SetHeadFollow(int v) { g_camHead = v; g_camHeadLog = 6; LOGI("camera object follows head -> %d", v); }
void __fastcall hkCamUpdate(uintptr_t cam) {
    g_lastCam = cam;
    if (cam && cam == g_mainCam && g_camPoseSwapped) { memcpy((void*)(cam + 0x100), g_camPoseSave, 48); g_camPoseSwapped = false; }
    if (cam && g_fovOverridden && g_cfg.fixCullFov && g_camFovOverride >= 0.f && g_ourFov > 0.f) {
        float* pf = (float*)(cam + 0x138);
        // edge_clipping: the camera's field of view for these checks (vertical; the camera is 16:9, so much wider
        // horizontally). Values over 180 deg are outside a camera's normal range ("experimental"): the user found
        // 200 best in testing, 165 nearly as good.
        float edge = g_cfg.edgeClipping == "experimental" ? 200.f * 0.0174533f : g_cfg.edgeClipping == "default" ? 165.f * 0.0174533f : 0.f;
        float want = g_camFovOverride > 0.f ? g_camFovOverride : (std::max)(g_ourFov, edge);
        if (*pf != want) {
            static int lg = 0;
            if (lg < 3) { ++lg; LOGI("Camera: camera FOV %.1f -> %.1f deg (culling)", *pf * 57.2958f, want * 57.2958f); }
            if (!g_camFovSet || cam != g_fovCam) { g_camGameFov = *pf; g_fovCam = cam; g_camFovSet = true; }
            else if (*pf != g_camWritten && *pf > 0.1f && *pf < 3.0f && *pf != g_camGameFov) g_camGameFov = *pf;   // the game changed it (zoom etc.)
            *pf = want; g_camWritten = want;
        }
    }
    oCamUpdate(cam);
    if (cam && g_fovView && *(volatile uintptr_t*)(cam + 0x60) == g_fovView) {
        if (g_mainCam != cam) { g_mainCam = cam; LOGI("Camera: main camera object %p", (void*)cam); }
        Pose head;
        if (g_camHead && g_fovOverridden && g_cfg.headTracking && VR_GetHeadPose(&head)) {
            float* c = (float*)(cam + 0x100);
            memcpy(g_camPoseSave, c, 48);
            alignas(16) float e[4], t[4], u[4];
            ApplyPose(c, c + 4, c + 8, head, e, t, u);
            if (g_camHeadLog > 0) { --g_camHeadLog; LOGI("camhead: eye (%.2f %.2f %.2f) target (%.2f %.2f %.2f) -> target (%.2f %.2f %.2f)", c[0], c[1], c[2], c[4], c[5], c[6], t[0], t[1], t[2]); }
            memcpy(c, e, 12); memcpy(c + 4, t, 12); memcpy(c + 8, u, 12);
            g_camPoseSwapped = true;
        }
    }
    if (!g_fovOverridden || !g_cfg.fixCullFov || !cam) return;
    uintptr_t view = *(volatile uintptr_t*)(cam + 0x60);
    if (!view || view != g_fovView) return;
    float a = *(float*)(view + 0x9d0), f = *(float*)(view + 0x9d4);
    if (a == g_ourAspect && f == g_ourFov) return;
    g_gameAspect = a; g_gameFov = f;   // the game's own values, for restoring when stereo stops
    *(float*)(view + 0x9d0) = g_ourAspect; *(float*)(view + 0x9d4) = g_ourFov;
    InterlockedIncrement(&g_camFovFixes);
}
static void NoteOurFov(uintptr_t view) { g_ourAspect = *(float*)(view + 0x9d0); g_ourFov = *(float*)(view + 0x9d4); }
static void RestoreGameFov() {
    if (!g_fovOverridden) return;
    g_fovOverridden = false;
    if (g_camFovSet && g_fovCam) { *(float*)(g_fovCam + 0x138) = g_camGameFov; g_camFovSet = false; }
    if (g_fovView && *(float*)(g_fovView + 0x9d0) == g_ourAspect && *(float*)(g_fovView + 0x9d4) == g_ourFov) {
        *(float*)(g_fovView + 0x9d0) = g_gameAspect; *(float*)(g_fovView + 0x9d4) = g_gameFov;
    }
}
// Camera blocks of a replayed view (slot 1 = eye 1, 2/3 = eye 0/1 inner) on a scratch copy of eye 0's view
// element (g_scr0, taken before render-frame): pose and projection of that view, its own previous-frame slots.
static void BuildScratchBlocks(int slot, const EyeSetup& es, float* A, float* B) {
    memcpy(g_scr1, g_scr0, 0x9f0);
    uintptr_t s = (uintptr_t)g_scr1; void* sp = g_scr1;
    alignas(16) float ee[4], tt[4], uu[4];
    ApplyPose(g_last.eye, g_last.target, g_last.up, es.pose, ee, tt, uu);
    oUpdateView((void**)&sp, ee, tt, uu, g_last.a5, 0, 0);
    float tx = (std::max)(es.tanR, -es.tanL), ty = (std::max)(es.tanU, -es.tanD);
    *(float*)(s + 0x9d4) = 2.f * atanf(ty);
    *(float*)(s + 0x9d0) = tx / ty;
    FixProjection(s, es);
    if (g_havePrev[slot]) memcpy((void*)(s + 0x150), g_prevSave[slot], 0x150);
    else memcpy((void*)(s + 0x150), (void*)s, 0x148);
    BuildViewBlocks(s, A, B);
    memcpy(g_prevSave[slot], (void*)s, 0x148); g_havePrev[slot] = true;
}
static void SinglePassStereo(void* renderer, EyeSetup* eyes, uintptr_t view, int nViews) {
    Hud_SetEnabled(g_cfg.hudLayer);
    g_spActive = true; Replay_EnableRedirect(true);
    TrackGameFov(view);
    { static bool sched = false; if (!sched && g_cfg.perEyeHistory) { sched = true; Temporal_Schedule(120); } }
    if (g_cfg.forceW > 0 && (g_cfg.debugStereo & 65536)) {
        // experiment 2: change the game's resolution settings table (exe+0x6b83460, 6 x 0x4c) and flag a re-apply
        static int applied = 0;
        if (applied < 1) {
            ++applied;
            uint8_t* t = (uint8_t*)(g_base + 0x6b83460);
            for (int i = 0; i < 6; ++i) {
                uint32_t* e = (uint32_t*)(t + i * 0x4c);
                LOGI("ResTable[%d]: %u %u %u %u | %u %u %u %u | %u %u %u %u %u", i, e[3], e[4], e[5], e[6], e[9], e[10], e[11], e[12], e[13], e[14], e[15], e[16], e[8]);
                e[9] = g_cfg.forceW; e[10] = g_cfg.forceH;
                if (g_cfg.debugStereo & 32768) { e[11] = g_cfg.forceW; e[12] = g_cfg.forceH; }
            }
            LOGI("ResTable: idx %u apply flag %u -> 1", *(uint32_t*)(g_base + 0x7035980), *(uint8_t*)(g_base + 0x7035984));
            *(volatile uint8_t*)(g_base + 0x7035984) = 1;
        }
    }
    if (g_cfg.forceW > 0 && (g_cfg.debugStereo & 16384)) {
        // experiment: the game's render-size globals (exe+0x6b832d0.. per frame from +0x6b832e0)
        volatile uint32_t* r = (volatile uint32_t*)(g_base + 0x6b832d0);
        r[0] = g_cfg.forceW; r[1] = g_cfg.forceH; r[2] = g_cfg.forceW; r[3] = g_cfg.forceH;
        r[4] = g_cfg.forceW; r[5] = g_cfg.forceH;
        if (g_cfg.debugStereo & 32768) { r[6] = g_cfg.forceW; r[7] = g_cfg.forceH; }
    }
    Watchdog_Beat();
    SetupEye(0, eyes[0]);
    NoteOurFov(view);
    memcpy(g_scr0, (void*)view, 0x9f0);
    ++t_renderDepth;
    oRenderFrame(renderer);
    --t_renderDepth;
    memcpy(g_prevSave[0], (void*)(view + 0x150), 0x150); g_havePrev[0] = true;
    // eye 1 (and, with quad views, the inner views) on a scratch copy of the view element
    BuildScratchBlocks(1, eyes[1], g_blkA, g_blkB);
    Replay_SetPresentJob((void*)(view + 0x4c0), (void*)(view + 0x6e0), g_blkA, g_blkB);
    if (nViews == 4) {
        for (int k = 2; k < 4; ++k) { BuildScratchBlocks(k, eyes[k], g_blkA, g_blkB); Replay_AddView(k - 1, g_blkA, g_blkB); }
    }
    if (g_spLog < 6) {
        // validation: the port applied to eye 0's inputs must reproduce the game's own blocks
        ++g_spLog;
        BuildViewBlocks((uintptr_t)g_scr0, g_chkA, g_chkB);
        float da = 0, db = 0; int ia = -1, ib = -1;
        const float* GA = (const float*)(view + 0x4c0); const float* GB = (const float*)(view + 0x6e0);
        for (int i = 0; i < 0x220 / 4; ++i) {
            float x = fabsf(GA[i] - g_chkA[i]); if (x > da) { da = x; ia = i; }
            float y = fabsf(GB[i] - g_chkB[i]); if (y > db) { db = y; ib = i; }
        }
        LOGD("SinglePass: block port check: A max diff %g at +0x%x, B max diff %g at +0x%x", da, ia * 4, db, ib * 4);
    }
    if ((++g_stereoFrames % 600) == 1) LOGI("Stereo: single-pass frame %llu (eye 1 replayed at Present)", g_stereoFrames.load());
}

bool g_flushArrived = false;
volatile bool g_eye0Present = false;

// Render resolution: the game reloads its render/output size every frame (0x6bbe70, from the main loop)
// from the current entry of its resolution preset table (exe+0x6b83460, 6 x 0x4c, index at exe+0x7035980;
// +0x24/+0x28 render W/H, +0x2c/+0x30 output W/H). Keeping that entry at our size makes the game resize
// its own swap chain and every render target (the ResizeBuffers hook keeps the swap chain at the size too).
static void ApplyRenderSize() {
    UINT w, h;
    if (!VR_RenderSize(&w, &h)) return;
    uint32_t idx = *(volatile uint32_t*)(g_base + 0x7035980);
    if (idx >= 6) return;
    uint32_t* e = (uint32_t*)(g_base + 0x6b83460 + idx * 0x4c);
    if (e[9] == w && e[10] == h && e[11] == w && e[12] == h) return;
    static int lg = 0;
    if (lg < 6) { ++lg; LOGI("Render size: preset %u %ux%u (output %ux%u) -> %ux%u", idx, e[9], e[10], e[11], e[12], w, h); }
    e[9] = w; e[10] = h; e[11] = w; e[12] = h;
}

void __fastcall hkRenderFrame(void* renderer) {
    struct TR { TR() { Trace_Event("renderFrame>"); } ~TR() { Trace_Event("renderFrame<"); } } tr_;
    struct PT { double t = Perf_Now(); ~PT() { Perf_AddRender(Perf_Now() - t); } } pt_;
    ++g_renderFrames;
    ApplyRenderSize();
    EyeSetup eyes[4];
    bool cameraLive = g_renderFrames.load() - g_lastMainUpdateAt <= 2;   // menus/loading don't update it
    if (g_kickPatched && g_last.valid && cameraLive && Payload() && IsMainView(g_last.handle) && VR_StereoWanted() && VR_StereoBegin(eyes)) {
        uintptr_t view = (uintptr_t)*g_last.handle;
        if (g_cfg.debugStereo & 8192) { SinglePassStereo(renderer, eyes, view, VR_ViewCount()); return; }
        float savedAspect = *(float*)(view + 0x9d0), savedFov = *(float*)(view + 0x9d4);
        Watchdog_Beat();
        bool lg = g_frameLog < 8;
        if (lg) { if (g_frameLog == 0) { g_jobLog = 40; Discovery_LogExecs(120); } ++g_frameLog; }
        g_logJobs = lg;
        // render-frame advances a 3-slot per-frame buffer index (exe+0x71ae324, previous at
        // +0x71ae310). Both eyes must use the same slot, so rewind it before the second eye.
        int ringCur = *(volatile int*)(g_base + 0x71ae324), ringPrev = *(volatile int*)(g_base + 0x71ae310);
        // render-frame clears each view's "valid/changed" flags (+0x9dd.., exe+0x1cdb44) after
        // uploading its GPU block; without them eye 1 would reuse eye 0's (stale) block descriptors.
        uint8_t viewFlags[64][8]; int nViews = 0;
        {
            uintptr_t arr = *(uintptr_t*)(g_base + RVA_VIEWS);
            int cnt = *(int*)(g_base + 0x7038140);
            nViews = (std::min)(cnt, 64);
            for (int i = 0; i < nViews; ++i) memcpy(viewFlags[i], (void*)(arr + i * 0x9f0 + 0x9d8), 8);
        }
        if (g_cfg.debugStereo & 128) InterlockedExchange(&g_workerExtra, 1);   // frame worker serves eye 1's request too
        // engineDraw (exe+0x1a97d0) flips its own double-buffer index for the per-object transform
        // lists (exe+0x718c908 / 0x718c8f8) every run; the producer flips its index (exe+0x7396274)
        // once per frame. Two engineDraw runs per frame desynchronise them.
        const bool replay = (g_cfg.debugStereo & 1024) != 0;
        if (g_instLog == 1 && g_upN[0] && g_upN[1]) LogUploadDiffs();
        g_upN[0] = g_upN[1] = 0;
        Replay_EndFrame();
        Replay_ClearUploads();
        int edA = *(volatile int*)(g_base + 0x718c908), edB = *(volatile int*)(g_base + 0x718c8f8);
        if (lg) LOGD("Stereo lists: engineDraw A %d B %d producer %d (A %d B %d)", edA, edB, *(volatile int*)(g_base + 0x7396274),
                     *(volatile int*)(g_base + 0x7ba4d24), *(volatile int*)(g_base + 0x7ba4d20));
        for (int e = 0; e < 2; ++e) {
            if (e > 0 && (g_cfg.debugStereo & 512)) {
                *(volatile int*)(g_base + 0x718c908) = edA;
                *(volatile int*)(g_base + 0x718c8f8) = edB;
            }
            if (e == 0 && g_instLog > 0) --g_instLog;
            if (e > 0 && g_cfg.debugStereo != 6) {
                *(volatile int*)(g_base + 0x71ae324) = ringCur;
                *(volatile int*)(g_base + 0x71ae310) = ringPrev;
            }
            if (e > 0 && !(g_cfg.debugStereo & 256)) {
                uintptr_t arr = *(uintptr_t*)(g_base + RVA_VIEWS);
                for (int i = 0; i < nViews; ++i) memcpy((void*)(arr + i * 0x9f0 + 0x9d8), viewFlags[i], 8);
            }
            MapLog_Marker('B', e);
            if (lg || g_instLog > 0) InstStats(e ? "before eye1" : "before eye0");
            SetupEye(e, eyes[e]);
            if (lg) LOGD("Stereo eye %d begin: payload %p pending %p semFlush %d semFrame %d jobs %llu execs %llu", e, Payload(), Pending(),
                         SemCount(RVA_SEM_FLUSH), SemCount(RVA_SEM_FRAME), g_jobSeq.load(), Discovery_ExecCount());
            if (e > 0 && !(g_cfg.debugStereo & 64)) {
                // Map the per-frame upload/constant rings again (eye 0's were unmapped and handed to
                // the GPU by its submission), exactly as the main loop does before render-frame
                // (exe+0x1afe5e / 0x1afe63).
                // New per-frame upload token (exe+0x703ffa0, a random value the main loop sets before
                // render-frame): worker threads keep their ring chunks only while it is unchanged.
                *(volatile uint64_t*)(g_base + 0x703ffa0) = (*(volatile uint64_t*)(g_base + 0x703ffa0) * 2654435761ull + 0x9E3779B9ull) & 0xffffffffull;
                ((void(*)())(g_base + 0x7dc600))();
                ((void(*)())(g_base + 0x7dc820))();
            }
            if (e > 0 && !g_flushArrived) {
                // Render-frame takes one token from this semaphore per frame (handed back by the
                // post-frame job once the previous frame is retired); give eye 1 its own.
                ((SemFn)(g_base + RVA_SEM_RELEASE))((void*)(g_base + RVA_SEM_FLUSH));
            }
            ++t_renderDepth;
            g_recEye = replay ? e : -1;
            if (e == 0) InterlockedExchange(&g_skipEyeJob, 0);
            if (replay && e == 1 && (g_cfg.debugStereo & 4096)) InterlockedExchange(&g_skipEyeJob, 1);
            t_holdPayload = false;
            t_pushMode = (e == 0) ? 1 : 2;
            g_skipKick = (e > 0);          // the simulation step is started once, by eye 0 (engineDraw needs its output)
            g_skipHandshake = (e > 0) && !(g_cfg.debugStereo & 128);
            oRenderFrame(renderer);
            MapLog_Marker('R', e);
            if (replay && e == 1) Replay_BeginEye1Submit();
            if (e == 1 && (g_cfg.debugStereo & 2048)) SyncListParity();
            if (lg || g_instLog > 0) InstStats(e ? "eye1 render" : "eye0 render");
            g_skipKick = 0;
            g_skipHandshake = 0;
            t_pushMode = 0;
            --t_renderDepth;
            g_recEye = -1;
            if (lg) LOGD("Stereo eye %d end: payload %p pending %p semFlush %d semFrame %d jobs %llu execs %llu", e, Payload(), Pending(),
                         SemCount(RVA_SEM_FLUSH), SemCount(RVA_SEM_FRAME), g_jobSeq.load(), Discovery_ExecCount());
            memcpy(g_prevSave[e], (void*)(view + 0x150), 0x150);
            g_havePrev[e] = true;
            if (e == 0) {
                // Submit eye 0 now: hand its command lists to the engineDraw thread and wait for it,
                // exactly as the main loop does after render-frame (exe+0x1afe9c..0x1aff23, 0x1b2b15).
                if (replay) Replay_BeginEye0Submit();
                if (g_cfg.debugStereo & 2048) SyncListParity();
                double ms = SubmitAndWait();
                if (replay) Replay_EndEye0Submit();
                if (g_cfg.debugStereo == 5) VR_GpuSync();
                if (lg) LOGD("Stereo eye 0 submitted in %.2f ms", ms);
                if (lg || g_instLog > 0) InstStats("eye0 submitted");
                // Present eye 0 through the game's own present path (exe+0x7e25e0, as the main loop
                // does at exe+0x1b2b21): it presents, then retires the frame, which hands back the
                // flush token that eye 1's render-frame waits for.
                if (g_cfg.debugStereo & 16) {
                    g_eye0Present = true;
                    ((void(__fastcall*)(void*))(g_base + 0x7e25e0))(*(void**)(g_base + 0x701d060));
                    g_eye0Present = false;
                } else {
                    VR_ReturnLatencyToken();
                    VR_StereoCaptureEye(0);
                }
                // Post the per-frame retirement job exactly as the main loop does (exe+0x1ca823, in
                // exe+0x1c87d0): it runs after everything queued for this frame, finishes the frame
                // (exe+0x22f9000 on the engineDraw object) and hands back the flush token.
                if (g_cfg.debugStereo & 32) {
                    alignas(16) uint64_t fn[12] = {};
                    fn[0] = g_base + 0x54bb4a8;          // job vtable: invoke = exe+0x1cbc50
                    fn[1] = g_base + 0x701ca88;          // capture: engineDraw object
                    ((void(__fastcall*)(void*, void*))(g_base + 0x15ffb0))((void*)(g_base + 0x701ca98), fn);
                    ((void(__fastcall*)(void*))((*(void***)fn)[3]))(&fn[1]);
                }
                g_flushArrived = false;
                if (g_cfg.debugStereo & 32) {
                    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
                    double waited = 0;
                    while (true) {
                        if (SemCount(RVA_SEM_FLUSH) >= 1) { g_flushArrived = true; break; }
                        QueryPerformanceCounter(&t1); waited = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
                        if (waited > 100) break;
                        SwitchToThread();
                    }
                    static int fl = 0;
                    if (fl++ < 5 || !g_flushArrived) LOGD("Stereo: frame retired after eye 0: %s (%.2f ms)", g_flushArrived ? "yes" : "NO - forcing", waited);
                }
            } else {
                // eye 1 is submitted by the main loop and captured at Present; its engineDraw run
                // waits for a sim step it doesn't have (eye 0 started the only one).
                RaiseSimDoneFlag();
            }
        }
        *(float*)(view + 0x9d0) = savedAspect;
        *(float*)(view + 0x9d4) = savedFov;
        if ((++g_stereoFrames % 600) == 1) LOGI("Stereo: rendered frame %llu for both eyes (eye-1 holders %llu, dropped %llu)", g_stereoFrames.load(), g_payloadHeld.load(), g_fakeDropped.load());
        return;
    }
    Hud_SetEnabled(false);
    if (g_spActive) { g_spActive = false; Replay_EnableRedirect(false); }
    RestoreGameFov();
    if (g_renderFrames == 4) g_logJobs = false;   // a few baseline frames, then quiet until stereo starts
    ++t_renderDepth;
    oRenderFrame(renderer);
    --t_renderDepth;
}
} // namespace

const CameraInputs& Camera_LastInputs() { return g_last; }
int Camera_LastStereoEye() { return 1; }
bool Camera_InEye0Present() { return g_eye0Present; }

void Camera_NotePresent() {
    if (t_renderDepth > 0) ++g_presentsInRender; else ++g_presentsOutside;
}

void Camera_LogStats() {
    Discovery_LogDrawSources();
    LOGD("Camera: %llu main view updates, %llu render frames (%llu stereo), presents inside render-frame %d, outside %d",
         g_mainUpdates.load(), g_renderFrames.load(), g_stereoFrames.load(), g_presentsInRender.load(), g_presentsOutside.load());
}


// ---- instance ring (exe+0x3a0c210) allocation statistics, per stereo eye
using InstAllocFn = uint64_t*(__fastcall*)(uint64_t* out, uint64_t n);
InstAllocFn oInstAlloc = nullptr;
std::atomic<uint64_t> g_iaCalls{0}, g_iaFails{0}, g_iaUnits{0}, g_iaMin{~0ull}, g_iaMax{0};
uint64_t* __fastcall hkInstAlloc(uint64_t* out, uint64_t n) {
    uint64_t* r = oInstAlloc(out, n);
    uint64_t v = *out; uint32_t off = (uint32_t)v, cnt = (uint32_t)(v >> 32);
    ++g_iaCalls; g_iaUnits += n;
    if (!cnt) ++g_iaFails;
    else { uint64_t m = g_iaMin.load(); while (off < m && !g_iaMin.compare_exchange_weak(m, off)) {}
           m = g_iaMax.load(); while (off + cnt > m && !g_iaMax.compare_exchange_weak(m, off + cnt)) {} }
    return r;
}
static void InstStats(const char* tag) {
    uintptr_t b = g_base;
    uint64_t vb = *(uint64_t*)(b + 0x718d350), ve = *(uint64_t*)(b + 0x718d358);
    LOGD("InstRing %s: allocs %llu fails %llu units %llu range [%llu,%llu) dirty %llu head %llu tail %llu used %llu cap %llu slot %u",
         tag, g_iaCalls.load(), g_iaFails.load(), g_iaUnits.load(), g_iaMin.load() == ~0ull ? 0ull : g_iaMin.load(), g_iaMax.load(),
         (ve - vb) / 8, *(uint64_t*)(b + 0x718c958), *(uint64_t*)(b + 0x718c960), *(uint64_t*)(b + 0x718c968), *(uint64_t*)(b + 0x718c970),
         *(uint32_t*)(b + 0x718d378));
    { uint64_t n, mn, mx, avg; Discovery_InstDraws(&n, &mn, &mx, &avg);
      LOGD("InstDraws %s: %llu DrawIndexedInstanced, StartInstance min %llu max(end) %llu avg %llu", tag, n, n ? mn : 0ull, mx, avg); }
    g_iaCalls = 0; g_iaFails = 0; g_iaUnits = 0; g_iaMin = ~0ull; g_iaMax = 0;
}


// constant-ring upload (exe+0x7dd4b0): record every upload render-frame makes on the main thread
using CbUploadFn = void*(__fastcall*)(void* desc, void* span);
CbUploadFn oCbUpload = nullptr;
volatile int g_recEye = -1;
volatile bool g_spActive = false;
struct UpInfo { uintptr_t ra, src; uint64_t size, hash; };
UpInfo g_upInfo[2][64]; int g_upN[2] = {};
void* __fastcall hkCbUpload(void* desc, void* span) {
    void* r = oCbUpload(desc, span);
    if (g_spActive && t_renderDepth > 0 && g_last.valid && g_last.handle) {
        uintptr_t src = *(uintptr_t*)span, mv = (uintptr_t)*g_last.handle;
        if (src == mv + 0x4c0) Replay_NoteMainViewUpload(0, desc);
        else if (src == mv + 0x6e0) Replay_NoteMainViewUpload(1, desc);
    }
    if (g_recEye >= 0 && t_renderDepth > 0) {
        Replay_AddUpload(g_recEye, desc);
        int e = g_recEye, i = g_upN[e];
        if (i < 64) {
            const uint8_t* p = *(const uint8_t**)span; uint64_t sz = ((uint64_t*)span)[1];
            uint64_t h = 1469598103934665603ull;
            for (uint64_t k = 0; k < sz && k < 0x1000; ++k) h = (h ^ p[k]) * 1099511628211ull;
            g_upInfo[e][i] = { (uintptr_t)_ReturnAddress() - g_base, (uintptr_t)p, sz, h };
            g_upN[e] = i + 1;
        }
    }
    return r;
}
static void LogUploadDiffs() {
    uintptr_t arr = *(uintptr_t*)(g_base + RVA_VIEWS);
    int n = (std::min)(g_upN[0], g_upN[1]);
    for (int i = 0; i < n; ++i) {
        const UpInfo& a = g_upInfo[0][i]; const UpInfo& b = g_upInfo[1][i];
        long long rel = (long long)a.src - (long long)arr; char where[64];
        if (rel >= 0 && rel < 64 * 0x9f0) snprintf(where, sizeof(where), "view %lld +0x%llx", rel / 0x9f0, rel % 0x9f0);
        else if (a.src >= g_base && a.src < g_base + 0x8300000) snprintf(where, sizeof(where), "exe+0x%llx", (unsigned long long)(a.src - g_base));
        else snprintf(where, sizeof(where), "%p", (void*)a.src);
        LOGD("Upload %2d: ra exe+0x%llx size 0x%llx src %s %s", i, (unsigned long long)a.ra, a.size, where, a.hash == b.hash ? "same" : "DIFFERS");
    }
}
bool Camera_Install() {
    g_base = (uintptr_t)GetModuleHandleW(nullptr);
    // Sanity-check the code we are about to hook (build 2026-08-26 / v2.0.6).
    const uint8_t expectUV[] = { 0x41, 0x56, 0x56, 0x57, 0x55, 0x53, 0x48, 0x81, 0xEC, 0x00, 0x01, 0x00, 0x00 };
    if (memcmp((void*)(g_base + RVA_UPDATEVIEW), expectUV, sizeof(expectUV)) != 0) {
        LOGE("Camera: UpdateView prologue mismatch - unsupported game version; head tracking disabled");
        return false;
    }
    bool ok = MH_CreateHook((void*)(g_base + RVA_UPDATEVIEW), (void*)hkUpdateView, (void**)&oUpdateView) == MH_OK &&
              MH_EnableHook((void*)(g_base + RVA_UPDATEVIEW)) == MH_OK;
    LOGI("Camera: UpdateView hook %s", ok ? "installed" : "FAILED");
    bool ok2 = MH_CreateHook((void*)(g_base + RVA_RENDERFRAME), (void*)hkRenderFrame, (void**)&oRenderFrame) == MH_OK &&
               MH_EnableHook((void*)(g_base + RVA_RENDERFRAME)) == MH_OK;
    LOGI("Camera: render-frame hook %s", ok2 ? "installed" : "FAILED");
    {
        const uint8_t* p = (const uint8_t*)(g_base + RVA_CAMUPDATE);
        static const uint8_t sig[] = { 0x56, 0x53, 0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00 };   // push rsi; push rbx; sub rsp,88h
        bool sigOk = memcmp(p, sig, sizeof(sig)) == 0;
        bool ok = sigOk && MH_CreateHook((void*)p, (void*)hkCamUpdate, (void**)&oCamUpdate) == MH_OK && MH_EnableHook((void*)p) == MH_OK;
        LOGI("Camera: camera-update hook %s", ok ? "installed" : sigOk ? "FAILED" : "skipped (unexpected code - different game version?)");
    }
    bool ok3 = MH_CreateHook((void*)(g_base + RVA_RENDERJOB), (void*)hkRenderJob, (void**)&oRenderJob) == MH_OK &&
               MH_EnableHook((void*)(g_base + RVA_RENDERJOB)) == MH_OK;
    LOGI("Camera: render-job hook %s", ok3 ? "installed" : "FAILED");
    bool ok4 = MH_CreateHook((void*)(g_base + RVA_QUEUEPUSH), (void*)hkQueuePush, (void**)&oQueuePush) == MH_OK &&
               MH_EnableHook((void*)(g_base + RVA_QUEUEPUSH)) == MH_OK;
    LOGI("Camera: payload-return hook %s", ok4 ? "installed" : "FAILED");
    if (MH_CreateHook((void*)(g_base + 0x7dd4b0), (void*)hkCbUpload, (void**)&oCbUpload) == MH_OK) MH_EnableHook((void*)(g_base + 0x7dd4b0));
    if (MH_CreateHook((void*)(g_base + 0x3a0c210), (void*)hkInstAlloc, (void**)&oInstAlloc) == MH_OK) MH_EnableHook((void*)(g_base + 0x3a0c210));
    g_kickPatched = ok4 && ok3 && InstallKickPatch() && InstallHandshakePatch() && InstallWorkerPatch() && InstallStageSkipPatch();
    return ok;
}

static void SyncListParity() {
    *(volatile int*)(g_base + 0x718c908) = *(volatile int*)(g_base + 0x7ba4d24) ^ 1;
    *(volatile int*)(g_base + 0x718c8f8) = *(volatile int*)(g_base + 0x7ba4d20) ^ 1;
}

void Camera_SetFovOverride(float rad) { g_camFovOverride = rad; }
LONG Camera_TakeFovFixes() { return InterlockedExchange(&g_camFovFixes, 0); }

// Diagnostics (trigger "camdump"): where the game camera object keeps its position / target / direction.
// Scans the camera object and its view element for float triples matching the last UpdateView eye / target
// (the game's own values, before head tracking) and for the normalised view direction.
void Camera_Dump() {
    uintptr_t cam = g_lastCam;
    if (!cam || !g_last.valid) { LOGI("camdump: no camera yet"); return; }
    const float* E = g_last.eye; const float* T = g_last.target;
    float fx = T[0] - E[0], fy = T[1] - E[1], fz = T[2] - E[2], fl = sqrtf(fx * fx + fy * fy + fz * fz);
    if (fl > 1e-5f) { fx /= fl; fy /= fl; fz /= fl; }
    LOGI("camdump: cam %p view %p eye (%.3f %.3f %.3f) target (%.3f %.3f %.3f) dir (%.3f %.3f %.3f) dist %.3f",
         (void*)cam, (void*)*(uintptr_t*)(cam + 0x60), E[0], E[1], E[2], T[0], T[1], T[2], fx, fy, fz, fl);
    auto near3 = [](const float* p, float a, float b, float c, float eps) { return fabsf(p[0] - a) < eps && fabsf(p[1] - b) < eps && fabsf(p[2] - c) < eps; };
    auto scan = [&](const char* what, uintptr_t base, size_t len) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)base, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return;
        size_t avail = (size_t)((uintptr_t)mbi.BaseAddress + mbi.RegionSize - base);
        if (len > avail) len = avail;
        for (size_t o = 0; o + 12 <= len; o += 4) {
            const float* p = (const float*)(base + o);
            if (near3(p, E[0], E[1], E[2], 0.01f)) LOGI("camdump: %s+0x%zx = eye", what, o);
            if (near3(p, T[0], T[1], T[2], 0.01f)) LOGI("camdump: %s+0x%zx = target", what, o);
            if (near3(p, fx, fy, fz, 0.01f)) LOGI("camdump: %s+0x%zx = direction", what, o);
            if (near3(p, -fx, -fy, -fz, 0.01f)) LOGI("camdump: %s+0x%zx = -direction", what, o);
        }
    };
    scan("cam", cam, 0x1000);
    if (uintptr_t v = *(uintptr_t*)(cam + 0x60)) scan("view", v, 0xa00);
    for (int k = 0; k < 0x400; k += 8) {   // pointers from the camera object to other objects holding the pose
        uintptr_t p = *(uintptr_t*)(cam + k);
        if (p > 0x10000 && p < 0x7fffffffffffull && (p & 7) == 0 && p != cam) {
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery((void*)p, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY)) && !(mbi.Protect & PAGE_GUARD)) {
                char nm[32]; snprintf(nm, sizeof(nm), "cam[0x%x]", k); scan(nm, p, 0x400);
            }
        }
    }
    for (int o = 0; o < 0x400; o += 0x20) {   // raw camera object: 8 floats per line
        const float* f = (const float*)(cam + o);
        LOGI("camdump: raw +%03x: %10.4f %10.4f %10.4f %10.4f %10.4f %10.4f %10.4f %10.4f", o, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
    }
    LOGI("camdump: done");
}

void Camera_ViewDump() { ViewDumpStart(); }
void Camera_SetHeadFollow(int v) { SetHeadFollow(v); }
