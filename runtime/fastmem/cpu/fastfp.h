/* Fast inline floating-point and paired-single operations for the recompiled chunks (included by
   the shadow cpu/cpu.h, after DolRecomp's declarations; DolRecomp's own cpu.c is untouched).

   DolRecomp's versions model the Gekko exactly, including FPSCR exception, FI/FR and FPRF flags,
   NaN propagation and NI flushing, as out-of-line calls from ~30k sites, which cost ~15% of the
   frame. Games do not read those flags, so these keep only what changes results, as Dolphin's JIT
   does: single rounding, the 25-bit truncation of the multiplier in single multiplies, paired
   results rounded to single, compares setting CR and FPSCR[FPCC], and quantized paired-single
   loads/stores. Multiply-adds use a*c+b in double: a single times a 25-bit multiplier is exact in a
   double, so only the final rounding differs from a fused operation, and only in rare ties. */
#ifndef PANDORA_FASTFP_H
#define PANDORA_FASTFP_H

#include <math.h>
#include <string.h>

PANDORA_INLINE f64 pandora_f64_from_u64(u64 bits) {
    f64 v;
    memcpy(&v, &bits, sizeof v);
    return v;
}
PANDORA_INLINE u64 pandora_u64_from_f64(f64 v) {
    u64 bits;
    memcpy(&bits, &v, sizeof bits);
    return bits;
}

/* The multiplier of single-precision multiplies keeps 25 bits of mantissa (rounded). */
PANDORA_INLINE f64 pandora_f25(f64 value) {
    const u64 bits = pandora_u64_from_f64(value);
    if ((bits & 0x7FF0000000000000ull) == 0x7FF0000000000000ull)
        return value; /* inf/NaN */
    return pandora_f64_from_u64((bits + 0x0000000008000000ull) & 0xFFFFFFFFF8000000ull);
}

PANDORA_INLINE f64 pandora_single(f64 value) { return (f64)(f32)value; }

PANDORA_INLINE void pandora_write_single(CPUState* c, u8 d, f64 value) {
    const f64 r = pandora_single(value);
    c->fpr[d] = r;
    c->ps1[d] = r;
}

PANDORA_INLINE void pandora_fadds(CPUState* c, u8 d, u8 a, u8 b) { pandora_write_single(c, d, c->fpr[a] + c->fpr[b]); }
PANDORA_INLINE void pandora_fsubs(CPUState* c, u8 d, u8 a, u8 b) { pandora_write_single(c, d, c->fpr[a] - c->fpr[b]); }
PANDORA_INLINE void pandora_fmuls(CPUState* c, u8 d, u8 a, u8 cc) {
    pandora_write_single(c, d, c->fpr[a] * pandora_f25(c->fpr[cc]));
}
PANDORA_INLINE void pandora_fdivs(CPUState* c, u8 d, u8 a, u8 b) { pandora_write_single(c, d, c->fpr[a] / c->fpr[b]); }
PANDORA_INLINE void pandora_fadd(CPUState* c, u8 d, u8 a, u8 b) { c->fpr[d] = c->fpr[a] + c->fpr[b]; }
PANDORA_INLINE void pandora_fsub(CPUState* c, u8 d, u8 a, u8 b) { c->fpr[d] = c->fpr[a] - c->fpr[b]; }
PANDORA_INLINE void pandora_fmul(CPUState* c, u8 d, u8 a, u8 cc) { c->fpr[d] = c->fpr[a] * c->fpr[cc]; }
PANDORA_INLINE void pandora_fdiv(CPUState* c, u8 d, u8 a, u8 b) { c->fpr[d] = c->fpr[a] / c->fpr[b]; }
PANDORA_INLINE void pandora_frsp(CPUState* c, u8 d, u8 b) { pandora_write_single(c, d, c->fpr[b]); }

/* fmadd family: out = +-(a*c +- b). Always "succeeds" (no enabled exceptions). */
PANDORA_INLINE bool pandora_fma(CPUState* cpu, f64 a, f64 c, f64 b, bool single, bool subtract, bool negative,
                                   f64* out) {
    (void)cpu;
    const f64 addend = subtract ? -b : b;
    f64 r = single ? pandora_single(a * pandora_f25(c) + addend) : a * c + addend;
    if (negative && !isnan(r))
        r = -r;
    *out = r;
    return true;
}

/* Compare: CR field crfd and FPSCR[FPCC] get FL/FG/FE/FU. */
PANDORA_INLINE void pandora_fcmp(CPUState* c, u8 crfd, f64 a, f64 b, bool ordered) {
    (void)ordered;
    const u32 bits = a < b ? 8u : a > b ? 4u : a == b ? 2u : 1u;
    c->fpscr = (c->fpscr & ~(0xFu << 12)) | (bits << 12);
    const u32 shift = 4u * (7u - crfd);
    c->cr = (c->cr & ~(0xFu << shift)) | (bits << shift);
}

PANDORA_INLINE void pandora_write_paired(CPUState* c, u8 d, f64 ps0, f64 ps1) {
    c->fpr[d] = pandora_single(ps0);
    c->ps1[d] = pandora_single(ps1);
}

PANDORA_INLINE void pandora_ps_add_op(CPUState* c, u8 d, u8 a, u8 b) {
    pandora_write_paired(c, d, c->fpr[a] + c->fpr[b], c->ps1[a] + c->ps1[b]);
}
PANDORA_INLINE void pandora_ps_sub_op(CPUState* c, u8 d, u8 a, u8 b) {
    pandora_write_paired(c, d, c->fpr[a] - c->fpr[b], c->ps1[a] - c->ps1[b]);
}
PANDORA_INLINE void pandora_ps_mul_op(CPUState* c, u8 d, u8 a, u8 cc) {
    pandora_write_paired(c, d, c->fpr[a] * pandora_f25(c->fpr[cc]), c->ps1[a] * pandora_f25(c->ps1[cc]));
}
PANDORA_INLINE void pandora_ps_muls0(CPUState* c, u8 d, u8 a, u8 cc) {
    const f64 s = pandora_f25(c->fpr[cc]);
    pandora_write_paired(c, d, c->fpr[a] * s, c->ps1[a] * s);
}
PANDORA_INLINE void pandora_ps_muls1(CPUState* c, u8 d, u8 a, u8 cc) {
    const f64 s = pandora_f25(c->ps1[cc]);
    pandora_write_paired(c, d, c->fpr[a] * s, c->ps1[a] * s);
}
PANDORA_INLINE void pandora_ps_madd_op(CPUState* c, u8 d, u8 a, u8 cc, u8 b, bool subtract, bool negative) {
    f64 ps0 = c->fpr[a] * pandora_f25(c->fpr[cc]) + (subtract ? -c->fpr[b] : c->fpr[b]);
    f64 ps1 = c->ps1[a] * pandora_f25(c->ps1[cc]) + (subtract ? -c->ps1[b] : c->ps1[b]);
    if (negative) {
        ps0 = pandora_single(ps0);
        ps1 = pandora_single(ps1);
        if (!isnan(ps0))
            ps0 = -ps0;
        if (!isnan(ps1))
            ps1 = -ps1;
    }
    pandora_write_paired(c, d, ps0, ps1);
}
PANDORA_INLINE void pandora_ps_madds0(CPUState* c, u8 d, u8 a, u8 cc, u8 b) {
    const f64 s = pandora_f25(c->fpr[cc]);
    pandora_write_paired(c, d, c->fpr[a] * s + c->fpr[b], c->ps1[a] * s + c->ps1[b]);
}
PANDORA_INLINE void pandora_ps_madds1(CPUState* c, u8 d, u8 a, u8 cc, u8 b) {
    const f64 s = pandora_f25(c->ps1[cc]);
    pandora_write_paired(c, d, c->fpr[a] * s + c->fpr[b], c->ps1[a] * s + c->ps1[b]);
}
PANDORA_INLINE void pandora_ps_sum0(CPUState* c, u8 d, u8 a, u8 cc, u8 b) {
    pandora_write_paired(c, d, c->fpr[a] + c->ps1[b], c->ps1[cc]);
}
PANDORA_INLINE void pandora_ps_sum1(CPUState* c, u8 d, u8 a, u8 cc, u8 b) {
    pandora_write_paired(c, d, c->fpr[cc], c->fpr[a] + c->ps1[b]);
}

/* Paired-single quantized loads/stores. GQR: load type/scale in bits 16-18/24-29, store in 0-2/8-13.
   Types: 0 float, 4 u8, 5 u16, 6 s8, 7 s16; 1-3 are invalid (left to DolRecomp, which raises the
   exception). Scales are signed 6-bit powers of two. */
PANDORA_INLINE f64 pandora_pow2(int e) { return pandora_f64_from_u64((u64)(1023 + e) << 52); }
PANDORA_INLINE int pandora_gqr_scale(u32 field) { return ((int)(field & 0x3F) ^ 0x20) - 0x20; }

PANDORA_INLINE f64 pandora_load_f32(CPUState* c, u32 ea) {
    const u32 bits = pandora_mem_read32(c, ea);
    f32 f;
    memcpy(&f, &bits, sizeof f);
    return (f64)f;
}

PANDORA_INLINE f64 pandora_psq_value(CPUState* c, u32 ea, u32 type, f64 factor) {
    switch (type) {
    case 4: return pandora_single((f64)pandora_mem_read8(c, ea) * factor);
    case 5: return pandora_single((f64)pandora_mem_read16(c, ea) * factor);
    case 6: return pandora_single((f64)(s8)pandora_mem_read8(c, ea) * factor);
    default: return pandora_single((f64)(s16)pandora_mem_read16(c, ea) * factor);
    }
}

PANDORA_INLINE bool pandora_psq_load(CPUState* c, u8 d, u32 ea, bool w, u8 gqr, bool indexed, u32 cia) {
    const u32 g = c->gqr[gqr & 7u];
    const u32 type = (g >> 16) & 7u;
    if (type == 0) {
        if (ea & 3u)
            return ppc_psq_load(c, d, ea, w, gqr, indexed, cia); /* alignment exception */
        c->fpr[d] = pandora_load_f32(c, ea);
        c->ps1[d] = w ? 1.0 : pandora_load_f32(c, ea + 4);
        return true;
    }
    if (type < 4)
        return ppc_psq_load(c, d, ea, w, gqr, indexed, cia);
    const f64 factor = pandora_pow2(-pandora_gqr_scale(g >> 24));
    const u32 size = (type & 1) ? 2u : 1u;
    c->fpr[d] = pandora_psq_value(c, ea, type, factor);
    c->ps1[d] = w ? 1.0 : pandora_psq_value(c, ea + size, type, factor);
    return true;
}

PANDORA_INLINE void pandora_store_f32(CPUState* c, u32 ea, f64 value) {
    const f32 f = (f32)value;
    u32 bits;
    memcpy(&bits, &f, sizeof bits);
    if ((bits & 0x7F800000u) == 0)
        bits &= 0x80000000u; /* denormals store as zero (as DolRecomp; keeps the sign) */
    pandora_mem_write32(c, ea, bits);
}

PANDORA_INLINE s32 pandora_quantize(f64 value, f64 factor, s32 lo, s32 hi) {
    if (isnan(value))
        return hi;
    const f64 scaled = trunc(value * factor);
    return scaled <= (f64)lo ? lo : scaled >= (f64)hi ? hi : (s32)scaled;
}

PANDORA_INLINE void pandora_psq_put(CPUState* c, u32 ea, u32 type, f64 factor, f64 value) {
    switch (type) {
    case 4: pandora_mem_write8(c, ea, (u8)pandora_quantize(value, factor, 0, 255)); break;
    case 5: pandora_mem_write16(c, ea, (u16)pandora_quantize(value, factor, 0, 65535)); break;
    case 6: pandora_mem_write8(c, ea, (u8)(s8)pandora_quantize(value, factor, -128, 127)); break;
    default: pandora_mem_write16(c, ea, (u16)(s16)pandora_quantize(value, factor, -32768, 32767)); break;
    }
}

PANDORA_INLINE bool pandora_psq_store(CPUState* c, u8 s, u32 ea, bool w, u8 gqr, bool indexed, u32 cia) {
    const u32 g = c->gqr[gqr & 7u];
    const u32 type = g & 7u;
    if (type == 0) {
        if (ea & 3u)
            return ppc_psq_store(c, s, ea, w, gqr, indexed, cia);
        pandora_store_f32(c, ea, c->fpr[s]);
        if (!w)
            pandora_store_f32(c, ea + 4, c->ps1[s]);
        return true;
    }
    if (type < 4)
        return ppc_psq_store(c, s, ea, w, gqr, indexed, cia);
    const f64 factor = pandora_pow2(pandora_gqr_scale(g >> 8));
    const u32 size = (type & 1) ? 2u : 1u;
    pandora_psq_put(c, ea, type, factor, c->fpr[s]);
    if (!w)
        pandora_psq_put(c, ea + size, type, factor, c->ps1[s]);
    return true;
}

#define ppc_fadds pandora_fadds
#define ppc_fsubs pandora_fsubs
#define ppc_fmuls pandora_fmuls
#define ppc_fdivs pandora_fdivs
#define ppc_fadd pandora_fadd
#define ppc_fsub pandora_fsub
#define ppc_fmul pandora_fmul
#define ppc_fdiv pandora_fdiv
#define ppc_frsp pandora_frsp
#define ppc_fma pandora_fma
#define ppc_fcmp pandora_fcmp
#define ppc_ps_add_op pandora_ps_add_op
#define ppc_ps_sub_op pandora_ps_sub_op
#define ppc_ps_mul_op pandora_ps_mul_op
#define ppc_ps_muls0 pandora_ps_muls0
#define ppc_ps_muls1 pandora_ps_muls1
#define ppc_ps_madd_op pandora_ps_madd_op
#define ppc_ps_madds0 pandora_ps_madds0
#define ppc_ps_madds1 pandora_ps_madds1
#define ppc_ps_sum0 pandora_ps_sum0
#define ppc_ps_sum1 pandora_ps_sum1
#define ppc_psq_load_inline pandora_psq_load
#define ppc_psq_store_inline pandora_psq_store

#endif
