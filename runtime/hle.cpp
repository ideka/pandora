// High-level replacements for guest functions whose hardware we do not emulate.
// Every function hooked here must also be listed in config/SX3P01/hooks.txt.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <cstdlib>

#include "input.h"
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

// Continue into a hooked guest function: its first instruction (carrying the hook marker in the
// recompiled code) is still the original in guest memory; it is a stack-frame push (stwu r1,
// -N(r1)) for every function hooked this way, so emulate it and resume at the second instruction.
static void run_original(Runtime& rt, u32 entry) {
    CPUState* c = &rt.cpu;
    const u32 insn = mem::read32(c, entry);
    if ((insn >> 26) != 37 || ((insn >> 21) & 31) != 1 || ((insn >> 16) & 31) != 1) {
        rt.halt("run_original: hooked function does not start with stwu r1 at " + rt.describe(entry));
        return;
    }
    const u32 sp = c->gpr[1] + (u32)(s32)(s16)(insn & 0xFFFF);
    mem::write32(c, sp, c->gpr[1]);
    c->gpr[1] = sp;
    c->pc = entry + 4;
}

void Runtime::install_hle() {
    // The DSP is not emulated at the hardware level: skip the boot-time DSP handshake.
    // TODO: replace with AX-level audio.
    for (const char* name : {"__OSInitAudioSystem", "__OSStopAudioSystem"})
        hook(name, [](Runtime& rt) { rt.ret(0); });

    // No Bluetooth stack: input is provided at the WPAD/KPAD level from the host's devices.
    // Channel 0 is a Wii Remote with a Nunchuk; the other channels are empty.
    hook("WPADInit", [](Runtime& rt) { rt.ret(0); });

    // s32 WPADProbe(s32 chan, u32* type): 0 = connected, -1 = no controller.
    hook("WPADProbe", [](Runtime& rt) {
        CPUState* c = &rt.cpu;
        if (c->gpr[3] != 0) {
            rt.ret((u32)-1);
            return;
        }
        if (c->gpr[4])
            mem::write32(c, c->gpr[4], 1);  // WPAD_DEV_FREESTYLE (Nunchuk attached)
        rt.ret(0);
    });

    // s32 WPADGetInfoAsync(s32 chan, WPADInfo* info, callback): battery/extension info.
    // TODO: invoke the callback (needs calling back into guest code).
    hook("WPADGetInfoAsync", [](Runtime& rt) {
        CPUState* c = &rt.cpu;
        if (c->gpr[3] != 0) {
            rt.ret((u32)-1);
            return;
        }
        const u32 info = c->gpr[4];
        for (u32 i = 0; i < 0x18; i += 4)
            mem::write32(c, info + i, 0);
        mem::write32(c, info + 0x00, 1);  // dpd
        mem::write32(c, info + 0x08, 1);  // extension attached
        mem::write8(c, info + 0x14, 4);   // battery: full
        mem::write8(c, info + 0x15, 1);   // player LED 1
        rt.ret(0);
    });

    // BOOL WPADIsMotorEnabled(void): the console's rumble setting, normally read by WPADInit (stubbed).
    // Always on; the settings menu's rumble intensity decides whether the host gamepad shakes.
    hook("WPADIsMotorEnabled", [](Runtime& rt) { rt.ret(1); });

    // void WPADControlMotor(s32 chan, u32 command): 1 starts the rumble motor, 0 stops it.
    hook("WPADControlMotor", [](Runtime& rt) {
        if (rt.cpu.gpr[3] == 0) {
            LOG_DEBUG("input", "rumble %s", rt.cpu.gpr[4] == 1 ? "on" : "off");
            rt.input().set_rumble(rt.cpu.gpr[4] == 1);
        }
        rt.ret(0);
    });

    // void dynMgrExtraStep(DynMgr*): an extra cloth step (Aeron's cape and other dynamics) after a
    // character's update. The game logic always ticks at 60 Hz and runs 60/target ticks per rendered
    // frame; the callers ran this only on the second tick of a frame (frame manager +0x448 clear),
    // so at 30 FPS cloth gets 3 steps per 2 ticks and was tuned that way. At 60 FPS (one tick per
    // frame) there is no second tick and the cape floats. The callers' tick check is patched out
    // (patches.txt) and decided here: unchanged with several ticks per frame, every other tick with
    // one.
    hook("dynMgrExtraStep", [](Runtime& rt) {
        CPUState* c = &rt.cpu;
        constexpr u32 FRAME_MANAGER = 0x80774678, TICK_COUNTER = 0x807F04F8;
        const u32 max_rate = mem::read32(c, FRAME_MANAGER + 0x434), rate = mem::read32(c, FRAME_MANAGER + 0x438);
        const bool one_tick_per_frame = rate != 0 && max_rate / rate <= 1;
        const bool run = one_tick_per_frame ? (mem::read32(c, TICK_COUNTER) & 1) != 0
                                            : mem::read8(c, FRAME_MANAGER + 0x448) == 0;
        if (!run) {
            rt.ret(c->gpr[3]);
            return;
        }
        // Run the original: its first instruction (stwu r1, -32(r1)) carries the hook marker.
        const u32 sp = c->gpr[1] - 32;
        mem::write32(c, sp, c->gpr[1]);
        c->gpr[1] = sp;
        c->pc = 0x802527DC;
    });

    // The cloth's anchors: dynClothSetAnchors (fn_804134FC) writes the pinned particles from the
    // skeleton's world matrices at the start of every step, and dynClothRefreshBones (fn_80412F14)
    // the cloth's root frame. The engine recomputes world matrices once per rendered frame, so at
    // 30 FPS the anchors move only on the first of a frame's two ticks, and the cloth (Aeron's
    // cape) was tuned for that: at 60 FPS they move every tick and the cape streams out behind
    // him (the 60 FPS code's known side effect). With one tick per frame, skip both on odd ticks:
    // the anchors then move every other tick by a frame's worth, as at 30 FPS.
    // PANDORA_CAPE_OFF=1 disables this (tests).
    auto hold_anchor_this_tick = [](Runtime& rt) {
        CPUState* c = &rt.cpu;
        constexpr u32 FRAME_MANAGER = 0x80774678, TICK_COUNTER = 0x807F04F8;
        static const bool off = std::getenv("PANDORA_CAPE_OFF") != nullptr;
        const u32 max_rate = mem::read32(c, FRAME_MANAGER + 0x434), rate = mem::read32(c, FRAME_MANAGER + 0x438);
        return !off && rate != 0 && max_rate / rate <= 1 && (mem::read32(c, TICK_COUNTER) & 1) != 0;
    };
    hook("dynClothRefreshBones", [hold_anchor_this_tick](Runtime& rt) {
        if (hold_anchor_this_tick(rt))
            rt.ret(0);
        else
            run_original(rt, 0x80412F14);
    });
    hook("dynClothSetAnchors", [hold_anchor_this_tick](Runtime& rt) {
        if (hold_anchor_this_tick(rt))
            rt.ret(0);
        else
            run_original(rt, 0x804134FC);
    });

    // void sceneRefreshSkinned(void) (fn_804A5780): a scene pass that (re)assigns skinned nodes
    // (type 14) their slots and marks their subtrees' matrices dirty; the game runs it only on the
    // second tick of a frame, which never comes at 60 FPS: the skinned meshes then keep their old
    // matrices and Aeron's cape renders frozen in its bind pose while the cloth moves underneath.
    // The tick check is patched out (patches.txt) and decided here: unchanged with several ticks
    // per frame, every tick with one (once per rendered frame either way).
    hook("sceneRefreshSkinned", [](Runtime& rt) {
        CPUState* c = &rt.cpu;
        constexpr u32 FRAME_MANAGER = 0x80774678;
        const u32 max_rate = mem::read32(c, FRAME_MANAGER + 0x434), rate = mem::read32(c, FRAME_MANAGER + 0x438);
        const bool one_tick_per_frame = rate != 0 && max_rate / rate <= 1;
        if (one_tick_per_frame || mem::read8(c, FRAME_MANAGER + 0x448) == 0)
            run_original(rt, 0x804A5780);
        else
            rt.ret(0);
    });

    // s32 KPADRead(s32 chan, KPADStatus* samples, u32 length): one sample per call.
    // KPADStatus (0xF0 bytes, RVL SDK 4.x): hold/trig/release, acc, pointer, horizon, distance,
    // device type/error/DPD-valid/format at 0x5C, Nunchuk status at 0x60.
    hook("KPADRead", [](Runtime& rt) {
        CPUState* c = &rt.cpu;
        static u32 previous = 0;
        const u32 chan = c->gpr[3], buf = c->gpr[4], len = c->gpr[5];
        if (chan != 0 || len == 0) {
            rt.ret(0);
            return;
        }
        auto f32 = [&](u32 off, float v) {
            u32 bits;
            std::memcpy(&bits, &v, 4);
            mem::write32(c, buf + off, bits);
        };
        PadState p = rt.input().state();
        p.buttons &= ~WPAD_HOME;  // the HOME Menu is disabled (a nuisance, and a source of softlocks)
        for (u32 i = 0; i < 0xF0; i += 4)
            mem::write32(c, buf + i, 0);
        mem::write32(c, buf + 0x00, p.buttons);
        mem::write32(c, buf + 0x04, p.buttons & ~previous);
        mem::write32(c, buf + 0x08, previous & ~p.buttons);
        previous = p.buttons;
        // acc (x, y, z in g) and its magnitude: a real Wii Remote's, or at rest (gravity along -y).
        f32(0x0C, p.acc[0]);
        f32(0x10, p.acc[1]);
        f32(0x14, p.acc[2]);
        f32(0x18, std::sqrt(p.acc[0] * p.acc[0] + p.acc[1] * p.acc[1] + p.acc[2] * p.acc[2]));
        f32(0x20, p.pointer_x);
        f32(0x24, p.pointer_y);
        f32(0x34, 1.0f);   // horizon: level
        f32(0x48, 2.0f);   // dist: 2 m from the sensor bar
        f32(0x54, 1.0f);   // acc_vertical
        mem::write8(c, buf + 0x5C, 1);                         // WPAD_DEV_FREESTYLE
        mem::write8(c, buf + 0x5D, 0);                         // WPAD_ERR_NONE
        mem::write8(c, buf + 0x5E, p.pointer_valid ? 2 : 0);   // pointer valid
        mem::write8(c, buf + 0x5F, 5);                         // WPAD_FMT_FS_BTN_ACC_DPD
        f32(0x60, p.stick_x);
        f32(0x64, p.stick_y);
        f32(0x68, p.nunchuk_acc[0]);  // Nunchuk acc and magnitude
        f32(0x6C, p.nunchuk_acc[1]);
        f32(0x70, p.nunchuk_acc[2]);
        f32(0x74, std::sqrt(p.nunchuk_acc[0] * p.nunchuk_acc[0] + p.nunchuk_acc[1] * p.nunchuk_acc[1] +
                            p.nunchuk_acc[2] * p.nunchuk_acc[2]));
        rt.ret(1);
    });

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
