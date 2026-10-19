// Thin C wrapper around DolRecomp's generated dispatcher (generated.h is C-only).
#include "generated.h"

int guest_dispatch(CPUState* ctx) {
    return dolrecomp_call(ctx, ctx->pc);
}
