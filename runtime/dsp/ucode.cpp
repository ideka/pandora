// Adapted from Dolphin (Source/Core/Core/HW/DSPHLE/UCodes/UCodes.cpp and ROM.cpp, tag 2606).
// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Only the microcodes a Wii AX title uses are kept: the DSP ROM bootloader and AX (Wii).
#include "ucode.h"

#include "ax_wii.h"
#include "dsp.h"
#include "../log.h"

namespace DSP::HLE
{
UCodeInterface::UCodeInterface(DSPHLE* dsphle, u32 crc)
    : m_mail_handler(dsphle->AccessMailHandler()), m_dsphle(dsphle), m_crc(crc)
{
}

UCodeInterface::~UCodeInterface() = default;

bool UCodeInterface::NeedsResumeMail()
{
  if (m_needs_resume_mail)
  {
    m_needs_resume_mail = false;
    return true;
  }
  return false;
}

void UCodeInterface::PrepareBootUCode(u32 mail)
{
  switch (m_next_ucode_steps)
  {
  case 0: m_next_ucode.mram_dest_addr = mail; break;
  case 1: m_next_ucode.mram_size = mail & 0xffff; break;
  case 2: m_next_ucode.mram_dram_addr = mail & 0xffff; break;
  case 3: m_next_ucode.iram_mram_addr = mail; break;
  case 4: m_next_ucode.iram_size = mail & 0xffff; break;
  case 5: m_next_ucode.iram_dest = mail & 0xffff; break;
  case 6: m_next_ucode.iram_startpc = mail & 0xffff; break;
  case 7: m_next_ucode.dram_mram_addr = mail; break;
  case 8: m_next_ucode.dram_size = mail & 0xffff; break;
  case 9: m_next_ucode.dram_dest = mail & 0xffff; break;
  }
  m_next_ucode_steps++;

  if (m_next_ucode_steps == 10)
  {
    m_next_ucode_steps = 0;
    m_needs_resume_mail = true;
    m_upload_setup_in_progress = false;

    auto& memory = m_dsphle->GetSystem().GetMemory();
    const u8* pointer = memory.GetPointerForRange(m_next_ucode.iram_mram_addr, m_next_ucode.iram_size);
    const u32 ector_crc = Common::HashEctor(pointer, m_next_ucode.iram_size);
    m_dsphle->SwapUCode(ector_crc);
  }
}

void UCodeInterface::DoStateShared(PointerWrap&)
{
}

namespace
{
// The DSP ROM: announces itself, then receives the descriptor of the microcode to boot.
class ROMUCode final : public UCodeInterface
{
public:
  ROMUCode(DSPHLE* dsphle, u32 crc) : UCodeInterface(dsphle, crc) {}

  void Initialize() override { m_mail_handler.PushMail(0x8071FEED); }
  void Update() override {}
  void DoState(PointerWrap&) override {}

  void HandleMail(u32 mail) override
  {
    if (m_next_parameter == 0)
    {
      if ((mail & 0xFFFF0000) != 0x80F30000)
        m_mail_handler.PushMail(0xFEEE0000 | (mail & 0xFFFF));
      else
        m_next_parameter = mail;
      return;
    }
    switch (m_next_parameter)
    {
    case 0x80F3A001: m_ram_address = mail; break;
    case 0x80F3A002: m_length = mail & 0xffff; break;
    case 0x80F3B002: break;  // DMEM length
    case 0x80F3C002: break;  // IMEM address
    case 0x80F3D001: {       // start PC: boot it
      auto& memory = m_dsphle->GetSystem().GetMemory();
      const u32 crc = Common::HashEctor(memory.GetPointerForRange(m_ram_address, m_length), m_length);
      m_dsphle->SetUCode(crc);  // destroys this object
      return;
    }
    }
    m_next_parameter = 0;
  }

private:
  u32 m_next_parameter = 0;
  u32 m_ram_address = 0;
  u32 m_length = 0;
};
}  // namespace

std::unique_ptr<UCodeInterface> UCodeFactory(u32 crc, DSPHLE* dsphle, bool /*wii*/)
{
  if (crc == UCODE_ROM)
    return std::make_unique<ROMUCode>(dsphle, crc);
  if (crc == UCODE_NULL)
    return nullptr;
  // Every Wii AX ucode is handled by the AXWii HLE; CRC-specific quirks are inside it.
  LOG_INFO("dsp", "booting ucode %08X as AX (Wii)", crc);
  return std::make_unique<AXWiiUCode>(dsphle, crc);
}
}  // namespace DSP::HLE
