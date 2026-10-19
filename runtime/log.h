// Minimal logging. Verbosity: 0 = errors, 1 = info (default), 2 = debug, 3 = trace.
#pragma once

#include <cstdio>

extern int g_log_level;

#define LOG_AT(level, tag, ...)                                  \
    do {                                                         \
        if (g_log_level >= (level)) {                            \
            std::fprintf(stderr, "[%s] ", tag);                  \
            std::fprintf(stderr, __VA_ARGS__);                   \
            std::fputc('\n', stderr);                            \
        }                                                        \
    } while (0)

#define LOG_ERROR(tag, ...) LOG_AT(0, tag, __VA_ARGS__)
#define LOG_INFO(tag, ...) LOG_AT(1, tag, __VA_ARGS__)
#define LOG_DEBUG(tag, ...) LOG_AT(2, tag, __VA_ARGS__)
#define LOG_TRACE(tag, ...) LOG_AT(3, tag, __VA_ARGS__)
