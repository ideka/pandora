// Sampling profiler for host code (--profile, Windows only): a side thread suspends the emulation
// thread every millisecond and records its instruction pointer. At the end the samples are
// resolved to function names with DbgHelp (needs the PDB next to the executable).
#pragma once

#include <atomic>
#include <thread>
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

private:
    std::atomic<bool> running_{false};
    std::thread thread_;
    void* target_ = nullptr;  // HANDLE of the sampled thread
    std::unordered_map<u64, u32> samples_;
    u32 total_ = 0;
};
