// Event tracer for engine synchronisation analysis (trigger word "trace": records ~4 frames).
#include <windows.h>
#include <cstdint>
#include <atomic>
#include <intrin.h>
#include "MinHook.h"
#include "globals.h"

namespace {
struct Ev { LARGE_INTEGER t; DWORD tid; const char* what; uintptr_t a, b; };
Ev g_ev[4096];
std::atomic<int> g_n{ 0 };
std::atomic<int> g_active{ 0 };
uintptr_t g_base = 0;
LARGE_INTEGER g_t0, g_freq;

void Rec(const char* what, uintptr_t a = 0, uintptr_t b = 0) {
    if (!g_active.load(std::memory_order_relaxed)) return;
    int i = g_n.fetch_add(1);
    if (i >= 4096) return;
    QueryPerformanceCounter(&g_ev[i].t);
    g_ev[i].tid = GetCurrentThreadId(); g_ev[i].what = what; g_ev[i].a = a; g_ev[i].b = b;
}

using V1 = void(__fastcall*)(void*);
V1 oRel = nullptr, oAcq = nullptr, oDraw = nullptr;
void __fastcall hkRel(void* s) {
    uintptr_t r = (uintptr_t)s - g_base;
    if (r == 0x701ce08 || r == 0x701cca8 || r == 0x701cd58) Rec("sem release", r, (uintptr_t)_ReturnAddress() - g_base);
    oRel(s);
}
void __fastcall hkAcq(void* s) {
    uintptr_t r = (uintptr_t)s - g_base;
    bool w = (r == 0x701ce08 || r == 0x701cca8 || r == 0x701cd58);
    if (w) Rec("sem acquire>", r, (uintptr_t)_ReturnAddress() - g_base);
    oAcq(s);
    if (w) Rec("sem acquire<", r);
}
void __fastcall hkDraw(void* o) { Rec("engineDraw>"); oDraw(o); Rec("engineDraw<"); }
V1 oSync = nullptr;
void __fastcall hkSync(void* c) { Rec("syncJob>", (uintptr_t)_ReturnAddress() - g_base); oSync(c); Rec("syncJob<"); }
}

void Trace_Install() {
    g_base = (uintptr_t)GetModuleHandleW(nullptr);
    QueryPerformanceFrequency(&g_freq);
    MH_CreateHook((void*)(g_base + 0x1a3320), (void*)hkRel, (void**)&oRel); MH_EnableHook((void*)(g_base + 0x1a3320));
    MH_CreateHook((void*)(g_base + 0x1a3260), (void*)hkAcq, (void**)&oAcq); MH_EnableHook((void*)(g_base + 0x1a3260));
    MH_CreateHook((void*)(g_base + 0x1a97d0), (void*)hkDraw, (void**)&oDraw); MH_EnableHook((void*)(g_base + 0x1a97d0));
    MH_CreateHook((void*)(g_base + 0x1da5e0), (void*)hkSync, (void**)&oSync); MH_EnableHook((void*)(g_base + 0x1da5e0));
    LOGI("Trace: hooks installed");
}
void Trace_Event(const char* what, uintptr_t a, uintptr_t b) { Rec(what, a, b); }
void Trace_Start() { g_n = 0; QueryPerformanceCounter(&g_t0); g_active = 1; LOGI("Trace: started"); }
void Trace_Poll() {
    if (!g_active) return;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if ((now.QuadPart - g_t0.QuadPart) * 1000 / g_freq.QuadPart < 400) return;
    g_active = 0;
    int n = g_n.load(); if (n > 4096) n = 4096;
    LOGI("Trace: %d events", n);
    for (int i = 0; i < n; ++i) {
        double ms = (g_ev[i].t.QuadPart - g_t0.QuadPart) * 1000.0 / g_freq.QuadPart;
        LOGI("  %8.3f t%05lu %-14s %llx %llx", ms, g_ev[i].tid, g_ev[i].what, (unsigned long long)g_ev[i].a, (unsigned long long)g_ev[i].b);
    }
}
