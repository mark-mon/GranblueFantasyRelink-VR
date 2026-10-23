#include "log.h"
#include <windows.h>
#include <cstdio>
#include <share.h>
#include <cstdarg>

static CRITICAL_SECTION g_cs;
static FILE* g_file = nullptr;
static LogLevel g_level = LogLevel::Debug;
static bool g_init = false;

const char* LogLevelName(LogLevel l) {
    switch (l) {
    case LogLevel::None: return "none";
    case LogLevel::Error: return "error";
    case LogLevel::Info: return "info";
    case LogLevel::Debug: return "debug";
    }
    return "?";
}

void LogOpen(const wchar_t* path, LogLevel level) {
    if (!g_init) { InitializeCriticalSection(&g_cs); g_init = true; }
    g_level = level;
    if (level == LogLevel::None) return;
    g_file = _wfsopen(path, L"w", _SH_DENYWR);
}

void LogSetLevel(LogLevel level) { g_level = level; }
LogLevel LogGetLevel() { return g_level; }

void LogWrite(LogLevel level, const char* fmt, ...) {
    if (!g_init || !g_file || level == LogLevel::None || level > g_level) return;
    char msg[2048];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    SYSTEMTIME t; GetLocalTime(&t);
    static const char* tags[] = { "", "ERROR", "INFO ", "DEBUG" };
    EnterCriticalSection(&g_cs);
    fprintf(g_file, "%02d:%02d:%02d.%03d [%s] [t%05lu] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
            tags[(int)level], GetCurrentThreadId(), msg);
    fflush(g_file);
    LeaveCriticalSection(&g_cs);
}
