// Game addresses by signature scan.
//
// The mod hooks a few game functions and reads a few game globals. Instead of fixed offsets into the exe (which
// break as soon as a game update moves code around), every address is found at startup by searching the game's
// code for a byte pattern taken from the build the mod was developed on (2026-08-26 / v2.0.6, image size
// 0x8218000):
//   - functions: the first instructions of the function itself, or a call to it;
//   - globals: an instruction that reads or writes the global (x64 RIP-relative), plus the instructions after it.
// Patterns have wildcards ("??") for everything that moves when code is relinked: RIP-relative displacements and
// relative jump / call targets. Struct offsets are kept, so a build whose data layout changed is not matched by
// mistake. Each entry has up to two patterns; it is found if a pattern matches exactly once.
//
// The search runs over the decrypted code in memory (the game's DRM decrypts it at startup), so the game's files
// are never read or changed.
//
// The patterns were made by GameAddr_MakeSigs (config key debug_make_sigs = 1, on the known build): for each entry
// it grows a pattern instruction by instruction from the function start, the call sites or the referencing
// instructions until it is unique, and writes the shortest ones to GranblueVR_sigs.txt, ready to paste below.
#include <windows.h>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include "gameaddr.h"
#include "globals.h"
#include "log.h"
#include "../third_party/minhook/src/hde/hde64.h"

namespace {
const uint32_t kKnownImageSize = 0x8218000;

struct Sig { const char* pat; int dispOff; int instrEnd; };   // dispOff < 0: the address is the match itself
struct Entry { const char* name; uintptr_t rva; bool code; bool required; Sig sig[2]; };

// Order must match GameAddrId.
Entry g_tab[GA_Count] = {
    // @@SIGS_BEGIN
    { "UpdateView", 0x74ad10, true, true, { { "41 56 56 57 55 53 48 81 EC 00 01 00 00 C5 78 29 BC 24 F0 00 00 00", -1, 0 }, { "E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 00 74", 1, 5 } } },
    { "RenderFrame", 0x1cbcd0, true, true, { { "55 41 57 41 56 41 55 41 54 56 57 53 B8 D8 57 00 00", -1, 0 }, { "E8 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 C0 0F 85 ?? ?? ?? ?? 81 3D ?? ?? ?? ?? FF FF FF 7F 0F 84 ?? ?? ?? ?? C6 05 ?? ?? ?? ?? 01", 1, 5 } } },
    { "CamUpdate", 0x68cb00, true, false, { { "56 53 48 81 EC 88 00 00 00 C5 F8 29 74 24 70", -1, 0 }, { "E8 ?? ?? ?? ?? 48 8B 35 ?? ?? ?? ?? 48 8B 0E", 1, 5 } } },
    { "RenderJob", 0x1dd4c0, true, false, { { "55 41 57 41 56 41 54 56 57 53 48 81 EC A0 01 00 00 48 8D AC 24 80 00 00 00 C5 F8 29 B5 10 01 00 00 48 C7 85 08 01 00 00 FE FF FF FF 8B 05", -1, 0 } } },
    { "CbUpload", 0x7dd4b0, true, true, { { "55 41 56 56 57 53 48 83 EC 50 48 8D 6C 24 50 48 C7 45 F8 FE FF FF FF 48 89 D7 48 89 CE 8B 05", -1, 0 }, { "E8 ?? ?? ?? ?? E9 ?? ?? ?? ?? 4B 8D 04 3E", 1, 5 } } },
    { "Views", 0x7038130, false, true, { { "48 8B 15 ?? ?? ?? ?? 4C 69 C0 F0 09 00 00", 3, 7 }, { "4C 8B 35 ?? ?? ?? ?? 4C 69 FE F0 09 00 00", 3, 7 } } },
    { "ViewCount", 0x7038140, false, false, { { "39 15 ?? ?? ?? ?? 7E ?? 4C 8B 05", 2, 6 }, { "83 3D ?? ?? ?? ?? 00 0F 8E ?? ?? ?? ?? 31 F6", 2, 7 } } },
    { "CurView", 0x70207c4, false, false, { { "83 3D ?? ?? ?? ?? 08 48 89 CE", 2, 7 }, { "89 35 ?? ?? ?? ?? F6 05 ?? ?? ?? ?? 01 0F 84", 2, 6 } } },
    { "Payload", 0x703ef78, false, true, { { "48 8B 0D ?? ?? ?? ?? 8B 45 24", 3, 7 }, { "48 8B 3D ?? ?? ?? ?? A8 01 0F 84", 3, 7 } } },
    { "Pending", 0x703ebd0, false, false, { { "48 8B 05 ?? ?? ?? ?? 48 8B 8D 78 01 00 00", 3, 7 }, { "48 C7 05 ?? ?? ?? ?? 00 00 00 00 48 8B 05 ?? ?? ?? ?? 48 89 0D", 3, 11 } } },
    { "FrameToken", 0x703e910, false, true, { { "48 FF 05 ?? ?? ?? ?? 8B 05 ?? ?? ?? ?? 89 C1", 3, 7 }, { "4C 8B 3D ?? ?? ?? ?? F6 46 18 02", 3, 7 } } },
    { "ResIndex", 0x7035980, false, false, { { "8B 05 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 89 0D", 2, 6 }, { "8B 1D ?? ?? ?? ?? 48 83 FB 06", 2, 6 } } },
    { "ResTable", 0x6b83460, false, false, { { "F3 4C 8D 25 ?? ?? ?? ?? 48 89 D9", 4, 8 }, { "4C 8D 25 ?? ?? ?? ?? 48 8B 43 68", 3, 7 } } },
    { "Cutscene", 0x7038388, false, false, { { "0F 95 05 ?? ?? ?? ?? C5 F8 29 15", 3, 7 }, { "80 3D ?? ?? ?? ?? 00 C5 F8 28 0D", 2, 7 } } },
    { "Cutscene2", 0x7396e38, false, false, { { "88 05 ?? ?? ?? ?? C5 C8 57 F6", 2, 6 } } },
    // @@SIGS_END
};

uintptr_t g_addr[GA_Count] = {};
bool g_resolved = false, g_ok = false, g_known = false;
uintptr_t g_base = 0;

struct Range { const uint8_t* b; const uint8_t* e; };
std::vector<Range> g_ranges;

// Committed, readable parts of the exe's executable sections.
void BuildRanges() {
    g_ranges.clear();
    auto dos = (const IMAGE_DOS_HEADER*)g_base;
    auto nt = (const IMAGE_NT_HEADERS64*)(g_base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!(s[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uint8_t* p = (const uint8_t*)(g_base + s[i].VirtualAddress);
        const uint8_t* end = p + (std::max)(s[i].Misc.VirtualSize, s[i].SizeOfRawData);
        while (p < end) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
            const uint8_t* re = (std::min)((const uint8_t*)mbi.BaseAddress + mbi.RegionSize, end);
            bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                            (mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY | PAGE_READONLY | PAGE_READWRITE));
            if (readable) {
                if (!g_ranges.empty() && g_ranges.back().e == p) g_ranges.back().e = re;
                else g_ranges.push_back({ p, re });
            }
            p = re;
        }
    }
}
const Range* RangeOf(const uint8_t* p) {
    for (const Range& r : g_ranges) if (p >= r.b && p < r.e) return &r;
    return nullptr;
}

using Pat = std::vector<int16_t>;   // -1 = wildcard
Pat Parse(const char* s) {
    Pat p;
    while (*s) {
        if (*s == ' ') { ++s; continue; }
        if (*s == '?') { p.push_back(-1); while (*s == '?') ++s; continue; }
        char* e; long v = strtol(s, &e, 16);
        if (e == s) break;
        p.push_back((int16_t)v); s = e;
    }
    return p;
}
std::string ToString(const Pat& p) {
    std::string s; char b[4];
    for (size_t i = 0; i < p.size(); ++i) {
        if (i) s += ' ';
        if (p[i] < 0) s += "??"; else { snprintf(b, sizeof(b), "%02X", p[i]); s += b; }
    }
    return s;
}
inline bool MatchAt(const uint8_t* m, const Pat& p) {
    for (size_t i = 0; i < p.size(); ++i) if (p[i] >= 0 && m[i] != (uint8_t)p[i]) return false;
    return true;
}
void FindAll(const Pat& p, std::vector<const uint8_t*>& out, size_t cap) {
    size_t a = 0;
    while (a < p.size() && p[a] < 0) ++a;
    if (a == p.size()) return;
    for (const Range& r : g_ranges) {
        if ((size_t)(r.e - r.b) < p.size()) continue;
        const uint8_t* q = r.b + a;
        const uint8_t* lim = r.e - p.size() + a + 1;
        while (q < lim) {
            q = (const uint8_t*)memchr(q, (uint8_t)p[a], lim - q);
            if (!q) break;
            if (MatchAt(q - a, p)) { out.push_back(q - a); if (out.size() >= cap) return; }
            ++q;
        }
    }
}
uintptr_t Target(const uint8_t* m, const Sig& s) {
    if (s.dispOff < 0) return (uintptr_t)m;
    int32_t d; memcpy(&d, m + s.dispOff, 4);
    return (uintptr_t)m + s.instrEnd + d;
}

// ---- signature generator (debug_make_sigs)

int ImmSize(const hde64s& hs) {
    return (hs.flags & F_IMM64) ? 8 : (hs.flags & F_IMM32) ? 4 : (hs.flags & F_IMM16) ? 2 : (hs.flags & F_IMM8) ? 1 : 0;
}
bool RipRel(const hde64s& hs) { return (hs.flags & F_MODRM) && hs.modrm_mod == 0 && hs.modrm_rm == 5; }

// Appends one instruction, with relinking-dependent fields as wildcards. Returns its length (0 = undecodable).
int AppendInstr(const uint8_t* p, Pat& pat) {
    hde64s hs;
    hde64_disasm(p, &hs);
    if (hs.flags & F_ERROR) return 0;
    const Range* r = RangeOf(p);
    if (!r || p + hs.len > r->e) return 0;
    size_t at = pat.size();
    for (int i = 0; i < hs.len; ++i) pat.push_back(p[i]);
    int imm = ImmSize(hs);
    if (RipRel(hs)) { int d = hs.len - imm - 4; for (int i = 0; i < 4; ++i) pat[at + d + i] = -1; }
    if (hs.flags & F_RELATIVE) for (int i = 0; i < imm; ++i) pat[at + hs.len - imm + i] = -1;
    return hs.len;
}
size_t FixedCount(const Pat& p) { size_t n = 0; for (int16_t v : p) n += v >= 0; return n; }

// Grows a pattern from the instruction at s until it matches only at s. Empty if that doesn't happen within maxLen.
Pat Grow(const uint8_t* s, size_t maxLen) {
    Pat pat; const uint8_t* p = s;
    std::vector<const uint8_t*> hits; bool scanned = false;
    while (pat.size() < maxLen) {
        int n = AppendInstr(p, pat);
        if (!n) break;
        p += n;
        if (FixedCount(pat) < 6) continue;
        if (!scanned) {
            hits.clear(); FindAll(pat, hits, 100000);
            if (hits.size() >= 100000) continue;   // too common yet: scan again once longer
            scanned = true;
        } else {
            std::vector<const uint8_t*> keep;
            for (const uint8_t* h : hits) { const Range* r = RangeOf(h); if (r && h + pat.size() <= r->e && MatchAt(h, pat)) keep.push_back(h); }
            hits.swap(keep);
        }
        if (hits.size() == 1 && hits[0] == s) {
            while (!pat.empty() && pat.back() < 0) pat.pop_back();
            return pat;
        }
        if (hits.empty()) break;
    }
    return {};
}

struct Ref { const uint8_t* s; int dispOff; int instrEnd; };

// Instructions in the code that address `target` RIP-relatively (data references), or call / jump to it.
void FindRefs(uintptr_t target, bool calls, std::vector<Ref>& refs, size_t cap) {
    for (const Range& r : g_ranges) {
        for (const uint8_t* i = r.b + 1; i + 4 <= r.e; ++i) {
            int32_t d; memcpy(&d, i, 4);
            int64_t k = (int64_t)target - (int64_t)(uintptr_t)i - 4 - d;   // bytes after the displacement
            if (k != 0 && k != 1 && k != 2 && k != 4) continue;
            if (calls) {
                if (k == 0 && (i[-1] == 0xE8 || i[-1] == 0xE9)) refs.push_back({ i - 1, 1, 5 });
            } else {
                int best = 0;
                for (int back = 1; back <= 8 && i - back >= r.b; ++back) {
                    hde64s hs; hde64_disasm(i - back, &hs);
                    if ((hs.flags & F_ERROR) || !RipRel(hs)) continue;
                    if (i - back + hs.len != i + 4 + k || ImmSize(hs) != k) continue;
                    best = back;
                    break;
                }
                if (!best) continue;
                // include prefixes (REX, operand size, rep) the shortest decode left out
                while (i - best - 1 >= r.b) {
                    uint8_t b = i[-best - 1];
                    if (!((b >= 0x40 && b <= 0x4F) || b == 0x66 || b == 0xF2 || b == 0xF3)) break;
                    hde64s hs; hde64_disasm(i - best - 1, &hs);
                    if ((hs.flags & F_ERROR) || !RipRel(hs) || i - best - 1 + hs.len != i + 4 + k) break;
                    ++best;
                }
                refs.push_back({ i - best, best, best + 4 + (int)k });
            }
            if (refs.size() >= cap) return;
        }
    }
}

struct Cand { Pat pat; int dispOff, instrEnd; const uint8_t* at; };
} // namespace

void GameAddr_MakeSigs() {
    g_base = (uintptr_t)GetModuleHandleW(nullptr);
    auto nt = (const IMAGE_NT_HEADERS64*)(g_base + ((const IMAGE_DOS_HEADER*)g_base)->e_lfanew);
    if (nt->OptionalHeader.SizeOfImage != kKnownImageSize) { LOGE("Signatures: debug_make_sigs needs the known game build"); return; }
    BuildRanges();
    std::wstring path = g_modDir + L"GranblueVR_sigs.txt";
    FILE* f = _wfopen(path.c_str(), L"w");
    if (!f) { LOGE("Signatures: can't write GranblueVR_sigs.txt"); return; }
    fprintf(f, "// image size 0x%x, timestamp 0x%x\n", nt->OptionalHeader.SizeOfImage, nt->FileHeader.TimeDateStamp);
    DWORD t0 = GetTickCount();
    for (int id = 0; id < GA_Count; ++id) {
        const Entry& e = g_tab[id];
        uintptr_t target = g_base + e.rva;
        std::vector<Cand> c;
        if (e.code) {
            Pat p = Grow((const uint8_t*)target, 128);
            if (!p.empty()) c.push_back({ p, -1, 0, (const uint8_t*)target });
        }
        std::vector<Ref> refs;
        FindRefs(target, e.code, refs, 400);
        std::vector<Cand> rc;
        for (const Ref& r : refs) {
            Pat p = Grow(r.s, 96);
            if (!p.empty()) rc.push_back({ p, r.dispOff, r.instrEnd, r.s });
        }
        std::sort(rc.begin(), rc.end(), [](const Cand& a, const Cand& b) { return FixedCount(a.pat) < FixedCount(b.pat); });
        // two patterns from different places in the code, so one edited function doesn't lose the entry
        for (const Cand& x : rc) {
            if (c.size() >= 2) break;
            bool distinct = true;
            for (const Cand& y : c) if (x.at > y.at - 0x400 && x.at < y.at + 0x400) distinct = false;
            if (distinct) c.push_back(x);
        }
        LOGI("Signatures: %s: %zu references, %zu patterns", e.name, refs.size(), c.size());
        fprintf(f, "    { \"%s\", 0x%llx, %s, %s, { ", e.name, (unsigned long long)e.rva, e.code ? "true" : "false", e.required ? "true" : "false");
        for (size_t k = 0; k < c.size() && k < 2; ++k)
            fprintf(f, "%s{ \"%s\", %d, %d }", k ? ", " : "", ToString(c[k].pat).c_str(), c[k].dispOff, c[k].instrEnd);
        fprintf(f, " } },\n");
        fflush(f);
    }
    fclose(f);
    LOGI("Signatures: wrote GranblueVR_sigs.txt in %lu ms", GetTickCount() - t0);
}

bool GameAddr_Resolve() {
    if (g_resolved) return g_ok;
    g_resolved = true;
    g_base = (uintptr_t)GetModuleHandleW(nullptr);
    auto nt = (const IMAGE_NT_HEADERS64*)(g_base + ((const IMAGE_DOS_HEADER*)g_base)->e_lfanew);
    uint32_t imageSize = nt->OptionalHeader.SizeOfImage;
    BuildRanges();
    DWORD t0 = GetTickCount();
    int found = 0, missingReq = 0, atKnown = 0;
    for (int id = 0; id < GA_Count; ++id) {
        const Entry& e = g_tab[id];
        uintptr_t a = 0; int from = -1; bool anyPat = false;
        for (int k = 0; k < 2; ++k) {
            if (!e.sig[k].pat) continue;
            anyPat = true;
            std::vector<const uint8_t*> hits;
            FindAll(Parse(e.sig[k].pat), hits, 4);
            if (hits.size() != 1) { LOGI("Signatures: %s pattern %d: %zu matches", e.name, k + 1, hits.size()); continue; }
            uintptr_t t = Target(hits[0], e.sig[k]);
            if (!a) { a = t; from = k; }
            else if (t != a) LOGI("Signatures: %s: pattern %d points to exe+0x%llx, pattern %d to exe+0x%llx - using the first", e.name,
                                  from + 1, (unsigned long long)(a - g_base), k + 1, (unsigned long long)(t - g_base));
        }
        if (!anyPat && imageSize == kKnownImageSize) { a = g_base + e.rva; from = 2; }   // no pattern yet (development)
        g_addr[id] = a;
        if (a) {
            ++found;
            if (a == g_base + e.rva) ++atKnown;
            if (a != g_base + e.rva || from > 0)
                LOGI("Signatures: %s at exe+0x%llx (%s)", e.name, (unsigned long long)(a - g_base), from == 2 ? "fixed" : from == 1 ? "pattern 2" : "pattern 1");
        } else {
            if (e.required) ++missingReq;
            LOGE("Signatures: %s NOT FOUND%s", e.name, e.required ? " (required)" : " (optional, feature disabled)");
        }
    }
    g_known = atKnown == GA_Count && imageSize == kKnownImageSize;
    g_ok = missingReq == 0;
    LOGI("Signatures: %d/%d game addresses found in %lu ms (image size 0x%x, timestamp 0x%x)%s", found, (int)GA_Count, GetTickCount() - t0,
         imageSize, nt->FileHeader.TimeDateStamp, g_known ? ", the build the mod was made on" : ", a different game build");
    return g_ok;
}

uintptr_t GA(GameAddrId id) { return (id >= 0 && id < GA_Count) ? g_addr[id] : 0; }
bool GameAddr_KnownBuild() { return g_known; }
