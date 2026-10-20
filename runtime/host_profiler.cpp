#include "host_profiler.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "log.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// windows.h first
#include <dbghelp.h>
#include <tlhelp32.h>

// Unwinds the suspended thread's stack to the first frame inside the executable.
void HostProfiler::record_outside_caller(const void* context) {
    CONTEXT c = *static_cast<const CONTEXT*>(context);
    int inside = 0;  // in-module frames seen: the first is recorded as the caller, the second as its caller
    for (int depth = 0; depth < 48 && c.Rip; depth++) {
        if (c.Rip >= module_lo_ && c.Rip < module_hi_) {
            if (inside == 0)
                outside_callers_[c.Rip]++;
            else {
                outside_callers2_[c.Rip]++;
                return;
            }
            inside++;
        }
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &image_base, nullptr);
        if (!fn) {  // leaf function: the return address is on top of the stack
            c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
            c.Rsp += 8;
            continue;
        }
        void* handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c.Rip, fn, &c, &handler_data, &establisher, nullptr);
    }
}

void HostProfiler::start() {
    {
        HMODULE exe = GetModuleHandleW(nullptr);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const u8*>(exe) + dos->e_lfanew);
        module_lo_ = reinterpret_cast<u64>(exe);
        module_hi_ = module_lo_ + nt->OptionalHeader.SizeOfImage;
    }
    HANDLE h = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    if (!h)
        return;
    target_ = h;
    running_ = true;
    const bool all_threads = std::getenv("PANDORA_PROFILE_THREADS") != nullptr;
    const DWORD self_tid = GetCurrentThreadId();
    thread_ = std::thread([this, all_threads, self_tid] {
        const DWORD sampler_tid = GetCurrentThreadId();
        std::vector<std::pair<DWORD, HANDLE>> others;
        for (u32 iteration = 0; running_; iteration++) {
            if (paused_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            // PANDORA_PROFILE_THREADS: also sample the process's other threads (render worker etc.).
            if (all_threads && iteration % 500 == 0) {
                for (auto& [tid, handle] : others)
                    CloseHandle(handle);
                others.clear();
                HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                THREADENTRY32 te{sizeof te};
                for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                    if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self_tid ||
                        te.th32ThreadID == sampler_tid)
                        continue;
                    if (HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                                               FALSE, te.th32ThreadID))
                        others.emplace_back(te.th32ThreadID, th);
                }
                CloseHandle(snap);
            }
            for (auto& [tid, th] : others) {
                if (SuspendThread(th) == (DWORD)-1)
                    continue;
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(th, &ctx)) {
                    other_samples_[tid][ctx.Rip]++;
                    other_totals_[tid]++;
                    if (other_names_.find(tid) == other_names_.end()) {
                        PWSTR desc = nullptr;
                        std::string name;
                        if (SUCCEEDED(GetThreadDescription(th, &desc)) && desc) {
                            for (PWSTR q = desc; *q; q++)
                                name += (char)*q;
                            LocalFree(desc);
                        }
                        other_names_[tid] = name;
                    }
                }
                ResumeThread(th);
            }
            HANDLE t = static_cast<HANDLE>(target_);
            if (SuspendThread(t) != (DWORD)-1) {
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(t, &ctx)) {
                    samples_[ctx.Rip]++;
                    total_++;
                    if (ctx.Rip < module_lo_ || ctx.Rip >= module_hi_)
                        record_outside_caller(&ctx);
                }
                ResumeThread(t);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for (auto& [tid, handle] : others)
            CloseHandle(handle);
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
    // Other threads: the busiest ones (samples outside wait functions), each with its hot spots.
    std::vector<std::pair<u32, std::string>> thread_reports;
    for (auto& [tid, rips] : other_samples_) {
        std::unordered_map<std::string, u32> fns;
        u32 busy = 0;
        for (auto [rip, n] : rips) {
            auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;
            DWORD64 disp = 0;
            std::string name = SymFromAddr(process, rip, &disp, sym) ? sym->Name : "[unknown]";
            const bool waiting = name.find("Wait") != std::string::npos || name.find("Delay") != std::string::npos ||
                                 name.find("RemoveIoCompletion") != std::string::npos ||
                                 name.find("AlertByThreadId") != std::string::npos;
            if (!waiting)
                busy += n;
            fns[name] += n;
        }
        std::vector<std::pair<std::string, u32>> top_fns(fns.begin(), fns.end());
        std::sort(top_fns.begin(), top_fns.end(), [](auto& a, auto& b) { return a.second > b.second; });
        std::string report;
        char line[400];
        std::snprintf(line, sizeof line, "thread %lu \"%s\": %u samples, %.1f%% busy", (unsigned long)tid,
                      other_names_.at(tid).c_str(), other_totals_.at(tid), 100.0 * busy / std::max(1u, other_totals_.at(tid)));
        report += line;
        for (size_t i = 0; i < top_fns.size() && i < 12; i++) {
            std::snprintf(line, sizeof line, "\n[hostprof]       %5.1f%%  %s", 100.0 * top_fns[i].second / other_totals_.at(tid),
                          top_fns[i].first.c_str());
            report += line;
        }
        thread_reports.emplace_back(busy, report);
    }
    std::sort(thread_reports.rbegin(), thread_reports.rend());
    for (size_t i = 0; i < thread_reports.size() && i < 4; i++)
        LOG_INFO("hostprof", "%s", thread_reports[i].second.c_str());

    std::unordered_map<std::string, u32> by_caller, by_caller2;
    for (auto [rip, n] : outside_callers_) {
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        by_caller[SymFromAddr(process, rip, &disp, sym) ? sym->Name : "[unknown]"] += n;
    }
    for (auto [rip, n] : outside_callers2_) {
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        by_caller2[SymFromAddr(process, rip, &disp, sym) ? sym->Name : "[unknown]"] += n;
    }
    // PANDORA_PROFILE_DUMP=<file>: every sampled address, for instruction-level analysis
    // (tools/hotspots.py): "<rva> <count> <function>+<disp> <file>:<line>".
    if (const char* dump = std::getenv("PANDORA_PROFILE_DUMP")) {
        if (FILE* f = std::fopen(dump, "w")) {
            std::fprintf(f, "# total %u samples, image base rva 0\n", total_);
            for (auto [rip, n] : samples_) {
                if (rip < module_lo_ || rip >= module_hi_)
                    continue;
                auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
                sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                sym->MaxNameLen = 255;
                DWORD64 disp = 0;
                const char* name = SymFromAddr(process, rip, &disp, sym) ? sym->Name : "?";
                IMAGEHLP_LINE64 line{};
                line.SizeOfStruct = sizeof(line);
                DWORD ldisp = 0;
                const bool has_line = SymGetLineFromAddr64(process, rip, &ldisp, &line);
                std::fprintf(f, "%llx %u %s+%llx %s:%lu\n", (unsigned long long)(rip - module_lo_), n, name,
                             (unsigned long long)disp, has_line ? line.FileName : "?", has_line ? line.LineNumber : 0ul);
            }
            std::fclose(f);
        }
    }
    // PANDORA_PROFILE_LINES=<n>: the hottest source lines; for the recompiled chunks, with the guest
    // instruction they implement (the nearest "// <address>: <instruction>" comment above).
    std::vector<std::pair<u32, std::string>> hot_lines;
    if (const char* env = std::getenv("PANDORA_PROFILE_LINES")) {
        std::unordered_map<std::string, u32> by_line;
        for (auto [rip, n] : samples_) {
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD disp = 0;
            if (SymGetLineFromAddr64(process, rip, &disp, &line))
                by_line[std::string(line.FileName) + ":" + std::to_string(line.LineNumber)] += n;
        }
        std::vector<std::pair<std::string, u32>> lines(by_line.begin(), by_line.end());
        std::sort(lines.begin(), lines.end(), [](auto& a, auto& b) { return a.second > b.second; });
        std::unordered_map<std::string, std::vector<std::string>> files;
        for (size_t i = 0; i < lines.size() && i < (size_t)std::atoi(env); i++) {
            const std::string& key = lines[i].first;
            const size_t colon = key.rfind(':');
            const std::string file = key.substr(0, colon);
            const int no = std::atoi(key.c_str() + colon + 1);
            auto [it, fresh] = files.try_emplace(file);
            if (fresh) {
                if (FILE* f = std::fopen(file.c_str(), "rb")) {
                    std::string cur;
                    for (int ch; (ch = std::fgetc(f)) != EOF;) {
                        if (ch == '\n') {
                            it->second.push_back(cur);
                            cur.clear();
                        } else if (ch != '\r') {
                            cur += (char)ch;
                        }
                    }
                    std::fclose(f);
                }
            }
            const auto& text = it->second;
            std::string code = no >= 1 && no <= (int)text.size() ? text[no - 1] : "";
            std::string insn;
            for (int k = no - 1; k >= 0 && k > no - 40 && k < (int)text.size(); k--)
                if (text[k].find("    // ") == 0 && text[k].size() > 16 && text[k][15] == ':') {
                    insn = text[k].substr(7);
                    break;
                }
            const size_t slash = file.find_last_of("\\/");
            char out[600];
            std::snprintf(out, sizeof out, "  %5.2f%%  %s:%d  %-40.40s | %s", 100.0 * lines[i].second / total_,
                          file.substr(slash + 1).c_str(), no, insn.c_str(), code.c_str());
            hot_lines.emplace_back(lines[i].second, out);
        }
    }
    SymCleanup(process);
    if (!hot_lines.empty()) {
        LOG_INFO("hostprof", "hottest source lines:");
        for (auto& [n, text] : hot_lines)
            LOG_INFO("hostprof", "%s", text.c_str());
    }
    std::vector<std::pair<std::string, u32>> sorted(by_fn.begin(), by_fn.end());
    std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.second > b.second; });
    LOG_INFO("hostprof", "host profile of the emulation thread (%u samples):", total_);
    for (int i = 0; i < top && i < (int)sorted.size(); i++)
        LOG_INFO("hostprof", "  %5.1f%%  %s", 100.0 * sorted[i].second / total_, sorted[i].first.c_str());
    std::vector<std::pair<std::string, u32>> callers(by_caller.begin(), by_caller.end());
    std::sort(callers.begin(), callers.end(), [](auto& a, auto& b) { return a.second > b.second; });
    LOG_INFO("hostprof", "time outside the executable, by nearest caller inside it:");
    for (int i = 0; i < 15 && i < (int)callers.size(); i++)
        LOG_INFO("hostprof", "  %5.1f%%  %s", 100.0 * callers[i].second / total_, callers[i].first.c_str());
    std::vector<std::pair<std::string, u32>> callers2(by_caller2.begin(), by_caller2.end());
    std::sort(callers2.begin(), callers2.end(), [](auto& a, auto& b) { return a.second > b.second; });
    LOG_INFO("hostprof", "  ... and by that caller's caller:");
    for (int i = 0; i < 15 && i < (int)callers2.size(); i++)
        LOG_INFO("hostprof", "  %5.1f%%  %s", 100.0 * callers2[i].second / total_, callers2[i].first.c_str());
}

#else
void HostProfiler::start() {}
void HostProfiler::stop() {}
void HostProfiler::report(int) const {}
#endif
