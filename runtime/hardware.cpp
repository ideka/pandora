#include "hardware.h"

#include "ios.h"
#include "log.h"
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

}  // namespace

u64 Hardware::read(u32 addr, u8 size) {
    u32 phys = canonical(addr);
    if (size == 8)
        return (u64)read_reg(phys, 4) << 32 | read_reg(phys + 4, 4);
    return read_reg(phys, size);
}

void Hardware::write(u32 addr, u64 value, u8 size) {
    u32 phys = canonical(addr);
    if ((phys & 0xFFFFFFE0) == 0x0C008000) {  // WGPIPE: GX command stream
        wgpipe_bytes_ += size;
        return;
    }
    if (size == 8) {
        write_reg(phys, (u32)(value >> 32), 4);
        write_reg(phys + 4, (u32)value, 4);
        return;
    }
    write_reg(phys, (u32)value, size);
}

u32 Hardware::read_reg(u32 phys, u8 size) {
    switch (phys) {
    case 0x0C003000: return pi_cause_;
    case 0x0C003004: return pi_mask_;
    case 0x0C00302C: return PI_FLIPPER_REV_HOLLYWOOD;
    case 0x0C00202C:  // VI vertical beam position (1-based line counter)
        return 1 + (u32)((tb_ % TB_PER_FIELD) * 263 / TB_PER_FIELD);
    case 0x0D000004:
        return (iy2_ << 5) | (iy1_ << 4) | (x2_ << 3) | (y1_ << 2) | (y2_ << 1) | (u32)x1_;
    case 0x0D000008: return ipc_arm_msg_;
    case 0x0D000030: return ipc_irq_flags_;
    case 0x0D000034: return ipc_irq_mask_;
    case 0x0D000064: return 0xFFFFFFFF;  // AHBPROT: no access restrictions
    }
    if (phys >= 0x0D006800 && phys < 0x0D006838)
        return exi_.read(phys - 0x0D006800);
    auto it = regs_.find(phys);
    u32 v = it == regs_.end() ? 0 : it->second;
    if (seen_.insert(phys).second)
        LOG_DEBUG("hw", "%s read%u  %08X -> %08X  (%s)", block_name(phys), size * 8, phys, v,
                  rt_.describe_pc().c_str());
    return v;
}

void Hardware::write_reg(u32 phys, u32 value, u8 size) {
    switch (phys) {
    case 0x0C003000:  // PI cause: writing 1 acknowledges edge-triggered causes
        pi_cause_ &= ~(value & 0x3);
        return;
    case 0x0C003004:
        pi_mask_ = value;
        return;
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
    }
    if (phys >= 0x0D006800 && phys < 0x0D006838) {
        exi_.write(phys - 0x0D006800, value);
        set_pi(PI_INT_EXI, exi_.interrupt_pending());
        return;
    }
    // VI display interrupt registers: writing bit 31 low acknowledges the interrupt.
    if (phys >= 0x0C002030 && phys <= 0x0C00203C && size == 4) {
        regs_[phys] = value;
        bool any = false;
        for (u32 r = 0x0C002030; r <= 0x0C00203C; r += 4)
            any |= (regs_[r] & 0x90000000) == 0x90000000;
        set_pi(PI_INT_VI, any);
        return;
    }
    if (seen_.insert(phys).second)
        LOG_DEBUG("hw", "%s write%u %08X <- %08X  (%s)", block_name(phys), size * 8, phys, value,
                  rt_.describe_pc().c_str());
    regs_[phys] = value;
}

void Hardware::set_pi(u32 cause, bool asserted) {
    if (asserted)
        pi_cause_ |= cause;
    else
        pi_cause_ &= ~cause;
}

void Hardware::tick(u64 tb_ticks) {
    tb_ += tb_ticks;
    if (tb_ < next_field_tb_)
        return;
    next_field_tb_ = tb_ + TB_PER_FIELD;
    vi_field_++;
    // Raise every enabled display interrupt once per field.
    bool any = false;
    for (u32 r = 0x0C002030; r <= 0x0C00203C; r += 4) {
        u32& v = regs_[r];
        if (v & 0x10000000)
            v |= 0x80000000;
        any |= (v & 0x90000000) == 0x90000000;
    }
    set_pi(PI_INT_VI, any);
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
