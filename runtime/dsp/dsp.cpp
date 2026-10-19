#include <algorithm>
#include <cstdlib>
// Register behaviour and HLE glue follow Dolphin's DSP.cpp, DSPHLE.cpp and MailHandler.cpp
// (tag 2606, GPL-2.0-or-later).
#include "dsp.h"

#include "ucode.h"
#include "../log.h"
#include "../runtime.h"

namespace {

// DSPCR bits owned by the DSP emulation: reset (0), assert-int-to-DSP (1), halt (2),
// init-code (10), init (11).
constexpr u16 DSP_CONTROL_MASK = 0x0C07;
constexpr u16 CR_RESET = 1u << 0, CR_HALT = 1u << 2, CR_INIT = 1u << 11;

constexpr u64 TB_PER_SECOND = 60750000;
constexpr u64 TB_PER_BLOCK = TB_PER_SECOND * 8 / 32000;  // one audio DMA block
constexpr u64 CYCLES_PER_TB_TICK = 12;

}  // namespace

bool dsp_trace() {
    static const bool on = std::getenv("PANDORA_DSP_TRACE") != nullptr;
    return on;
}

namespace DSP::HLE {

void CMailHandler::PushMail(u32 mail, bool interrupt, int cycles_into_future) {
    if (interrupt) {
        if (m_pending_mails.empty())
            m_dsp.GenerateDSPInterruptFromDSPEmu(INT_DSP, cycles_into_future);
        else
            m_pending_mails.front().second = true;
    }
    m_pending_mails.emplace_back(mail, false);
}

u16 CMailHandler::ReadDSPMailboxHigh() {
    if (!m_halted && !m_pending_mails.empty())
        m_last_mail = m_pending_mails.front().first;
    return (u16)(m_last_mail >> 16);
}

u16 CMailHandler::ReadDSPMailboxLow() {
    if (!m_halted && !m_pending_mails.empty()) {
        m_last_mail = m_pending_mails.front().first;
        const bool interrupt = m_pending_mails.front().second;
        m_pending_mails.pop_front();
        if (interrupt)
            m_dsp.GenerateDSPInterruptFromDSPEmu(INT_DSP);
    }
    // The top bit of the high word reads back clear once the mail has been read.
    m_last_mail &= ~0x80000000u;
    return (u16)m_last_mail;
}

DSPHLE::DSPHLE(Core::System& system, DSPManager& dsp) : m_system(system), m_mail_handler(dsp) {
    SetUCode(UCODE_ROM);
    m_control = CR_HALT | CR_INIT;
    m_mail_handler.SetHalted(true);
}

DSPHLE::~DSPHLE() = default;

void DSPHLE::SetUCode(u32 crc) {
    m_mail_handler.ClearPending();
    m_ucode = UCodeFactory(crc, this, true);
    m_ucode->Initialize();
}

void DSPHLE::SwapUCode(u32 crc) {
    m_mail_handler.ClearPending();
    if (m_last_ucode && UCodeInterface::GetCRC(m_last_ucode.get()) == crc) {
        m_ucode = std::move(m_last_ucode);
    } else {
        if (!m_last_ucode)
            m_last_ucode = std::move(m_ucode);
        m_ucode = UCodeFactory(crc, this, true);
        m_ucode->Initialize();
    }
}

void DSPHLE::Update() {
    if (m_ucode)
        m_ucode->Update();
}

u16 DSPHLE::ReadMailBoxHigh(bool cpu_mailbox) {
    return cpu_mailbox ? (u16)(m_cpu_mailbox >> 16) : m_mail_handler.ReadDSPMailboxHigh();
}

u16 DSPHLE::ReadMailBoxLow(bool cpu_mailbox) {
    return cpu_mailbox ? (u16)m_cpu_mailbox : m_mail_handler.ReadDSPMailboxLow();
}

void DSPHLE::WriteMailBoxHigh(u16 value) { m_cpu_mailbox = (m_cpu_mailbox & 0xFFFF) | ((u32)value << 16); }

void DSPHLE::WriteMailBoxLow(u16 value) {
    m_cpu_mailbox = (m_cpu_mailbox & 0xFFFF0000) | value;
    LOG_TRACE("dsp", "mail to DSP %08X", m_cpu_mailbox);
    if (m_ucode)
        m_ucode->HandleMail(m_cpu_mailbox);
    m_cpu_mailbox &= 0x7FFFFFFF;  // the DSP has taken it
}

u16 DSPHLE::WriteControlRegister(u16 value) {
    if ((m_control ^ value) & CR_HALT)
        m_mail_handler.SetHalted(value & CR_HALT);
    if (value & CR_RESET) {
        SetUCode(UCODE_ROM);
        value &= ~CR_RESET;
    }
    // The init-audio-system handshake is skipped at the OS level (__OSInitAudioSystem hook).
    m_control = value;
    return m_control;
}

u16 DSPHLE::ReadControlRegister() { return m_control; }

}  // namespace DSP::HLE

namespace DSP {

DSPManager::DSPManager(Runtime& rt)
    : rt_(rt), memory_(&rt.cpu), system_(memory_, *this),
      hle_(std::make_unique<HLE::DSPHLE>(system_, *this)) {}

DSPManager::~DSPManager() = default;

// On Wii the DSP addresses MEM2 when bit 28 is set and MEM1 otherwise.
u8 DSPManager::ReadARAM(u32 address) const {
    return mem::read8(&rt_.cpu, (address & 0x10000000) ? (0x10000000 | (address & 0x03FFFFFF)) : (address & 0x01FFFFFF));
}

void DSPManager::WriteARAM(u8 value, u32 address) { mem::write8(&rt_.cpu, 0x10000000 | (address & 0x03FFFFFF), value); }

void DSPManager::GenerateDSPInterruptFromDSPEmu(DSPInterruptType type, int cycles_into_future) {
    s64 ticks = cycles_into_future > 0 ? (cycles_into_future + CYCLES_PER_TB_TICK - 1) / CYCLES_PER_TB_TICK : 0;
    if (ticks == 0)
        generate_interrupt(type);
    else
        delayed_.emplace_back(ticks, (u16)type);
}

void DSPManager::generate_interrupt(u16 type) { control_ |= type & (INT_DSP | INT_ARAM | INT_AID); }

bool DSPManager::interrupt_pending() const {
    return ((control_ >> 1) & control_ & (INT_DSP | INT_ARAM | INT_AID)) != 0;
}

u16 DSPManager::read16(u32 off) {
    switch (off) {
    case 0x00: return hle_->ReadMailBoxHigh(true);
    case 0x02: return hle_->ReadMailBoxLow(true);
    case 0x04: return hle_->ReadMailBoxHigh(false);
    case 0x06: return hle_->ReadMailBoxLow(false);
    case 0x0A: return (u16)((control_ & ~DSP_CONTROL_MASK) | (hle_->ReadControlRegister() & DSP_CONTROL_MASK));
    case 0x12: return aram_info_;
    case 0x16: return aram_mode_;
    case 0x1A: return aram_refresh_;
    case 0x20: return (u16)(aram_mm_addr_ >> 16);
    case 0x22: return (u16)aram_mm_addr_;
    case 0x24: return (u16)(aram_ar_addr_ >> 16);
    case 0x26: return (u16)aram_ar_addr_;
    case 0x28: return (u16)(aram_count_ >> 16);
    case 0x2A: return (u16)aram_count_;
    case 0x30: return (u16)(audio_dma_addr_ >> 16);
    case 0x32: return (u16)audio_dma_addr_;
    case 0x36: return audio_dma_control_;
    case 0x3A: return (u16)(audio_dma_left_ > 0 ? audio_dma_left_ - 1 : 0);  // zero-based
    }
    LOG_DEBUG("dsp", "read of unknown register %02X", off);
    return 0;
}

void DSPManager::write16(u32 off, u16 v) {
    switch (off) {
    case 0x00: hle_->WriteMailBoxHigh(v); return;
    case 0x02: hle_->WriteMailBoxLow(v); return;
    case 0x0A: {
        u16 hle_bits = hle_->WriteControlRegister(v);
        u16 next = (u16)((v & ~DSP_CONTROL_MASK) | (hle_bits & DSP_CONTROL_MASK));
        if (v & CR_RESET)
            audio_dma_control_ = 0;
        // Interrupt status bits are acknowledged by writing 1; masks and the rest are stored.
        u16 status = control_ & (INT_DSP | INT_ARAM | INT_AID);
        status &= ~(v & (INT_DSP | INT_ARAM | INT_AID));
        control_ = (u16)((next & ~(INT_DSP | INT_ARAM | INT_AID) & ~0x0200) | status | (control_ & 0x0200));
        return;
    }
    case 0x12: aram_info_ = v & 0x7F; return;
    case 0x1A: aram_refresh_ = v & 0x7FF; return;
    case 0x20: aram_mm_addr_ = (aram_mm_addr_ & 0xFFFF) | ((u32)(v & 0x03FF) << 16); return;
    case 0x22: aram_mm_addr_ = (aram_mm_addr_ & 0xFFFF0000) | (v & 0xFFE0); return;
    case 0x24: aram_ar_addr_ = (aram_ar_addr_ & 0xFFFF) | ((u32)(v & 0x03FF) << 16); return;
    case 0x26: aram_ar_addr_ = (aram_ar_addr_ & 0xFFFF0000) | (v & 0xFFE0); return;
    case 0x28: aram_count_ = (aram_count_ & 0xFFFF) | ((u32)(v & 0x83FF) << 16); return;
    case 0x2A:
        aram_count_ = (aram_count_ & 0xFFFF0000) | (v & 0xFFE0);
        do_aram_dma();
        return;
    case 0x30: audio_dma_addr_ = (audio_dma_addr_ & 0xFFFF) | ((u32)(v & 0x1FFF) << 16); return;
    case 0x32: audio_dma_addr_ = (audio_dma_addr_ & 0xFFFF0000) | (v & 0xFFE0); return;
    case 0x36: {
        bool was_enabled = audio_dma_control_ & 0x8000;
        audio_dma_control_ = v;
        // New parameters take effect when idle; otherwise at the end of the current transfer.
        if (!was_enabled && (v & 0x8000)) {
            audio_dma_cur_ = audio_dma_addr_;
            audio_dma_left_ = v & 0x7FFF;
            GenerateDSPInterruptFromDSPEmu(INT_AID, 200);
        }
        return;
    }
    }
    LOG_DEBUG("dsp", "write of unknown register %02X <- %04X", off, v);
}

// ARAM on Wii is MEM2. Transfers complete immediately.
void DSPManager::do_aram_dma() {
    bool to_main = aram_count_ & 0x80000000;
    u32 count = aram_count_ & 0x7FFFFFFF;
    u32 mm = aram_mm_addr_ & 0x03FFFFFF, ar = aram_ar_addr_ & 0x03FFFFFF;
    u8* main = mem::ptr(&rt_.cpu, mm, count);
    u8* aram = mem::ptr(&rt_.cpu, 0x10000000 | ar, count);
    if (main && aram)
        std::memmove(to_main ? main : aram, to_main ? aram : main, count);
    else
        LOG_ERROR("dsp", "ARAM DMA out of range: mm %08X ar %08X count %X", mm, ar, count);
    aram_count_ &= 0x80000000;
    generate_interrupt(INT_ARAM);
}

u64 DSPManager::ticks_to_next_event() const {
    u64 next = ~0ull;
    for (const auto& d : delayed_)
        next = std::min<u64>(next, d.first > 0 ? (u64)d.first : 1);
    if (audio_dma_control_ & 0x8000)
        next = std::min<u64>(next, TB_PER_BLOCK - audio_dma_carry_);
    return next;
}

void DSPManager::tick(u64 ticks) {
    // Delayed interrupts (mail and audio DMA start).
    for (auto it = delayed_.begin(); it != delayed_.end();) {
        it->first -= (s64)ticks;
        if (it->first <= 0) {
            generate_interrupt(it->second);
            it = delayed_.erase(it);
        } else {
            ++it;
        }
    }

    // The DSP HLE runs its periodic update at 1 kHz.
    update_carry_ += ticks;
    while (update_carry_ >= TB_PER_SECOND / 1000) {
        update_carry_ -= TB_PER_SECOND / 1000;
        hle_->Update();
    }

    // Audio DMA: one 32-byte block (8 stereo samples) per 8 sample periods at 32 kHz.
    if (!(audio_dma_control_ & 0x8000)) {
        audio_dma_carry_ = 0;
        return;
    }
    audio_dma_carry_ += ticks;
    while (audio_dma_carry_ >= TB_PER_BLOCK) {
        audio_dma_carry_ -= TB_PER_BLOCK;
        if (sink_)
            if (const u8* block = mem::ptr(&rt_.cpu, audio_dma_cur_, 32))
                sink_(sink_user_, block);
        if (audio_dma_left_ != 0) {
            audio_dma_left_--;
            audio_dma_cur_ += 32;
        }
        if (audio_dma_left_ == 0) {
            if (dsp_trace())
                LOG_INFO("dsptrace", "tb %llu AI DMA reload: play %08X (%u blocks)", (unsigned long long)rt_.cpu.timebase,
                         audio_dma_addr_, audio_dma_control_ & 0x7FFF);
            audio_dma_cur_ = audio_dma_addr_;
            audio_dma_left_ = audio_dma_control_ & 0x7FFF;
            generate_interrupt(INT_AID);
        }
    }
}

}  // namespace DSP
