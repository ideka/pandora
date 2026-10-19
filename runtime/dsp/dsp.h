// Audio DSP: the Flipper/Hollywood DSP register block plus high-level emulation of the DSP
// microcode (after Dolphin's DSP.cpp, DSPHLE.cpp and MailHandler.cpp, GPL-2.0-or-later).
#pragma once

#include <deque>
#include <memory>
#include <utility>

#include "compat.h"

class Runtime;

namespace DSP {

enum DSPInterruptType : u16 {
    INT_AID = 0x0008,   // audio DMA to the DAC finished a buffer
    INT_ARAM = 0x0020,  // ARAM DMA complete
    INT_DSP = 0x0080,   // DSP -> CPU mail
};

class DSPManager;
namespace HLE {
class DSPHLE;
}

}  // namespace DSP

namespace Core {
class System {
public:
    System(Memory::MemoryManager& memory, DSP::DSPManager& dsp) : memory_(memory), dsp_(dsp) {}
    Memory::MemoryManager& GetMemory() { return memory_; }
    DSP::DSPManager& GetDSP() { return dsp_; }

private:
    Memory::MemoryManager& memory_;
    DSP::DSPManager& dsp_;
};
}  // namespace Core

namespace DSP::HLE {

class UCodeInterface;

class CMailHandler {
public:
    explicit CMailHandler(DSPManager& dsp) : m_dsp(dsp) {}
    void PushMail(u32 mail, bool interrupt = false, int cycles_into_future = 0);
    u16 ReadDSPMailboxHigh();
    u16 ReadDSPMailboxLow();
    void ClearPending() { m_pending_mails.clear(); }
    bool HasPending() const { return !m_pending_mails.empty(); }
    void SetHalted(bool halt) { m_halted = halt; }

private:
    DSPManager& m_dsp;
    std::deque<std::pair<u32, bool>> m_pending_mails;  // mail, interrupt after it is read
    u32 m_last_mail = 0;
    bool m_halted = false;
};

class DSPHLE {
public:
    DSPHLE(Core::System& system, DSPManager& dsp);
    ~DSPHLE();

    Core::System& GetSystem() { return m_system; }
    CMailHandler& AccessMailHandler() { return m_mail_handler; }
    void SetUCode(u32 crc);
    void SwapUCode(u32 crc);

    void Update();
    u16 ReadMailBoxHigh(bool cpu_mailbox);
    u16 ReadMailBoxLow(bool cpu_mailbox);
    void WriteMailBoxHigh(u16 value);
    void WriteMailBoxLow(u16 value);
    u16 WriteControlRegister(u16 value);
    u16 ReadControlRegister();

private:
    Core::System& m_system;
    CMailHandler m_mail_handler;
    std::unique_ptr<UCodeInterface> m_ucode;
    std::unique_ptr<UCodeInterface> m_last_ucode;
    u16 m_control = 0;
    u32 m_cpu_mailbox = 0;
};

}  // namespace DSP::HLE

namespace DSP {

class DSPManager {
public:
    explicit DSPManager(Runtime& rt);
    ~DSPManager();

    // Registers at 0x0C005000-0x0C0050FF; 16-bit (32-bit accesses are split by the caller).
    u16 read16(u32 offset);
    void write16(u32 offset, u16 value);

    // Advance emulated time (timebase ticks): DSP updates, audio DMA, delayed interrupts.
    void tick(u64 tb_ticks);
    bool interrupt_pending() const;
    // Ticks until the next thing tick() would do that the CPU may need to react to (an audio DMA
    // block or a delayed interrupt); idle skipping must not jump past it.
    u64 ticks_to_next_event() const;

    // Used by the HLE microcode.
    u8 ReadARAM(u32 address) const;
    void WriteARAM(u8 value, u32 address);
    void GenerateDSPInterruptFromDSPEmu(DSPInterruptType type, int cycles_into_future = 0);

    // Called for each 32-byte block the audio DMA sends to the DAC (8 stereo samples,
    // big-endian, right channel first).
    void set_audio_sink(void (*sink)(void* user, const u8* block), void* user) {
        sink_ = sink;
        sink_user_ = user;
    }

private:
    void do_aram_dma();
    void generate_interrupt(u16 type);

    Runtime& rt_;
    Memory::MemoryManager memory_;
    Core::System system_;
    std::unique_ptr<HLE::DSPHLE> hle_;

    u16 control_ = 0;  // interrupt status/mask bits outside the HLE-owned mask
    u32 aram_mm_addr_ = 0, aram_ar_addr_ = 0, aram_count_ = 0;
    u16 aram_info_ = 0, aram_mode_ = 1, aram_refresh_ = 0;

    u32 audio_dma_addr_ = 0;
    u16 audio_dma_control_ = 0;  // bit 15 enable, bits 0-14 block count
    u32 audio_dma_cur_ = 0;
    u32 audio_dma_left_ = 0;
    u64 audio_dma_carry_ = 0;

    u64 update_carry_ = 0;
    std::deque<std::pair<s64, u16>> delayed_;  // ticks until raised, interrupt type

    void (*sink_)(void*, const u8*) = nullptr;
    void* sink_user_ = nullptr;
};

}  // namespace DSP

// Debugging: PANDORA_DSP_TRACE logs audio DMA reloads and AX output writes.
bool dsp_trace();
