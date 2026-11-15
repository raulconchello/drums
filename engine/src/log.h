// Minimal stderr logging for non-real-time code. Never call from the audio callback.
#pragma once

#include <cstdarg>
#include <cstdio>

namespace drums::engine {

#if defined(__GNUC__)
#define DRUMS_PRINTF_FORMAT __attribute__((format(printf, 1, 2)))
#else
#define DRUMS_PRINTF_FORMAT
#endif

inline void logv(const char* level, const char* fmt, va_list args) {
    std::fprintf(stderr, "[drums-engine] %s", level);
    std::vfprintf(stderr, fmt, args);
    std::fputc('\n', stderr);
}

DRUMS_PRINTF_FORMAT inline void logInfo(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    logv("", fmt, a);
    va_end(a);
}

DRUMS_PRINTF_FORMAT inline void logWarn(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    logv("warning: ", fmt, a);
    va_end(a);
}

DRUMS_PRINTF_FORMAT inline void logError(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    logv("error: ", fmt, a);
    va_end(a);
}

}  // namespace drums::engine
