#include "disc.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#include "log.h"

namespace {

std::vector<u8> read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<u8>(std::istreambuf_iterator<char>(in), {});
}

u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
void put32(u8* p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }

}  // namespace

bool Disc::open(const std::filesystem::path& root) {
    root_ = root;
    boot_ = read_file(root / "sys/boot.bin");
    bi2_ = read_file(root / "sys/bi2.bin");
    apploader_ = read_file(root / "sys/apploader.img");
    dol_ = read_file(root / "sys/main.dol");
    fst_ = read_file(root / "sys/fst.bin");
    tmd_ = read_file(root / "tmd.bin");
    ticket_ = read_file(root / "ticket.bin");
    if (boot_.size() < 0x440 || fst_.size() < 12 || tmd_.empty() || ticket_.empty()) {
        LOG_ERROR("disc", "incomplete extracted partition under %s", root.string().c_str());
        return false;
    }

    // Partition layout. Wii offsets in boot.bin are stored divided by 4.
    u64 dol_off = (u64)be32(&boot_[0x420]) << 2, fst_off = (u64)be32(&boot_[0x424]) << 2;
    extents_.push_back({0, boot_.size(), &boot_, {}});
    extents_.push_back({0x440, bi2_.size(), &bi2_, {}});
    extents_.push_back({0x2440, apploader_.size(), &apploader_, {}});
    extents_.push_back({dol_off, dol_.size(), &dol_, {}});
    extents_.push_back({fst_off, fst_.size(), &fst_, {}});

    // FST: 12-byte entries {flags|name_offset, offset (/4), size}; directories store the parent
    // index and the index one past their last child.
    u32 count = be32(&fst_[8]);
    const char* names = (const char*)&fst_[count * 12];
    std::vector<std::pair<u32, std::filesystem::path>> dirs{{count, root / "files"}};
    for (u32 i = 1; i < count; i++) {
        while (dirs.size() > 1 && i >= dirs.back().first)
            dirs.pop_back();
        const u8* e = &fst_[i * 12];
        std::filesystem::path p = dirs.back().second / std::string(names + (be32(e) & 0xFFFFFF));
        if (e[0] & 1)
            dirs.push_back({be32(e + 8), p});
        else
            extents_.push_back({(u64)be32(e + 4) << 2, be32(e + 8), nullptr, p});
    }
    std::sort(extents_.begin(), extents_.end(), [](const Extent& a, const Extent& b) { return a.offset < b.offset; });

    // Raw disc system area: header at 0, partition table at 0x40000, region info at 0x4E000.
    raw_.assign(0x50000, 0);
    auto header = read_file(root / "disc/header.bin");
    if (header.size() < 0x100)
        header.assign(boot_.begin(), boot_.begin() + 0x100);  // partition header is a close copy
    std::memcpy(raw_.data(), header.data(), 0x100);
    put32(&raw_[0x40000], 1);                   // one partition in table 0
    put32(&raw_[0x40004], 0x40020 >> 2);
    put32(&raw_[0x40020], (u32)(GAME_PARTITION_OFFSET >> 2));
    put32(&raw_[0x40024], 0);                   // type 0: game data
    auto region = read_file(root / "disc/region.bin");
    std::memcpy(&raw_[0x4E000], region.data(), std::min<size_t>(region.size(), 0x20));

    LOG_INFO("disc", "%s: %zu extents", std::string((const char*)boot_.data(), 6).c_str(), extents_.size());
    return true;
}

bool Disc::read_partition(u64 offset, u32 length, u8* out) {
    std::memset(out, 0, length);
    u64 end = offset + length;
    auto it = std::upper_bound(extents_.begin(), extents_.end(), offset,
                               [](u64 off, const Extent& e) { return off < e.offset; });
    if (it != extents_.begin())
        --it;
    for (; it != extents_.end() && it->offset < end; ++it) {
        u64 lo = std::max(offset, it->offset), hi = std::min(end, it->offset + it->size);
        if (lo >= hi)
            continue;
        u8* dst = out + (lo - offset);
        u64 src = lo - it->offset;
        if (it->data) {
            std::memcpy(dst, it->data->data() + src, hi - lo);
            continue;
        }
        if (it->file != cached_path_) {
            if (cached_)
                std::fclose(cached_);
            cached_ = std::fopen(it->file.string().c_str(), "rb");
            cached_path_ = it->file;
        }
        if (!cached_ || _fseeki64(cached_, (long long)src, SEEK_SET) != 0 ||
            std::fread(dst, 1, hi - lo, cached_) != hi - lo) {
            LOG_ERROR("disc", "read failed: %s", it->file.string().c_str());
            return false;
        }
    }
    return true;
}

bool Disc::read_raw(u64 offset, u32 length, u8* out) {
    std::memset(out, 0, length);
    if (offset >= raw_.size())
        return true;
    std::memcpy(out, raw_.data() + offset, std::min<u64>(length, raw_.size() - offset));
    return true;
}
