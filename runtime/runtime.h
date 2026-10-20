// Owns the guest CPU and drives the recompiled code.
#pragma once

#include <array>
#include <atomic>
#include <thread>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "audio_dump.h"
#include "audio_out.h"
#include "host_profiler.h"
#include "input.h"
#include "guest.h"
#include "symbols.h"

class AuroraBridge;
class Hardware;
class Ios;

struct Options {
    std::filesystem::path disc;  // extracted game partition (contains sys/ and files/)
    std::filesystem::path nand = "build/nand";  // emulated NAND (saves, SYSCONF)
    std::string map = "build/SX3P01.map";
    std::string hooks = "config/SX3P01/hooks.txt";  // functions marked for hooking at recompile time
    u64 max_blocks = 0;          // stop after this many blocks (0 = unlimited)
    bool trace_calls = false;    // log every block entry
    std::string audio_dump;      // write mixed audio to this WAV file
    std::vector<std::string> watch;  // log arguments when these functions are dispatched
    std::vector<std::pair<u32, u32>> dumps;  // memory ranges to hexdump at halt
    std::vector<u32> watch_writes;           // log every store to these MEM1 addresses
    struct FileDump {
        u32 addr, len;
        std::string path;
    };
    std::vector<FileDump> file_dumps;        // guest memory ranges written to files when stopping
    bool profile = false;                    // sample the guest pc and report hot functions
    std::vector<std::pair<u64, std::string>> screenshots;  // (guest frame, PNG path), windowed only
    u64 exit_after_frames = 0;               // windowed: quit after this many guest frames
    struct CopyShot {
        u64 frame;
        u32 index;
        std::string path;
    };
    std::vector<CopyShot> copy_shots;
    std::vector<CopyShot> draw_shots;        // capture the EFB before a draw (debugging)        // read back a guest EFB texture copy (debugging)
};

class Runtime {
public:
    using Hook = std::function<void(Runtime&)>;

    explicit Runtime(Options opts);
    ~Runtime();

    bool boot();  // boot.cpp
    int run();    // headless: run until halted

    // Windowed: start(), then run_frame() once per host frame, then finish().
    void start();
    bool run_frame();
    int finish();
    bool step();

    // Send GPU commands to aurora for rendering.
    void enable_rendering();
    // Play the guest's audio on the host's default device.
    bool enable_audio_output();
    void disable_audio_output() { audio_out_.close(); }
    void set_audio_volume(float volume) { audio_out_.set_volume(volume); }
    int audio_queued_ms() const { return audio_out_.queued_ms(); }
    AuroraBridge* bridge() { return bridge_.get(); }
    Input& input() { return input_; }
    std::string backtrace(int max_frames) const;

    // Replace the guest function at addr (or named) with host code. The hook must leave
    // cpu.pc at the next guest address, usually via ret().
    void hook(u32 addr, Hook fn);
    bool hook(std::string_view name, Hook fn);
    void ret(u32 r3) {
        cpu.gpr[3] = r3;
        cpu.pc = cpu.lr;
    }

    void halt(std::string reason);
    std::string describe(u32 addr) const { return syms_.describe(addr); }
    std::string describe_pc() const { return describe(cpu.pc); }
    // --profile: pause/resume both samplers (PANDORA_PROFILE_FROM=<frame>).
    void set_profiling_paused(bool paused) {
        profile_paused_ = paused;
        host_profiler_.set_paused(paused);
    }

    Hardware& hw() { return *hw_; }
    Ios& ios() { return *ios_; }
    const Symbols& syms() const { return syms_; }
    const Options& options() const { return opts_; }

    CPUState cpu{};

private:
    static bool host_call(CPUState* c, u32 addr);
    static u64 external_read(CPUState* c, u32 addr, u8 size);
    static void external_write(CPUState* c, u32 addr, u64 value, u8 size);
    static u32 spr_read(CPUState* c, u16 spr, u32 cia);
    static void spr_write(CPUState* c, u16 spr, u32 value, u32 cia);
    static void instruction_fallback(CPUState* c, u32 raw, u32 cia);

    void install_hle();  // hle.cpp
    bool check_hooks() const;
    void exception_entry(u32 vector);
    bool run_copied_code(u32 pc);
    void advance(s64 cycles);
    void deliver_interrupts();
    void find_idle_loop();
    void dump() const;
    static void on_mem1_write(u32 offset, u32 size, void* self);

    Options opts_;
    Symbols syms_;
    std::unique_ptr<Hardware> hw_;
    std::unique_ptr<Ios> ios_;
    AudioDump audio_dump_;
    AudioOut audio_out_;
    static void audio_sink(void* self, const u8* block);
    Input input_;
    std::unique_ptr<AuroraBridge> bridge_;
    std::atomic<bool> profiling_{false};
    std::thread sampler_;
    std::atomic<bool> profile_paused_{false};
    HostProfiler host_profiler_;
    std::unordered_map<u32, u32> samples_;
    std::unordered_map<u32, Hook> hooks_;
    std::unordered_map<u32, u32> code_copies_;  // low-memory pc -> original text address

    std::array<u32, 1024> spr_{};
    std::array<u8, 0x4000> locked_cache_{};  // L1 locked cache at 0xE0000000 (16 KB)
    void locked_cache_dma(u32 dmal);
    bool dec_pending_ = false;
    s64 cycle_carry_ = 0;
    u64 hw_ticks_pending_ = 0;
    std::vector<bool> hooked_ = std::vector<bool>(0x01800000 / 4);  // MEM1 word index -> has hook

    std::array<u32, 32> recent_{};  // ring of recent block entry pcs
    u32 recent_pos_ = 0;
    u64 blocks_ = 0;
    u32 idle_pc_ = 0;
    std::unordered_set<u32> watch_;
    u64 idle_skips_ = 0;
    bool halted_ = false;
    std::string halt_reason_;
};
