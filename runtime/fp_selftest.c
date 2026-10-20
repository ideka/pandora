/* --fp-selftest: compares runtime/fastmem/cpu/fastfp.h's inline floating-point and paired-single
   operations with DolRecomp's (cpu.c) on random and special inputs, bit for bit. */
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

/* Values the game's code meets: singles (most), doubles, small/denormal, zero, inf, NaN. */
static f64 random_value(void) {
    switch (rng() % 12) {
    case 0: return 0.0;
    case 1: return -0.0;
    case 2: return from_f32((u32)rng() & 0x807FFFFFu); /* single denormal */
    case 3: return from_u64(0x7FF0000000000000ull | (rng() & 0x8000000000000000ull)); /* inf */
    case 4: return from_u64(0x7FF8000000000000ull | (rng() & 0x8007FFFFFFFFFFFFull)); /* NaN */
    case 5: return from_u64((rng() & 0x800FFFFFFFFFFFFFull) | (u64)(0x370 + rng() % 0x20) << 52); /* near 2^-126 */
    case 6: return from_u64(rng()); /* any double */
    case 7: return (f64)((int)(rng() % 512) - 256) * 0.25;
    default: {
        u32 b = (u32)rng();
        if (((b >> 23) & 0xFF) == 0xFF)
            b &= ~0x40000000u;
        return from_f32(b); /* single */
    }
    }
}

static CPUState a, b;
static u8 ram_a[0x1000];
static u8* ram_w; /* the fast paths read RAM at its fixed host address: MEM1 in the guest window */
extern bool pandora_guest_memory_adopt(CPUState* cpu);
static int failures;
static u32 last_ea, last_g; static bool last_w;

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

static void compare(const char* op, int d) {
    const int bad = to_u64(a.fpr[d]) != to_u64(b.fpr[d]) || to_u64(a.ps1[d]) != to_u64(b.ps1[d]) || a.cr != b.cr ||
                    memcmp(ram_a, ram_w, sizeof ram_a) != 0;
    if (bad && failures++ < 40) {
        for (size_t k = 0; k < sizeof ram_a; k++)
            if (ram_a[k] != ram_w[k]) {
                fprintf(stderr, "fp-selftest:   memory differs first at +%zx: dolrecomp %02X fast %02X\n", k, ram_a[k],
                        ram_w[k]);
                break;
            }
        fprintf(stderr, "fp-selftest: %s f%d ea %08X w %d gqr %08X (NI %d): dolrecomp %016llX %016llX cr %08X, fast %016llX %016llX cr %08X%s\n",
                op, d, last_ea, last_w, a.gqr[last_g], (int)(a.fpscr & 4) != 0, (unsigned long long)to_u64(a.fpr[d]), (unsigned long long)to_u64(a.ps1[d]),
                a.cr, (unsigned long long)to_u64(b.fpr[d]), (unsigned long long)to_u64(b.ps1[d]), b.cr,
                memcmp(ram_a, ram_w, sizeof ram_a) ? " (memory differs)" : "");
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
        const u32 ea = 0x80000000u + (u32)(rng() % 0x7F0) * 2;  /* + 8 bytes stays inside ram_a */
        f64 ra = 0, rb = 0;
        switch (op) {
        case 0: ppc_fadds(&a, d, x, y); pandora_fadds(&b, d, x, y); break;
        case 1: ppc_fsubs(&a, d, x, y); pandora_fsubs(&b, d, x, y); break;
        case 2: ppc_fmuls(&a, d, x, y); pandora_fmuls(&b, d, x, y); break;
        case 3: ppc_fdivs(&a, d, x, y); pandora_fdivs(&b, d, x, y); break;
        case 4: ppc_fadd(&a, d, x, y); pandora_fadd(&b, d, x, y); break;
        case 5: ppc_fsub(&a, d, x, y); pandora_fsub(&b, d, x, y); break;
        case 6: ppc_fmul(&a, d, x, y); pandora_fmul(&b, d, x, y); break;
        case 7: ppc_fdiv(&a, d, x, y); pandora_fdiv(&b, d, x, y); break;
        case 8: ppc_frsp(&a, d, x); pandora_frsp(&b, d, x); break;
        case 9:
            if (ppc_fma(&a, a.fpr[x], a.fpr[y], a.fpr[z], f1, f2, f3, &ra)) {
                a.fpr[d] = ra;
                if (f1)
                    a.ps1[d] = ra;
            }
            if (pandora_fma(&b, b.fpr[x], b.fpr[y], b.fpr[z], f1, f2, f3, &rb)) {
                b.fpr[d] = rb;
                if (f1)
                    b.ps1[d] = rb;
            }
            break;
        case 10: ppc_fcmp(&a, d & 7, a.fpr[x], a.fpr[y], f1); pandora_fcmp(&b, d & 7, b.fpr[x], b.fpr[y], f1); break;
        case 11: ppc_ps_add_op(&a, d, x, y); pandora_ps_add_op(&b, d, x, y); break;
        case 12: ppc_ps_sub_op(&a, d, x, y); pandora_ps_sub_op(&b, d, x, y); break;
        case 13: ppc_ps_mul_op(&a, d, x, y); pandora_ps_mul_op(&b, d, x, y); break;
        case 14: ppc_ps_muls0(&a, d, x, y); pandora_ps_muls0(&b, d, x, y); break;
        case 15: ppc_ps_muls1(&a, d, x, y); pandora_ps_muls1(&b, d, x, y); break;
        case 16: ppc_ps_madd_op(&a, d, x, y, z, f1, f2); pandora_ps_madd_op(&b, d, x, y, z, f1, f2); break;
        case 17: ppc_ps_madds0(&a, d, x, y, z); pandora_ps_madds0(&b, d, x, y, z); break;
        case 18: ppc_ps_madds1(&a, d, x, y, z); pandora_ps_madds1(&b, d, x, y, z); break;
        case 19: ppc_ps_sum0(&a, d, x, y, z); pandora_ps_sum0(&b, d, x, y, z); break;
        case 20: ppc_ps_sum1(&a, d, x, y, z); pandora_ps_sum1(&b, d, x, y, z); break;
        case 21: ppc_psq_load_inline(&a, d, ea, f1, x & 7, false, 0); pandora_psq_load(&b, d, ea, f1, x & 7, false, 0); break;
        case 22: ppc_psq_store_inline(&a, d, ea, f1, x & 7, false, 0); pandora_psq_store(&b, d, ea, f1, x & 7, false, 0); break;
        }
        last_ea = ea; last_w = f1; last_g = x & 7; compare(names[op], op == 10 ? 0 : d);
    }
    /* lfs: every 32-bit pattern; stfs: random doubles and specials. */
    for (u64 bits = 0; bits < 0x100000000ull; bits++) {
        const u32 b = (u32)bits;
        if (to_u64(pandora_f32_from_bits(b)) != to_u64(dolrecomp_f32_from_bits(b)) && failures++ < 40)
            fprintf(stderr, "fp-selftest: f32_from_bits(%08X): dolrecomp %016llX, fast %016llX\n", b,
                    (unsigned long long)to_u64(dolrecomp_f32_from_bits(b)),
                    (unsigned long long)to_u64(pandora_f32_from_bits(b)));
    }
    for (int i = 0; i < 20000000; i++) {
        const f64 v = random_value();
        if (pandora_f32_to_bits(v) != dolrecomp_f32_to_bits(v) && failures++ < 40)
            fprintf(stderr, "fp-selftest: f32_to_bits(%016llX): dolrecomp %08X, fast %08X\n",
                    (unsigned long long)to_u64(v), dolrecomp_f32_to_bits(v), pandora_f32_to_bits(v));
    }
    fprintf(stderr, "fp-selftest: %d mismatches\n", failures);
    return failures ? 1 : 0;
}
