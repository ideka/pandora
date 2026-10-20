#include <algorithm>
#include "hardware.h"

#include <cstdlib>
#include <cstring>

#include "dsp/dsp.h"
#include "gpu/command_processor.h"
#include "ios.h"
#include "log.h"
#include "memory.h"
#include "runtime.h"

namespace {

constexpr u32 HOLLYWOOD_IRQ_IPC_BROADWAY = 0x40000000;
constexpr u32 PI_FLIPPER_REV_HOLLYWOOD = 0x246500B1;

// NTSC field rate: 60.75 MHz timebase / 59.94 Hz.
constexpr u64 TB_PER_FIELD = 1013513;

// The Wii keeps the GameCube-era DI/SI/EXI/AI blocks at 0x0D006000 and also mirrors Hollywood
// registers at 0x0D800000. Fold everything onto one canonical address.
u32 canonical(u32 addr) {
    u32 phys = addr & 0x0FFFFFFF;
    if ((phys & 0xFFFFF000) == 0x0C006000)
        phys = 0x0D006000 | (phys & 0xFFF);
    if ((phys & 0xFFF00000) == 0x0D800000)
        phys = 0x0D000000 | (phys & 0xFFFFF);
    return phys;
}

const char* block_name(u32 phys) {
    switch (phys & 0xFFFFFC00) {
    case 0x0C000000: return "CP";
    case 0x0C001000: return "PE";
    case 0x0C002000: return "VI";
    case 0x0C003000: return "PI";
    case 0x0C004000: return "MI";
    case 0x0C005000: return "DSP";
    case 0x0D006000: return "DI";
    case 0x0D006400: return "SI";
    case 0x0D006800: return "EXI";
    case 0x0D006C00: return "AI";
    }
    if ((phys & 0xFFFF0000) == 0x0D000000)
        return "HW";
    return "?";
}

constexpr u32 DSP_BASE = 0x0C005000;
bool is_dsp(u32 phys) { return (phys & 0xFFFFFF00) == DSP_BASE; }
bool is_cp(u32 phys) { return (phys & 0xFFFFFF80) == 0x0C000000; }
bool is_pe(u32 phys) { return (phys & 0xFFFFFF80) == 0x0C001000; }

}  // namespace

namespace {
Hardware* g_hardware = nullptr;  // for the recompiled code's write-gather fast path
}

extern "C" void pandora_wgpipe_write(u64 value, u32 size) { g_hardware->gather(value, (u8)size); }

Hardware::Hardware(Runtime& rt)
    : rt_(rt), exi_(rt), dsp_(std::make_unique<DSP::DSPManager>(rt)),
      cp_(std::make_unique<CommandProcessor>(rt, *this)) {
    g_hardware = this;
}
Hardware::~Hardware() {
    if (g_hardware == this)
        g_hardware = nullptr;
}

u64 Hardware::read(u32 addr, u8 size) {
    u32 phys = canonical(addr);
    if (size == 8)
        return (u64)read_reg(phys, 4) << 32 | read_reg(phys + 4, 4);
    return read_reg(phys, size);
}

void Hardware::write(u32 addr, u64 value, u8 size) {
    u32 phys = canonical(addr);
    if ((phys & 0xFFFFFFE0) == 0x0C008000) {  // WGPIPE: GX command stream
        gather(value, size);
        return;
    }
    if (size == 8) {
        write_reg(phys, (u32)(value >> 32), 4);
        write_reg(phys + 4, (u32)value, 4);
        return;
    }
    write_reg(phys, (u32)value, size);
}

// Write-gather pipe: stores to 0x0C008000 collect in a 32-byte buffer that is flushed to memory at
// the PI CPU FIFO write pointer, which wraps from the FIFO end back to its base (as in Dolphin's
// GPFifo.cpp). GX display lists rely on this: GXEndDisplayList measures the pointer's advance.
void Hardware::gather(u64 value, u8 size) {
    const u64 be = _byteswap_uint64(value << (8 * (8 - size)));  // the value's bytes, big-endian first
    std::memcpy(gather_ + gather_count_, &be, 8);                // the buffer has 8 bytes of slack
    gather_count_ += size;
    wgpipe_bytes_ += size;
    while (gather_count_ >= 32) {
        if (u8* dst = mem::ptr(&rt_.cpu, fifo_wptr_, 32))
            std::memcpy(dst, gather_, 32);
        if (fifo_wptr_ == fifo_end_)
            fifo_wptr_ = fifo_base_;
        else
            fifo_wptr_ += 32;
        gather_count_ -= 32;
        std::memmove(gather_, gather_ + 32, gather_count_);
        cp_->gather_burst();
        cp_->link_pi_fifo(fifo_base_, fifo_end_, fifo_wptr_);
    }
}

u32 Hardware::read_reg(u32 phys, u8 size) {
    switch (phys) {
    case 0x0C003000: return pi_cause_;
    case 0x0C003004: return pi_mask_;
    case 0x0C00300C: return fifo_base_;
    case 0x0C003010: return fifo_end_;
    case 0x0C003014: return fifo_wptr_;
    case 0x0C00302C: return PI_FLIPPER_REV_HOLLYWOOD;
    case 0x0C00202C:  // VI vertical beam position (1-based line counter)
        return 1 + (u32)((tb_ % TB_PER_FIELD) * 263 / TB_PER_FIELD);
    case 0x0D000004:
        return (iy2_ << 5) | (iy1_ << 4) | (x2_ << 3) | (y1_ << 2) | (y2_ << 1) | (u32)x1_;
    case 0x0D000008: return ipc_arm_msg_;
    case 0x0D000030: return ipc_irq_flags_;
    case 0x0D000034: return ipc_irq_mask_;
    case 0x0D000064: return 0xFFFFFFFF;  // AHBPROT: no access restrictions
    case 0x0D006C00: return ai_cr_;
    case 0x0D006C08: return ai_sample_count();
    }
    if (phys >= 0x0D006800 && phys < 0x0D006838)
        return exi_.read(phys - 0x0D006800);
    if (is_cp(phys) || is_pe(phys)) {  // 16-bit registers
        u32 off = phys & 0x7F;
        auto rd = [&](u32 o) { return is_cp(phys) ? cp_->cp_read16(o) : cp_->pe_read16(o); };
        return size == 4 ? (u32)rd(off) << 16 | rd(off + 2) : rd(off & ~1u);
    }
    if (is_dsp(phys)) {  // 16-bit registers; wider accesses are split high half first
        u32 off = phys - DSP_BASE;
        u32 v = size == 4 ? (u32)dsp_->read16(off) << 16 | dsp_->read16(off + 2) : dsp_->read16(off & ~1u);
        set_pi(PI_INT_DSP, dsp_->interrupt_pending());
        return size == 1 ? (v >> ((~off & 1) * 8)) & 0xFF : v;
    }
    u32 v = load(phys, size);
    if (seen_.insert(phys).second)
        LOG_DEBUG("hw", "%s read%u  %08X -> %08X  (%s)", block_name(phys), size * 8, phys, v,
                  rt_.describe_pc().c_str());
    return v;
}

// Plain register storage, word-backed so 8/16/32-bit accesses to the same register agree.
u32 Hardware::load(u32 phys, u8 size) const {
    auto it = regs_.find(phys & ~3u);
    u32 word = it == regs_.end() ? 0 : it->second;
    if (size == 4)
        return word;
    u32 shift = (4 - size - (phys & 3)) * 8;
    return (word >> shift) & ((1u << (size * 8)) - 1);
}

void Hardware::store(u32 phys, u32 value, u8 size) {
    u32& word = regs_[phys & ~3u];
    if (size == 4) {
        word = value;
        return;
    }
    u32 shift = (4 - size - (phys & 3)) * 8, mask = ((1u << (size * 8)) - 1) << shift;
    word = (word & ~mask) | ((value << shift) & mask);
}

// AI streaming sample counter: advances at the AIS rate (AISFR: 1 = 48 kHz, 0 = 32 kHz) while
// PSTAT is set.
u32 Hardware::ai_sample_count() const {
    if (!(ai_cr_ & 1))
        return ai_count_base_;
    u64 rate = (ai_cr_ & 2) ? 48000 : 32000;
    return ai_count_base_ + (u32)((tb_ - ai_count_tb_) * rate / 60750000);
}

u64 Hardware::ticks_to_next_event() const {
    const u64 field = next_field_tb_ > tb_ ? next_field_tb_ - tb_ : 1;
    return std::max<u64>(1, std::min(field, dsp_->ticks_to_next_event()));
}

void Hardware::write_reg(u32 phys, u32 value, u8 size) {
    switch (phys) {
    case 0x0C003000:  // PI cause: writing 1 acknowledges edge-triggered causes
        pi_cause_ &= ~(value & 0x3);
        return;
    case 0x0C003004:
        pi_mask_ = value;
        return;
    case 0x0C00300C: fifo_base_ = value & 0xFFFFFFE0; return;
    case 0x0C003010: fifo_end_ = value & 0xFFFFFFE0; return;
    case 0x0C003014: fifo_wptr_ = value & 0xFFFFFFE0; return;
    case 0x0D000000:
        ipc_ppc_msg_ = value;
        return;
    case 0x0D000004:
        write_ipc_ctrl(value);
        return;
    case 0x0D000030:
        ipc_irq_flags_ &= ~value;
        update_ipc_irq();
        deliver_next_reply();
        return;
    case 0x0D000034:
        ipc_irq_mask_ = value;
        update_ipc_irq();
        return;
    case 0x0D006C00: {  // AICR: PSTAT(0) AISFR(1) AIINTMSK(2) AIINT(3) AIINTVLD(4) SCRESET(5) AIDFR(6)
        u32 count = ai_sample_count();
        u32 next = (ai_cr_ & 0x08) | (value & 0x57);
        if (value & 0x08)
            next &= ~0x08u;  // writing AIINT acknowledges it
        ai_cr_ = next;
        ai_count_base_ = (value & 0x20) ? 0 : count;  // SCRESET zeroes the counter
        ai_count_tb_ = tb_;
        return;
    }
    case 0x0D006C08:
        ai_count_base_ = value;
        ai_count_tb_ = tb_;
        return;
    }
    if (phys >= 0x0D006800 && phys < 0x0D006838) {
        exi_.write(phys - 0x0D006800, value);
        set_pi(PI_INT_EXI, exi_.interrupt_pending());
        return;
    }
    if (is_cp(phys) || is_pe(phys)) {
        u32 off = phys & 0x7F;
        auto wr = [&](u32 o, u16 v) { is_cp(phys) ? cp_->cp_write16(o, v) : cp_->pe_write16(o, v); };
        if (size == 4) {
            wr(off, (u16)(value >> 16));
            wr(off + 2, (u16)value);
        } else {
            wr(off & ~1u, (u16)value);
        }
        return;
    }
    if (is_dsp(phys)) {
        u32 off = phys - DSP_BASE;
        if (size == 4) {
            dsp_->write16(off, (u16)(value >> 16));
            dsp_->write16(off + 2, (u16)value);
        } else {
            dsp_->write16(off & ~1u, (u16)value);
        }
        set_pi(PI_INT_DSP, dsp_->interrupt_pending());
        return;
    }
    if (seen_.insert(phys).second)
        LOG_DEBUG("hw", "%s write%u %08X <- %08X  (%s)", block_name(phys), size * 8, phys, value,
                  rt_.describe_pc().c_str());
    store(phys, value, size);
    // VI display interrupt registers (DI0-DI3): writing bit 31 low acknowledges the interrupt.
    if (phys >= 0x0C002030 && phys < 0x0C002040)
        update_vi_irq();
}

void Hardware::update_vi_irq() {
    bool any = false;
    for (u32 r = 0x0C002030; r < 0x0C002040; r += 4)
        any |= (load(r, 4) & 0x90000000) == 0x90000000;
    set_pi(PI_INT_VI, any);
}

void Hardware::set_pi(u32 cause, bool asserted) {
    if (asserted)
        pi_cause_ |= cause;
    else
        pi_cause_ &= ~cause;
}

void Hardware::tick(u64 tb_ticks) {
    tb_ += tb_ticks;
    dsp_->tick(tb_ticks);
    cp_->tick(tb_ticks);
    set_pi(PI_INT_DSP, dsp_->interrupt_pending());
    if (tb_ < next_field_tb_)
        return;
    next_field_tb_ = tb_ + TB_PER_FIELD;
    vi_field_++;
    // Raise every enabled display interrupt once per field.
    for (u32 r = 0x0C002030; r < 0x0C002040; r += 4) {
        u32 v = load(r, 4);
        if (v & 0x10000000)
            store(r, v | 0x80000000, 4);
    }
    update_vi_irq();
}

// PPCCTRL bits: X1 execute (0), Y2 ack (1), Y1 reply (2), X2 relaunch (3), IY1 (4), IY2 (5).
// Y1/Y2 are cleared by writing 1.
void Hardware::write_ipc_ctrl(u32 v) {
    x1_ = v & 1;
    x2_ = (v >> 3) & 1;
    if (v & 4)
        y1_ = false;
    if (v & 2)
        y2_ = false;
    iy1_ = (v >> 4) & 1;
    iy2_ = (v >> 5) & 1;
    // Writing Y1/Y2 with the matching interrupt enabled re-raises the IPC interrupt.
    if (((v & 4) && iy1_) || ((v & 2) && iy2_))
        ipc_irq_flags_ |= HOLLYWOOD_IRQ_IPC_BROADWAY;
    if (x1_) {
        x1_ = false;
        y2_ = true;  // acknowledge immediately
        rt_.ios().submit(ipc_ppc_msg_);
    }
    deliver_next_reply();
    update_ipc_irq();
}

void Hardware::ipc_reply(u32 request) {
    replies_.push_back(request);
    deliver_next_reply();
    update_ipc_irq();
}

// One reply is visible at a time: the next is posted once the PPC has cleared Y1/Y2 and the
// interrupt flag.
void Hardware::deliver_next_reply() {
    if (replies_.empty() || y1_ || y2_ || (ipc_irq_flags_ & HOLLYWOOD_IRQ_IPC_BROADWAY))
        return;
    ipc_arm_msg_ = replies_.front();
    replies_.pop_front();
    y1_ = true;
    update_ipc_irq();
}

void Hardware::update_ipc_irq() {
    if ((y1_ && iy1_) || (y2_ && iy2_))
        ipc_irq_flags_ |= HOLLYWOOD_IRQ_IPC_BROADWAY;
    set_pi(PI_INT_WII_IPC, (ipc_irq_flags_ & ipc_irq_mask_) != 0);
}
