// Register behaviour follows Dolphin's EXI_Channel.cpp / EXI_DeviceIPL.cpp; default SRAM is
// Dolphin's Sram.cpp dump.
#include "exi.h"

#include <ctime>

#include "log.h"
#include "memory.h"
#include "runtime.h"

namespace {

constexpr u32 ST_EXIINT = 1u << 1, ST_TCINTMASK = 1u << 2, ST_TCINT = 1u << 3, ST_CS_SHIFT = 7,
              ST_CS_MASK = 7u << 7, ST_EXTINTMASK = 1u << 10, ST_EXTINT = 1u << 11;
constexpr u32 CT_TSTART = 1, CT_DMA = 2;
constexpr u32 EXI_READ = 0, EXI_WRITE = 1, EXI_READWRITE = 2;

constexpr u32 RTC_ADDR = 0x20000000, SRAM_ADDR = 0x20000100;
constexpr u32 SECONDS_1970_TO_2000 = 946684800;

}  // namespace

Exi::Exi(Runtime& rt) : rt_(rt) {
    // SramSettings: checksum 0x002C, ~checksum 0xFFD0, flags = 0x20 | OOBE done | stereo.
    sram_[0] = 0x00; sram_[1] = 0x2C;
    sram_[2] = 0xFF; sram_[3] = 0xD0;
    sram_[0x13] = 0x2C;
    // SramSettingsEx: flash ID checksums.
    sram_[0x14 + 0x26] = 0x6E;
    sram_[0x14 + 0x27] = 0x6D;
}

u32 Exi::read(u32 offset) {
    int ch = (int)(offset / 0x14);
    if (ch > 2)
        return 0;
    Channel& c = ch_[ch];
    switch (offset % 0x14) {
    case 0x00: return c.status;  // EXT (device inserted) stays 0: no memory cards
    case 0x04: return c.dma_addr;
    case 0x08: return c.dma_len;
    case 0x0C: return c.control;
    case 0x10: return c.data;
    }
    return 0;
}

void Exi::write(u32 offset, u32 v) {
    int ch = (int)(offset / 0x14);
    if (ch > 2)
        return;
    Channel& c = ch_[ch];
    switch (offset % 0x14) {
    case 0x00: {
        u32 s = c.status;
        s = (s & ~0x5u) | (v & 0x5);   // EXIINTMASK, TCINTMASK
        s = (s & ~0x70u) | (v & 0x70); // CLK
        if (v & ST_EXIINT) s &= ~ST_EXIINT;
        if (v & ST_TCINT) s &= ~ST_TCINT;
        if (ch < 2) {
            s = (s & ~ST_EXTINTMASK) | (v & ST_EXTINTMASK);
            if (v & ST_EXTINT) s &= ~ST_EXTINT;
        }
        if (ch == 0)
            s = (s & ~(1u << 13)) | (v & (1u << 13));  // ROMDIS
        u32 old_cs = (s & ST_CS_MASK) >> ST_CS_SHIFT, new_cs = (v & ST_CS_MASK) >> ST_CS_SHIFT;
        s = (s & ~ST_CS_MASK) | (v & ST_CS_MASK);
        c.status = s;
        if (old_cs != new_cs)
            select(ch, old_cs, new_cs);
        break;
    }
    case 0x04: c.dma_addr = v; break;
    case 0x08: c.dma_len = v; break;
    case 0x0C:
        c.control = v;
        if (v & CT_TSTART)
            transfer(ch);
        break;
    case 0x10: c.data = v; break;
    }
}

bool Exi::interrupt_pending() const {
    for (const Channel& c : ch_)
        if ((c.status & ST_TCINT) && (c.status & ST_TCINTMASK))
            return true;
    return false;
}

void Exi::select(int ch, u32, u32 new_cs) {
    // Selecting the RTC/SRAM chip starts a new command.
    if (ch == 0 && (new_cs & 2)) {
        command_ = 0;
        command_bytes_ = 0;
        cursor_ = 0;
    }
}

void Exi::transfer(int ch) {
    Channel& c = ch_[ch];
    u32 rw = (c.control >> 2) & 3, len = ((c.control >> 4) & 3) + 1;
    if (!(c.control & CT_DMA)) {
        if (rw == EXI_WRITE) {
            for (u32 i = 0; i < len; i++) {
                u8 b = (u8)(c.data >> (24 - 8 * i));
                transfer_byte(ch, b);
            }
        } else if (rw == EXI_READ) {
            u32 result = 0;
            for (u32 i = 0; i < len; i++) {
                u8 b = 0;
                transfer_byte(ch, b);
                result |= (u32)b << (24 - 8 * i);
            }
            c.data = result;
        } else if (rw == EXI_READWRITE) {
            LOG_DEBUG("exi", "ch%d: read-write transfer ignored", ch);
        }
    } else {
        for (u32 i = 0; i < c.dma_len; i++) {
            u32 a = c.dma_addr + i;
            u8 b = rw == EXI_WRITE ? mem::read8(&rt_.cpu, a) : 0;
            transfer_byte(ch, b);
            if (rw == EXI_READ)
                mem::write8(&rt_.cpu, a, b);
        }
    }
    c.control &= ~CT_TSTART;
    c.status |= ST_TCINT;
}

void Exi::transfer_byte(int ch, u8& byte) {
    u32 cs = (ch_[ch].status & ST_CS_MASK) >> ST_CS_SHIFT;
    if (ch != 0 || !(cs & 2))
        return;  // no device: reads return 0
    if (command_bytes_ < 4) {
        command_ = command_ << 8 | byte;
        command_bytes_++;
        return;
    }
    // Command word: bit 31 = write, remaining bits = device address (low byte unused here).
    bool is_write = command_ & 0x80000000;
    u32 addr = command_ & 0x7FFFFF00;
    u32 pos = cursor_++;
    if (addr == RTC_ADDR) {
        if (!is_write) {
            u32 rtc = (u32)(std::time(nullptr) - SECONDS_1970_TO_2000);
            byte = (u8)(rtc >> (24 - 8 * (pos % 4)));
        }
    } else if (addr == SRAM_ADDR) {
        u32 i = pos % sram_.size();
        if (is_write)
            sram_[i] = byte;
        else
            byte = sram_[i];
    } else if (pos == 0) {
        LOG_DEBUG("exi", "RTC/SRAM chip: %s of unmodelled address %08X", is_write ? "write" : "read", addr);
    }
}
