// The OS's lazy FPU context switch, done in place.
//
// The OS runs threads with MSR[FP] clear after a context switch; the first floating-point
// instruction raises the FP-unavailable exception, whose handler (OSSwitchFPUContext) saves the
// previous FPU owner's registers into its OSContext, loads the current thread's and returns to
// the faulting instruction. Returning into the middle of a block forces the recompiled code to
// accept an entry at every FP instruction, so the chunks call this instead: it does what the
// handler does and execution simply continues. Only used while the game's handler is the SDK's.
#include <cstring>

#include "memory.h"

extern "C" void pandora_fpscr_updated(CPUState* c);

namespace {

constexpr u32 OS_CURRENT_CONTEXT = 0x800000D4;
constexpr u32 OS_FPU_CONTEXT = 0x800000D8;
constexpr u32 OS_EXCEPTION_TABLE = 0x80003000;  // __OSExceptionTable: one handler per exception
constexpr u32 OS_EXCEPTION_FPU_UNAVAILABLE = 7;
constexpr u32 OS_SWITCH_FPU_CONTEXT = 0x805456B0;  // OSSwitchFPUContext

// OSContext layout (RVL SDK).
constexpr u32 CTX_FPR = 144;     // f64[32]
constexpr u32 CTX_FPSCR = 400;   // FPSCR in the low word of a double (mffs/mtfsf)
constexpr u32 CTX_STATE = 418;   // u16: bit 0 FPRs saved, bit 1 cleared by the handler
constexpr u32 CTX_PSF = 456;     // paired-single halves: f32 pairs (psq with GQR0), saved when HID2[PSE]
constexpr u16 STATE_FPSAVED = 0x1, STATE_EXC = 0x2;
constexpr u32 MSR_FP = 0x2000;
constexpr u32 HID2_PSE = 0x20000000;

u64 f64_bits(f64 v) {
    u64 b;
    std::memcpy(&b, &v, 8);
    return b;
}
f64 f64_from(u64 b) {
    f64 v;
    std::memcpy(&v, &b, 8);
    return v;
}
u32 f32_bits(f64 v) {
    const f32 f = (f32)v;
    u32 b;
    std::memcpy(&b, &f, 4);
    return b;
}
f64 f32_from(u32 b) {
    f32 f;
    std::memcpy(&f, &b, 4);
    return (f64)f;
}

// __OSSaveFPUContext
void save_fpu(CPUState* c, u32 ctx) {
    mem::write16(c, ctx + CTX_STATE, mem::read16(c, ctx + CTX_STATE) | STATE_FPSAVED);
    for (u32 i = 0; i < 32; i++)
        mem::write64(c, ctx + CTX_FPR + 8 * i, f64_bits(c->fpr[i]));
    mem::write64(c, ctx + CTX_FPSCR, c->fpscr);
    if (c->hid2 & HID2_PSE)
        for (u32 i = 0; i < 32; i++) {
            mem::write32(c, ctx + CTX_PSF + 8 * i, f32_bits(c->fpr[i]));
            mem::write32(c, ctx + CTX_PSF + 8 * i + 4, f32_bits(c->ps1[i]));
        }
}

// __OSLoadFPUContext
void load_fpu(CPUState* c, u32 ctx) {
    if (!(mem::read16(c, ctx + CTX_STATE) & STATE_FPSAVED))
        return;
    c->fpscr = (u32)mem::read64(c, ctx + CTX_FPSCR);
    pandora_fpscr_updated(c);
    if (c->hid2 & HID2_PSE)
        for (u32 i = 0; i < 32; i++)
            c->ps1[i] = f32_from(mem::read32(c, ctx + CTX_PSF + 8 * i + 4));
    for (u32 i = 0; i < 32; i++)
        c->fpr[i] = f64_from(mem::read64(c, ctx + CTX_FPR + 8 * i));
}

}  // namespace

// Called by the recompiled code when an FP instruction finds MSR[FP] clear. Returns true when the
// switch was done here (execution continues); otherwise raises the exception as usual.
extern "C" bool pandora_fp_unavailable(CPUState* c, u32 cia) {
    if (mem::read32(c, OS_EXCEPTION_TABLE + 4 * OS_EXCEPTION_FPU_UNAVAILABLE) != OS_SWITCH_FPU_CONTEXT)
        return ppc_fp_available(c, cia);
    const u32 ctx = mem::read32(c, OS_CURRENT_CONTEXT);
    if (!ctx)
        return ppc_fp_available(c, cia);
    c->msr |= MSR_FP;
    const u32 owner = mem::read32(c, OS_FPU_CONTEXT);
    mem::write32(c, OS_FPU_CONTEXT, ctx);
    if (owner != ctx) {
        if (owner)
            save_fpu(c, owner);
        load_fpu(c, ctx);
    }
    mem::write16(c, ctx + CTX_STATE, mem::read16(c, ctx + CTX_STATE) & ~STATE_EXC);
    return true;
}

// FPSCR[NI] (non-IEEE mode): denormal results flush to zero (and, as in Dolphin, denormal inputs
// read as zero). Mirrored to this thread's MXCSR so the fast floating-point code needs no flag
// test per operation. Called for every FPSCR write in the recompiled code and on context loads.
#include <immintrin.h>
extern "C" void pandora_fpscr_updated(CPUState* c) {
    ppc_fpscr_updated(c);
    const unsigned csr = _mm_getcsr() & ~0x8040u;  // FTZ (bit 15) | DAZ (bit 6)
    _mm_setcsr(csr | ((c->fpscr & 0x4u) ? 0x8040u : 0u));
}
