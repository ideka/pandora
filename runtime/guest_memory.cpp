#include "guest_memory.h"

#include <cstdlib>
#include <cstring>

#include "fastmem/cpu/guest_base.h"
#include "log.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace guest_memory {

namespace {

constexpr u64 WINDOW = 0x100000000ull;  // the whole 32-bit guest address space

u8* at(u32 guest) { return reinterpret_cast<u8*>(static_cast<uintptr_t>(PANDORA_GUEST_BASE + guest)); }

#ifdef _WIN32
bool commit(u32 guest, u32 size) {
    return VirtualAlloc(at(guest), size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

// A fault inside the window is a guest access to an address with no memory behind it (on the
// console a DSI exception, which the game would not survive either): say which address, then let
// the crash handler write its dump.
LONG CALLBACK fault_handler(EXCEPTION_POINTERS* info) {
    const auto& rec = *info->ExceptionRecord;
    if (rec.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec.NumberParameters >= 2) {
        const u64 addr = rec.ExceptionInformation[1];
        if (addr >= PANDORA_GUEST_BASE && addr < PANDORA_GUEST_BASE + WINDOW)
            LOG_ERROR("mem", "guest %s of unmapped address %08X (host %p)",
                      rec.ExceptionInformation[0] ? "write" : "read", (u32)(addr - PANDORA_GUEST_BASE),
                      (void*)rec.ExceptionAddress);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#else
bool commit(u32 guest, u32 size) {
    return mprotect(at(guest), size, PROT_READ | PROT_WRITE) == 0;
}
#endif

}  // namespace

bool adopt(CPUState* cpu) {
#ifdef _WIN32
    if (!VirtualAlloc(at(0), WINDOW, MEM_RESERVE, PAGE_NOACCESS)) {
        LOG_ERROR("mem", "cannot reserve the guest address window at %llx (error %lu); the recompiled code "
                  "is built for that address",
                  (unsigned long long)PANDORA_GUEST_BASE, GetLastError());
        return false;
    }
    AddVectoredExceptionHandler(1, fault_handler);
#else
    if (mmap(at(0), WINDOW, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != at(0)) {
        LOG_ERROR("mem", "cannot reserve the guest address window at %llx", (unsigned long long)PANDORA_GUEST_BASE);
        return false;
    }
#endif
    if (!commit(PANDORA_MEM1_BASE, PANDORA_MEM1_SIZE) || !commit(PANDORA_MEM2_BASE, PANDORA_MEM2_SIZE) ||
        !commit(PANDORA_LC_BASE, PANDORA_LC_SIZE)) {
        LOG_ERROR("mem", "cannot commit guest memory");
        return false;
    }
    // Carry over whatever cpu_init put there (it is zeroed, but be exact) and take over.
    if (cpu->ram) {
        std::memcpy(at(PANDORA_MEM1_BASE), cpu->ram, cpu->ram_size < PANDORA_MEM1_SIZE ? cpu->ram_size : PANDORA_MEM1_SIZE);
        std::free(cpu->ram);
    }
    if (cpu->mem2) {
        std::memcpy(at(PANDORA_MEM2_BASE), cpu->mem2, cpu->mem2_size < PANDORA_MEM2_SIZE ? cpu->mem2_size : PANDORA_MEM2_SIZE);
        std::free(cpu->mem2);
    }
    cpu->ram = at(PANDORA_MEM1_BASE);
    cpu->ram_size = PANDORA_MEM1_SIZE;
    cpu->mem2 = at(PANDORA_MEM2_BASE);
    cpu->mem2_size = PANDORA_MEM2_SIZE;
    return true;
}

void release(CPUState* cpu) {
    cpu->ram = nullptr;
    cpu->mem2 = nullptr;
    cpu->mem2_size = 0;
}

u8* locked_cache() { return at(PANDORA_LC_BASE); }

}  // namespace guest_memory

extern "C" bool pandora_guest_memory_adopt(CPUState* cpu) { return guest_memory::adopt(cpu); }
