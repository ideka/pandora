// GPU front end: the command processor (CP) that consumes the GX FIFO from guest memory, and the
// pixel engine (PE) token/finish signals. Register behaviour follows Dolphin's
// CommandProcessor.cpp and PixelEngine.cpp (GPL-2.0-or-later).
//
// Commands are decoded far enough to know their length and to act on synchronisation (draw done,
// tokens). Rendering is not done here.
#pragma once

#include <array>
#include <deque>
#include <utility>

#include "../guest.h"

class AuroraBridge;
class Runtime;
class Hardware;

class CommandProcessor {
public:
    CommandProcessor(Runtime& rt, Hardware& hw) : rt_(rt), hw_(hw) {}

    // CP registers at 0x0C000000, PE registers at 0x0C001000 (16-bit).
    u16 cp_read16(u32 offset);
    void cp_write16(u32 offset, u16 value);
    u16 pe_read16(u32 offset);
    void pe_write16(u32 offset, u16 value);

    // The write-gather pipe flushed 32 bytes into the CPU FIFO.
    void gather_burst();
    void tick(u64 tb_ticks);

    // PI FIFO registers mirror the CP FIFO while the GP is linked.
    void link_pi_fifo(u32& base, u32& end, u32& wptr) const;

    void set_bridge(AuroraBridge* bridge) { bridge_ = bridge; }

    u64 commands() const { return commands_; }
    u64 draws() const { return draws_; }
    u64 draw_dones() const { return draw_dones_; }

private:
    void run();
    // Length of the command at the start of [p, p+avail), or 0 if incomplete.
    u32 command_length(const u8* p, u32 avail) const;
    void execute(const u8* p, u32 len);
    void run_display_list(u32 addr, u32 size, int depth);
    u32 vertex_size(u32 vat) const;
    void bp_write(u32 value);
    void update_interrupts();
    void raise_pe(bool finish, bool token_irq, u16 token);

    Runtime& rt_;
    Hardware& hw_;
    AuroraBridge* bridge_ = nullptr;

    // CP FIFO state.
    u32 base_ = 0, end_ = 0, hi_wm_ = 0, lo_wm_ = 0, distance_ = 0, wptr_ = 0, rptr_ = 0, bp_ = 0;
    u16 ctrl_ = 0;
    u16 status_overflow_ = 0;

    // CP register file (vertex descriptors and attribute formats).
    std::array<u32, 256> cpreg_{};

    // PE.
    u16 pe_ctrl_ = 0, pe_token_ = 0;
    bool token_signal_ = false, finish_signal_ = false;
    struct PendingPE {
        s64 ticks;
        bool finish;
        bool token_irq;
        u16 token;
    };
    std::deque<PendingPE> pending_;

    u64 commands_ = 0, draws_ = 0, draw_dones_ = 0;
    bool running_ = false;
};
