#include "logger.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>

namespace {

FILE *gLogFile = nullptr;

const char *LevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "?";
}

} // namespace

bool LoggerInit(const char *path) {
#ifdef _MSC_VER
    fopen_s(&gLogFile, path, "w");
#else
    gLogFile = std::fopen(path, "w");
#endif
    return gLogFile != nullptr;
}

void LoggerShutdown() {
    if (gLogFile) {
        std::fclose(gLogFile);
        gLogFile = nullptr;
    }
}

void LoggerLog(LogLevel level, const char *fmt, ...) {
    std::time_t now = std::time(nullptr);
    std::tm localNow{};
    localtime_s(&localNow, &now);
    char timeBuf[16];
    std::strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &localNow);

    char message[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    std::fprintf(stdout, "[%s] [%s] %s\n", timeBuf, LevelName(level), message);
    std::fflush(stdout);

    if (gLogFile) {
        std::fprintf(gLogFile, "[%s] [%s] %s\n", timeBuf, LevelName(level), message);
        std::fflush(gLogFile);
    }
}
