#include "input.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {

constexpr ControlInfo CONTROLS[kControlCount] = {
    {"a", "A"},
    {"b", "B"},
    {"1", "1"},
    {"2", "2"},
    {"plus", "+ (Plus)"},
    {"minus", "- (Minus)"},
    {"home", "Home"},
    {"dpad_up", "D-Pad Up"},
    {"dpad_down", "D-Pad Down"},
    {"dpad_left", "D-Pad Left"},
    {"dpad_right", "D-Pad Right"},
    {"z", "Nunchuk Z"},
    {"c", "Nunchuk C"},
    {"stick_up", "Nunchuk Stick Up"},
    {"stick_down", "Nunchuk Stick Down"},
    {"stick_left", "Nunchuk Stick Left"},
    {"stick_right", "Nunchuk Stick Right"},
    {"pointer_up", "Pointer Up"},
    {"pointer_down", "Pointer Down"},
    {"pointer_left", "Pointer Left"},
    {"pointer_right", "Pointer Right"},
};

// Wii buttons driven by each button control (the rest are analog).
constexpr u32 BUTTON_BITS[kControlCount] = {
    WPAD_A, WPAD_B, WPAD_1, WPAD_2, WPAD_PLUS, WPAD_MINUS, WPAD_HOME,
    WPAD_UP, WPAD_DOWN, WPAD_LEFT, WPAD_RIGHT, WPAD_Z, WPAD_C,
};

struct ButtonName {
    const char* name;
    u32 bit;
};
constexpr ButtonName SCRIPT_BUTTONS[] = {
    {"A", WPAD_A},       {"B", WPAD_B},         {"1", WPAD_1},       {"2", WPAD_2},
    {"PLUS", WPAD_PLUS}, {"MINUS", WPAD_MINUS}, {"HOME", WPAD_HOME}, {"UP", WPAD_UP},
    {"DOWN", WPAD_DOWN}, {"LEFT", WPAD_LEFT},   {"RIGHT", WPAD_RIGHT}, {"Z", WPAD_Z},
    {"C", WPAD_C},
};

constexpr float DEAD_ZONE = 0.2f;

SDL_Gamepad* first_gamepad() {
    static SDL_Gamepad* pad = nullptr;
    if (pad && SDL_GamepadConnected(pad))
        return pad;
    pad = nullptr;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (ids && count > 0)
        pad = SDL_OpenGamepad(ids[0]);
    SDL_free(ids);
    return pad;
}

// 0..1 for one physical input.
float input_value(const PhysicalInput& in, const bool* keys, SDL_Gamepad* pad) {
    switch (in.kind) {
    case PhysicalInput::Kind::Key:
        return keys[in.code] ? 1.0f : 0.0f;
    case PhysicalInput::Kind::Button:
        return pad && SDL_GetGamepadButton(pad, (SDL_GamepadButton)in.code) ? 1.0f : 0.0f;
    case PhysicalInput::Kind::Axis: {
        if (!pad)
            return 0.0f;
        const float v = SDL_GetGamepadAxis(pad, (SDL_GamepadAxis)in.code) / 32767.0f * (float)in.sign;
        return v < DEAD_ZONE ? 0.0f : std::min(1.0f, (v - DEAD_ZONE) / (1.0f - DEAD_ZONE));
    }
    case PhysicalInput::Kind::None:
        break;
    }
    return 0.0f;
}

}  // namespace

const ControlInfo& control_info(Control c) { return CONTROLS[(size_t)c]; }

std::string PhysicalInput::to_string() const {
    switch (kind) {
    case Kind::Key:
        return std::string("key:") + SDL_GetScancodeName((SDL_Scancode)code);
    case Kind::Button:
        if (const char* s = SDL_GetGamepadStringForButton((SDL_GamepadButton)code))
            return std::string("button:") + s;
        break;
    case Kind::Axis:
        if (const char* s = SDL_GetGamepadStringForAxis((SDL_GamepadAxis)code))
            return std::string("axis:") + s + (sign < 0 ? "-" : "+");
        break;
    case Kind::None:
        break;
    }
    return "";
}

PhysicalInput PhysicalInput::parse(const std::string& s) {
    const size_t colon = s.find(':');
    if (colon == std::string::npos)
        return {};
    const std::string kind = s.substr(0, colon), value = s.substr(colon + 1);
    if (kind == "key") {
        const SDL_Scancode sc = SDL_GetScancodeFromName(value.c_str());
        return sc == SDL_SCANCODE_UNKNOWN ? PhysicalInput{} : key(sc);
    }
    if (kind == "button") {
        const SDL_GamepadButton b = SDL_GetGamepadButtonFromString(value.c_str());
        return b == SDL_GAMEPAD_BUTTON_INVALID ? PhysicalInput{} : button(b);
    }
    if (kind == "axis" && value.size() > 1) {
        const char dir = value.back();
        const SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(value.substr(0, value.size() - 1).c_str());
        if (a == SDL_GAMEPAD_AXIS_INVALID || (dir != '+' && dir != '-'))
            return {};
        return axis(a, dir == '-' ? -1 : 1);
    }
    return {};
}

std::string PhysicalInput::label() const {
    switch (kind) {
    case Kind::Key: {
        const char* name = SDL_GetScancodeName((SDL_Scancode)code);
        return name && *name ? name : "Unknown key";
    }
    case Kind::Button:
        switch ((SDL_GamepadButton)code) {
        case SDL_GAMEPAD_BUTTON_SOUTH: return "South (A/Cross)";
        case SDL_GAMEPAD_BUTTON_EAST: return "East (B/Circle)";
        case SDL_GAMEPAD_BUTTON_WEST: return "West (X/Square)";
        case SDL_GAMEPAD_BUTTON_NORTH: return "North (Y/Triangle)";
        case SDL_GAMEPAD_BUTTON_BACK: return "Back/Select";
        case SDL_GAMEPAD_BUTTON_GUIDE: return "Guide";
        case SDL_GAMEPAD_BUTTON_START: return "Start";
        case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "Left Stick Click";
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "Right Stick Click";
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "Left Bumper";
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "Right Bumper";
        case SDL_GAMEPAD_BUTTON_DPAD_UP: return "D-Pad Up";
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "D-Pad Down";
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "D-Pad Left";
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "D-Pad Right";
        default: {
            const char* s = SDL_GetGamepadStringForButton((SDL_GamepadButton)code);
            return s ? s : "Button";
        }
        }
    case Kind::Axis:
        switch ((SDL_GamepadAxis)code) {
        case SDL_GAMEPAD_AXIS_LEFTX: return sign < 0 ? "Left Stick Left" : "Left Stick Right";
        case SDL_GAMEPAD_AXIS_LEFTY: return sign < 0 ? "Left Stick Up" : "Left Stick Down";
        case SDL_GAMEPAD_AXIS_RIGHTX: return sign < 0 ? "Right Stick Left" : "Right Stick Right";
        case SDL_GAMEPAD_AXIS_RIGHTY: return sign < 0 ? "Right Stick Up" : "Right Stick Down";
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return "Left Trigger";
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return "Right Trigger";
        default: return "Axis";
        }
    case Kind::None:
        break;
    }
    return "-";
}

Bindings Bindings::defaults() {
    Bindings b;
    auto set = [&](Control c, PhysicalInput pad, PhysicalInput key) {
        b.at(c, BindingSlot::Controller) = pad;
        b.at(c, BindingSlot::Keyboard) = key;
    };
    using P = PhysicalInput;
    set(Control::A, P::button(SDL_GAMEPAD_BUTTON_SOUTH), P::key(SDL_SCANCODE_RETURN));
    set(Control::B, P::button(SDL_GAMEPAD_BUTTON_EAST), P::key(SDL_SCANCODE_BACKSPACE));
    set(Control::One, P::button(SDL_GAMEPAD_BUTTON_WEST), P::key(SDL_SCANCODE_1));
    set(Control::Two, P::button(SDL_GAMEPAD_BUTTON_NORTH), P::key(SDL_SCANCODE_2));
    set(Control::Plus, P::button(SDL_GAMEPAD_BUTTON_START), P::key(SDL_SCANCODE_EQUALS));
    set(Control::Minus, P::button(SDL_GAMEPAD_BUTTON_BACK), P::key(SDL_SCANCODE_MINUS));
    set(Control::Home, P{}, P::key(SDL_SCANCODE_H));  // the gamepad's Guide button opens the menu
    set(Control::DpadUp, P::button(SDL_GAMEPAD_BUTTON_DPAD_UP), P::key(SDL_SCANCODE_UP));
    set(Control::DpadDown, P::button(SDL_GAMEPAD_BUTTON_DPAD_DOWN), P::key(SDL_SCANCODE_DOWN));
    set(Control::DpadLeft, P::button(SDL_GAMEPAD_BUTTON_DPAD_LEFT), P::key(SDL_SCANCODE_LEFT));
    set(Control::DpadRight, P::button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT), P::key(SDL_SCANCODE_RIGHT));
    set(Control::Z, P::axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1), P::key(SDL_SCANCODE_Z));
    set(Control::C, P::button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER), P::key(SDL_SCANCODE_C));
    set(Control::StickUp, P::axis(SDL_GAMEPAD_AXIS_LEFTY, -1), P::key(SDL_SCANCODE_W));
    set(Control::StickDown, P::axis(SDL_GAMEPAD_AXIS_LEFTY, 1), P::key(SDL_SCANCODE_S));
    set(Control::StickLeft, P::axis(SDL_GAMEPAD_AXIS_LEFTX, -1), P::key(SDL_SCANCODE_A));
    set(Control::StickRight, P::axis(SDL_GAMEPAD_AXIS_LEFTX, 1), P::key(SDL_SCANCODE_D));
    set(Control::PointerUp, P::axis(SDL_GAMEPAD_AXIS_RIGHTY, -1), P::key(SDL_SCANCODE_I));
    set(Control::PointerDown, P::axis(SDL_GAMEPAD_AXIS_RIGHTY, 1), P::key(SDL_SCANCODE_K));
    set(Control::PointerLeft, P::axis(SDL_GAMEPAD_AXIS_RIGHTX, -1), P::key(SDL_SCANCODE_J));
    set(Control::PointerRight, P::axis(SDL_GAMEPAD_AXIS_RIGHTX, 1), P::key(SDL_SCANCODE_L));
    return b;
}

bool Input::add_script(const std::string& spec) {
    const size_t at = spec.find('@');
    if (at == std::string::npos)
        return false;
    const std::string name = spec.substr(0, at);
    u32 bit = 0;
    for (const auto& b : SCRIPT_BUTTONS)
        if (name == b.name)
            bit = b.bit;
    if (!bit)
        return false;
    char* end = nullptr;
    const u64 start = std::strtoull(spec.c_str() + at + 1, &end, 10);
    const u64 frames = *end == '+' ? std::strtoull(end + 1, nullptr, 10) : 6;
    script_.push_back({bit, start, frames});
    return true;
}

void Input::update(u64 frame, bool use_devices) {
    static const Bindings default_bindings = Bindings::defaults();
    const Bindings& bindings = bindings_ ? *bindings_ : default_bindings;

    PadState s;
    float px = 0, py = 0;
    if (use_devices) {
        const bool* keys = SDL_GetKeyboardState(nullptr);
        SDL_Gamepad* pad = first_gamepad();
        // Alt+Enter toggles fullscreen; don't let it also press a control bound to Enter.
        const bool alt = SDL_GetModState() & SDL_KMOD_ALT;
        std::array<float, kControlCount> v{};
        for (size_t c = 0; c < kControlCount; c++)
            for (const PhysicalInput& in : bindings.map[c]) {
                if (alt && in.kind == PhysicalInput::Kind::Key &&
                    (in.code == SDL_SCANCODE_RETURN || in.code == SDL_SCANCODE_KP_ENTER))
                    continue;
                v[c] = std::max(v[c], input_value(in, keys, pad));
            }
        for (size_t c = 0; c < kControlCount; c++)
            if (BUTTON_BITS[c] && v[c] >= 0.5f)
                s.buttons |= BUTTON_BITS[c];
        auto val = [&](Control c) { return v[(size_t)c]; };
        s.stick_x = std::clamp(val(Control::StickRight) - val(Control::StickLeft), -1.0f, 1.0f);
        s.stick_y = std::clamp(val(Control::StickUp) - val(Control::StickDown), -1.0f, 1.0f);
        px = val(Control::PointerRight) - val(Control::PointerLeft);
        py = val(Control::PointerDown) - val(Control::PointerUp);
    }
    // The pointer moves at a rate set by its controls and stays where it was left.
    pointer_x_ = std::clamp(pointer_x_ + px * 0.05f, -1.0f, 1.0f);
    pointer_y_ = std::clamp(pointer_y_ + py * 0.05f, -1.0f, 1.0f);
    s.pointer_x = pointer_x_;
    s.pointer_y = pointer_y_;

    for (const auto& p : script_)
        if (frame >= p.start && frame < p.start + p.frames)
            s.buttons |= p.buttons;
    state_ = s;
}
