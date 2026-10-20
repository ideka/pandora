#include "log.h"

#include <cstdarg>
#include <mutex>

namespace {
std::mutex g_mutex;
std::FILE* g_file = nullptr;
}  // namespace

bool log_open_file(const char* path) {
    std::lock_guard lock(g_mutex);
    if (g_file)
        std::fclose(g_file);
    g_file = std::fopen(path, "w");
    return g_file != nullptr;
}

void log_write(const char* tag, const char* fmt, ...) {
    char message[4096];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(message, sizeof message, fmt, args);
    va_end(args);
    std::lock_guard lock(g_mutex);
    std::fprintf(stderr, "[%s] %s\n", tag, message);
    if (g_file) {
        std::fprintf(g_file, "[%s] %s\n", tag, message);
        std::fflush(g_file);  // keep everything up to a crash
    }
}
