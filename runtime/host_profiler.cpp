#include "host_profiler.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "log.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// windows.h first
#include <dbghelp.h>

void HostProfiler::start() {
    HANDLE h = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    if (!h)
        return;
    target_ = h;
    running_ = true;
    thread_ = std::thread([this] {
        while (running_) {
            HANDLE t = static_cast<HANDLE>(target_);
            if (SuspendThread(t) != (DWORD)-1) {
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(t, &ctx)) {
                    samples_[ctx.Rip]++;
                    total_++;
                }
                ResumeThread(t);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
}

void HostProfiler::stop() {
    running_ = false;
    if (thread_.joinable())
        thread_.join();
    if (target_) {
        CloseHandle(static_cast<HANDLE>(target_));
        target_ = nullptr;
    }
}

void HostProfiler::report(int top) const {
    if (!total_)
        return;
    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    if (!SymInitialize(process, nullptr, TRUE)) {
        LOG_ERROR("hostprof", "SymInitialize failed");
        return;
    }
    std::unordered_map<std::string, u32> by_fn;
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    for (auto [rip, n] : samples_) {
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        std::string name;
        if (SymFromAddr(process, rip, &disp, sym)) {
            name = sym->Name;
        } else {
            IMAGEHLP_MODULE64 mod{};
            mod.SizeOfStruct = sizeof(mod);
            name = SymGetModuleInfo64(process, rip, &mod) ? std::string("[") + mod.ModuleName + "]" : "[unknown]";
        }
        by_fn[name] += n;
    }
    SymCleanup(process);
    std::vector<std::pair<std::string, u32>> sorted(by_fn.begin(), by_fn.end());
    std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.second > b.second; });
    LOG_INFO("hostprof", "host profile of the emulation thread (%u samples):", total_);
    for (int i = 0; i < top && i < (int)sorted.size(); i++)
        LOG_INFO("hostprof", "  %5.1f%%  %s", 100.0 * sorted[i].second / total_, sorted[i].first.c_str());
}

#else
void HostProfiler::start() {}
void HostProfiler::stop() {}
void HostProfiler::report(int) const {}
#endif
