// Guest CPU interface shared by the runtime and the recompiled code.
#pragma once

#include <cstdint>

extern "C" {
#include "cpu/cpu.h"

// Runs one block of recompiled code starting at ctx->pc. Returns 0 if no code (or hook) exists
// at that address.
int guest_dispatch(CPUState* ctx);
}
