// Read-only view of the game disc built from an extracted partition (sys/, files/, disc/).
#pragma once

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "guest.h"

class Disc {
public:
    bool open(const std::filesystem::path& root);

    // Decrypted game-partition data at a byte offset. Gaps read as zero. Returns false on I/O error.
    bool read_partition(u64 offset, u32 length, u8* out);
    // Unencrypted raw disc reads (system area: header, partition table, region info).
    bool read_raw(u64 offset, u32 length, u8* out);

    const std::vector<u8>& header() const { return boot_; }
    const std::vector<u8>& tmd() const { return tmd_; }
    const std::vector<u8>& ticket() const { return ticket_; }
    static constexpr u64 GAME_PARTITION_OFFSET = 0x0F800000;

private:
    struct Extent {
        u64 offset;
        u64 size;
        const std::vector<u8>* data;  // in-memory blob, or
        std::filesystem::path file;   // a file on the host
    };

    std::filesystem::path root_;
    std::vector<u8> boot_, bi2_, apploader_, dol_, fst_, tmd_, ticket_, raw_;
    std::vector<Extent> extents_;  // sorted by offset

    std::FILE* cached_ = nullptr;
    std::filesystem::path cached_path_;
};
