/* Fast inline floating-point and paired-single operations for the recompiled chunks (included by
   the shadow cpu/cpu.h, after DolRecomp's declarations; DolRecomp's own cpu.c is untouched).

   DolRecomp's versions are out-of-line calls from ~30k sites that also maintain FPSCR exception,
   FI/FR and FPRF bits. These produce the same results bit for bit (runtime/fp_selftest.c checks
   that against cpu.c: run pandora --fp-selftest) without the flag bookkeeping, which games do not
   read: single rounding with FPSCR[NI] flushing, the 25-bit multiplier of single multiplies,
   DolRecomp's NaN propagation, fused single multiply-adds, compares (CR and FPSCR[FPCC]), and
   quantized paired-single loads/stores. */
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

/* The multiplier of single-precision multiplies keeps 25 bits of mantissa, as DolRecomp's (and
   Dolphin's) force_25_bit: (bits & keep) + (bits & round bit). Denormals out of line. */
PANDORA_NOINLINE f64 pandora_f25_denormal(u64 bits) {
    const u64 fraction = bits & 0x000FFFFFFFFFFFFFull;
    const unsigned shift = (unsigned)__builtin_clzll(fraction) - 11;
    u64 keep = ~0ull, round = 0;
    if (shift < 28) {
        keep = ~((1ull << (27 - shift)) - 1);
        round = 0x0000000008000000ull >> shift;
    }
    return pandora_f64_from_u64((bits & keep) + (bits & round));
}
PANDORA_INLINE f64 pandora_f25(f64 value) {
    const u64 bits = pandora_u64_from_f64(value);
    if ((bits & 0x7FF0000000000000ull) == 0 && (bits & 0x000FFFFFFFFFFFFFull) != 0)
        return pandora_f25_denormal(bits);
    return pandora_f64_from_u64((bits & 0xFFFFFFFFF8000000ull) + (bits & 0x0000000008000000ull));
}

PANDORA_INLINE f64 pandora_single(f64 value) { return (f64)(f32)value; }

/* NaN results as DolRecomp: the first NaN operand, quieted, else the default NaN. */
PANDORA_INLINE f64 pandora_quiet(f64 v) { return pandora_f64_from_u64(pandora_u64_from_f64(v) | 0x0008000000000000ull); }
PANDORA_NOINLINE f64 pandora_nan2(f64 a, f64 b) {
    if (isnan(a))
        return pandora_quiet(a);
    if (isnan(b))
        return pandora_quiet(b);
    return pandora_f64_from_u64(0x7FF8000000000000ull);
}
PANDORA_NOINLINE f64 pandora_nan3(f64 a, f64 b, f64 c) {
    if (isnan(a))
        return pandora_quiet(a);
    if (isnan(b))
        return pandora_quiet(b);
    if (isnan(c))
        return pandora_quiet(c);
    return pandora_f64_from_u64(0x7FF8000000000000ull);
}
#define PANDORA_NAN2(r, a, b) (__builtin_expect(isnan(r), 0) ? pandora_nan2(a, b) : (r))
#define PANDORA_NAN3(r, a, b, c) (__builtin_expect(isnan(r), 0) ? pandora_nan3(a, b, c) : (r))

/* Single-precision results, as DolRecomp's force_single: in non-IEEE mode (FPSCR[NI], which games
   set) magnitudes below the smallest normal single flush to a signed zero. */
PANDORA_INLINE f64 pandora_round_single(CPUState* c, f64 value) {
    if (c->fpscr & 0x4u) { /* FPSCR[NI] */
        const u64 bits = pandora_u64_from_f64(value);
        if ((bits & 0x7FFFFFFFFFFFFFFFull) < 0x3810000000000000ull)
            return pandora_f64_from_u64(bits & 0x8000000000000000ull);
    }
    return pandora_single(value);
}
/* force_double: NI flushes double denormals. */
PANDORA_INLINE f64 pandora_round_double(CPUState* c, f64 value) {
    if ((c->fpscr & 0x4u) && value != 0.0 && fabs(value) < 2.2250738585072014e-308)
        return copysign(0.0, value);
    return value;
}

PANDORA_INLINE void pandora_write_single(CPUState* c, u8 d, f64 value) {
    const f64 r = pandora_round_single(c, value);
    c->fpr[d] = r;
    c->ps1[d] = r;
}

PANDORA_INLINE void pandora_fadds(CPUState* c, u8 d, u8 a, u8 b) {
    const f64 x = c->fpr[a], y = c->fpr[b];
    pandora_write_single(c, d, PANDORA_NAN2(x + y, x, y));
}
PANDORA_INLINE void pandora_fsubs(CPUState* c, u8 d, u8 a, u8 b) {
    const f64 x = c->fpr[a], y = c->fpr[b];
    pandora_write_single(c, d, PANDORA_NAN2(x - y, x, y));
}
PANDORA_INLINE void pandora_fmuls(CPUState* c, u8 d, u8 a, u8 b) {
    const f64 x = c->fpr[a], y = pandora_f25(c->fpr[b]);
    pandora_write_single(c, d, PANDORA_NAN2(x * y, x, y));
}
PANDORA_INLINE void pandora_fdivs(CPUState* c, u8 d, u8 a, u8 b) {
    const f64 x = c->fpr[a], y = c->fpr[b];
    pandora_write_single(c, d, PANDORA_NAN2(x / y, x, y));
}
PANDORA_INLINE void pandora_fadd(CPUState* c, u8 d, u8 a, u8 b) {
    const f64 x = c->fpr[a], y = c->fpr[b];
    c->fpr[d] = pandora_round_double(c, PANDORA_NAN2(x + y, x, y));
}
PANDORA_INLINE void pandora_fsub(CPUState* c, u8 d, u8 a, u8 b) {
    const f64 x = c->fpr[a], y = c->fpr[b];
    c->fpr[d] = pandora_round_double(c, PANDORA_NAN2(x - y, x, y));
}
PANDORA_INLINE void pandora_fmul(CPUState* c, u8 d, u8 a, u8 b) {
    const f64 x = c->fpr[a], y = c->fpr[b];
    c->fpr[d] = pandora_round_double(c, PANDORA_NAN2(x * y, x, y));
}
PANDORA_INLINE void pandora_fdiv(CPUState* c, u8 d, u8 a, u8 b) {
    const f64 x = c->fpr[a], y = c->fpr[b];
    c->fpr[d] = pandora_round_double(c, PANDORA_NAN2(x / y, x, y));
}
PANDORA_INLINE void pandora_frsp(CPUState* c, u8 d, u8 b) { pandora_write_single(c, d, c->fpr[b]); }

/* DolRecomp's single_fma_value: fused a * 25-bit c + addend, then (in round-to-nearest) corrected
   when the double result sits exactly on a single-precision tie (double rounding). When a is a
   single, its product with a 25-bit c is exact in a double, so a plain multiply-add is fused. */
PANDORA_NOINLINE f64 pandora_fma_tie(f64 a, f64 c, f64 addend, f64 result) {
    const u64 bits = pandora_u64_from_f64(result);
    const f64 a_prime = addend - result;
    const f64 b_prime = result + a_prime;
    const f64 error = fma(a, c, a_prime) + (addend - b_prime);
    if (error == 0.0)
        return result;
    return pandora_f64_from_u64((error > 0.0) == (result > 0.0) ? bits + 1 : bits - 1);
}
PANDORA_INLINE f64 pandora_single_fma(CPUState* cpu, f64 a, f64 c, f64 addend) {
    const f64 c25 = pandora_f25(c);
    /* exact only if the product neither under- nor overflows the double range */
    const f64 p = a * c25;
    const f64 r = (pandora_u64_from_f64(a) & 0x1FFFFFFFull) == 0 && fabs(p) > 1e-290 && fabs(p) < 1e290
                      ? p + addend
                      : fma(a, c25, addend);
    if ((cpu->fpscr & 3u) == 0 && (pandora_u64_from_f64(r) & 0x1FFFFFFFull) == 0x10000000ull)
        return pandora_fma_tie(a, c25, addend, r);
    return r;
}

/* fmadd family: out = +-(a*c +- b). Always "succeeds" (no enabled exceptions). */
PANDORA_INLINE bool pandora_fma(CPUState* cpu, f64 a, f64 c, f64 b, bool single, bool subtract, bool negative,
                                f64* out) {
    const f64 addend = subtract ? -b : b;
    f64 r = single ? pandora_single(pandora_single_fma(cpu, a, c, addend)) : fma(a, c, addend);
    r = PANDORA_NAN3(r, a, b, c);
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
    const f64 a0 = c->fpr[a], a1 = c->ps1[a], b0 = c->fpr[b], b1 = c->ps1[b];
    pandora_write_paired(c, d, PANDORA_NAN2(a0 + b0, a0, b0), PANDORA_NAN2(a1 + b1, a1, b1));
}
PANDORA_INLINE void pandora_ps_sub_op(CPUState* c, u8 d, u8 a, u8 b) {
    const f64 a0 = c->fpr[a], a1 = c->ps1[a], b0 = c->fpr[b], b1 = c->ps1[b];
    pandora_write_paired(c, d, PANDORA_NAN2(a0 - b0, a0, b0), PANDORA_NAN2(a1 - b1, a1, b1));
}
PANDORA_INLINE void pandora_ps_mul_op(CPUState* c, u8 d, u8 a, u8 cc) {
    const f64 a0 = c->fpr[a], a1 = c->ps1[a], c0 = c->fpr[cc], c1 = c->ps1[cc];
    pandora_write_paired(c, d, PANDORA_NAN2(a0 * pandora_f25(c0), a0, c0),
                         PANDORA_NAN2(a1 * pandora_f25(c1), a1, c1));
}
PANDORA_INLINE void pandora_ps_muls0(CPUState* c, u8 d, u8 a, u8 cc) {
    const f64 a0 = c->fpr[a], a1 = c->ps1[a], k = c->fpr[cc], s = pandora_f25(k);
    pandora_write_paired(c, d, PANDORA_NAN2(a0 * s, a0, k), PANDORA_NAN2(a1 * s, a1, k));
}
PANDORA_INLINE void pandora_ps_muls1(CPUState* c, u8 d, u8 a, u8 cc) {
    const f64 a0 = c->fpr[a], a1 = c->ps1[a], k = c->ps1[cc], s = pandora_f25(k);
    pandora_write_paired(c, d, PANDORA_NAN2(a0 * s, a0, k), PANDORA_NAN2(a1 * s, a1, k));
}
PANDORA_INLINE void pandora_ps_madd_op(CPUState* c, u8 d, u8 a, u8 cc, u8 b, bool subtract, bool negative) {
    const f64 a0 = c->fpr[a], a1 = c->ps1[a], c0 = c->fpr[cc], c1 = c->ps1[cc], b0 = c->fpr[b], b1 = c->ps1[b];
    f64 ps0 = PANDORA_NAN3(pandora_single_fma(c, a0, c0, subtract ? -b0 : b0), a0, b0, c0);
    f64 ps1 = PANDORA_NAN3(pandora_single_fma(c, a1, c1, subtract ? -b1 : b1), a1, b1, c1);
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
    const f64 a0 = c->fpr[a], a1 = c->ps1[a], k = c->fpr[cc], b0 = c->fpr[b], b1 = c->ps1[b];
    pandora_write_paired(c, d, PANDORA_NAN3(pandora_single_fma(c, a0, k, b0), a0, b0, k),
                         PANDORA_NAN3(pandora_single_fma(c, a1, k, b1), a1, b1, k));
}
PANDORA_INLINE void pandora_ps_madds1(CPUState* c, u8 d, u8 a, u8 cc, u8 b) {
    const f64 a0 = c->fpr[a], a1 = c->ps1[a], k = c->ps1[cc], b0 = c->fpr[b], b1 = c->ps1[b];
    pandora_write_paired(c, d, PANDORA_NAN3(pandora_single_fma(c, a0, k, b0), a0, b0, k),
                         PANDORA_NAN3(pandora_single_fma(c, a1, k, b1), a1, b1, k));
}
PANDORA_INLINE void pandora_ps_sum0(CPUState* c, u8 d, u8 a, u8 cc, u8 b) {
    const f64 a0 = c->fpr[a], b1 = c->ps1[b];
    pandora_write_paired(c, d, PANDORA_NAN2(a0 + b1, a0, b1), c->ps1[cc]);
}
PANDORA_INLINE void pandora_ps_sum1(CPUState* c, u8 d, u8 a, u8 cc, u8 b) {
    const f64 a0 = c->fpr[a], b1 = c->ps1[b];
    pandora_write_paired(c, d, c->fpr[cc], PANDORA_NAN2(a0 + b1, a0, b1));
}

/* Paired-single quantized loads/stores. GQR: load type/scale in bits 16-18/24-29, store in 0-2/8-13.
   Types: 0 float, 4 u8, 5 u16, 6 s8, 7 s16; 1-3 are invalid (left to DolRecomp, which raises the
   exception). Scales are signed 6-bit powers of two. */
PANDORA_INLINE f64 pandora_pow2(int e) { return pandora_f64_from_u64((u64)(1023 + e) << 52); }
PANDORA_INLINE int pandora_gqr_scale(u32 field) { return ((int)(field & 0x3F) ^ 0x20) - 0x20; }

PANDORA_INLINE f64 pandora_f32_from_bits(u32 bits);
PANDORA_INLINE f64 pandora_load_f32(CPUState* c, u32 ea) { return pandora_f32_from_bits(pandora_mem_read32(c, ea)); }

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
    if ((bits & 0x7F800000u) == 0 && (bits & 0x007FFFFFu) != 0)
        bits = 0; /* denormals store as +0 (as DolRecomp) */
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

/* lfs/stfs bit conversions (Gekko-exact, as DolRecomp's dolrecomp_f32_from_bits/_to_bits): the
   common cases inline, denormals out of line. tools/strip_pc.py renames the chunks' calls. */
PANDORA_NOINLINE f64 pandora_f32_from_bits_slow(u32 bits) {
    u64 x = bits;
    u64 exp = (x >> 23) & 0xFFu;
    u64 frac = x & 0x007FFFFFu;
    u64 result;
    if (exp == 0 && frac != 0) {
        exp = 1023 - 126;
        do {
            frac <<= 1;
            exp -= 1;
        } while ((frac & 0x00800000u) == 0);
        result = ((x & 0x80000000u) << 32) | (exp << 52) | ((frac & 0x007FFFFFu) << 29);
    } else {
        u64 y = exp >> 7;
        u64 z = (y << 61) | (y << 60) | (y << 59);
        result = ((x & 0xC0000000u) << 32) | z | ((x & 0x3FFFFFFFu) << 29);
    }
    return pandora_f64_from_u64(result);
}
PANDORA_INLINE f64 pandora_f32_from_bits(u32 bits) {
    const u32 exp = (bits >> 23) & 0xFFu;
    if (exp > 0 && exp < 255) {
        const u64 x = bits;
        const u64 y = !(exp >> 7);
        const u64 z = (y << 61) | (y << 60) | (y << 59);
        return pandora_f64_from_u64(((x & 0xC0000000u) << 32) | z | ((x & 0x3FFFFFFFu) << 29));
    }
    return pandora_f32_from_bits_slow(bits);
}
PANDORA_NOINLINE u32 pandora_f32_to_bits_slow(u64 bits) {
    const u32 exp = (u32)((bits >> 52) & 0x7FFu);
    if (exp >= 874) {
        u32 result = (u32)(0x80000000u | ((bits & 0x000FFFFFFFFFFFFFull) >> 21));
        result >>= 905 - exp;
        result |= (u32)((bits >> 32) & 0x80000000u);
        return result;
    }
    return (u32)(((bits >> 32) & 0xC0000000u) | ((bits >> 29) & 0x3FFFFFFFu));
}
PANDORA_INLINE u32 pandora_f32_to_bits(f64 value) {
    const u64 bits = pandora_u64_from_f64(value);
    const u32 exp = (u32)((bits >> 52) & 0x7FFu);
    if (exp > 896 || (bits & 0x7FFFFFFFFFFFFFFFull) == 0)
        return (u32)(((bits >> 32) & 0xC0000000u) | ((bits >> 29) & 0x3FFFFFFFu));
    return pandora_f32_to_bits_slow(bits);
}

#ifndef PANDORA_NO_FASTFP
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

#endif
