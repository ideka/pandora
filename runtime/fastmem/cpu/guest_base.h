/* Guest memory lives at a fixed host address: host = PANDORA_GUEST_BASE + guest address, for MEM1
   (0x80000000, 24 MB), MEM2 (0x90000000, 64 MB) and the locked cache (0xE0000000, 16 KB). The
   recompiled code then needs no base-pointer or size loads per access. Everything else in the
   4 GB window is reserved but inaccessible, so a guest access to nowhere faults (reported by
   runtime/guest_memory.cpp) instead of reading zeros. */
#ifndef PANDORA_GUEST_BASE_H
#define PANDORA_GUEST_BASE_H

#define PANDORA_GUEST_BASE 0x200000000000ull
#define PANDORA_MEM1_BASE 0x80000000u
#define PANDORA_MEM1_SIZE 0x01800000u
#define PANDORA_MEM2_BASE 0x90000000u
#define PANDORA_MEM2_SIZE 0x04000000u
#define PANDORA_LC_BASE 0xE0000000u
#define PANDORA_LC_SIZE 0x4000u

#endif
