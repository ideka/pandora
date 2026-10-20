#include "runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cinttypes>
#include <thread>
#include <fstream>

#include "gpu/aurora_bridge.h"
#include "gpu/command_processor.h"
#include "hardware.h"
#include "ios.h"
#include "log.h"
#include "memory.h"

extern "C" u8* g_pandora_locked_cache = nullptr;

namespace {

constexpr u32 MSR_EE = 0x8000;
constexpr u32 MSR_RI = 0x2;
constexpr u32 MSR_IR_DR = 0x30;

// Broadway runs at 729 MHz with a 60.75 MHz timebase.
constexpr s64 CYCLES_PER_TB_TICK = 12;

constexpr u64 HW_TICK_BATCH = 64;

constexpr u32 SPR_DEC = 22;
constexpr u32 SPR_PVR = 287;
constexpr u32 SPR_DMA_U = 922, SPR_DMA_L = 923;  // locked cache DMA
constexpr u32 BROADWAY_PVR = 0x00087102;

// First instruction of the SDK exception stub (mtspr SPRG0, r4) as copied to each vector.
constexpr u32 OS_EXCEPTION_STUB_WORD0 = 0x7C9043A6;

// Low-memory globals used by the stub.
constexpr u32 OS_CURRENT_CONTEXT_PHYS = 0x000000C0;
constexpr u32 OS_CURRENT_CONTEXT = 0x800000D4;
constexpr u32 OS_EXCEPTION_TABLE = 0x80003000;

constexpr u32 TEXT_BEGIN = 0x80004000, TEXT_END = 0x80688240;

// Written over hooked functions' first instruction before recompiling (tools/patch_hooks.py).
constexpr u32 HOOK_MARKER = 0x04C0FFEE;

Runtime* self(CPUState* c) { return static_cast<Runtime*>(c->external_user_data); }

// SPRs held in CPUState by DolRecomp's cpu.c (XER, LR, CTR, DSISR, DAR, SRR0/1, EAR, TB, GQRs,
// HID2). Everything else lives in the runtime's SPR file.
bool cpu_models_spr(u16 spr) {
    switch (spr) {
    case 1: case 8: case 9: case 18: case 19: case 26: case 27: case 268: case 269: case 282: case 284: case 285:
        return true;
    }
    return spr >= 912 && spr <= 920;
}

}  // namespace

Runtime::Runtime(Options opts) : opts_(std::move(opts)) {
    hw_ = std::make_unique<Hardware>(*this);
    ios_ = std::make_unique<Ios>(*this);
    g_pandora_locked_cache = locked_cache_.data();  // for the recompiled code's fast path
}

Runtime::~Runtime() {
    profiling_ = false;
    if (sampler_.joinable())
        sampler_.join();
    cpu_free(&cpu);
}

bool Runtime::enable_audio_output() { return audio_out_.open(); }

// Each audio DMA block goes to the WAV dump and the host output (each is a no-op when not open).
void Runtime::audio_sink(void* self, const u8* block) {
    auto* rt = static_cast<Runtime*>(self);
    AudioDump::sink(&rt->audio_dump_, block);
    rt->audio_out_.push(block);
}

void Runtime::enable_rendering() {
    bridge_ = std::make_unique<AuroraBridge>(*this);
    hw_->gpu().set_bridge(bridge_.get());
}

void Runtime::hook(u32 addr, Hook fn) {
    hooks_[addr] = std::move(fn);
    if (u32 i = (addr - 0x80000000u) >> 2; i < hooked_.size())
        hooked_[i] = true;
}

bool Runtime::hook(std::string_view name, Hook fn) {
    auto addr = syms_.find(name);
    if (!addr) {
        LOG_ERROR("hle", "cannot hook %.*s: symbol not found", (int)name.size(), name.data());
        return false;
    }
    hook(*addr, std::move(fn));
    return true;
}

// Every registered hook must be marked in the recompiled code (hooks.txt) and vice versa;
// otherwise direct calls would silently run the original function, or hit a missing hook.
bool Runtime::check_hooks() const {
    std::ifstream in(opts_.hooks);
    if (!in) {
        LOG_ERROR("hle", "cannot read %s", opts_.hooks.c_str());
        return false;
    }
    std::unordered_map<u32, std::string> marked;
    for (std::string line; std::getline(in, line);) {
        line = line.substr(0, line.find('#'));
        line.erase(0, line.find_first_not_of(" \t\r"));
        line.erase(line.find_last_not_of(" \t\r") + 1);
        if (line.empty())
            continue;
        if (auto a = syms_.find(line))
            marked.emplace(*a, line);
        else
            LOG_ERROR("hle", "%s lists unknown symbol %s", opts_.hooks.c_str(), line.c_str());
    }
    bool ok = true;
    for (const auto& [addr, fn] : hooks_)
        if (!marked.count(addr) && (addr & 0x3FFFFFFF) >= 0x4000) {
            LOG_ERROR("hle", "hook %s is not listed in %s (rerun tools/recompile.sh)", describe(addr).c_str(),
                      opts_.hooks.c_str());
            ok = false;
        }
    for (const auto& [addr, name] : marked)
        if (!hooks_.count(addr)) {
            LOG_ERROR("hle", "%s is marked in %s but has no hook", name.c_str(), opts_.hooks.c_str());
            ok = false;
        }
    return ok;
}

void Runtime::halt(std::string reason) {
    if (!halted_) {
        halted_ = true;
        halt_reason_ = std::move(reason);
    }
}

// --- CPU callbacks -------------------------------------------------------------------------

bool Runtime::host_call(CPUState* c, u32 addr) {
    Runtime* rt = self(c);
    // Called for every dispatched block: reject ordinary MEM1 code without a hash lookup.
    if (u32 i = (addr - 0x80000000u) >> 2; i < rt->hooked_.size() && addr >= 0x80004000u && !rt->hooked_[i])
        return false;
    if (auto it = rt->hooks_.find(addr); it != rt->hooks_.end()) {
        it->second(*rt);
        return true;
    }
    u32 phys = addr & 0x3FFFFFFF;
    if (phys < 0x3000 && (phys & 0xFF) == 0) {  // exception vector
        if (mem::read32(c, phys) == OS_EXCEPTION_STUB_WORD0) {
            rt->exception_entry(phys);
            return true;
        }
    }
    if (phys < 0x4000)  // code the game copied to low memory
        return rt->run_copied_code(addr);
    return false;
}

// The locked cache is reached here by accesses that do not take the recompiled code's inline
// fast path (DolRecomp's own cpu.c, e.g. dcbz_l).
u64 Runtime::external_read(CPUState* c, u32 addr, u8 size) {
    Runtime* rt = self(c);
    if (addr - 0xE0000000u <= rt->locked_cache_.size() - size) {
        u64 v = 0;
        for (u32 i = 0; i < size; i++)
            v = v << 8 | rt->locked_cache_[addr - 0xE0000000u + i];
        return v;
    }
    return rt->hw_->read(addr, size);
}

void Runtime::external_write(CPUState* c, u32 addr, u64 value, u8 size) {
    Runtime* rt = self(c);
    if (addr - 0xE0000000u <= rt->locked_cache_.size() - size) {
        for (u32 i = 0; i < size; i++)
            rt->locked_cache_[addr - 0xE0000000u + i] = (u8)(value >> (8 * (size - 1 - i)));
        return;
    }
    rt->hw_->write(addr, value, size);
}

// DMA between the locked cache and memory, triggered by writing DMA_L with T set (as in Dolphin,
// it completes instantly). DMA_U: memory address (bits 31-5), length high bits (4-0). DMA_L:
// cache address (31-5), LD (4: memory -> cache), length low bits (3-2), T (1), F (0).
void Runtime::locked_cache_dma(u32 dmal) {
    const u32 dmau = spr_[SPR_DMA_U];
    const u32 mem_addr = 0x80000000u | (dmau & 0x1FFFFFE0u);
    const u32 lc_off = (dmal & ~31u) - 0xE0000000u;
    u32 blocks = (dmau & 0x1F) << 2 | ((dmal >> 2) & 3);
    if (blocks == 0)
        blocks = 128;
    const u32 len = blocks * 32;
    u8* mem = mem::ptr(&cpu, mem_addr, len);
    if (!mem || lc_off > locked_cache_.size() - len) {
        LOG_ERROR("lc", "bad locked cache DMA: mem %08X lc %08X len %u", mem_addr, dmal & ~31u, len);
        return;
    }
    if (dmal & 0x10)
        std::memcpy(&locked_cache_[lc_off], mem, len);
    else {
        std::memcpy(mem, &locked_cache_[lc_off], len);
        if (mem_addr < 0x81800000u)
            on_mem1_write(mem_addr - 0x80000000u, len, this);
    }
}

u32 Runtime::spr_read(CPUState* c, u16 spr, u32) {
    Runtime* rt = self(c);
    if (spr == SPR_PVR)
        return BROADWAY_PVR;
    return spr < 1024 ? rt->spr_[spr] : 0;
}

void Runtime::spr_write(CPUState* c, u16 spr, u32 value, u32) {
    Runtime* rt = self(c);
    if (spr >= 1024)
        return;
    if (spr == SPR_DMA_L && (value & 2)) {
        rt->locked_cache_dma(value);
        value &= ~2u;  // T clears when the transfer is done
    }
    rt->spr_[spr] = value;
    if (spr == SPR_DEC)
        rt->dec_pending_ = false;
}

// Instructions DolRecomp leaves to the host: moves to/from SPRs it does not model and cache
// management. The generated code returns right after the call, so advance pc here.
void Runtime::instruction_fallback(CPUState* c, u32 raw, u32 cia) {
    Runtime* rt = self(c);
    if (raw == HOOK_MARKER) {  // entry of a function replaced by host code (tools/patch_hooks.py)
        auto it = rt->hooks_.find(cia);
        if (it == rt->hooks_.end()) {
            rt->halt("hook marker without a registered hook at " + rt->describe(cia));
            return;
        }
        c->pc = cia;
        it->second(*rt);
        return;
    }
    u32 op = raw >> 26, xo = (raw >> 1) & 0x3FF;
    u32 rd = (raw >> 21) & 31, ra = (raw >> 16) & 31, rb = (raw >> 11) & 31;
    u16 spr = (u16)(((raw >> 16) & 0x1F) | (((raw >> 11) & 0x1F) << 5));
    if (op == 31) {
        switch (xo) {
        case 339:  // mfspr
            c->gpr[rd] = cpu_models_spr(spr) ? ppc_mfspr(c, spr, cia) : spr_read(c, spr, cia);
            break;
        case 467:  // mtspr
            if (cpu_models_spr(spr))
                ppc_mtspr(c, spr, c->gpr[rd], cia);
            else
                spr_write(c, spr, c->gpr[rd], cia);
            break;
        case 371:  // mftb
            c->gpr[rd] = ppc_mftb(c, spr, cia);
            break;
        case 1014: {  // dcbz
            u32 ea = ((ra ? c->gpr[ra] : 0) + c->gpr[rb]) & ~31u;
            for (u32 i = 0; i < 32; i += 4)
                mem_write32(c, ea + i, 0);
            break;
        }
        case 86: case 54: case 470: case 982: case 598: case 854:  // dcbf dcbst dcbi icbi sync eieio
            break;
        default:
            goto unhandled;
        }
    } else if (op == 19 && xo == 150) {  // isync
    } else {
    unhandled:
        char buf[80];
        std::snprintf(buf, sizeof buf, "unhandled fallback instruction %08X at ", raw);
        rt->halt(buf + rt->describe(cia));
        return;
    }
    c->pc = cia + 4;
}

// --- Exceptions ----------------------------------------------------------------------------

// Native equivalent of the SDK's OSExceptionVector stub (which OSExceptionInit copies to each
// vector, patching "li r3, <exception index>"). Saves the volatile state into the current
// OSContext and enters the registered handler with address translation enabled.
void Runtime::exception_entry(u32 vector) {
    CPUState* c = &cpu;
    // __OSExceptionTable index for each vector (0x100 reset ... 0x1700 thermal).
    u32 index;
    switch (vector) {
    case 0x0C00: index = 9; break;   // system call
    case 0x0D00: index = 10; break;  // trace
    case 0x0F00: index = 11; break;  // performance monitor
    case 0x1300: index = 12; break;  // instruction breakpoint
    case 0x1400: index = 13; break;  // system management
    case 0x1700: index = 14; break;  // thermal
    default: index = (vector >> 8) - 1; break;  // 0x100-0x900 -> 0-8
    }
    u32 ctx = mem::read32(c, OS_CURRENT_CONTEXT_PHYS);
    mem::write32(c, ctx + 0x0C, c->gpr[3]);
    mem::write32(c, ctx + 0x10, c->gpr[4]);
    mem::write32(c, ctx + 0x14, c->gpr[5]);
    mem::write16(c, ctx + 0x1A2, mem::read16(c, ctx + 0x1A2) | 2);
    mem::write32(c, ctx + 0x80, c->cr);
    mem::write32(c, ctx + 0x84, c->lr);
    mem::write32(c, ctx + 0x88, c->ctr);
    mem::write32(c, ctx + 0x8C, c->xer);
    mem::write32(c, ctx + 0x198, c->srr0);
    mem::write32(c, ctx + 0x19C, c->srr1);

    c->gpr[3] = index;
    c->gpr[4] = mem::read32(c, OS_CURRENT_CONTEXT);
    c->gpr[5] = c->srr1 & MSR_RI;
    c->msr |= MSR_IR_DR;
    if (!(c->srr1 & MSR_RI)) {
        halt("unrecoverable exception " + std::to_string(index) + " at " + describe(c->srr0));
        return;
    }
    u32 handler = mem::read32(c, OS_EXCEPTION_TABLE + index * 4);
    if (!handler) {
        halt("no handler for exception " + std::to_string(index) + " at " + describe(c->srr0));
        return;
    }
    LOG_TRACE("exc", "exception %u at %s -> %s", index, describe(c->srr0).c_str(), describe(handler).c_str());
    c->pc = handler;
}

// The SDK copies a few routines (e.g. the system call handler) into low memory. They are
// byte-identical to code in .text, so run the recompiled original instead.
bool Runtime::run_copied_code(u32 pc) {
    CPUState* c = &cpu;
    auto it = code_copies_.find(pc);
    if (it == code_copies_.end()) {
        constexpr int N = 6;
        u32 want[N];
        for (int i = 0; i < N; i++)
            want[i] = mem::read32(c, pc + 4 * i);
        u32 found = 0;
        for (u32 a = TEXT_BEGIN; a + 4 * N <= TEXT_END && !found; a += 4) {
            if (mem::read32(c, a) != want[0])
                continue;
            int i = 1;
            while (i < N && mem::read32(c, a + 4 * i) == want[i])
                i++;
            if (i == N)
                found = a;
        }
        if (!found)
            return false;
        LOG_DEBUG("cpu", "low-memory code at %08X is a copy of %s", pc, describe(found).c_str());
        it = code_copies_.emplace(pc, found).first;
    }
    c->pc = it->second;
    return true;
}

// --- Main loop -----------------------------------------------------------------------------

void Runtime::advance(s64 cycles) {
    cycle_carry_ += cycles > 0 ? cycles : 1;
    s64 ticks = cycle_carry_ / CYCLES_PER_TB_TICK;
    if (ticks == 0)
        return;
    cycle_carry_ -= ticks * CYCLES_PER_TB_TICK;
    cpu.timebase += (u64)ticks;
    // Hardware timing (VI, DSP, audio DMA) runs in batches; ~1 us of granularity is plenty.
    hw_ticks_pending_ += (u64)ticks;
    if (hw_ticks_pending_ >= HW_TICK_BATCH) {
        hw_->tick(hw_ticks_pending_);
        hw_ticks_pending_ = 0;
    }
    s32 dec = (s32)spr_[SPR_DEC];
    s32 next = dec - (s32)ticks;
    if (dec >= 0 && next < 0)
        dec_pending_ = true;
    spr_[SPR_DEC] = (u32)next;
}

void Runtime::deliver_interrupts() {
    if (!(cpu.msr & MSR_EE))
        return;
    if (hw_->interrupt_pending()) {
        ppc_take_exception(&cpu, 0, 0x500, cpu.pc, 0);
    } else if (dec_pending_) {
        dec_pending_ = false;
        ppc_take_exception(&cpu, 0, 0x900, cpu.pc, 0);
    }
}

// The SDK idle loop in SelectThread: lwz r0, RunQueueBits(r13); cmpwi r0, 0; beq -8.
void Runtime::find_idle_loop() {
    if (auto a = syms_.find("SelectThread")) {
        u32 pc = *a + 0x150;
        if (mem::read32(&cpu, pc) == 0x800DABE0 && mem::read32(&cpu, pc + 8) == 0x4182FFF8)
            idle_pc_ = pc;
    }
    if (!idle_pc_)
        LOG_ERROR("rt", "OS idle loop not found; idle time will be spun");
}

std::string Runtime::backtrace(int max_frames) const {
    CPUState* c = const_cast<CPUState*>(&cpu);
    std::string out;
    u32 sp = cpu.gpr[1];
    for (int i = 0; i < max_frames && sp && mem::ptr(c, sp, 8); i++) {
        u32 caller = mem::read32(c, sp);
        if (!caller || caller <= sp)
            break;
        out += "    " + describe(mem::read32(c, caller + 4)) + "\n";
        sp = caller;
    }
    return out;
}

// Write watchpoints (debugging): called by cpu.c before every store to MEM1.
void Runtime::on_mem1_write(u32 offset, u32 size, void* user) {
    auto* rt = static_cast<Runtime*>(user);
    for (u32 w : rt->opts_.watch_writes) {
        u32 woff = w & 0x01FFFFFF;
        if (woff < offset + size && offset < woff + 4)
            LOG_INFO("watch", "store%u to %08X at %s (lr %s) r3=%08X r4=%08X r5=%08X\n%s", size * 8,
                     0x80000000 | offset, rt->describe_pc().c_str(), rt->describe(rt->cpu.lr).c_str(), rt->cpu.gpr[3],
                     rt->cpu.gpr[4], rt->cpu.gpr[5], rt->backtrace(10).c_str());
    }
}

void Runtime::start() {
    // Sampling profiler: a side thread reads the (racy, but word-sized) guest pc every millisecond.
    if (opts_.profile) {
        host_profiler_.start();
        profiling_ = true;
        sampler_ = std::thread([this] {
            while (profiling_) {
                if (!profile_paused_)
                    samples_[(u32) * (volatile u32*)&cpu.pc]++;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }
    find_idle_loop();
    if (!opts_.watch_writes.empty())
        ppc_set_mem_write_journal(&Runtime::on_mem1_write, this);
    for (const auto& name : opts_.watch) {
        if (auto a = syms_.find(name))
            watch_.insert(*a);
        else
            LOG_ERROR("watch", "unknown symbol %s", name.c_str());
    }
}

bool Runtime::step() {
    if (halted_)
        return false;
    if (opts_.max_blocks && blocks_ >= opts_.max_blocks) {
        halt("block limit reached");
        return false;
    }
    // The OS idle loop (no runnable thread) can only be left by an interrupt: skip ahead to
    // the next timed event instead of spinning.
    if (cpu.pc == idle_pc_ && (cpu.msr & MSR_EE) && mem::read32(&cpu, cpu.gpr[13] - 0x5420) == 0) {
        u64 ticks = hw_->ticks_to_next_event();
        s32 dec = (s32)spr_[SPR_DEC];
        if (dec >= 0 && (u64)dec + 1 < ticks)
            ticks = (u64)dec + 1;
        advance((s64)ticks * CYCLES_PER_TB_TICK);
        deliver_interrupts();
        idle_skips_++;
        return true;
    }
    recent_[recent_pos_++ % recent_.size()] = cpu.pc;
    if (!watch_.empty() && watch_.count(cpu.pc))
        LOG_INFO("watch", "%s(%08X, %08X, %08X) r3=\"%s\" from %s", describe_pc().c_str(), cpu.gpr[3], cpu.gpr[4],
                 cpu.gpr[5], mem::cstr(&cpu, cpu.gpr[3], 96).c_str(), describe(cpu.lr).c_str());
    if (opts_.trace_calls)
        LOG_INFO("trace", "%s", describe_pc().c_str());

    cpu.downcount = 0;
    if (!guest_dispatch(&cpu)) {
        halt("no code at " + describe_pc());
        return false;
    }
    blocks_++;

    if (u32 e = cpu.exception) {
        cpu.exception = 0;
        if (e & PPC_EXC_PROGRAM) {
            char buf[96];
            std::snprintf(buf, sizeof buf, "program exception (cause %08X) at ", cpu.program_exception);
            halt(buf + describe(cpu.srr0));
        } else if (e & (PPC_EXC_DSI | PPC_EXC_ALIGNMENT | PPC_EXC_MACHINE_CHECK)) {
            char buf[96];
            std::snprintf(buf, sizeof buf, "memory exception %X (dar %08X) at ", e, cpu.dar);
            halt(buf + describe(cpu.srr0));
        }
        // System calls and FP-unavailable continue at their vector.
    }
    advance(-cpu.downcount);
    deliver_interrupts();
    return !halted_;
}

// Run until the guest finishes a frame (display copy), or a time cap so the host stays
// responsive if the guest stops presenting.
bool Runtime::run_frame() {
    const u64 deadline = cpu.timebase + 60750000 / 10;  // 100 ms emulated
    while (step()) {
        if (bridge_ && bridge_->take_frame_end())
            return true;
        if (cpu.timebase >= deadline)
            return true;
    }
    return false;
}

int Runtime::finish() {
    for (const auto& d : opts_.file_dumps) {
        const u8* p = mem::ptr(&cpu, d.addr, d.len);
        std::FILE* f = p ? std::fopen(d.path.c_str(), "wb") : nullptr;
        if (f) {
            std::fwrite(p, 1, d.len, f);
            std::fclose(f);
            LOG_INFO("rt", "wrote %08X+%X to %s", d.addr, d.len, d.path.c_str());
        } else {
            LOG_ERROR("rt", "cannot dump %08X+%X to %s", d.addr, d.len, d.path.c_str());
        }
    }
    profiling_ = false;
    if (sampler_.joinable())
        sampler_.join();
    host_profiler_.stop();
    if (opts_.profile)
        host_profiler_.report();
    if (opts_.profile) {
        std::unordered_map<std::string, u32> by_fn;
        u32 total = 0;
        for (auto [pc, n] : samples_) {
            std::string d = describe(pc);
            by_fn[d.substr(0, d.find('+'))] += n;
            total += n;
        }
        std::vector<std::pair<u32, std::string>> top;
        for (auto& [fn, n] : by_fn)
            top.emplace_back(n, fn);
        std::sort(top.rbegin(), top.rend());
        std::fprintf(stderr, "profile (%u samples):\n", total);
        const char* top_env = std::getenv("PANDORA_PROFILE_TOP");  // more lines, e.g. to diff runs
        const size_t lines = top_env ? (size_t)std::atoi(top_env) : 25;
        for (size_t i = 0; i < top.size() && i < lines; i++)
            std::fprintf(stderr, "  %5.1f%%  %s\n", 100.0 * top[i].first / total, top[i].second.c_str());
    }
    LOG_INFO("rt", "stopped after %" PRIu64 " blocks (%" PRIu64 " idle skips, %.2f s emulated): %s", blocks_,
             idle_skips_, (double)cpu.timebase / 60750000.0, halt_reason_.c_str());
    if (halt_reason_ == "quit")
        return 0;
    dump();
    return 1;
}

int Runtime::run() {
    start();
    while (step()) {
    }
    return finish();
}

void Runtime::dump() const {
    std::fprintf(stderr, "\npc  %s\nlr  %s\nmsr %08X  cr %08X  ctr %08X  tb %" PRIu64 "\n", describe_pc().c_str(),
                 describe(cpu.lr).c_str(), cpu.msr, cpu.cr, cpu.ctr, cpu.timebase);
    for (int i = 0; i < 32; i += 4)
        std::fprintf(stderr, "r%-2d %08X  r%-2d %08X  r%-2d %08X  r%-2d %08X\n", i, cpu.gpr[i], i + 1,
                     cpu.gpr[i + 1], i + 2, cpu.gpr[i + 2], i + 3, cpu.gpr[i + 3]);
    // Back-chain: each frame stores the caller's r1 at 0(r1) and the saved LR at 4(caller r1).
    std::fprintf(stderr, "backtrace:\n");
    CPUState* c = const_cast<CPUState*>(&cpu);
    u32 sp = cpu.gpr[1], last = 0, repeats = 0;
    for (int i = 0; i < 4096 && sp && mem::ptr(c, sp, 8); i++) {
        u32 caller = mem::read32(c, sp);
        if (!caller || caller <= sp)
            break;
        u32 ret = mem::read32(c, caller + 4);
        if (ret == last) {
            repeats++;
        } else {
            if (repeats)
                std::fprintf(stderr, "    ... repeated %u more times\n", repeats);
            std::fprintf(stderr, "  %s\n", describe(ret).c_str());
            last = ret;
            repeats = 0;
        }
        sp = caller;
    }
    if (repeats)
        std::fprintf(stderr, "    ... repeated %u more times\n", repeats);
    // OS threads: the active-thread queue head is at 0x800000DC; OSThread starts with its
    // OSContext (saved LR 0x84, SRR0 0x198), then state (0x2C8: 1 ready, 2 running, 4 waiting,
    // 8 moribund), priority (0x2D0) and the active-queue link (0x2FC).
    std::fprintf(stderr, "threads (current %08X):\n", mem::read32(c, 0x800000E4));
    for (u32 t = mem::read32(c, 0x800000DC), n = 0; t && n < 64; t = mem::read32(c, t + 0x2FC), n++)
        std::fprintf(stderr, "  %08X state %u prio %2u  pc %s  lr %s\n", t, mem::read16(c, t + 0x2C8),
                     mem::read32(c, t + 0x2D0), describe(mem::read32(c, t + 0x198)).c_str(),
                     describe(mem::read32(c, t + 0x84)).c_str());
    std::fprintf(stderr, "recent blocks (oldest first):\n");
    for (size_t i = 0; i < recent_.size(); i++) {
        u32 pc = recent_[(recent_pos_ + i) % recent_.size()];
        if (pc)
            std::fprintf(stderr, "  %s\n", describe(pc).c_str());
    }
    std::fprintf(stderr, "GX FIFO bytes written: %" PRIu64 ", GPU commands %" PRIu64 ", draws %" PRIu64
                         ", draw done %" PRIu64 "\n",
                 hw_->wgpipe_bytes(), hw_->gpu().commands(), hw_->gpu().draws(), hw_->gpu().draw_dones());
    for (auto [addr, len] : opts_.dumps) {
        std::fprintf(stderr, "memory %08X:\n", addr);
        for (u32 off = 0; off < len; off += 16) {
            std::fprintf(stderr, "  %08X ", addr + off);
            for (u32 i = 0; i < 16 && off + i < len; i += 4)
                std::fprintf(stderr, " %08X", mem::read32(c, addr + off + i));
            std::fputc('\n', stderr);
        }
    }
}
