// Minimal stand-ins for the Dolphin APIs used by the vendored DSP HLE code (ax*.cpp,
// accelerator.cpp). Only what that code needs; save states, logging and analytics are no-ops.
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>

extern "C" {
#include "common/types.h"
}
#include "../memory.h"

#define ASSERT(cond) ((void)0)
#define ERROR_LOG_FMT(...) ((void)0)
#define WARN_LOG_FMT(...) ((void)0)
#define INFO_LOG_FMT(...) ((void)0)
#define DEBUG_LOG_FMT(...) ((void)0)
#define PanicAlertFmt(...) ((void)0)
#define PanicAlertFmtT(...) ((void)0)

namespace Common {
inline u16 swap16(u16 v) { return (u16)((v >> 8) | (v << 8)); }
inline u32 swap32(u32 v) { return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24); }
inline u16 swap16(const u8* p) { return (u16)(p[0] << 8 | p[1]); }
inline u32 swap32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

template <typename T, typename From>
std::array<T, sizeof(From) / sizeof(T)> BitCastToArray(const From& from) {
    std::array<T, sizeof(From) / sizeof(T)> out;
    std::memcpy(out.data(), &from, sizeof(From));
    return out;
}

inline u32 HashEctor(const u8* data, size_t len) {
    u32 crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        crc = (crc << 3) | (crc >> 29);
    }
    return crc;
}
}  // namespace Common

namespace MathUtil {
template <typename To, typename From>
constexpr To SaturatingCast(From value) {
    return (To)std::clamp<From>(value, (From)std::numeric_limits<To>::min(), (From)std::numeric_limits<To>::max());
}
}  // namespace MathUtil

// Dolphin's BitField: a bit range of the enclosing union's storage, readable as T.
template <typename T, bool = std::is_enum_v<T>>
struct BitFieldStorage {
    using type = T;
};
template <typename T>
struct BitFieldStorage<T, true> {
    using type = std::underlying_type_t<T>;
};
template <std::size_t Position, std::size_t Bits, typename T, typename Storage = typename BitFieldStorage<T>::type>
struct BitField {
    Storage storage;
    static constexpr Storage mask() { return (Storage)(((1ull << Bits) - 1) << Position); }
    constexpr operator T() const { return (T)((storage & mask()) >> Position); }
    BitField& operator=(T v) {
        storage = (Storage)((storage & ~mask()) | (((Storage)v << Position) & mask()));
        return *this;
    }
};

// Save states are not supported; DoState() bodies compile against this no-op.
class PointerWrap {
public:
    template <typename T>
    void Do(T&) {}
    template <typename T>
    void DoArray(T*, size_t) {}
    bool IsReadMode() const { return false; }
    void SetVerifyMode() {}
};

namespace Core {
inline void DisplayMessage(const std::string&, int) {}
}

enum class GameQuirk {
    UsesAXInitialTimeDelay,
    UsesAXWiimoteBiquad,
    UsesAXWiimoteLowPass,
    UsesUnimplementedAXCommand,
};
struct DolphinAnalytics {
    static DolphinAnalytics& Instance() {
        static DolphinAnalytics instance;
        return instance;
    }
    void ReportGameQuirk(GameQuirk) {}
};

namespace Memory {
// Guest memory as seen by the DSP: physical (or mirrored) MEM1/MEM2 addresses, big-endian data.
class MemoryManager {
public:
    explicit MemoryManager(CPUState* cpu) : cpu_(cpu) {}

    u8* GetPointerForRange(u32 addr, size_t size) const {
        if (u8* p = mem::ptr(cpu_, addr, (u32)size))
            return p;
        return scratch(size);
    }
    u8 Read_U8(u32 addr) const { return mem::read8(cpu_, addr); }
    u32 GetRamMask() const { return 0x01FFFFFF; }

    void CopyToEmu(u32 addr, const void* src, size_t size) const {
        if (u8* p = mem::ptr(cpu_, addr, (u32)size))
            std::memcpy(p, src, size);
    }
    void CopyFromEmu(void* dst, u32 addr, size_t size) const {
        if (const u8* p = mem::ptr(cpu_, addr, (u32)size))
            std::memcpy(dst, p, size);
        else
            std::memset(dst, 0, size);
    }
    // Copy host values of type T to guest memory as big-endian (size in bytes).
    template <typename T>
    void CopyToEmuSwapped(u32 addr, const T* src, size_t size) const {
        u8* p = mem::ptr(cpu_, addr, (u32)size);
        if (!p)
            return;
        for (size_t i = 0; i < size / sizeof(T); i++, p += sizeof(T))
            store_be(p, src[i]);
    }
    template <typename T>
    void CopyFromEmuSwapped(T* dst, u32 addr, size_t size) const {
        const u8* p = mem::ptr(cpu_, addr, (u32)size);
        for (size_t i = 0; i < size / sizeof(T); i++)
            dst[i] = p ? load_be<T>(p + i * sizeof(T)) : T{};
    }

private:
    template <typename T>
    static void store_be(u8* p, T v) {
        using U = std::make_unsigned_t<T>;
        U u = (U)v;
        for (size_t b = 0; b < sizeof(T); b++)
            p[b] = (u8)(u >> (8 * (sizeof(T) - 1 - b)));
    }
    template <typename T>
    static T load_be(const u8* p) {
        using U = std::make_unsigned_t<T>;
        U u = 0;
        for (size_t b = 0; b < sizeof(T); b++)
            u = (U)((u << 8) | p[b]);
        return (T)u;
    }
    // Out-of-range DSP accesses read zeros and write nowhere instead of crashing the host.
    static u8* scratch(size_t size) {
        static std::array<u8, 0x10000> buf;
        std::memset(buf.data(), 0, buf.size());
        return size <= buf.size() ? buf.data() : nullptr;
    }

    CPUState* cpu_;
};
}  // namespace Memory
