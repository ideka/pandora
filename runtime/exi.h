// EXI bus: three channels; only the RTC/SRAM chip (channel 0, device 1) is present.
#pragma once

#include <array>

#include "guest.h"

class Runtime;

class Exi {
public:
    explicit Exi(Runtime& rt);

    // offset is relative to 0x0D006800.
    u32 read(u32 offset);
    void write(u32 offset, u32 value);
    bool interrupt_pending() const;

private:
    struct Channel {
        u32 status = 0;  // EXIINTMASK(0) EXIINT(1) TCINTMASK(2) TCINT(3) CLK(4-6) CS(7-9) ...
        u32 dma_addr = 0;
        u32 dma_len = 0;
        u32 control = 0;  // TSTART(0) DMA(1) RW(2-3) TLEN(4-5)
        u32 data = 0;
    };

    void transfer(int ch);
    void transfer_byte(int ch, u8& byte);
    void select(int ch, u32 old_cs, u32 new_cs);

    Runtime& rt_;
    std::array<Channel, 3> ch_;

    // RTC/SRAM device state.
    u32 command_ = 0;
    int command_bytes_ = 0;
    u32 cursor_ = 0;
    std::array<u8, 64> sram_{};
};
