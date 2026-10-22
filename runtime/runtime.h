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
    std::vector<u32> watch_writes;           // log every store to these addresses (MEM1 or MEM2)
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
    // The Wii Remote speaker's sounds, played through the host mixer (hooks).
    void set_remote_speaker_state(bool on, bool playing);
    bool remote_speaker_wants_data() const { return speaker_on_ && speaker_playing_; }
    void remote_speaker_data(const u8* packet, size_t len);
    void reset_remote_speaker_decoder() { audio_out_.reset_speaker(); }
    int audio_queued_ms() const { return audio_out_.queued_ms(); }
    AuroraBridge* bridge() { return bridge_.get(); }
    Input& input() { return input_; }
    std::string backtrace(int max_frames) const;

    // Replace the guest function at addr (or named) with host code. The hook must leave
    // cpu.pc at the next guest address, usually via ret().
    void hook(u32 addr, Hook fn);
    bool hook(std::string_view name, Hook fn);
    // Calls a guest function from a hook: fn(args...) runs on the guest's stack with the hook's
    // caller's frame, and when it returns, `then` runs with its result (r3). The hook must not
    // touch the CPU after this (the function's own hooks may run in between).
    void call_guest(u32 fn, std::initializer_list<u32> args, std::function<void(Runtime&, u32)> then);
    void host_continue();  // the hostContinue hook: a called guest function returned

    void ret(u32 r3) {
        cpu.gpr[3] = r3;
        cpu.pc = cpu.lr;
    }

    void halt(std::string reason);
    std::string describe(u32 addr) const { return syms_.describe(addr); }
    std::string describe_pc() const { return describe(cpu.pc); }
    u64 executed_cycles() const { return executed_cycles_; }
    u64 blocks() const { return blocks_; }  // dispatches so far
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
    bool speaker_on_ = false, speaker_playing_ = false;  // the game's speaker state
    struct Continuation {
        u32 lr;
        std::function<void(Runtime&, u32)> then;
    };
    std::vector<Continuation> continuations_;  // call_guest's pending returns, innermost last
    u64 speaker_last_tb_ = 0;    // timebase of the last packet, for the stream's sample rate
    float speaker_rate_ = 6000;  // samples per second the game produces (40 per packet)
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
    u8* locked_cache_ = nullptr;  // L1 locked cache at 0xE0000000 (16 KB), in the guest memory window
    static constexpr u32 LOCKED_CACHE_SIZE = 0x4000;
    void locked_cache_dma(u32 dmal);
    bool dec_pending_ = false;
    s64 cycle_carry_ = 0;
    u64 hw_ticks_pending_ = 0;
    std::vector<bool> hooked_ = std::vector<bool>(0x01800000 / 4);  // MEM1 word index -> has hook

    std::array<u32, 32> recent_{};  // ring of recent block entry pcs
    u32 recent_pos_ = 0;
    u64 blocks_ = 0;
    std::unique_ptr<std::unordered_map<u32, u64>> dispatch_hist_;  // PANDORA_DISPATCH_HIST
    u32 idle_pc_ = 0;
    std::unordered_set<u32> watch_;
    u64 idle_skips_ = 0;
    u64 executed_cycles_ = 0;  // guest cycles run (not skipped in the idle loop)
    bool halted_ = false;
    std::string halt_reason_;
};
