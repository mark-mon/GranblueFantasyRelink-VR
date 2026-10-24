// Pre-rendered movies (Bink 2, bink2w64.dll). While a full-screen movie plays, the game keeps running its
// render-frame on some frames and presents the movie on others; the mod then alternated between stereo frames
// (the movie copied into both eyes as a huge image) and theater frames (the movie on the flat screen), which
// flickered badly. While a large Bink movie is open the mod stays in theater mode (flat screen, black around it).
#include <windows.h>
#include <atomic>
#include "MinHook.h"
#include "log.h"

namespace {
using OpenFn = void*(__cdecl*)(const char*, unsigned);
using CloseFn = void(__cdecl*)(void*);
OpenFn oOpen = nullptr; CloseFn oClose = nullptr;
std::atomic<int> g_open{ 0 };
CRITICAL_SECTION g_cs; bool g_csInit = false;
void* g_handles[16] = {};
bool g_hooked = false;

void* __cdecl hkOpen(const char* name, unsigned flags) {
    void* h = oOpen(name, flags);
    if (h) {
        unsigned w = ((unsigned*)h)[0], ht = ((unsigned*)h)[1];   // BINK starts with Width, Height
        bool big = w >= 1280 && ht >= 600 && w <= 16384 && ht <= 16384;
        LOGI("Movie: open '%s' %ux%u%s", name ? name : "?", w, ht, big ? " -> theater mode" : "");
        if (big) {
            EnterCriticalSection(&g_cs);
            for (auto& s : g_handles) if (!s) { s = h; ++g_open; break; }
            LeaveCriticalSection(&g_cs);
        }
    }
    return h;
}
void __cdecl hkClose(void* h) {
    EnterCriticalSection(&g_cs);
    for (auto& s : g_handles) if (s && s == h) { s = nullptr; --g_open; LOGI("Movie: closed"); }
    LeaveCriticalSection(&g_cs);
    oClose(h);
}
} // namespace

// Called periodically (the game may load the Bink DLL late). Safe to call repeatedly.
void Movie_Install() {
    if (g_hooked) return;
    HMODULE m = GetModuleHandleW(L"bink2w64.dll");
    if (!m) return;
    if (!g_csInit) { InitializeCriticalSection(&g_cs); g_csInit = true; }
    void* po = (void*)GetProcAddress(m, "BinkOpen");
    void* pc = (void*)GetProcAddress(m, "BinkClose");
    g_hooked = true;
    if (!po || !pc) { LOGI("Movie: bink2w64.dll without BinkOpen/BinkClose exports; movie detection off"); return; }
    bool ok = MH_CreateHook(po, (void*)hkOpen, (void**)&oOpen) == MH_OK && MH_EnableHook(po) == MH_OK &&
              MH_CreateHook(pc, (void*)hkClose, (void**)&oClose) == MH_OK && MH_EnableHook(pc) == MH_OK;
    LOGI("Movie: Bink hooks %s", ok ? "installed" : "FAILED");
}
bool Movie_Playing() { return g_open.load() > 0; }
