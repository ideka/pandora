#include "command_processor.h"

#include <vector>

#include "../hardware.h"
#include "../log.h"
#include "../memory.h"
#include "../runtime.h"

namespace {

// CP control bits.
constexpr u16 CTRL_GP_READ_ENABLE = 1 << 0, CTRL_GP_LINK_ENABLE = 1 << 4;
// CP status bits.
constexpr u16 STATUS_READ_IDLE = 1 << 2, STATUS_COMMAND_IDLE = 1 << 3;
// PE control bits.
constexpr u16 PE_TOKEN_ENABLE = 1 << 0, PE_FINISH_ENABLE = 1 << 1, PE_TOKEN_ACK = 1 << 2, PE_FINISH_ACK = 1 << 3;

// Dolphin delays PE events by at least 500 CPU cycles so games can set up interrupt state first.
constexpr s64 PE_EVENT_DELAY_TICKS = 500 / 12 + 1;

u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
u16 be16(const u8* p) { return (u16)(p[0] << 8 | p[1]); }

// Bytes per component for position/texcoord formats (u8, s8, u16, s16, f32).
u32 comp_size(u32 fmt) {
    static const u32 sizes[8] = {1, 1, 2, 2, 4, 0, 0, 0};
    return sizes[fmt & 7];
}
// Bytes per colour for RGB565, RGB888, RGB888x, RGBA4444, RGBA6, RGBA8888.
u32 color_size(u32 fmt) {
    static const u32 sizes[8] = {2, 3, 4, 2, 3, 4, 0, 0};
    return sizes[fmt & 7];
}

}  // namespace

// --- Registers -------------------------------------------------------------------------------

u16 CommandProcessor::cp_read16(u32 off) {
    switch (off) {
    case 0x00: {  // status: the GP consumes synchronously, so it is always idle
        u16 s = STATUS_READ_IDLE | STATUS_COMMAND_IDLE | status_overflow_;
        return s;
    }
    case 0x02: return ctrl_;
    case 0x20: return (u16)base_;
    case 0x22: return (u16)(base_ >> 16);
    case 0x24: return (u16)end_;
    case 0x26: return (u16)(end_ >> 16);
    case 0x28: return (u16)hi_wm_;
    case 0x2A: return (u16)(hi_wm_ >> 16);
    case 0x2C: return (u16)lo_wm_;
    case 0x2E: return (u16)(lo_wm_ >> 16);
    case 0x30: return (u16)distance_;
    case 0x32: return (u16)(distance_ >> 16);
    case 0x34: return (u16)wptr_;
    case 0x36: return (u16)(wptr_ >> 16);
    case 0x38: return (u16)rptr_;
    case 0x3A: return (u16)(rptr_ >> 16);
    case 0x3C: return (u16)bp_;
    case 0x3E: return (u16)(bp_ >> 16);
    }
    return 0;  // performance counters etc.
}

void CommandProcessor::cp_write16(u32 off, u16 v) {
    auto lo = [&](u32& r, u16 mask) { r = (r & 0xFFFF0000) | (v & mask); };
    auto hi = [&](u32& r, u16 mask) { r = (r & 0xFFFF) | ((u32)(v & mask) << 16); };
    switch (off) {
    case 0x02:
        ctrl_ = v;
        break;
    case 0x04:  // clear: watermark interrupts
        if (v & 1) status_overflow_ &= ~1;
        break;
    case 0x20: lo(base_, 0xFFE0); break;
    case 0x22: hi(base_, 0x1FFF); break;
    case 0x24: lo(end_, 0xFFE0); break;
    case 0x26: hi(end_, 0x1FFF); break;
    case 0x28: lo(hi_wm_, 0xFFE0); break;
    case 0x2A: hi(hi_wm_, 0x1FFF); break;
    case 0x2C: lo(lo_wm_, 0xFFE0); break;
    case 0x2E: hi(lo_wm_, 0x1FFF); break;
    case 0x30: lo(distance_, 0xFFE0); break;
    case 0x32: hi(distance_, 0x1FFF); break;
    case 0x34: lo(wptr_, 0xFFE0); break;
    case 0x36: hi(wptr_, 0x1FFF); break;
    case 0x38: lo(rptr_, 0xFFE0); break;
    case 0x3A: hi(rptr_, 0x1FFF); break;
    case 0x3C: lo(bp_, 0xFFE0); break;
    case 0x3E: hi(bp_, 0x1FFF); break;
    default: return;
    }
    run();
}

u16 CommandProcessor::pe_read16(u32 off) {
    switch (off) {
    case 0x0A: return pe_ctrl_;
    case 0x0E: return pe_token_;
    }
    return 0;
}

void CommandProcessor::pe_write16(u32 off, u16 v) {
    if (off != 0x0A)
        return;  // Z/alpha/dst-alpha config: rendering state, not needed here
    if (v & PE_TOKEN_ACK)
        token_signal_ = false;
    if (v & PE_FINISH_ACK)
        finish_signal_ = false;
    pe_ctrl_ = v & (PE_TOKEN_ENABLE | PE_FINISH_ENABLE);
    update_interrupts();
}

void CommandProcessor::update_interrupts() {
    hw_.set_pi(PI_INT_PE_TOKEN, token_signal_ && (pe_ctrl_ & PE_TOKEN_ENABLE));
    hw_.set_pi(PI_INT_PE_FINISH, finish_signal_ && (pe_ctrl_ & PE_FINISH_ENABLE));
}

void CommandProcessor::raise_pe(bool finish, bool token_irq, u16 token) {
    pending_.push_back({PE_EVENT_DELAY_TICKS, finish, token_irq, token});
}

void CommandProcessor::tick(u64 ticks) {
    if (pending_.empty())
        return;
    for (auto& e : pending_)
        e.ticks -= (s64)ticks;
    while (!pending_.empty() && pending_.front().ticks <= 0) {
        PendingPE e = pending_.front();
        pending_.pop_front();
        if (e.finish) {
            finish_signal_ = true;
        } else {
            pe_token_ = e.token;
            token_signal_ |= e.token_irq;
        }
    }
    update_interrupts();
}

// --- FIFO ------------------------------------------------------------------------------------

// Same advance rule as the PI side: the write pointer moves 32 bytes, wrapping from end to base.
void CommandProcessor::gather_burst() {
    if (!(ctrl_ & CTRL_GP_LINK_ENABLE))
        return;
    wptr_ = wptr_ == end_ ? base_ : wptr_ + 32;
    distance_ += 32;
    if (distance_ > end_ - base_)
        status_overflow_ |= 1;  // the GP fell a whole FIFO behind; should not happen here
    run();
}

void CommandProcessor::link_pi_fifo(u32& base, u32& end, u32& wptr) const {
    if ((ctrl_ & (CTRL_GP_READ_ENABLE | CTRL_GP_LINK_ENABLE)) == (CTRL_GP_READ_ENABLE | CTRL_GP_LINK_ENABLE)) {
        base = base_;
        end = end_;
        wptr = wptr_;
    }
}

// Consume complete commands between the read and write pointers. The ring spans [base, end + 32).
void CommandProcessor::run() {
    if (running_ || !(ctrl_ & CTRL_GP_READ_ENABLE) || end_ <= base_)
        return;
    running_ = true;
    CPUState* c = &rt_.cpu;
    const u32 ring = end_ + 32 - base_;
    std::vector<u8> buf;
    while (distance_ > 0) {
        // Copy out what is available (handles wraparound), then decode as many commands as fit.
        u32 avail = std::min<u32>(distance_, 0x10000);
        buf.resize(avail);
        u32 first = std::min(avail, end_ + 32 - rptr_);
        if (const u8* p = mem::ptr(c, rptr_, first))
            std::memcpy(buf.data(), p, first);
        if (avail > first)
            if (const u8* p = mem::ptr(c, base_, avail - first))
                std::memcpy(buf.data() + first, p, avail - first);

        u32 used = 0;
        while (used < avail) {
            u32 len = command_length(buf.data() + used, avail - used);
            if (len == 0)
                break;
            execute(buf.data() + used, len);
            used += len;
        }
        if (used == 0)
            break;  // incomplete command: wait for more data
        rptr_ += used;
        if (rptr_ >= end_ + 32)
            rptr_ -= ring;
        distance_ -= used;
    }
    running_ = false;
}

u32 CommandProcessor::vertex_size(u32 vat) const {
    u32 vcd_lo = cpreg_[0x50], vcd_hi = cpreg_[0x60];
    u32 a = cpreg_[0x70 + vat], b = cpreg_[0x80 + vat], cc = cpreg_[0x90 + vat];
    u32 size = 0;
    // Matrix indices (PNMTXIDX, TEX0-7MTXIDX): one byte each.
    for (u32 i = 0; i < 9; i++)
        size += (vcd_lo >> i) & 1;
    auto attr = [&](u32 mode, u32 direct_size, u32 index_count = 1) {
        switch (mode) {
        case 1: size += direct_size; break;
        case 2: size += index_count; break;
        case 3: size += 2 * index_count; break;
        }
    };
    attr((vcd_lo >> 9) & 3, ((a & 1) ? 3 : 2) * comp_size(a >> 1));                           // position
    bool nbt = (a >> 9) & 1, index3 = (a >> 31) & 1;
    attr((vcd_lo >> 11) & 3, (nbt ? 9 : 3) * comp_size(a >> 10), nbt && index3 ? 3 : 1);    // normal
    attr((vcd_lo >> 13) & 3, color_size(a >> 14));                                           // color 0
    attr((vcd_lo >> 15) & 3, color_size(a >> 18));                                           // color 1
    // Texture coordinates 0-7: count bit and format field per VAT word.
    const u32 tex_cnt_fmt[8][2] = {
        {(a >> 21) & 1, (a >> 22) & 7},  {(b >> 0) & 1, (b >> 1) & 7},   {(b >> 9) & 1, (b >> 10) & 7},
        {(b >> 18) & 1, (b >> 19) & 7},  {(b >> 27) & 1, (b >> 28) & 7}, {(cc >> 5) & 1, (cc >> 6) & 7},
        {(cc >> 14) & 1, (cc >> 15) & 7}, {(cc >> 23) & 1, (cc >> 24) & 7},
    };
    for (u32 t = 0; t < 8; t++)
        attr((vcd_hi >> (2 * t)) & 3, (tex_cnt_fmt[t][0] ? 2 : 1) * comp_size(tex_cnt_fmt[t][1]));
    return size;
}

u32 CommandProcessor::command_length(const u8* p, u32 avail) const {
    u8 op = p[0];
    auto need = [&](u32 n) { return n <= avail ? n : 0u; };
    switch (op) {
    case 0x00: return 1;                          // NOP
    case 0x08: return need(6);                    // LOAD_CP_REG
    case 0x10: {                                  // LOAD_XF_REG
        if (avail < 5) return 0;
        u32 count = (be32(p + 1) >> 16) + 1;
        return need(5 + 4 * count);
    }
    case 0x20: case 0x28: case 0x30: case 0x38:   // LOAD_INDX_A-D
        return need(5);
    case 0x40: return need(9);                    // CALL_DL
    case 0x44: case 0x48: return 1;               // metrics, INVL_VC
    case 0x61: return need(5);                    // LOAD_BP_REG
    }
    if (op >= 0x80 && op < 0xC0) {                // draw primitive
        if (avail < 3) return 0;
        u32 count = be16(p + 1);
        return need(3 + count * vertex_size(op & 7));
    }
    // Unknown opcode: treat as a one-byte NOP (Dolphin reports these as FIFO errors).
    LOG_DEBUG("gpu", "unknown FIFO opcode %02X", op);
    return 1;
}

void CommandProcessor::execute(const u8* p, u32 len) {
    commands_++;
    switch (p[0]) {
    case 0x08:
        cpreg_[p[1]] = be32(p + 2);
        return;
    case 0x40:
        run_display_list(be32(p + 1), be32(p + 5), 0);
        return;
    case 0x61:
        bp_write(be32(p + 1));
        return;
    }
    if (p[0] >= 0x80 && p[0] < 0xC0)
        draws_++;
    (void)len;
}

void CommandProcessor::run_display_list(u32 addr, u32 size, int depth) {
    const u8* p = mem::ptr(&rt_.cpu, addr, size);
    if (!p || depth > 4) {
        LOG_ERROR("gpu", "bad display list %08X +%X", addr, size);
        return;
    }
    for (u32 used = 0; used < size;) {
        u32 len = command_length(p + used, size - used);
        if (len == 0)
            break;
        if (p[used] == 0x40)  // nested call
            run_display_list(be32(p + used + 1), be32(p + used + 5), depth + 1);
        else
            execute(p + used, len);
        used += len;
    }
}

void CommandProcessor::bp_write(u32 value) {
    switch (value >> 24) {
    case 0x45:  // PE_DONE: draw done
        if ((value & 0xFF) == 0x02) {
            draw_dones_++;
            raise_pe(true, false, 0);
        }
        break;
    case 0x47:  // PE_TOKEN
    case 0x48:  // PE_TOKEN_INT
        raise_pe(false, (value >> 24) == 0x48, (u16)value);
        break;
    }
}
