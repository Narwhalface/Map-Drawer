// Lightweight logger: writes timestamped lines to stdout and a log file,
// flushing after every call so the file can be tailed live while the app runs.
#pragma once

enum class LogLevel { Info, Warn, Error };

// Opens the log file (truncating any previous contents). Returns true on success.
bool LoggerInit(const char *path);
void LoggerShutdown();
void LoggerLog(LogLevel level, const char *fmt, ...);

#define LOG_INFO(...) LoggerLog(LogLevel::Info, __VA_ARGS__)
#define LOG_WARN(...) LoggerLog(LogLevel::Warn, __VA_ARGS__)
#define LOG_ERROR(...) LoggerLog(LogLevel::Error, __VA_ARGS__)
