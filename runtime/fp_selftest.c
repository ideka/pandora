/* --fp-selftest: compares runtime/fastmem/cpu/fastfp.h's inline floating-point and paired-single
   operations with DolRecomp's (cpu.c) on random and special inputs, bit for bit except for the
   deviations fastfp.h documents (NaN sign/payload, denormals under non-IEEE mode, the fused
   multiply-add double-rounding tie), which are allowed and counted separately. */
#include <immintrin.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PANDORA_NO_FASTFP /* ppc_* stay DolRecomp's here: that is the side compared against */
#include "fastmem/cpu/cpu.h"
#include "generated.h" /* dolrecomp_f32_from_bits / _to_bits */

static u64 rng_state = 0x9E3779B97F4A7C15ull;
static u64 rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static f64 from_u64(u64 b) {
    f64 v;
    memcpy(&v, &b, 8);
    return v;
}
static u64 to_u64(f64 v) {
    u64 b;
    memcpy(&b, &v, 8);
    return b;
}
static f64 from_f32(u32 b) {
    f32 f;
    memcpy(&f, &b, 4);
    return (f64)f;
}
static int is_nan_bits(u64 b) { return (b & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull; }
/* |x| below the smallest normal single (what non-IEEE mode flushes), or zero */
static int is_tiny(u64 b) { return (b & 0x7FFFFFFFFFFFFFFFull) < 0x3810000000000000ull; }

/* Values the game's code meets: singles (most), doubles, small, zero, inf, NaN. No denormal
   doubles: the fast side reads those as zero (DAZ), DolRecomp does not. */
static f64 random_value(void) {
    switch (rng() % 12) {
    case 0: return 0.0;
    case 1: return -0.0;
    case 2: return from_f32((u32)rng() & 0x80FFFFFFu | 0x00800000u); /* single just above denormal range */
    case 3: return from_u64(0x7FF0000000000000ull | (rng() & 0x8000000000000000ull)); /* inf */
    case 4: return from_u64(0x7FF8000000000000ull | (rng() & 0x8007FFFFFFFFFFFFull)); /* NaN */
    case 5: return from_u64((rng() & 0x800FFFFFFFFFFFFFull) | (u64)(0x370 + rng() % 0x20) << 52); /* near 2^-126 */
    case 6: {
        u64 b = rng();
        if ((b & 0x7FF0000000000000ull) == 0)
            b |= 0x0010000000000000ull;
        return from_u64(b); /* any (normal) double */
    }
    case 7: return (f64)((int)(rng() % 512) - 256) * 0.25;
    default: {
        u32 b = (u32)rng();
        if (((b >> 23) & 0xFF) == 0xFF)
            b &= ~0x40000000u;
        return from_f32(b); /* single (a denormal single is a normal double) */
    }
    }
}

static CPUState a, b;
static u8 ram_a[0x1000];
static u8* ram_w; /* the fast paths read RAM at its fixed host address: MEM1 in the guest window */
extern bool pandora_guest_memory_adopt(CPUState* cpu);
extern void pandora_fpscr_updated(CPUState* c);
static int failures, nan_deviations, tiny_deviations, tie_deviations;
static u32 last_ea, last_g;
static bool last_w;

static void host_ieee_mode(void) { _mm_setcsr(_mm_getcsr() & ~0x8040u); }

static void setup(void) {
    memset(&a, 0, sizeof a);
    a.ram = ram_a;
    a.ram_size = sizeof ram_a;
    a.hid2 = PPC_HID2_PSE | PPC_HID2_LSQE;
    a.msr = 0x2000;
    a.fpscr = (rng() & 1) ? 0x4u : 0; /* NI on or off */
    for (int i = 0; i < 32; i++) {
        a.fpr[i] = random_value();
        a.ps1[i] = random_value();
    }
    for (int i = 0; i < 8; i++) {
        const u32 type = (u32[]){0, 4, 5, 6, 7}[rng() % 5];
        const u32 scale = (u32)(rng() % 64);
        a.gqr[i] = type | scale << 8 | type << 16 | scale << 24;
    }
    for (size_t i = 0; i < sizeof ram_a; i++)
        ram_a[i] = (u8)rng();
    b = a;
    b.ram = ram_w;
    memcpy(ram_w, ram_a, sizeof ram_a);
}

/* Same result, or a documented deviation (counted). */
static int same(u64 x, u64 y, int ni, int fma_op) {
    if (x == y)
        return 1;
    if (is_nan_bits(x) && is_nan_bits(y)) {
        nan_deviations++;
        return 1;
    }
    if (ni && is_tiny(x) && is_tiny(y)) {
        tiny_deviations++;
        return 1;
    }
    if (fma_op) {
        const f32 fx = (f32)from_u64(x), fy = (f32)from_u64(y);
        u32 bx, by;
        memcpy(&bx, &fx, 4);
        memcpy(&by, &fy, 4);
        if ((bx > by ? bx - by : by - bx) == 1 && (f64)fx == from_u64(x) && (f64)fy == from_u64(y)) {
            tie_deviations++;
            return 1;
        }
    }
    return 0;
}

/* Stored floats: a flushed denormal keeps its sign here (as in Dolphin), DolRecomp stores +0. */
static int same_memory(void) {
    for (size_t k = 0; k < sizeof ram_a; k += 4) {
        u32 x, y;
        memcpy(&x, ram_a + k, 4);
        memcpy(&y, ram_w + k, 4);
        if (x == y)
            continue;
        if ((x & ~0x80u) == 0 && (y & ~0x80u) == 0) { /* +-0 (big-endian: the sign is bit 7 of byte 0) */
            tiny_deviations++;
            continue;
        }
        return 0;
    }
    return 1;
}

static void compare(const char* op, int d, int fma_op) {
    const int ni = (a.fpscr & 4) != 0;
    const int bad = !same(to_u64(a.fpr[d]), to_u64(b.fpr[d]), ni, fma_op) ||
                    !same(to_u64(a.ps1[d]), to_u64(b.ps1[d]), ni, fma_op) || a.cr != b.cr || !same_memory();
    if (bad && failures++ < 40) {
        for (size_t k = 0; k < sizeof ram_a; k++)
            if (ram_a[k] != ram_w[k]) {
                fprintf(stderr, "fp-selftest:   memory differs first at +%zx: dolrecomp %02X fast %02X\n", k, ram_a[k],
                        ram_w[k]);
                break;
            }
        fprintf(stderr,
                "fp-selftest: %s f%d ea %08X w %d gqr %08X (NI %d): dolrecomp %016llX %016llX cr %08X, fast %016llX "
                "%016llX cr %08X%s\n",
                op, d, last_ea, last_w, a.gqr[last_g], ni, (unsigned long long)to_u64(a.fpr[d]),
                (unsigned long long)to_u64(a.ps1[d]), a.cr, (unsigned long long)to_u64(b.fpr[d]),
                (unsigned long long)to_u64(b.ps1[d]), b.cr, memcmp(ram_a, ram_w, sizeof ram_a) ? " (memory differs)" : "");
    }
}

int pandora_fp_selftest(void) {
    {
        CPUState w;
        memset(&w, 0, sizeof w);
        if (!pandora_guest_memory_adopt(&w))
            return 2;
        ram_w = w.ram;
    }
    static const char* names[] = {"fadds", "fsubs", "fmuls", "fdivs", "fadd", "fsub", "fmul", "fdiv", "frsp",
                                  "fma", "fcmp", "ps_add", "ps_sub", "ps_mul", "ps_muls0", "ps_muls1", "ps_madd",
                                  "ps_madds0", "ps_madds1", "ps_sum0", "ps_sum1", "psq_l", "psq_st"};
    const int ops = (int)(sizeof names / sizeof names[0]);
    for (int iter = 0; iter < 2000000; iter++) {
        setup();
        const int op = (int)(rng() % (u64)ops);
        const u8 d = (u8)(rng() % 32), x = (u8)(rng() % 32), y = (u8)(rng() % 32), z = (u8)(rng() % 32);
        const bool f1 = rng() & 1, f2 = rng() & 1, f3 = rng() & 1;
        const u32 ea = 0x80000000u + (u32)(rng() % 0x7F0) * 2; /* + 8 bytes stays inside ram_a */
        f64 ra = 0, rb = 0;
        /* DolRecomp's side runs in plain IEEE mode (it models NI itself); the fast side with the
           host mode the runtime would set for this FPSCR. */
        host_ieee_mode();
        switch (op) {
        case 0: ppc_fadds(&a, d, x, y); break;
        case 1: ppc_fsubs(&a, d, x, y); break;
        case 2: ppc_fmuls(&a, d, x, y); break;
        case 3: ppc_fdivs(&a, d, x, y); break;
        case 4: ppc_fadd(&a, d, x, y); break;
        case 5: ppc_fsub(&a, d, x, y); break;
        case 6: ppc_fmul(&a, d, x, y); break;
        case 7: ppc_fdiv(&a, d, x, y); break;
        case 8: ppc_frsp(&a, d, x); break;
        case 9:
            if (ppc_fma(&a, a.fpr[x], a.fpr[y], a.fpr[z], f1, f2, f3, &ra)) {
                a.fpr[d] = ra;
                if (f1)
                    a.ps1[d] = ra;
            }
            break;
        case 10: ppc_fcmp(&a, d & 7, a.fpr[x], a.fpr[y], f1); break;
        case 11: ppc_ps_add_op(&a, d, x, y); break;
        case 12: ppc_ps_sub_op(&a, d, x, y); break;
        case 13: ppc_ps_mul_op(&a, d, x, y); break;
        case 14: ppc_ps_muls0(&a, d, x, y); break;
        case 15: ppc_ps_muls1(&a, d, x, y); break;
        case 16: ppc_ps_madd_op(&a, d, x, y, z, f1, f2); break;
        case 17: ppc_ps_madds0(&a, d, x, y, z); break;
        case 18: ppc_ps_madds1(&a, d, x, y, z); break;
        case 19: ppc_ps_sum0(&a, d, x, y, z); break;
        case 20: ppc_ps_sum1(&a, d, x, y, z); break;
        case 21: ppc_psq_load_inline(&a, d, ea, f1, x & 7, false, 0); break;
        case 22: ppc_psq_store_inline(&a, d, ea, f1, x & 7, false, 0); break;
        }
        pandora_fpscr_updated(&b);
        switch (op) {
        case 0: pandora_fadds(&b, d, x, y); break;
        case 1: pandora_fsubs(&b, d, x, y); break;
        case 2: pandora_fmuls(&b, d, x, y); break;
        case 3: pandora_fdivs(&b, d, x, y); break;
        case 4: pandora_fadd(&b, d, x, y); break;
        case 5: pandora_fsub(&b, d, x, y); break;
        case 6: pandora_fmul(&b, d, x, y); break;
        case 7: pandora_fdiv(&b, d, x, y); break;
        case 8: pandora_frsp(&b, d, x); break;
        case 9:
            if (pandora_fma(&b, b.fpr[x], b.fpr[y], b.fpr[z], f1, f2, f3, &rb)) {
                b.fpr[d] = rb;
                if (f1)
                    b.ps1[d] = rb;
            }
            break;
        case 10: pandora_fcmp(&b, d & 7, b.fpr[x], b.fpr[y], f1); break;
        case 11: pandora_ps_add_op(&b, d, x, y); break;
        case 12: pandora_ps_sub_op(&b, d, x, y); break;
        case 13: pandora_ps_mul_op(&b, d, x, y); break;
        case 14: pandora_ps_muls0(&b, d, x, y); break;
        case 15: pandora_ps_muls1(&b, d, x, y); break;
        case 16: pandora_ps_madd_op(&b, d, x, y, z, f1, f2); break;
        case 17: pandora_ps_madds0(&b, d, x, y, z); break;
        case 18: pandora_ps_madds1(&b, d, x, y, z); break;
        case 19: pandora_ps_sum0(&b, d, x, y, z); break;
        case 20: pandora_ps_sum1(&b, d, x, y, z); break;
        case 21: pandora_psq_load(&b, d, ea, f1, x & 7, false, 0); break;
        case 22: pandora_psq_store(&b, d, ea, f1, x & 7, false, 0); break;
        }
        host_ieee_mode();
        last_ea = ea;
        last_w = f1;
        last_g = x & 7;
        const int fma_op = (op == 9 && f1) || op == 16 || op == 17 || op == 18;
        compare(names[op], op == 10 ? 0 : d, fma_op);
    }
    /* lfs: every 32-bit pattern (in the game's non-IEEE mode: denormal singles read as zero);
       stfs: random doubles and specials (exact). */
    {
        CPUState ni;
        memset(&ni, 0, sizeof ni);
        ni.fpscr = 4;
        pandora_fpscr_updated(&ni);
    }
    for (u64 bits = 0; bits < 0x100000000ull; bits++) {
        const u32 w = (u32)bits;
        const u64 fast = to_u64(pandora_f32_from_bits(w)), ref = to_u64(dolrecomp_f32_from_bits(w));
        if (!same(fast, ref, 1, 0) && failures++ < 40)
            fprintf(stderr, "fp-selftest: f32_from_bits(%08X): dolrecomp %016llX, fast %016llX\n", w,
                    (unsigned long long)ref, (unsigned long long)fast);
    }
    host_ieee_mode();
    for (int i = 0; i < 20000000; i++) {
        const f64 v = random_value();
        if (pandora_f32_to_bits(v) != dolrecomp_f32_to_bits(v) && failures++ < 40)
            fprintf(stderr, "fp-selftest: f32_to_bits(%016llX): dolrecomp %08X, fast %08X\n",
                    (unsigned long long)to_u64(v), dolrecomp_f32_to_bits(v), pandora_f32_to_bits(v));
    }
    fprintf(stderr, "fp-selftest: %d mismatches (allowed deviations: %d NaN, %d denormal, %d fma tie)\n", failures,
            nan_deviations, tiny_deviations, tie_deviations);
    return failures ? 1 : 0;
}
