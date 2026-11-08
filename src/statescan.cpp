// Debug tool: find a game-state flag (e.g. "a cutscene is playing") by comparing the exe's writable data in
// different states. Trigger "snap=<0..3>" copies the writable sections into slot k; "snapdiff" logs the dwords and
// bytes that are equal within slots 0/1 (state A), equal within slots 2/3 (state B), and differ between A and B,
// keeping small integer / boolean values (flags, enums). Run twice (A, A, B, B) to rule out counters.
#include <windows.h>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include "log.h"

namespace {
struct Sec { uint32_t rva; uint32_t size; };
std::vector<Sec> g_secs;
std::vector<uint8_t> g_snap[4];
uintptr_t g_base = 0;

void Sections() {
    if (!g_secs.empty()) return;
    g_base = (uintptr_t)GetModuleHandleW(nullptr);
    auto nt = (const IMAGE_NT_HEADERS64*)(g_base + ((const IMAGE_DOS_HEADER*)g_base)->e_lfanew);
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!(s[i].Characteristics & IMAGE_SCN_MEM_WRITE) || (s[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        g_secs.push_back({ s[i].VirtualAddress, (std::max)(s[i].Misc.VirtualSize, s[i].SizeOfRawData) & ~3u });
    }
}
bool Readable(uintptr_t p) {
    MEMORY_BASIC_INFORMATION m;
    return VirtualQuery((void*)p, &m, sizeof(m)) && m.State == MEM_COMMIT && !(m.Protect & (PAGE_GUARD | PAGE_NOACCESS));
}
} // namespace

void StateScan_Snap(int k) {
    if (k < 0 || k > 3) return;
    Sections();
    size_t total = 0; for (auto& s : g_secs) total += s.size;
    g_snap[k].assign(total, 0);
    size_t off = 0;
    for (auto& s : g_secs) {
        // copy page by page (skip uncommitted pages, which stay zero)
        for (uint32_t p = 0; p < s.size; p += 0x1000) {
            uint32_t n = (std::min)(0x1000u, s.size - p);
            if (Readable(g_base + s.rva + p)) memcpy(&g_snap[k][off + p], (const void*)(g_base + s.rva + p), n);
        }
        off += s.size;
    }
    LOGI("StateScan: snapshot %d: %zu sections, %zu MB", k, g_secs.size(), total >> 20);
}

void StateScan_Diff() {
    for (int k = 0; k < 4; ++k) if (g_snap[k].empty() || g_snap[k].size() != g_snap[0].size()) { LOGI("StateScan: need snapshots 0..3"); return; }
    const uint8_t *a0 = g_snap[0].data(), *a1 = g_snap[1].data(), *b0 = g_snap[2].data(), *b1 = g_snap[3].data();
    int shown = 0, total = 0;
    size_t off = 0;
    for (auto& s : g_secs) {
        for (uint32_t i = 0; i + 4 <= s.size; i += 4) {
            uint32_t A0, A1, B0, B1;
            memcpy(&A0, a0 + off + i, 4); memcpy(&A1, a1 + off + i, 4); memcpy(&B0, b0 + off + i, 4); memcpy(&B1, b1 + off + i, 4);
            if (A0 != A1 || B0 != B1 || A0 == B0) continue;
            // flags / enums: small values, or a single byte changing
            bool small = A0 <= 64 && B0 <= 64;
            uint32_t x = A0 ^ B0;
            bool oneByte = (x & 0xffffff00) == 0 || (x & 0xffff00ff) == 0 || (x & 0xff00ffff) == 0 || (x & 0x00ffffff) == 0;
            if (!small && !oneByte) continue;
            ++total;
            if (shown < 300) { ++shown; LOGI("StateScan: exe+0x%x  A %08x  B %08x", s.rva + i, A0, B0); }
        }
        off += s.size;
    }
    LOGI("StateScan: %d candidates (%d shown)", total, shown);
}

// Watch a few addresses (trigger "snapwatch=rva,rva,..."), logged every second by StateScan_Tick.
static uint32_t g_watch[16]; static int g_nWatch = 0;
void StateScan_Watch(const char* list) {
    Sections();
    g_nWatch = 0;
    for (const char* p = list; *p && g_nWatch < 16; ) { char* e; unsigned long v = strtoul(p, &e, 16); if (e == p) { ++p; continue; } g_watch[g_nWatch++] = (uint32_t)v; p = e; }
    LOGI("StateScan: watching %d addresses", g_nWatch);
}
void StateScan_Tick() {
    static DWORD last = 0;
    if (!g_nWatch || GetTickCount() - last < 1000) return;
    last = GetTickCount();
    char buf[512]; int n = 0;
    for (int i = 0; i < g_nWatch; ++i) n += snprintf(buf + n, sizeof(buf) - n, " %x=%08x", g_watch[i], *(volatile uint32_t*)(g_base + g_watch[i]));
    LOGI("StateScan:%s", buf);
}
