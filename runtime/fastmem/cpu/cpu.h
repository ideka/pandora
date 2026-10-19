/* Shadows DolRecomp's cpu/cpu.h for the recompiled chunks only (the include path lists this
   directory first). It adds inline fast paths for guest RAM accesses, which the chunks otherwise
   make through out-of-line calls into cpu.c. Anything that is not plain cached MEM1/MEM2, plus
   stores while a write journal (watchpoint) or lwarx reservation is active, takes the original
   path. */
#ifndef PANDORA_FASTMEM_CPU_H
#define PANDORA_FASTMEM_CPU_H

#include <string.h>

#include "../../../extern/DolRecomp/src/cpu/cpu.h"

/* The locked L1 cache: 16 KB of scratchpad at 0xE0000000, owned by the runtime. */
#define PANDORA_LC_BASE 0xE0000000u
#define PANDORA_LC_SIZE 0x4000u
#ifdef __cplusplus
extern "C" u8* g_pandora_locked_cache;
#else
extern u8* g_pandora_locked_cache;
#endif

static inline u8* pandora_host(CPUState* c, u32 a, u32 n) {
    u32 off = a - 0x80000000u;
    if (off <= c->ram_size - n)
        return c->ram + off;
    off = a - 0x90000000u;
    if (off <= c->mem2_size - n)
        return c->mem2 + off;
    off = a - PANDORA_LC_BASE;
    if (off <= PANDORA_LC_SIZE - n)
        return g_pandora_locked_cache + off;
    return NULL;
}

static inline int pandora_store_fast(CPUState* c) { return !g_mem_write_journal && !c->reserve_valid; }

static inline u8 pandora_mem_read8(CPUState* c, u32 a) {
    const u8* p = pandora_host(c, a, 1);
    return p ? *p : mem_read8(c, a);
}
static inline u16 pandora_mem_read16(CPUState* c, u32 a) {
    const u8* p = pandora_host(c, a, 2);
    u16 v;
    if (!p)
        return mem_read16(c, a);
    memcpy(&v, p, 2);
    return bswap16(v);
}
static inline u32 pandora_mem_read32(CPUState* c, u32 a) {
    const u8* p = pandora_host(c, a, 4);
    u32 v;
    if (!p)
        return mem_read32(c, a);
    memcpy(&v, p, 4);
    return bswap32(v);
}
static inline u64 pandora_mem_read64(CPUState* c, u32 a) {
    const u8* p = pandora_host(c, a, 8);
    u32 hi, lo;
    if (!p)
        return mem_read64(c, a);
    memcpy(&hi, p, 4);
    memcpy(&lo, p + 4, 4);
    return (u64)bswap32(hi) << 32 | bswap32(lo);
}
static inline void pandora_mem_write8(CPUState* c, u32 a, u8 v) {
    u8* p = pandora_host(c, a, 1);
    if (p && pandora_store_fast(c))
        *p = v;
    else
        mem_write8(c, a, v);
}
static inline void pandora_mem_write16(CPUState* c, u32 a, u16 v) {
    u8* p = pandora_host(c, a, 2);
    if (p && pandora_store_fast(c)) {
        v = bswap16(v);
        memcpy(p, &v, 2);
    } else {
        mem_write16(c, a, v);
    }
}
static inline void pandora_mem_write32(CPUState* c, u32 a, u32 v) {
    u8* p = pandora_host(c, a, 4);
    if (p && pandora_store_fast(c)) {
        v = bswap32(v);
        memcpy(p, &v, 4);
    } else {
        mem_write32(c, a, v);
    }
}
static inline void pandora_mem_write64(CPUState* c, u32 a, u64 v) {
    u8* p = pandora_host(c, a, 8);
    if (p && pandora_store_fast(c)) {
        u32 hi = bswap32((u32)(v >> 32)), lo = bswap32((u32)v);
        memcpy(p, &hi, 4);
        memcpy(p + 4, &lo, 4);
    } else {
        mem_write64(c, a, v);
    }
}

#define mem_read8 pandora_mem_read8
#define mem_read16 pandora_mem_read16
#define mem_read32 pandora_mem_read32
#define mem_read64 pandora_mem_read64
#define mem_write8 pandora_mem_write8
#define mem_write16 pandora_mem_write16
#define mem_write32 pandora_mem_write32
#define mem_write64 pandora_mem_write64

#endif /* PANDORA_FASTMEM_CPU_H */
