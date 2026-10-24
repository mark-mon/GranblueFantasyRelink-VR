// Entry point: loads as dinput8.dll next to the game exe, forwards DirectInput to the
// real system DLL, and installs the D3D11 hooks on a worker thread.
#include <windows.h>
#include <unknwn.h>
#include "globals.h"

Config g_cfg;
std::wstring g_modDir;

#define GBVR_VERSION "1.0.1"
const char* kModVersion = "1.0.1";

// ---------------- dinput8 proxy ----------------
static HMODULE g_realDI = nullptr;
static FARPROC RealProc(const char* name) {
    if (!g_realDI) {
        wchar_t p[MAX_PATH];
        GetSystemDirectoryW(p, MAX_PATH);
        wcscat_s(p, L"\\dinput8.dll");
        g_realDI = LoadLibraryW(p);
        if (!g_realDI) LOGE("Failed to load system dinput8.dll (err %lu)", GetLastError());
    }
    return g_realDI ? GetProcAddress(g_realDI, name) : nullptr;
}

extern "C" {
HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE h, DWORD v, REFIID r, LPVOID* o, IUnknown* u) {
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, IUnknown*);
    auto f = (Fn)RealProc("DirectInput8Create");
    return f ? f(h, v, r, o, u) : E_FAIL;
}
HRESULT WINAPI Proxy_DllCanUnloadNow() {
    using Fn = HRESULT(WINAPI*)();
    auto f = (Fn)RealProc("DllCanUnloadNow");
    return f ? f() : S_FALSE;
}
HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID c, REFIID r, LPVOID* o) {
    using Fn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
    auto f = (Fn)RealProc("DllGetClassObject");
    return f ? f(c, r, o) : CLASS_E_CLASSNOTAVAILABLE;
}
HRESULT WINAPI Proxy_DllRegisterServer() {
    using Fn = HRESULT(WINAPI*)();
    auto f = (Fn)RealProc("DllRegisterServer");
    return f ? f() : E_FAIL;
}
HRESULT WINAPI Proxy_DllUnregisterServer() {
    using Fn = HRESULT(WINAPI*)();
    auto f = (Fn)RealProc("DllUnregisterServer");
    return f ? f() : E_FAIL;
}
const void* WINAPI Proxy_GetdfDIJoystick() {
    using Fn = const void*(WINAPI*)();
    auto f = (Fn)RealProc("GetdfDIJoystick");
    return f ? f() : nullptr;
}
}

// ---------------- crash logging ----------------
static HMODULE g_self = nullptr;
static LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;
static volatile LONG g_firstChanceLogged = 0, g_foreignAvLogged = 0;

static void ModOffset(uintptr_t a, char* out, size_t n) {
    HMODULE m = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)a, &m) && m) {
        wchar_t w[MAX_PATH]; GetModuleFileNameW(m, w, MAX_PATH);
        const wchar_t* b = wcsrchr(w, L'\\'); b = b ? b + 1 : w;
        char nb[MAX_PATH]; WideCharToMultiByte(CP_UTF8, 0, b, -1, nb, sizeof(nb), nullptr, nullptr);
        snprintf(out, n, "%s+0x%llx", nb, (unsigned long long)(a - (uintptr_t)m));
    } else snprintf(out, n, "0x%llx", (unsigned long long)a);
}

static void LogException(const char* kind, EXCEPTION_POINTERS* ep) {
    char where[300];
    ModOffset((uintptr_t)ep->ExceptionRecord->ExceptionAddress, where, sizeof(where));
    LOGE("%s: code 0x%08lx at %s", kind, ep->ExceptionRecord->ExceptionCode, where);
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
        LOGE("  %s address 0x%llx", ep->ExceptionRecord->ExceptionInformation[0] ? "write to" : "read from",
             (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    CONTEXT c = *ep->ContextRecord;
    __try {
        for (int i = 0; i < 16; ++i) {
            DWORD64 base = 0;
            PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
            if (!fe) { c.Rip = *(DWORD64*)c.Rsp; c.Rsp += 8; }
            else { PVOID hd; DWORD64 ef; RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fe, &c, &hd, &ef, nullptr); }
            if (!c.Rip) break;
            ModOffset((uintptr_t)c.Rip, where, sizeof(where));
            LOGE("  #%d %s", i, where);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void LogStack(const char* title, CONTEXT c) {
    char where[300];
    LOGE("%s", title);
    __try {
        for (int i = 0; i < 24; ++i) {
            ModOffset((uintptr_t)c.Rip, where, sizeof(where));
            LOGE("  #%d %s", i, where);
            DWORD64 base = 0;
            PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
            if (!fe) { c.Rip = *(DWORD64*)c.Rsp; c.Rsp += 8; }
            else { PVOID hd; DWORD64 ef; RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fe, &c, &hd, &ef, nullptr); }
            if (!c.Rip) break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

#include <tlhelp32.h>
// Log the stacks of every other game thread that is inside the game exe (for deadlock analysis).
static void DumpAllThreads(DWORD skip) {
    HMODULE exe = GetModuleHandleW(nullptr);
    uintptr_t eb = (uintptr_t)exe, ee = eb + ((IMAGE_NT_HEADERS*)(eb + ((IMAGE_DOS_HEADER*)exe)->e_lfanew))->OptionalHeader.SizeOfImage;
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te = { sizeof(te) };
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self || te.th32ThreadID == skip) continue;
        HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!th) continue;
        if (SuspendThread(th) != (DWORD)-1) {
            CONTEXT c = {}; c.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(th, &c)) {
                uintptr_t fr[14]; int n = 0; bool inExe = false;
                CONTEXT k = c;
                __try {
                    for (; n < 14; ++n) {
                        fr[n] = (uintptr_t)k.Rip;
                        if (fr[n] >= eb && fr[n] < ee && fr[n] < eb + 0x4700000) inExe = true;
                        DWORD64 base = 0;
                        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(k.Rip, &base, nullptr);
                        if (!fe) { k.Rip = *(DWORD64*)k.Rsp; k.Rsp += 8; }
                        else { PVOID hd; DWORD64 ef; RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, k.Rip, fe, &k, &hd, &ef, nullptr); }
                        if (!k.Rip) { ++n; break; }
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
                if (inExe) {
                    char line[1024]; int o = snprintf(line, sizeof(line), "  thread t%05lu:", te.th32ThreadID);
                    for (int i = 0; i < n && o < (int)sizeof(line) - 40; ++i) {
                        if (fr[i] >= eb && fr[i] < ee) o += snprintf(line + o, sizeof(line) - o, " exe+%llx", (unsigned long long)(fr[i] - eb));
                        else o += snprintf(line + o, sizeof(line) - o, " .");
                    }
                    LOGE("%s", line);
                }
            }
            ResumeThread(th);
        }
        CloseHandle(th);
    }
    CloseHandle(snap);
}

// Hang watchdog: the render thread calls Watchdog_Beat() every frame; if it stops for 4 s,
// log its call stack (once per hang).
static volatile DWORD g_beatTick = 0, g_beatTid = 0;
void Watchdog_Beat() { g_beatTid = GetCurrentThreadId(); g_beatTick = GetTickCount(); }
static DWORD WINAPI WatchdogThread(LPVOID) {
    bool reported = false;
    for (;;) {
        Sleep(100);
        DWORD t = g_beatTick, tid = g_beatTid;
        if (!t || !tid) continue;
        static int slowReports = 0;
        DWORD stall = GetTickCount() - t;
        if (stall < 700) { reported = false; continue; }
        if (reported) continue;
        if (stall < 4000 && slowReports >= 2) continue;
        if (stall < 4000) ++slowReports;
        reported = true;
        HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION, FALSE, tid);
        if (!th) continue;
        if (SuspendThread(th) != (DWORD)-1) {
            CONTEXT c = {}; c.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(th, &c)) {
                char title[96]; snprintf(title, sizeof(title), "HANG: render thread t%05lu stuck for %lu ms; stack:", tid, GetTickCount() - t);
                LogStack(title, c);
            }
            ResumeThread(th);
        }
        CloseHandle(th);
        DumpAllThreads(tid);
    }
}

// Minidump of the first access violation in the game (written next to the DLL).
static void WriteCrashDump(EXCEPTION_POINTERS* ep) {
    typedef BOOL(WINAPI* MdwdFn)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
    HMODULE dbg = LoadLibraryW(L"dbghelp.dll");
    MdwdFn fn = dbg ? (MdwdFn)GetProcAddress(dbg, "MiniDumpWriteDump") : nullptr;
    if (!fn) return;
    std::wstring p = g_modDir + L"GranblueVR_crash.dmp";
    HANDLE f = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
#pragma pack(push, 4)
    struct Mei { DWORD tid; EXCEPTION_POINTERS* ep; BOOL client; };   // MINIDUMP_EXCEPTION_INFORMATION (pack 4)
#pragma pack(pop)
    Mei mei = { GetCurrentThreadId(), ep, FALSE };
    // MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules
    BOOL ok = fn(GetCurrentProcess(), GetCurrentProcessId(), f, 0x40 | 0x1 | 0x1000 | 0x20, &mei, nullptr, nullptr);
    CloseHandle(f);
    LOGE("Crash dump %s: GranblueVR_crash.dmp", ok ? "written" : "FAILED");
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    LogException("UNHANDLED EXCEPTION (crash)", ep);
    return g_prevFilter ? g_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

// First-chance: log access violations raised inside our own DLL (the game may swallow them).
static LONG CALLBACK FirstChance(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW) return EXCEPTION_CONTINUE_SEARCH;
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)ep->ExceptionRecord->ExceptionAddress, &m);
    bool ours = m == g_self;
    if (ours || code == EXCEPTION_STACK_OVERFLOW) {
        if (InterlockedIncrement(&g_firstChanceLogged) <= 8) LogException(ours ? "First-chance exception in GranblueVR" : "Stack overflow", ep);
    } else if (InterlockedIncrement(&g_foreignAvLogged) <= 4) {
        LogException("First-chance exception in game (may be handled)", ep);
        if (g_foreignAvLogged == 1) WriteCrashDump(ep);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------- startup ----------------
static DWORD WINAPI InitThread(LPVOID) {
    LOGI("Init thread started");
    RDoc_Load();
    if (!InstallD3DHooks()) LOGE("D3D hook installation failed; VR will not start");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(inst, path, MAX_PATH);
        g_modDir = path;
        g_modDir = g_modDir.substr(0, g_modDir.find_last_of(L"\\/") + 1);

        void Perf_Init(); Perf_Init();
        LoadConfig(g_modDir + L"config.txt", g_cfg);
        LogOpen((g_modDir + L"GranblueVR.log").c_str(), g_cfg.logLevel);
        LOGI("GranblueVR %s loaded (log level %s)", GBVR_VERSION, LogLevelName(g_cfg.logLevel));
        if (g_cfg.createdDefault) LOGI("config.txt not found; wrote defaults");
        for (auto& m : g_cfg.messages) LOGE("%s", m.c_str());
        LOGI("Config: vr_enabled=%d vr_mode=%s screen_distance=%.2f screen_width=%.2f render_scale=%.2f hud_distance=%.2f hud_size=%.2f recenter_key=F%d",
             g_cfg.vrEnabled, g_cfg.vrMode.c_str(), g_cfg.screenDistance, g_cfg.screenWidth, g_cfg.renderScale,
             g_cfg.hudDistance, g_cfg.hudSize, g_cfg.recenterKey - VK_F1 + 1);
        LOGI("Config: head_tracking=%d world_scale=%.2f debug_fake_head=%.1f", g_cfg.headTracking, g_cfg.worldScale, g_cfg.debugFakeHead);
        if (g_cfg.vrMode == "aer") LOGI("vr_mode 'aer' is not implemented (native stereo is the target); using stereo");

        g_self = inst;
        g_prevFilter = SetUnhandledExceptionFilter(CrashFilter);
        AddVectoredExceptionHandler(0, FirstChance);
        if (HANDLE w = CreateThread(nullptr, 0, WatchdogThread, nullptr, 0, nullptr)) CloseHandle(w);
        HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
