#include "crash.h"

#include <atomic>

#include "log.h"
#include "runtime.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// windows.h first
#include <dbghelp.h>
#endif

namespace crash {
namespace {

std::string g_dump_path;
Runtime* g_rt = nullptr;
std::atomic<bool> g_reported{false};

#ifdef _WIN32
void write_dump(EXCEPTION_POINTERS* info) {
    HANDLE file = CreateFileA(g_dump_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), info, FALSE};
    const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                                      static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules),
                                      info ? &exception : nullptr, nullptr, nullptr);
    CloseHandle(file);
    if (ok)
        log_write("crash", "wrote %s", g_dump_path.c_str());
}

LONG WINAPI on_exception(EXCEPTION_POINTERS* info) {
    char reason[96];
    std::snprintf(reason, sizeof reason, "unhandled exception %08lX at %p", info->ExceptionRecord->ExceptionCode,
                  info->ExceptionRecord->ExceptionAddress);
    if (!g_reported.exchange(true)) {
        log_write("crash", "%s", reason);
        if (g_rt) {
            log_write("crash", "guest at %s", g_rt->describe_pc().c_str());
            log_write("crash", "guest backtrace:\n%s", g_rt->backtrace(16).c_str());
        }
        write_dump(info);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

}  // namespace

void install(const std::string& dir, Runtime* rt) {
    g_dump_path = dir + "/pandora.dmp";
    g_rt = rt;
#ifdef _WIN32
    SetUnhandledExceptionFilter(&on_exception);
#endif
}

void report(const char* reason) {
    if (g_reported.exchange(true))
        return;
    log_write("crash", "%s", reason);
    if (g_rt) {
        log_write("crash", "guest at %s", g_rt->describe_pc().c_str());
        log_write("crash", "guest backtrace:\n%s", g_rt->backtrace(16).c_str());
    }
#ifdef _WIN32
    write_dump(nullptr);
#endif
}

}  // namespace crash
