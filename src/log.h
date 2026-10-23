#pragma once
enum class LogLevel { None = 0, Error = 1, Info = 2, Debug = 3 };

void LogOpen(const wchar_t* path, LogLevel level);
void LogSetLevel(LogLevel level);
LogLevel LogGetLevel();
void LogWrite(LogLevel level, const char* fmt, ...);
const char* LogLevelName(LogLevel level);

#define LOGE(...) LogWrite(LogLevel::Error, __VA_ARGS__)
#define LOGI(...) LogWrite(LogLevel::Info, __VA_ARGS__)
#define LOGD(...) LogWrite(LogLevel::Debug, __VA_ARGS__)
