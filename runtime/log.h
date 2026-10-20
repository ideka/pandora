// Minimal logging. Verbosity: 0 = errors, 1 = info (default), 2 = debug, 3 = trace. Messages go to
// stderr and, once opened, to a log file (overwritten each run, flushed per message).
#pragma once

#include <cstdio>

extern int g_log_level;

// Writes "[tag] message" (printf-style) to stderr and the log file. Thread-safe.
void log_write(const char* tag, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
bool log_open_file(const char* path);

#define LOG_AT(level, tag, ...)              \
    do {                                     \
        if (g_log_level >= (level))          \
            log_write(tag, __VA_ARGS__);     \
    } while (0)

#define LOG_ERROR(tag, ...) LOG_AT(0, tag, __VA_ARGS__)
#define LOG_INFO(tag, ...) LOG_AT(1, tag, __VA_ARGS__)
#define LOG_DEBUG(tag, ...) LOG_AT(2, tag, __VA_ARGS__)
#define LOG_TRACE(tag, ...) LOG_AT(3, tag, __VA_ARGS__)
