// Host-side access to guest memory (big-endian), for virtual or physical addresses.
#pragma once

#include <string>

#include "guest.h"

namespace mem {

// Host pointer for [addr, addr + len), or nullptr if it is not RAM. Accepts the cached
// (0x8/0x9) and uncached (0xC/0xD) mirrors as well as physical addresses (0x0/0x1).
inline u8* ptr(CPUState* c, u32 addr, u32 len = 1) {
    u8* base;
    u32 size;
    switch (addr >> 28) {
    case 0x0: case 0x8: case 0xC: base = c->ram; size = c->ram_size; break;
    case 0x1: case 0x9: case 0xD: base = c->mem2; size = c->mem2_size; break;
    default: return nullptr;
    }
    u32 off = addr & 0x0FFFFFFF;
    if (!base || off > size || len > size - off)
        return nullptr;
    return base + off;
}

inline u32 bswap32(u32 v) { return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24); }
inline u16 bswap16(u16 v) { return (u16)((v >> 8) | (v << 8)); }

inline u32 read32(CPUState* c, u32 a) {
    const u8* p = ptr(c, a, 4);
    return p ? (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3] : 0;
}
inline u16 read16(CPUState* c, u32 a) {
    const u8* p = ptr(c, a, 2);
    return p ? (u16)(p[0] << 8 | p[1]) : 0;
}
inline u64 read64(CPUState* c, u32 a) { return (u64)read32(c, a) << 32 | read32(c, a + 4); }
inline u8 read8(CPUState* c, u32 a) {
    const u8* p = ptr(c, a, 1);
    return p ? *p : 0;
}
inline void write32(CPUState* c, u32 a, u32 v) {
    if (u8* p = ptr(c, a, 4)) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
}
inline void write16(CPUState* c, u32 a, u16 v) {
    if (u8* p = ptr(c, a, 2)) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
}
inline void write64(CPUState* c, u32 a, u64 v) {
    write32(c, a, (u32)(v >> 32));
    write32(c, a + 4, (u32)v);
}
inline void write8(CPUState* c, u32 a, u8 v) {
    if (u8* p = ptr(c, a, 1)) *p = v;
}

inline std::string cstr(CPUState* c, u32 a, size_t max = 256) {
    std::string s;
    for (u8 ch; s.size() < max && (ch = read8(c, a + (u32)s.size())) != 0;)
        s.push_back((char)ch);
    return s;
}

}  // namespace mem
