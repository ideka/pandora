// Sampling profiler for host code (--profile, Windows only): a side thread suspends the emulation
// thread every millisecond and records its instruction pointer. At the end the samples are
// resolved to function names with DbgHelp (needs the PDB next to the executable).
#pragma once

#include <atomic>
#include <thread>
#include <string>
#include <unordered_map>

#include "guest.h"

class HostProfiler {
public:
    ~HostProfiler() { stop(); }
    // Start sampling the calling thread.
    void start();
    void stop();
    // Log the hottest functions.
    void report(int top = 40) const;
    // While paused no samples are taken (PANDORA_PROFILE_FROM profiles from a given frame).
    void set_paused(bool paused) { paused_ = paused; }

private:
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    std::thread thread_;
    void* target_ = nullptr;  // HANDLE of the sampled thread
    u64 module_lo_ = 0, module_hi_ = 0;  // the executable's image
    void record_outside_caller(const void* context);  // a CONTEXT
    std::unordered_map<u64, u32> samples_;
    // For samples outside the executable (system calls, drivers): the nearest return addresses
    // inside it, so waits can be attributed to their callers.
    std::unordered_map<u64, u32> outside_callers_;
    std::unordered_map<u64, u32> outside_callers2_;  // and the frame above that one
    // PANDORA_PROFILE_THREADS: samples of the process's other threads, per thread id.
    std::unordered_map<u32, std::unordered_map<u64, u32>> other_samples_;
    std::unordered_map<u32, u32> other_totals_;
    std::unordered_map<u32, std::string> other_names_;
    u32 total_ = 0;
};
