// Owns the guest CPU and drives the recompiled code.
#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "guest.h"
#include "symbols.h"

class Hardware;
class Ios;

struct Options {
    std::filesystem::path disc;  // extracted game partition (contains sys/ and files/)
    std::filesystem::path nand = "build/nand";  // emulated NAND (saves, SYSCONF)
    std::string map = "build/SX3P01.map";
    std::string hooks = "config/SX3P01/hooks.txt";  // functions marked for hooking at recompile time
    u64 max_blocks = 0;          // stop after this many blocks (0 = unlimited)
    bool trace_calls = false;    // log every block entry
};

class Runtime {
public:
    using Hook = std::function<void(Runtime&)>;

    explicit Runtime(Options opts);
    ~Runtime();

    bool boot();  // boot.cpp
    int run();

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

    Options opts_;
    Symbols syms_;
    std::unique_ptr<Hardware> hw_;
    std::unique_ptr<Ios> ios_;
    std::unordered_map<u32, Hook> hooks_;
    std::unordered_map<u32, u32> code_copies_;  // low-memory pc -> original text address

    std::array<u32, 1024> spr_{};
    bool dec_pending_ = false;
    s64 cycle_carry_ = 0;

    std::array<u32, 32> recent_{};  // ring of recent block entry pcs
    u32 recent_pos_ = 0;
    u64 blocks_ = 0;
    u32 idle_pc_ = 0;
    u64 idle_skips_ = 0;
    bool halted_ = false;
    std::string halt_reason_;
};
