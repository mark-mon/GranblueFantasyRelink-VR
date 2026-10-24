// Shader identification and post-processing pass inventory.
//
// Every pixel/compute shader the game creates is hashed (FNV-1a 64 of its bytecode) so passes can be
// recognised across runs. Trigger "passes": for one frame, every draw/dispatch recorded on the deferred
// contexts is written to GranblueVR_passes.txt with its shader hash and its inputs/outputs, and the
// disassembly of each shader used is written to GranblueVR_shaders\<hash>.txt (D3DDisassemble).
#include <windows.h>
#include <d3d11_4.h>
#include <mutex>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <thread>
#include <condition_variable>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdint>
#include "log.h"
#include "globals.h"

namespace {
std::mutex g_mx;
struct ShaderInfo { uint64_t hash; char kind; std::vector<uint8_t> code; };
std::unordered_map<void*, ShaderInfo> g_shaders;   // shader object -> info
size_t g_codeBytes = 0;

uint64_t Fnv(const void* p, size_t n) {
    uint64_t h = 1469598103934665603ull; const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

// ---- capture state
volatile bool g_cap = false;
int g_capFrames = 0;
struct CtxSt { ID3D11DeviceContext* c; void* ps; void* cs; ID3D11View* srv[16]; ID3D11View* csrv[16]; ID3D11View* rtv[8]; ID3D11View* dsv; ID3D11View* uav[8]; int nlines; };
std::vector<CtxSt> g_ctx;
std::vector<std::string> g_lines;
std::unordered_map<ID3D11Resource*, int> g_resId;
std::vector<std::string> g_resDesc;
std::unordered_map<uint64_t, void*> g_usedShaders;
LONG g_seq = 0;

CtxSt* Ctx(ID3D11DeviceContext* c) {
    for (auto& s : g_ctx) if (s.c == c) return &s;
    CtxSt s = {}; s.c = c; g_ctx.push_back(s); return &g_ctx.back();
}
int ResId(ID3D11View* v) {
    if (!v) return -1;
    ID3D11Resource* r = nullptr; v->GetResource(&r);
    if (!r) return -1;
    r->Release();
    auto it = g_resId.find(r);
    if (it != g_resId.end()) return it->second;
    int id = (int)g_resDesc.size();
    g_resId[r] = id;
    char d[160];
    D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) { D3D11_TEXTURE2D_DESC t; ((ID3D11Texture2D*)r)->GetDesc(&t); snprintf(d, sizeof(d), "T%d %ux%u fmt %d mips %u arr %u bind 0x%x", id, t.Width, t.Height, (int)t.Format, t.MipLevels, t.ArraySize, t.BindFlags); }
    else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D) { D3D11_TEXTURE3D_DESC t; ((ID3D11Texture3D*)r)->GetDesc(&t); snprintf(d, sizeof(d), "T%d 3D %ux%ux%u fmt %d", id, t.Width, t.Height, t.Depth, (int)t.Format); }
    else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) { D3D11_BUFFER_DESC b; ((ID3D11Buffer*)r)->GetDesc(&b); snprintf(d, sizeof(d), "T%d buffer %u bytes bind 0x%x stride %u", id, b.ByteWidth, b.BindFlags, b.StructureByteStride); }
    else snprintf(d, sizeof(d), "T%d other", id);
    g_resDesc.push_back(d);
    return id;
}
uint64_t HashOf(void* sh, char* kind = nullptr) {
    if (!sh) return 0;
    auto it = g_shaders.find(sh);
    if (it == g_shaders.end()) return 0;
    if (kind) *kind = it->second.kind;
    return it->second.hash;
}
void List(std::string& s, const char* tag, ID3D11View* const* v, int n) {
    bool any = false;
    for (int i = 0; i < n; ++i) if (v[i]) {
        if (!any) { s += " "; s += tag; s += "["; any = true; } else s += ",";
        char b[24]; snprintf(b, sizeof(b), "%d:T%d", i, ResId(v[i])); s += b;
    }
    if (any) s += "]";
}
} // namespace

// ---- debug_dump_shaders: every distinct pixel / compute shader the game creates is disassembled on a background
// thread into GranblueVR_allshaders\<hash>.<ps|cs>.txt, and listed in GranblueVR_allshaders\index.txt with its
// constant-buffer and resource names (one line per shader, for searching: e.g. "Dof", "Bokeh", "Gauss").
namespace dump {
std::mutex mx; std::condition_variable cv;
struct Job { uint64_t hash; char kind; std::vector<uint8_t> code; };
std::deque<Job> queue; std::unordered_set<uint64_t> seen;
bool started = false; size_t written = 0;
using DisasmFn = HRESULT(WINAPI*)(const void*, SIZE_T, UINT, const char*, ID3DBlob**);
// names from the disassembly's comment header: "// cbuffer Name" and the resource-binding table
std::string Names(const char* t) {
    std::string out; bool inBind = false;
    for (const char* line = t; line && *line; ) {
        const char* e = strchr(line, '\n'); size_t n = e ? (size_t)(e - line) : strlen(line);
        std::string l(line, n); if (!l.empty() && l.back() == '\r') l.pop_back();
        if (l.rfind("// cbuffer ", 0) == 0) out += " cb:" + l.substr(11);
        else if (l.find("// Name ") == 0 && l.find("Type") != std::string::npos) inBind = true;
        else if (inBind) {
            if (l.size() < 4 || l.rfind("//", 0) != 0) inBind = false;
            else if (l[3] != '-' && l[3] != ' ') {
                size_t sp = l.find(' ', 3);
                std::string name = l.substr(3, sp == std::string::npos ? std::string::npos : sp - 3);
                if (!name.empty()) out += " " + name;
            } else if (l.size() > 3 && l[3] == ' ') inBind = false;
        }
        if (l.rfind("ps_", 0) == 0 || l.rfind("cs_", 0) == 0) break;   // end of the header
        line = e ? e + 1 : nullptr;
    }
    return out;
}
void Worker() {
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    DisasmFn dis = dc ? (DisasmFn)GetProcAddress(dc, "D3DDisassemble") : nullptr;
    std::wstring dir = g_modDir + L"GranblueVR_allshaders\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    FILE* idx = _wfopen((dir + L"index.txt").c_str(), L"ab");
    if (idx) { fprintf(idx, "# hash stage bytes | constant buffers (cb:) and resources\r\n"); fflush(idx); }
    for (;;) {
        Job j;
        { std::unique_lock<std::mutex> lk(mx); cv.wait(lk, [] { return !queue.empty(); }); j = std::move(queue.front()); queue.pop_front(); }
        char name[64]; snprintf(name, sizeof(name), "%016llx.%s", (unsigned long long)j.hash, j.kind == 'C' ? "cs" : "ps");
        std::string names;
        ID3DBlob* blob = nullptr;
        if (dis && SUCCEEDED(dis(j.code.data(), j.code.size(), 0, nullptr, &blob)) && blob) {
            const char* t = (const char*)blob->GetBufferPointer();
            names = Names(t);
            FILE* f = _wfopen((dir + std::wstring(name, name + strlen(name)) + L".txt").c_str(), L"wb");
            if (f) { fwrite(t, 1, blob->GetBufferSize() > 0 ? blob->GetBufferSize() - 1 : 0, f); fclose(f); }
            blob->Release();
        }
        if (idx) { fprintf(idx, "%016llx %s %zu |%s\r\n", (unsigned long long)j.hash, j.kind == 'C' ? "cs" : "ps", j.code.size(), names.c_str()); fflush(idx); }
        if (++written % 250 == 0) LOGI("Shader dump: %zu shaders written to GranblueVR_allshaders", written);
    }
}
void Add(uint64_t hash, char kind, const void* code, SIZE_T len) {
    std::lock_guard<std::mutex> lk(mx);
    if (!seen.insert(hash).second) return;
    if (!started) { started = true; std::thread(Worker).detach(); LOGI("Shader dump: on (debug_dump_shaders) -> GranblueVR_allshaders\\index.txt"); }
    queue.push_back({ hash, kind, std::vector<uint8_t>((const uint8_t*)code, (const uint8_t*)code + len) });
    cv.notify_one();
}
} // namespace dump

void PostFx_OnShaderCreated(void* shader, uint64_t hash);
// ---- device hooks (installed from hooks.cpp on the shared device vtable)
void Passes_OnShaderCreated(void* shader, char kind, const void* code, SIZE_T len) {
    if (!shader || !code) return;
    std::lock_guard<std::mutex> lk(g_mx);
    ShaderInfo si; si.hash = Fnv(code, len); si.kind = kind;
    if (g_codeBytes < (256u << 20)) { si.code.assign((const uint8_t*)code, (const uint8_t*)code + len); g_codeBytes += len; }
    uint64_t h = si.hash;
    g_shaders[shader] = std::move(si);
    if (g_cfg.debugDumpShaders) dump::Add(h, kind, code, len);
    PostFx_OnShaderCreated(shader, h);
}
uint64_t Passes_ShaderHash(void* shader) { std::lock_guard<std::mutex> lk(g_mx); return HashOf(shader); }
size_t Passes_ShaderCount() { std::lock_guard<std::mutex> lk(g_mx); return g_shaders.size(); }

// ---- deferred-context state tracking (only while capturing)
bool Passes_Capturing() { return g_cap; }
void Passes_SetShader(ID3D11DeviceContext* c, char kind, void* sh) {
    if (!g_cap) return;
    std::lock_guard<std::mutex> lk(g_mx);
    CtxSt* s = Ctx(c);
    if (kind == 'P') s->ps = sh; else if (kind == 'C') s->cs = sh;
}
void Passes_SetSRVs(ID3D11DeviceContext* c, char stage, UINT start, UINT n, ID3D11ShaderResourceView* const* v) {
    if (!g_cap) return;
    std::lock_guard<std::mutex> lk(g_mx);
    CtxSt* s = Ctx(c);
    ID3D11View** arr = stage == 'C' ? s->csrv : s->srv;
    if (stage == 'V') return;
    for (UINT i = 0; i < n && start + i < 16; ++i) arr[start + i] = v ? v[i] : nullptr;
}
void Passes_SetRTVs(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* r, ID3D11DepthStencilView* d) {
    if (!g_cap) return;
    std::lock_guard<std::mutex> lk(g_mx);
    CtxSt* s = Ctx(c);
    for (int i = 0; i < 8; ++i) s->rtv[i] = (r && (UINT)i < n) ? r[i] : nullptr;
    s->dsv = d;
}
void Passes_SetUAVs(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11UnorderedAccessView* const* u) {
    if (!g_cap) return;
    std::lock_guard<std::mutex> lk(g_mx);
    CtxSt* s = Ctx(c);
    for (UINT i = 0; i < n && start + i < 8; ++i) s->uav[start + i] = u ? u[i] : nullptr;
}
void Passes_OnFinish(ID3D11DeviceContext* c) {
    if (!g_cap) return;
    std::lock_guard<std::mutex> lk(g_mx);
    CtxSt* s = Ctx(c); ID3D11DeviceContext* keep = s->c; int nl = s->nlines;
    *s = {}; s->c = keep; s->nlines = nl;
}
void Passes_OnWork(ID3D11DeviceContext* c, const char* what, UINT a, UINT b) {
    if (!g_cap) return;
    std::lock_guard<std::mutex> lk(g_mx);
    CtxSt* s = Ctx(c);
    if (g_lines.size() > 60000) return;
    bool compute = what[0] == 'X';
    char kind = 0; uint64_t h = HashOf(compute ? s->cs : s->ps, &kind);
    if (h) g_usedShaders[h] = compute ? s->cs : s->ps;
    char head[128];
    int ci = (int)(s - g_ctx.data());
    snprintf(head, sizeof(head), "%05ld c%d %s(%u,%u) %s %016llx", InterlockedIncrement(&g_seq), ci, what, a, b, compute ? "CS" : "PS", (unsigned long long)h);
    std::string line = head;
    if (compute) { List(line, "srv", s->csrv, 16); List(line, "uav", s->uav, 8); }
    else { List(line, "srv", s->srv, 16); List(line, "rtv", s->rtv, 8); if (s->dsv) { char b2[24]; snprintf(b2, sizeof(b2), " dsv[T%d]", ResId(s->dsv)); line += b2; } }
    g_lines.push_back(line);
}

using DisasmFn = HRESULT(WINAPI*)(const void*, SIZE_T, UINT, const char*, ID3DBlob**);
// trigger "passes": capture the next frame
void Passes_Start() {
    std::lock_guard<std::mutex> lk(g_mx);
    g_lines.clear(); g_resId.clear(); g_resDesc.clear(); g_usedShaders.clear(); g_ctx.clear(); g_seq = 0;
    g_capFrames = 2;   // frame 1: the state tracking starts mid-frame; frame 2 is written
    g_cap = true;
    LOGI("Passes: capturing (%zu shaders known)", g_shaders.size());
}
// Present (main thread)
void Passes_OnPresent() {
    if (!g_cap) return;
    std::lock_guard<std::mutex> lk(g_mx);
    if (--g_capFrames == 1) { g_lines.clear(); g_seq = 0; return; }
    if (g_capFrames > 0) return;
    g_cap = false;
    std::wstring dir = g_modDir + L"GranblueVR_shaders\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    FILE* f = _wfopen((g_modDir + L"GranblueVR_passes.txt").c_str(), L"wb");
    if (f) {
        fprintf(f, "# seq context call shader-stage shader-hash inputs/outputs (Tn = resource, listed below)\n");
        for (auto& l : g_lines) fprintf(f, "%s\n", l.c_str());
        fprintf(f, "\n# resources\n");
        for (auto& d : g_resDesc) fprintf(f, "%s\n", d.c_str());
        fclose(f);
    }
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    DisasmFn dis = dc ? (DisasmFn)GetProcAddress(dc, "D3DDisassemble") : nullptr;
    int written = 0;
    for (auto& u : g_usedShaders) {
        auto it = g_shaders.find(u.second);
        if (it == g_shaders.end() || it->second.code.empty()) continue;
        char name[64]; snprintf(name, sizeof(name), "%016llx", (unsigned long long)u.first);
        std::wstring base = dir + std::wstring(name, name + strlen(name));
        FILE* bf = _wfopen((base + L".dxbc").c_str(), L"wb");
        if (bf) { fwrite(it->second.code.data(), 1, it->second.code.size(), bf); fclose(bf); }
        ID3DBlob* blob = nullptr;
        if (dis && SUCCEEDED(dis(it->second.code.data(), it->second.code.size(), 0, nullptr, &blob)) && blob) {
            FILE* tf = _wfopen((base + L".txt").c_str(), L"wb");
            if (tf) { fwrite(blob->GetBufferPointer(), 1, blob->GetBufferSize() > 0 ? blob->GetBufferSize() - 1 : 0, tf); fclose(tf); ++written; }
            blob->Release();
        }
    }
    LOGI("Passes: wrote %zu draws/dispatches, %zu resources, %zu shaders (%d disassembled)", g_lines.size(), g_resDesc.size(), g_usedShaders.size(), written);
}
