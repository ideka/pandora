// Host input mapped to a Wii Remote + Nunchuk: SDL gamepads and keyboard through remappable
// bindings, plus scripted presses for automated runs.
#pragma once

#include <array>
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

// The emulated controls that can be bound.
enum class Control : u8 {
    A, B, One, Two, Plus, Minus, Home,
    DpadUp, DpadDown, DpadLeft, DpadRight,
    Z, C,
    StickUp, StickDown, StickLeft, StickRight,
    PointerUp, PointerDown, PointerLeft, PointerRight,
    Count,
};
constexpr size_t kControlCount = (size_t)Control::Count;

struct ControlInfo {
    const char* id;     // settings file key
    const char* label;  // shown in the menu
};
const ControlInfo& control_info(Control c);

// One physical input: a keyboard key, a gamepad button, or one direction of a gamepad axis.
struct PhysicalInput {
    enum class Kind : u8 { None, Key, Button, Axis };
    Kind kind = Kind::None;
    int code = 0;  // SDL_Scancode, SDL_GamepadButton or SDL_GamepadAxis
    int sign = 1;  // axis direction: +1 (right/down, or trigger pulled) or -1

    bool operator==(const PhysicalInput&) const = default;
    static PhysicalInput key(int scancode) { return {Kind::Key, scancode, 1}; }
    static PhysicalInput button(int b) { return {Kind::Button, b, 1}; }
    static PhysicalInput axis(int a, int sign) { return {Kind::Axis, a, sign}; }

    std::string to_string() const;  // "key:Return", "button:a", "axis:lefty-", "" for none
    static PhysicalInput parse(const std::string& s);
    std::string label() const;  // for display
};

// Each control has one controller binding and one keyboard binding.
enum class BindingSlot : u8 { Controller, Keyboard };

struct Bindings {
    std::array<std::array<PhysicalInput, 2>, kControlCount> map{};

    static Bindings defaults();
    PhysicalInput& at(Control c, BindingSlot s) { return map[(size_t)c][(size_t)s]; }
    const PhysicalInput& at(Control c, BindingSlot s) const { return map[(size_t)c][(size_t)s]; }
};

class Input {
public:
    // "A@150+10": hold A from guest frame 150 for 10 frames (default 6).
    bool add_script(const std::string& spec);

    // Bindings in effect (owned by the caller, e.g. the settings); defaults when unset.
    void set_bindings(const Bindings* bindings) { bindings_ = bindings; }

    // Poll the host devices (when use_devices) and apply scripted presses for this guest frame.
    void update(u64 frame, bool use_devices);
    const PadState& state() const { return state_; }

private:
    struct Scripted {
        u32 buttons;
        u64 start, frames;
    };
    std::vector<Scripted> script_;
    const Bindings* bindings_ = nullptr;
    PadState state_;
    float pointer_x_ = 0, pointer_y_ = 0;
};
