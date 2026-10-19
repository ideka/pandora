// Memory-mapped hardware registers (Flipper/Hollywood). Unmodelled registers behave as plain
// storage and are logged on first access, so new boot blockers show up in the log.
#pragma once

#include <deque>
#include <unordered_map>
#include <unordered_set>

#include <memory>

#include "exi.h"
#include "guest.h"

class Runtime;
namespace DSP {
class DSPManager;
}
class CommandProcessor;

// PI interrupt causes (PI_INTERRUPT_CAUSE bits).
enum : u32 {
    PI_INT_DI = 0x4,
    PI_INT_SI = 0x8,
    PI_INT_EXI = 0x10,
    PI_INT_AI = 0x20,
    PI_INT_DSP = 0x40,
    PI_INT_VI = 0x100,
    PI_INT_PE_TOKEN = 0x200,
    PI_INT_PE_FINISH = 0x400,
    PI_INT_WII_IPC = 0x4000,
};

class Hardware {
public:
    explicit Hardware(Runtime& rt);
    ~Hardware();

    DSP::DSPManager& dsp() { return *dsp_; }
    CommandProcessor& gpu() { return *cp_; }

    u64 read(u32 addr, u8 size);
    void write(u32 addr, u64 value, u8 size);

    bool interrupt_pending() const { return (pi_cause_ & pi_mask_) != 0; }
    void set_pi(u32 cause, bool asserted);

    // Advance emulated time by timebase ticks (drives video retrace).
    void tick(u64 tb_ticks);
    u64 ticks_to_next_event() const;

    // IPC to the emulated IOS (see ios.cpp).
    void ipc_reply(u32 request);

    u64 wgpipe_bytes() const { return wgpipe_bytes_; }

private:
    u32 read_reg(u32 addr, u8 size);
    void write_reg(u32 addr, u32 value, u8 size);
    u32 load(u32 phys, u8 size) const;
    void store(u32 phys, u32 value, u8 size);
    void update_vi_irq();
    u32 ai_sample_count() const;
    void write_ipc_ctrl(u32 value);
    void update_ipc_irq();
    void deliver_next_reply();

    Runtime& rt_;
    Exi exi_;
    std::unique_ptr<DSP::DSPManager> dsp_;
    std::unique_ptr<CommandProcessor> cp_;
    std::unordered_map<u32, u32> regs_;
    std::unordered_set<u32> seen_;

    u32 pi_cause_ = 0;
    u32 pi_mask_ = 0;

    // IPC (Hollywood 0x0D000000).
    u32 ipc_ppc_msg_ = 0;
    u32 ipc_arm_msg_ = 0;
    bool x1_ = false, x2_ = false, y1_ = false, y2_ = false, iy1_ = false, iy2_ = false;
    u32 ipc_irq_flags_ = 0;
    u32 ipc_irq_mask_ = 0;
    std::deque<u32> replies_;

    // VI retrace.
    u64 tb_ = 0;
    u64 next_field_tb_ = 0;
    u32 vi_field_ = 0;

    // AI (audio interface).
    u32 ai_cr_ = 0;
    u32 ai_count_base_ = 0;
    u64 ai_count_tb_ = 0;

    // Write-gather pipe and the PI CPU FIFO it drains into.
    void gather(u64 value, u8 size);
    u8 gather_[64 + 8] = {};
    u32 gather_count_ = 0;
    u32 fifo_base_ = 0, fifo_end_ = 0, fifo_wptr_ = 0;

    u64 wgpipe_bytes_ = 0;
};
