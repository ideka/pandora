// High-level replacements for guest functions whose hardware we do not emulate.
// Every function hooked here must also be listed in config/SX3EXJ/hooks.txt.
#include <algorithm>
#include <cstring>

#include "log.h"
#include "memory.h"
#include "runtime.h"

namespace {

// printf-style formatting of a guest format string. Integer arguments come from GPRs starting at
// first_gpr, then from the caller's parameter save area; doubles from f1-f8 (EABI varargs).
std::string guest_format(Runtime& rt, u32 fmt_addr, int first_gpr) {
    CPUState* c = &rt.cpu;
    int gpr = first_gpr, fpr = 1;
    u32 stack = c->gpr[1] + 8;
    auto next_int = [&]() -> u32 {
        if (gpr <= 10)
            return c->gpr[gpr++];
        u32 v = mem::read32(c, stack);
        stack += 4;
        return v;
    };
    std::string fmt = mem::cstr(c, fmt_addr, 1024), out;
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%') {
            out += fmt[i];
            continue;
        }
        size_t start = i++;
        while (i < fmt.size() && std::strchr("-+ #0123456789.*", fmt[i]))
            i++;
        while (i < fmt.size() && std::strchr("hlLqjzt", fmt[i]))
            i++;
        if (i >= fmt.size())
            break;
        std::string spec = fmt.substr(start, i - start);
        spec.erase(std::remove_if(spec.begin() + 1, spec.end(), [](char ch) { return std::strchr("hlLqjzt", ch); }),
                   spec.end());
        char buf[512];
        switch (char conv = fmt[i]) {
        case '%': out += '%'; continue;
        case 's': std::snprintf(buf, sizeof buf, (spec + 's').c_str(), mem::cstr(c, next_int()).c_str()); break;
        case 'c': std::snprintf(buf, sizeof buf, (spec + 'c').c_str(), (int)next_int()); break;
        case 'd': case 'i': std::snprintf(buf, sizeof buf, (spec + 'd').c_str(), (int)next_int()); break;
        case 'u': case 'x': case 'X': case 'o':
            std::snprintf(buf, sizeof buf, (spec + conv).c_str(), next_int());
            break;
        case 'p': std::snprintf(buf, sizeof buf, "0x%08X", next_int()); break;
        case 'f': case 'e': case 'g': case 'F': case 'E': case 'G':
            std::snprintf(buf, sizeof buf, (spec + conv).c_str(), fpr <= 8 ? c->fpr[fpr++] : 0.0);
            break;
        default: std::snprintf(buf, sizeof buf, "%s%c", spec.c_str(), conv); break;
        }
        out += buf;
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    return out;
}

}  // namespace

void Runtime::install_hle() {
    // The DSP is not emulated at the hardware level: skip the boot-time DSP handshake.
    // TODO: replace with AX-level audio.
    for (const char* name : {"__OSInitAudioSystem", "__OSStopAudioSystem"})
        hook(name, [](Runtime& rt) { rt.ret(0); });

    // OSReport writes to the (absent) debug UART: print it instead.
    hook("OSReport", [](Runtime& rt) {
        LOG_INFO("guest", "%s", guest_format(rt, rt.cpu.gpr[3], 4).c_str());
        rt.ret(0);
    });
    // OSPanic(file, line, fmt, ...)
    hook("OSPanic", [](Runtime& rt) {
        std::string file = mem::cstr(&rt.cpu, rt.cpu.gpr[3]);
        rt.halt("OSPanic " + file + ":" + std::to_string(rt.cpu.gpr[4]) + ": " + guest_format(rt, rt.cpu.gpr[5], 6));
    });
    // OSFatal(GXColor fg, GXColor bg, const char* msg)
    hook("OSFatal", [](Runtime& rt) { rt.halt("OSFatal: " + mem::cstr(&rt.cpu, rt.cpu.gpr[5], 1024)); });
}
