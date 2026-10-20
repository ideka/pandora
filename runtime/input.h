// Host input mapped to a Wii Remote + Nunchuk: SDL gamepads, keyboard and mouse through remappable
// bindings, plus scripted presses for automated runs. The settings menu's own actions are bound
// the same way.
#pragma once

#include <array>
#include <string>
#include <vector>

#include "guest.h"
#include "wiimote.h"
#include "wiimote_scanner.h"

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
    float horizon_x = 1, horizon_y = 0;  // KPAD horizon (the remote's roll) and dist (metres to the bar)
    float dist = 2;
    bool motion = false;  // accelerometers from a real Wii Remote (otherwise "at rest")
    float acc[3] = {0, -1, 0}, nunchuk_acc[3] = {0, -1, 0};
};

// The emulated controls that can be bound.
enum class Control : u8 {
    A, B, One, Two, Plus, Minus,  // no Home: the HOME Menu is disabled
    DpadUp, DpadDown, DpadLeft, DpadRight,
    Z, C,
    StickUp, StickDown, StickLeft, StickRight,
    PointerUp, PointerDown, PointerLeft, PointerRight,
    Count,
};
constexpr size_t kControlCount = (size_t)Control::Count;

// The settings menu's actions.
enum class MenuAction : u8 { Toggle, Accept, Cancel, Up, Down, Left, Right, PrevTab, NextTab, Count };
constexpr size_t kMenuActionCount = (size_t)MenuAction::Count;

struct BindingInfo {
    const char* id;     // settings file key
    const char* label;  // shown in the menu
    bool required;      // the last mapping on each device cannot be removed
};
const BindingInfo& control_info(Control c);
const BindingInfo& menu_action_info(MenuAction a);

// Bindings are kept per device class; each control has two mappings on each.
enum class InputDevice : u8 { Gamepad, KeyboardMouse };
constexpr size_t kDeviceCount = 2;
constexpr size_t kMappingsPerDevice = 2;

// One physical input: a key, a mouse button, a gamepad button, or one direction of a gamepad axis.
struct PhysicalInput {
    enum class Kind : u8 { None, Key, MouseButton, Button, Axis };
    Kind kind = Kind::None;
    int code = 0;  // SDL_Scancode, SDL_BUTTON_*, SDL_GamepadButton or SDL_GamepadAxis
    int sign = 1;  // axis direction: +1 (right/down, or trigger pulled) or -1

    bool operator==(const PhysicalInput&) const = default;
    explicit operator bool() const { return kind != Kind::None; }
    static PhysicalInput key(int scancode) { return {Kind::Key, scancode, 1}; }
    static PhysicalInput mouse(int b) { return {Kind::MouseButton, b, 1}; }
    static PhysicalInput button(int b) { return {Kind::Button, b, 1}; }
    static PhysicalInput axis(int a, int sign) { return {Kind::Axis, a, sign}; }

    InputDevice device() const { return kind == Kind::Button || kind == Kind::Axis ? InputDevice::Gamepad : InputDevice::KeyboardMouse; }
    std::string to_string() const;  // "key:Return", "mouse:x1", "button:a", "axis:lefty-", "" for none
    static PhysicalInput parse(const std::string& s);
    std::string label() const;  // for display
};

// [device][slot]
using Mappings = std::array<std::array<PhysicalInput, kMappingsPerDevice>, kDeviceCount>;

struct Bindings {
    std::array<Mappings, kControlCount> controls{};
    std::array<Mappings, kMenuActionCount> menu{};

    static Bindings defaults();
};

class Input {
public:
    // "A@150+10": hold A from guest frame 150 for 10 frames (default 6).
    bool add_script(const std::string& spec);

    // Bindings in effect (owned by the caller, e.g. the settings); defaults when unset.
    void set_bindings(const Bindings* bindings) { bindings_ = bindings; }

    // Rumble from the game (WPADControlMotor), scaled by intensity (0 = off).
    void set_rumble(bool on) { rumble_on_ = on; }
    void set_rumble_intensity(float intensity) { rumble_intensity_ = intensity; }
    // Whether the mouse position aims the pointer (absolute, over the window).
    void set_mouse_pointer(bool enabled) { mouse_pointer_ = enabled; }
    // Keep the Nunchuk stick short of the game's snap to the four cardinal directions.
    void set_free_stick(bool enabled) { free_stick_ = enabled; }

    // A real Wii Remote, if one is paired: poll every host frame.
    void poll_wiimote();
    const Wiimote& wiimote() const { return wiimote_; }
    void set_sensor_bar_above(bool above) { wiimote_.set_sensor_bar_above(above); }
    void set_ir_sensitivity(int level) { wiimote_.set_ir_sensitivity(level); }
    // Continuous Bluetooth scanning for Wii Remotes (press 1+2 to connect).
    void set_wiimote_scanning(bool enabled) { scanner_.set_enabled(enabled); }
    WiimoteScanner::Status wiimote_scan_status() const { return scanner_.status(); }

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
    float mouse_x_ = -1, mouse_y_ = -1;  // last mouse position seen (window coordinates)
    bool mouse_pointer_ = true;
    bool free_stick_ = true;
    bool rumble_on_ = false, rumbling_ = false;
    float rumble_intensity_ = 1.0f;
    Wiimote wiimote_;
    WiimoteScanner scanner_;
    u64 wiimote_rumble_until_ = 0;  // SDL ticks (ms): the remote's motor has no timeout of its own
    u64 pointer_input_at_ = 0;      // SDL ticks of the last mouse/bound pointer movement
};
