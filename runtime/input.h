// Host input mapped to a Wii Remote + Nunchuk: SDL gamepads and keyboard, plus scripted presses
// for automated runs.
#pragma once

#include <string>
#include <vector>

#include "guest.h"

// KPAD/WPAD core button bits (Nunchuk Z/C as KPAD reports them).
enum : u32 {
    WPAD_LEFT = 0x0001,
    WPAD_RIGHT = 0x0002,
    WPAD_DOWN = 0x0004,
    WPAD_UP = 0x0008,
    WPAD_PLUS = 0x0010,
    WPAD_2 = 0x0100,
    WPAD_1 = 0x0200,
    WPAD_B = 0x0400,
    WPAD_A = 0x0800,
    WPAD_MINUS = 0x1000,
    WPAD_Z = 0x2000,
    WPAD_C = 0x4000,
    WPAD_HOME = 0x8000,
};

struct PadState {
    u32 buttons = 0;
    float stick_x = 0, stick_y = 0;      // Nunchuk stick, -1..1 (y up)
    float pointer_x = 0, pointer_y = 0;  // screen pointer, -1..1 (y down), as KPAD reports it
    bool pointer_valid = true;
};

class Input {
public:
    // "A@150+10": hold A from guest frame 150 for 10 frames (default 6). Buttons can be joined
    // with '+' before the '@' only via separate --press options.
    bool add_script(const std::string& spec);

    // Poll the host devices (when use_devices) and apply scripted presses for this guest frame.
    void update(u64 frame, bool use_devices);
    const PadState& state() const { return state_; }

private:
    struct Scripted {
        u32 buttons;
        u64 start, frames;
    };
    std::vector<Scripted> script_;
    PadState state_;
    float pointer_x_ = 0, pointer_y_ = 0;
};
