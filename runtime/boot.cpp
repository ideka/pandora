// Boot without the apploader: load main.dol and recreate the low-memory state that the system
// menu, apploader and IOS56 leave behind. Values follow Dolphin's HLE boot
// (Boot_BS2Emu.cpp, IOS/VersionInfo.cpp).
#include <cstring>
#include <fstream>
#include <vector>

#include "dsp/dsp.h"
#include "hardware.h"
#include "ios.h"
#include "log.h"
#include "memory.h"
#include "guest_memory.h"
#include "runtime.h"

namespace {

std::vector<u8> read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<u8>(std::istreambuf_iterator<char>(in), {});
}

u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

// IOS56 memory values.
constexpr u32 MEM1_SIZE = 0x01800000;
constexpr u32 MEM2_SIZE = 0x04000000;
constexpr u32 IOS56_VERSION = 0x0038161E;
constexpr u32 IOS56_DATE = 0x00030110;
constexpr u32 MEM2_END = 0x93600000;
constexpr u32 MEM2_ARENA_BEGIN = 0x90000800;
constexpr u32 MEM2_ARENA_END = 0x935E0000;
constexpr u32 IPC_BUFFER_BEGIN = 0x935E0000, IPC_BUFFER_END = 0x93600000;
constexpr u32 IOS_RESERVED_BEGIN = 0x93600000, IOS_RESERVED_END = 0x93620000;
constexpr u32 PLACEHOLDER = 0xDEADBEEF;

constexpr u32 GAME_PARTITION_OFFSET = 0x0F800000;

}  // namespace

bool Runtime::boot() {
    if (!syms_.load(opts_.map)) {
        LOG_ERROR("boot", "cannot load symbol map %s", opts_.map.c_str());
        return false;
    }
    if (!cpu_init(&cpu) || !cpu_alloc_mem2(&cpu, MEM2_SIZE) || !guest_memory::adopt(&cpu) ||
        !ios_->init(opts_.disc, opts_.nand))
        return false;
    cpu.external_user_data = this;
    cpu.host_call = host_call;
    cpu.external_read = external_read;
    cpu.external_write = external_write;
    cpu.spr_read = spr_read;
    cpu.spr_write = spr_write;
    cpu.instruction_fallback = instruction_fallback;
    CPUState* c = &cpu;

    auto dol = read_file(opts_.disc / "sys/main.dol");
    auto header = read_file(opts_.disc / "sys/boot.bin");
    auto bi2 = read_file(opts_.disc / "sys/bi2.bin");
    auto fst = read_file(opts_.disc / "sys/fst.bin");
    if (dol.size() < 0x100 || header.size() < 0x440 || bi2.size() < 0x2000 || fst.empty()) {
        LOG_ERROR("boot", "missing or truncated files under %s/sys", opts_.disc.string().c_str());
        return false;
    }

    // DOL sections. The header's BSS range encloses .sdata/.sdata2, so clear it before loading.
    u32 bss = be32(&dol[0xD8]), bss_size = be32(&dol[0xDC]), entry = be32(&dol[0xE0]);
    std::memset(mem::ptr(c, bss, bss_size), 0, bss_size);
    for (int i = 0; i < 18; i++) {
        u32 off = be32(&dol[i * 4]), addr = be32(&dol[0x48 + i * 4]), size = be32(&dol[0x90 + i * 4]);
        if (!size)
            continue;
        u8* dst = mem::ptr(c, addr, size);
        if (!dst || off + size > dol.size()) {
            LOG_ERROR("boot", "bad DOL section %d", i);
            return false;
        }
        std::memcpy(dst, &dol[off], size);
    }

    // Disc header (game ID etc.) and the 4-byte ID copy checked by the SDK.
    std::memcpy(mem::ptr(c, 0x80000000, 0x20), header.data(), 0x20);
    std::memcpy(mem::ptr(c, 0x80003180, 4), header.data(), 4);

    // IOS memory values.
    mem::write32(c, 0x80003100, MEM1_SIZE);
    mem::write32(c, 0x80003104, MEM1_SIZE);
    mem::write32(c, 0x80003108, 0x80000000 + MEM1_SIZE);
    mem::write32(c, 0x8000310C, 0);
    mem::write32(c, 0x80003110, 0x80000000 + MEM1_SIZE);
    mem::write32(c, 0x80003114, PLACEHOLDER);
    mem::write32(c, 0x80003118, MEM2_SIZE);
    mem::write32(c, 0x8000311C, MEM2_SIZE);
    mem::write32(c, 0x80003120, MEM2_END);
    mem::write32(c, 0x80003124, MEM2_ARENA_BEGIN);
    mem::write32(c, 0x80003128, MEM2_ARENA_END);
    mem::write32(c, 0x8000312C, PLACEHOLDER);
    mem::write32(c, 0x80003130, IPC_BUFFER_BEGIN);
    mem::write32(c, 0x80003134, IPC_BUFFER_END);
    mem::write32(c, 0x80003138, 0x00000011);  // Hollywood revision
    mem::write32(c, 0x8000313C, PLACEHOLDER);
    mem::write32(c, 0x80003140, IOS56_VERSION);
    mem::write32(c, 0x80003144, IOS56_DATE);
    mem::write32(c, 0x80003148, IOS_RESERVED_BEGIN);
    mem::write32(c, 0x8000314C, IOS_RESERVED_END);
    mem::write32(c, 0x80003150, PLACEHOLDER);
    mem::write32(c, 0x80003154, PLACEHOLDER);
    mem::write32(c, 0x80003158, 0x0000FF01);  // RAM vendor
    mem::write16(c, 0x8000315E, 0x0113);      // devkit boot program version
    mem::write8(c, 0x8000315C, 0x80);         // OSInit

    // System menu / apploader globals.
    mem::write32(c, 0x80000020, 0x0D15EA5E);
    mem::write32(c, 0x80000024, 0x00000001);
    mem::write32(c, 0x80000028, MEM1_SIZE);
    mem::write32(c, 0x8000002C, 0x00000023);  // board model: RVL retail 3
    mem::write32(c, 0x800000CC, 1);           // VI mode set by the IPL: PAL (SX3P01)
    mem::write32(c, 0x800000F0, MEM1_SIZE);
    mem::write32(c, 0x800000F8, 0x0E7BE2C0);  // bus clock 243 MHz
    mem::write32(c, 0x800000FC, 0x2B73A840);  // CPU clock 729 MHz
    mem::write32(c, 0x800030D8, 0xFFFFFFFF);
    mem::write16(c, 0x800030E6, 0x8201);
    mem::write32(c, 0x80003184, 0x80000000);  // game ID address
    mem::write32(c, 0x80003188, IOS56_VERSION);  // IOS the title was launched with (OSInit checks it)
    mem::write8(c, 0x8000319C, 0x80);         // single-layer disc
    mem::write32(c, 0x80003194, 0);           // data partition type
    mem::write32(c, 0x80003198, GAME_PARTITION_OFFSET >> 2);

    // The apploader places the FST and bi2 at the top of MEM1 and lowers ArenaHi below them.
    u32 fst_addr = (0x80000000 + MEM1_SIZE - (u32)fst.size()) & ~0x1Fu;
    std::memcpy(mem::ptr(c, fst_addr, (u32)fst.size()), fst.data(), fst.size());
    u32 bi2_addr = (fst_addr - 0x2000) & ~0x1Fu;
    std::memcpy(mem::ptr(c, bi2_addr, 0x2000), bi2.data(), 0x2000);
    mem::write32(c, 0x80000030, 0);
    mem::write32(c, 0x80000034, bi2_addr);
    mem::write32(c, 0x80003110, bi2_addr);  // MEM1 arena end: the Wii OS takes ArenaHi from here
    mem::write32(c, 0x80000038, fst_addr);
    mem::write32(c, 0x8000003C, (u32)fst.size());
    mem::write32(c, 0x800000F4, bi2_addr);

    // CPU state after the apploader: translation and FP on, interrupts off.
    cpu.msr = 0x00002032;
    cpu.hid2 = 0xE0000000;
    spr_[1008] = 0x0011C664;  // HID0
    spr_[1011] = 0x83900000;  // HID4
    cpu.gpr[1] = 0x816FFFF0;
    cpu.pc = entry;

    if (!opts_.audio_dump.empty()) {
        if (!audio_dump_.open(opts_.audio_dump)) {
            LOG_ERROR("boot", "cannot write %s", opts_.audio_dump.c_str());
            return false;
        }
    }
    hw_->dsp().set_audio_sink(&Runtime::audio_sink, this);
    install_hle();
    if (!check_hooks())
        return false;
    LOG_INFO("boot", "loaded %s, entry %s, FST at %08X (%zu bytes)", "main.dol", describe(entry).c_str(), fst_addr,
             fst.size());
    return true;
}
