/* Fast inline floating-point and paired-single operations for the recompiled chunks (included by
   the shadow cpu/cpu.h, after DolRecomp's declarations; DolRecomp's own cpu.c is untouched).

   Accuracy: that of Dolphin's JIT with its default settings, which every game is fine with:
   - results are the host's SSE results: NaNs are whatever SSE produces (a NaN operand quieted,
     or SSE's default NaN, which has the sign bit set where the Gekko's is clear), with no
     per-operation NaN checks;
   - FPSCR[NI] (non-IEEE mode, set by games at boot) is mirrored to the host's MXCSR flush-to-zero
     and denormals-are-zero bits (runtime/fpu_switch.cpp, via ppc_fpscr_updated), so denormal
     results flush in hardware instead of a flag test per operation;
   - fused single multiply-adds are one FMA rounded to single, without correcting the rare
     double-rounding tie (DolRecomp's cpu.c does; the deviation is one single-precision ulp,
     about once in 2^29 operations).
   Everything else is exact: single rounding, the 25-bit multiplier of single multiplies (done in
   the SIMD domain), compares, quantized paired-single loads and stores. runtime/fp_selftest.c
   checks all of it against DolRecomp's cpu.c, allowing exactly those deviations. */
#ifndef PANDORA_FASTFP_H
#define PANDORA_FASTFP_H

#include <immintrin.h>
#include <math.h>
#include <string.h>

#define PANDORA_ISNAN(x) __builtin_isnan(x)

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
   Dolphin's) force_25_bit: (bits & keep) + (bits & round bit), in the SIMD domain (no GPR round
   trip). Denormal multipliers are zero under DAZ, so their special rounding does not matter. */
PANDORA_INLINE f64 pandora_f25(f64 value) {
    const __m128i b = _mm_castpd_si128(_mm_set_sd(value));
    const __m128i keep = _mm_set_epi64x(0, (long long)0xFFFFFFFFF8000000ull);
    const __m128i round = _mm_set_epi64x(0, (long long)0x0000000008000000ull);
    return _mm_cvtsd_f64(_mm_castsi128_pd(_mm_add_epi64(_mm_and_si128(b, keep), _mm_and_si128(b, round))));
}

PANDORA_INLINE f64 pandora_single(f64 value) { return (f64)(f32)value; }

PANDORA_INLINE void pandora_write_single(CPUState* c, u8 d, f64 value) {
    const f64 r = pandora_single(value);
    c->fpr[d] = r;
    c->ps1[d] = r;
}

PANDORA_INLINE void pandora_fadds(CPUState* c, u8 d, u8 a, u8 b) { pandora_write_single(c, d, c->fpr[a] + c->fpr[b]); }
PANDORA_INLINE void pandora_fsubs(CPUState* c, u8 d, u8 a, u8 b) { pandora_write_single(c, d, c->fpr[a] - c->fpr[b]); }
PANDORA_INLINE void pandora_fmuls(CPUState* c, u8 d, u8 a, u8 b) {
    pandora_write_single(c, d, c->fpr[a] * pandora_f25(c->fpr[b]));
}
PANDORA_INLINE void pandora_fdivs(CPUState* c, u8 d, u8 a, u8 b) { pandora_write_single(c, d, c->fpr[a] / c->fpr[b]); }
PANDORA_INLINE void pandora_fadd(CPUState* c, u8 d, u8 a, u8 b) { c->fpr[d] = c->fpr[a] + c->fpr[b]; }
PANDORA_INLINE void pandora_fsub(CPUState* c, u8 d, u8 a, u8 b) { c->fpr[d] = c->fpr[a] - c->fpr[b]; }
PANDORA_INLINE void pandora_fmul(CPUState* c, u8 d, u8 a, u8 b) { c->fpr[d] = c->fpr[a] * c->fpr[b]; }
PANDORA_INLINE void pandora_fdiv(CPUState* c, u8 d, u8 a, u8 b) { c->fpr[d] = c->fpr[a] / c->fpr[b]; }
PANDORA_INLINE void pandora_frsp(CPUState* c, u8 d, u8 b) { pandora_write_single(c, d, c->fpr[b]); }

/* Fused single multiply-add: a * (25-bit c) + addend, one FMA (the guest is built with FMA). */
PANDORA_INLINE f64 pandora_single_fma(f64 a, f64 c, f64 addend) { return __builtin_fma(a, pandora_f25(c), addend); }

/* fmadd family: out = +-(a*c +- b). Always "succeeds" (no enabled exceptions). */
PANDORA_INLINE bool pandora_fma(CPUState* cpu, f64 a, f64 c, f64 b, bool single, bool subtract, bool negative,
                                f64* out) {
    (void)cpu;
    const f64 addend = subtract ? -b : b;
    f64 r = single ? pandora_single(pandora_single_fma(a, c, addend)) : __builtin_fma(a, c, addend);
    if (negative && !PANDORA_ISNAN(r))
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
    f64 ps0 = pandora_single_fma(c->fpr[a], c->fpr[cc], subtract ? -c->fpr[b] : c->fpr[b]);
    f64 ps1 = pandora_single_fma(c->ps1[a], c->ps1[cc], subtract ? -c->ps1[b] : c->ps1[b]);
    if (negative) {
        ps0 = pandora_single(ps0);
        ps1 = pandora_single(ps1);
        if (!PANDORA_ISNAN(ps0))
            ps0 = -ps0;
        if (!PANDORA_ISNAN(ps1))
            ps1 = -ps1;
    }
    pandora_write_paired(c, d, ps0, ps1);
}
PANDORA_INLINE void pandora_ps_madds0(CPUState* c, u8 d, u8 a, u8 cc, u8 b) {
    const f64 s = pandora_f25(c->fpr[cc]);
    pandora_write_paired(c, d, __builtin_fma(c->fpr[a], s, c->fpr[b]), __builtin_fma(c->ps1[a], s, c->ps1[b]));
}
PANDORA_INLINE void pandora_ps_madds1(CPUState* c, u8 d, u8 a, u8 cc, u8 b) {
    const f64 s = pandora_f25(c->ps1[cc]);
    pandora_write_paired(c, d, __builtin_fma(c->fpr[a], s, c->fpr[b]), __builtin_fma(c->ps1[a], s, c->ps1[b]));
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
    /* Floats, no scaling (the GQR's load fields are 0; games keep GQR0 so), in RAM: the common case. */
    if (__builtin_expect((g & 0x3F070000u) == 0, 1)) {
        if (__builtin_expect(ea & 3u, 0))
            return ppc_psq_load(c, d, ea, w, gqr, indexed, cia); /* alignment exception */
        const u8* p = pandora_fast_host(ea, 8);
        if (__builtin_expect(p != NULL, 1)) {
            u32 b0, b1;
            memcpy(&b0, p, 4);
            memcpy(&b1, p + 4, 4);
            c->fpr[d] = pandora_f32_from_bits(__builtin_bswap32(b0));
            c->ps1[d] = w ? 1.0 : pandora_f32_from_bits(__builtin_bswap32(b1));
            return true;
        }
        c->fpr[d] = pandora_load_f32(c, ea);
        c->ps1[d] = w ? 1.0 : pandora_load_f32(c, ea + 4);
        return true;
    }
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
    if (PANDORA_ISNAN(value))
        return hi;
    const f64 scaled = __builtin_trunc(value * factor);
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

PANDORA_INLINE u32 pandora_f32_store_bits(f64 value) {
    const f32 f = (f32)value;
    u32 bits;
    memcpy(&bits, &f, sizeof bits);
    if ((bits & 0x7F800000u) == 0 && (bits & 0x007FFFFFu) != 0)
        bits = 0; /* denormals store as +0 (as DolRecomp) */
    return bits;
}

PANDORA_INLINE bool pandora_psq_store(CPUState* c, u8 s, u32 ea, bool w, u8 gqr, bool indexed, u32 cia) {
    const u32 g = c->gqr[gqr & 7u];
    if (__builtin_expect((g & 0x3F07u) == 0, 1)) { /* floats, no scaling, in RAM */
        if (__builtin_expect(ea & 3u, 0))
            return ppc_psq_store(c, s, ea, w, gqr, indexed, cia);
        u8* p = pandora_fast_host(ea, 8);
        if (__builtin_expect(p != NULL && !g_mem_write_journal, 1)) {
            u32 b0 = __builtin_bswap32(pandora_f32_store_bits(c->fpr[s]));
            memcpy(p, &b0, 4);
            if (!w) {
                u32 b1 = __builtin_bswap32(pandora_f32_store_bits(c->ps1[s]));
                memcpy(p + 4, &b1, 4);
            }
            return true;
        }
        pandora_store_f32(c, ea, c->fpr[s]);
        if (!w)
            pandora_store_f32(c, ea + 4, c->ps1[s]);
        return true;
    }
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
    f32 f;
    memcpy(&f, &bits, sizeof f);
    return (f64)f;
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
