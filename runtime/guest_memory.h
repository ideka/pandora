// Guest RAM at a fixed host address (see fastmem/cpu/guest_base.h).
#pragma once

#include "guest.h"

namespace guest_memory {

// Reserves the 4 GB window, commits MEM1, MEM2 and the locked cache inside it, and points the CPU
// state's ram/mem2 at them (replacing cpu_init's own allocations). Installs a handler that reports
// guest accesses to unmapped addresses. False if the window cannot be reserved.
bool adopt(CPUState* cpu);
// Before cpu_free: cpu_init's pointers were freed here, so cpu_free must not free these.
void release(CPUState* cpu);
u8* locked_cache();

}  // namespace guest_memory
