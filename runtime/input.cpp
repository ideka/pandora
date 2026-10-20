#include "input.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <tuple>
#include <utility>

namespace {

constexpr BindingInfo CONTROLS[kControlCount] = {
    {"a", "A", false},
    {"b", "B", false},
    {"1", "1", false},
    {"2", "2", false},
    {"plus", "+ (Plus)", false},
    {"minus", "- (Minus)", false},
    {"dpad_up", "D-Pad Up", false},
    {"dpad_down", "D-Pad Down", false},
    {"dpad_left", "D-Pad Left", false},
    {"dpad_right", "D-Pad Right", false},
    {"z", "Nunchuk Z", false},
    {"c", "Nunchuk C", false},
    {"stick_up", "Nunchuk Stick Up", false},
    {"stick_down", "Nunchuk Stick Down", false},
    {"stick_left", "Nunchuk Stick Left", false},
    {"stick_right", "Nunchuk Stick Right", false},
    {"pointer_up", "Pointer Up", false},
    {"pointer_down", "Pointer Down", false},
    {"pointer_left", "Pointer Left", false},
    {"pointer_right", "Pointer Right", false},
};

constexpr BindingInfo MENU_ACTIONS[kMenuActionCount] = {
    {"toggle", "Open / Close Menu", true},
    {"accept", "Accept", true},
    {"cancel", "Cancel / Back", false},
    {"up", "Up", false},
    {"down", "Down", false},
    {"left", "Left", false},
    {"right", "Right", false},
    {"prev_tab", "Previous Tab", false},
    {"next_tab", "Next Tab", false},
};

// Wii buttons driven by each button control (the rest are analog).
constexpr u32 BUTTON_BITS[kControlCount] = {
    WPAD_A, WPAD_B, WPAD_1, WPAD_2, WPAD_PLUS, WPAD_MINUS,
    WPAD_UP, WPAD_DOWN, WPAD_LEFT, WPAD_RIGHT, WPAD_Z, WPAD_C,
};

struct ButtonName {
    const char* name;
    u32 bit;
};
constexpr ButtonName SCRIPT_BUTTONS[] = {
    {"A", WPAD_A},       {"B", WPAD_B},         {"1", WPAD_1},       {"2", WPAD_2},
    {"PLUS", WPAD_PLUS}, {"MINUS", WPAD_MINUS}, {"UP", WPAD_UP},
    {"DOWN", WPAD_DOWN}, {"LEFT", WPAD_LEFT},   {"RIGHT", WPAD_RIGHT}, {"Z", WPAD_Z},
    {"C", WPAD_C},
};
// Scripted Nunchuk stick directions (not WPAD buttons): pseudo-bits above the button mask.
constexpr u32 SCRIPT_STICK_UP = 1u << 28, SCRIPT_STICK_DOWN = 1u << 29, SCRIPT_STICK_LEFT = 1u << 30,
              SCRIPT_STICK_RIGHT = 1u << 31;
constexpr ButtonName SCRIPT_STICK[] = {
    {"StickUp", SCRIPT_STICK_UP},
    {"StickDown", SCRIPT_STICK_DOWN},
    {"StickLeft", SCRIPT_STICK_LEFT},
    {"StickRight", SCRIPT_STICK_RIGHT},
};

struct MouseName {
    const char* id;
    const char* label;
    int button;
};
constexpr MouseName MOUSE_BUTTONS[] = {
    {"left", "Mouse Left", SDL_BUTTON_LEFT},     {"middle", "Mouse Middle", SDL_BUTTON_MIDDLE},
    {"right", "Mouse Right", SDL_BUTTON_RIGHT},  {"x1", "Mouse Back", SDL_BUTTON_X1},
    {"x2", "Mouse Forward", SDL_BUTTON_X2},
};

constexpr float DEAD_ZONE = 0.2f;

// KPAD pointer values at the screen edges. The pointer is tracked in screen units (-1..1 across
// the window) and scaled by these: in 16:9 the game puts the cursor 262 px (of 640) from the centre
// per unit horizontally and 264 px (of 480) vertically, so the edges are at about 1.22 and 0.91
// (a real Wii Remote reports values past +-1 when it points at the screen's sides).
constexpr float POINTER_RANGE_X = 320.0f / 262.0f;
constexpr float POINTER_RANGE_Y = 240.0f / 264.0f;

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

struct DeviceState {
    const bool* keys;
    SDL_MouseButtonFlags mouse;
    SDL_Gamepad* pad;
};

// Sticks are shaped as vectors: a radial dead zone, full deflection from STICK_FULL (pads rarely
// reach 1.0) and the direction kept, within a circle of the given radius.
constexpr float STICK_DEAD_ZONE = 0.15f, STICK_FULL = 0.85f;

// The game's stick processing (fn_804F1318) snaps the stick to a pure cardinal direction whenever
// one axis exceeds 0.995. Free direction keeps the stick inside a circle just short of that.
constexpr float FREE_STICK_RADIUS = 0.994f;

std::pair<float, float> shape_stick(float x, float y, float radius) {
    const float mag = std::hypot(x, y);
    if (mag < STICK_DEAD_ZONE)
        return {0.0f, 0.0f};
    const float m = std::min(1.0f, (mag - STICK_DEAD_ZONE) / (STICK_FULL - STICK_DEAD_ZONE)) * radius;
    return {x / mag * m, y / mag * m};
}

// 0..1 for one physical input. raw: axes without the dead zone (sticks are shaped as vectors).
float input_value(const PhysicalInput& in, const DeviceState& d, bool raw = false) {
    switch (in.kind) {
    case PhysicalInput::Kind::Key:
        return d.keys[in.code] ? 1.0f : 0.0f;
    case PhysicalInput::Kind::MouseButton:
        return (d.mouse & SDL_BUTTON_MASK(in.code)) ? 1.0f : 0.0f;
    case PhysicalInput::Kind::Button:
        return d.pad && SDL_GetGamepadButton(d.pad, (SDL_GamepadButton)in.code) ? 1.0f : 0.0f;
    case PhysicalInput::Kind::Axis: {
        if (!d.pad)
            return 0.0f;
        const float v = SDL_GetGamepadAxis(d.pad, (SDL_GamepadAxis)in.code) / 32767.0f * (float)in.sign;
        if (raw)
            return std::clamp(v, 0.0f, 1.0f);
        return v < DEAD_ZONE ? 0.0f : std::min(1.0f, (v - DEAD_ZONE) / (1.0f - DEAD_ZONE));
    }
    case PhysicalInput::Kind::None:
        break;
    }
    return 0.0f;
}

}  // namespace

const BindingInfo& control_info(Control c) { return CONTROLS[(size_t)c]; }
const BindingInfo& menu_action_info(MenuAction a) { return MENU_ACTIONS[(size_t)a]; }

std::string PhysicalInput::to_string() const {
    switch (kind) {
    case Kind::Key:
        return std::string("key:") + SDL_GetScancodeName((SDL_Scancode)code);
    case Kind::MouseButton:
        for (const auto& m : MOUSE_BUTTONS)
            if (m.button == code)
                return std::string("mouse:") + m.id;
        break;
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
    if (kind == "mouse") {
        for (const auto& m : MOUSE_BUTTONS)
            if (value == m.id)
                return mouse(m.button);
        return {};
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
    case Kind::MouseButton:
        for (const auto& m : MOUSE_BUTTONS)
            if (m.button == code)
                return m.label;
        return "Mouse Button " + std::to_string(code);
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
    using P = PhysicalInput;
    constexpr size_t GP = (size_t)InputDevice::Gamepad, KBM = (size_t)InputDevice::KeyboardMouse;
    const P none{};
    Bindings b;
    auto set = [&](Mappings& m, P pad1, P pad2, P kbm1, P kbm2) {
        m[GP] = {pad1, pad2};
        m[KBM] = {kbm1, kbm2};
    };
    auto control = [&](Control c) -> Mappings& { return b.controls[(size_t)c]; };
    set(control(Control::A), P::button(SDL_GAMEPAD_BUTTON_SOUTH), none, P::key(SDL_SCANCODE_RETURN),
        P::key(SDL_SCANCODE_SPACE));
    set(control(Control::B), P::button(SDL_GAMEPAD_BUTTON_EAST), none, P::key(SDL_SCANCODE_BACKSPACE),
        P::mouse(SDL_BUTTON_LEFT));
    set(control(Control::One), P::button(SDL_GAMEPAD_BUTTON_WEST), none, P::key(SDL_SCANCODE_1), none);
    set(control(Control::Two), P::button(SDL_GAMEPAD_BUTTON_NORTH), none, P::key(SDL_SCANCODE_2), none);
    set(control(Control::Plus), P::button(SDL_GAMEPAD_BUTTON_START), none, P::key(SDL_SCANCODE_EQUALS), none);
    set(control(Control::Minus), P::button(SDL_GAMEPAD_BUTTON_BACK), none, P::key(SDL_SCANCODE_MINUS), none);
    set(control(Control::DpadUp), P::button(SDL_GAMEPAD_BUTTON_DPAD_UP), none, P::key(SDL_SCANCODE_UP), none);
    set(control(Control::DpadDown), P::button(SDL_GAMEPAD_BUTTON_DPAD_DOWN), none, P::key(SDL_SCANCODE_DOWN), none);
    set(control(Control::DpadLeft), P::button(SDL_GAMEPAD_BUTTON_DPAD_LEFT), none, P::key(SDL_SCANCODE_LEFT), none);
    set(control(Control::DpadRight), P::button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT), none, P::key(SDL_SCANCODE_RIGHT),
        none);
    set(control(Control::Z), P::axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1), none, P::key(SDL_SCANCODE_Z), none);
    set(control(Control::C), P::button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER), none, P::key(SDL_SCANCODE_C), none);
    set(control(Control::StickUp), P::axis(SDL_GAMEPAD_AXIS_LEFTY, -1), none, P::key(SDL_SCANCODE_W), none);
    set(control(Control::StickDown), P::axis(SDL_GAMEPAD_AXIS_LEFTY, 1), none, P::key(SDL_SCANCODE_S), none);
    set(control(Control::StickLeft), P::axis(SDL_GAMEPAD_AXIS_LEFTX, -1), none, P::key(SDL_SCANCODE_A), none);
    set(control(Control::StickRight), P::axis(SDL_GAMEPAD_AXIS_LEFTX, 1), none, P::key(SDL_SCANCODE_D), none);
    set(control(Control::PointerUp), P::axis(SDL_GAMEPAD_AXIS_RIGHTY, -1), none, P::key(SDL_SCANCODE_I), none);
    set(control(Control::PointerDown), P::axis(SDL_GAMEPAD_AXIS_RIGHTY, 1), none, P::key(SDL_SCANCODE_K), none);
    set(control(Control::PointerLeft), P::axis(SDL_GAMEPAD_AXIS_RIGHTX, -1), none, P::key(SDL_SCANCODE_J), none);
    set(control(Control::PointerRight), P::axis(SDL_GAMEPAD_AXIS_RIGHTX, 1), none, P::key(SDL_SCANCODE_L), none);

    auto action = [&](MenuAction a) -> Mappings& { return b.menu[(size_t)a]; };
    set(action(MenuAction::Toggle), P::button(SDL_GAMEPAD_BUTTON_GUIDE), none, P::key(SDL_SCANCODE_ESCAPE), none);
    set(action(MenuAction::Accept), P::button(SDL_GAMEPAD_BUTTON_SOUTH), none, P::key(SDL_SCANCODE_RETURN),
        P::key(SDL_SCANCODE_SPACE));
    set(action(MenuAction::Cancel), P::button(SDL_GAMEPAD_BUTTON_EAST), none, P::key(SDL_SCANCODE_BACKSPACE), none);
    set(action(MenuAction::Up), P::button(SDL_GAMEPAD_BUTTON_DPAD_UP), P::axis(SDL_GAMEPAD_AXIS_LEFTY, -1),
        P::key(SDL_SCANCODE_UP), P::key(SDL_SCANCODE_W));
    set(action(MenuAction::Down), P::button(SDL_GAMEPAD_BUTTON_DPAD_DOWN), P::axis(SDL_GAMEPAD_AXIS_LEFTY, 1),
        P::key(SDL_SCANCODE_DOWN), P::key(SDL_SCANCODE_S));
    set(action(MenuAction::Left), P::button(SDL_GAMEPAD_BUTTON_DPAD_LEFT), P::axis(SDL_GAMEPAD_AXIS_LEFTX, -1),
        P::key(SDL_SCANCODE_LEFT), P::key(SDL_SCANCODE_A));
    set(action(MenuAction::Right), P::button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT), P::axis(SDL_GAMEPAD_AXIS_LEFTX, 1),
        P::key(SDL_SCANCODE_RIGHT), P::key(SDL_SCANCODE_D));
    set(action(MenuAction::PrevTab), P::button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER), none, P::key(SDL_SCANCODE_PAGEUP),
        P::key(SDL_SCANCODE_Q));
    set(action(MenuAction::NextTab), P::button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER), none,
        P::key(SDL_SCANCODE_PAGEDOWN), P::key(SDL_SCANCODE_E));
    return b;
}

void Input::poll_wiimote() {
    if (scanner_.take_found())
        wiimote_.rescan_soon();
    wiimote_.poll();
    scanner_.set_connected(wiimote_.state().connected);
    if (SDL_GetTicks() > wiimote_rumble_until_)
        wiimote_.set_rumble(false);  // the game stopped updating (paused, menu open)
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
    for (const auto& b : SCRIPT_STICK)
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
        const DeviceState d{SDL_GetKeyboardState(nullptr), SDL_GetMouseState(nullptr, nullptr), first_gamepad()};
        // Alt+Enter toggles fullscreen; don't let it also press a control bound to Enter.
        const bool alt = SDL_GetModState() & SDL_KMOD_ALT;
        std::array<float, kControlCount> v{}, raw{};
        for (size_t c = 0; c < kControlCount; c++)
            for (const auto& device : bindings.controls[c])
                for (const PhysicalInput& in : device) {
                    if (alt && in.kind == PhysicalInput::Kind::Key &&
                        (in.code == SDL_SCANCODE_RETURN || in.code == SDL_SCANCODE_KP_ENTER))
                        continue;
                    v[c] = std::max(v[c], input_value(in, d));
                    raw[c] = std::max(raw[c], input_value(in, d, true));
                }
        for (size_t c = 0; c < kControlCount; c++)
            if (BUTTON_BITS[c] && v[c] >= 0.5f)
                s.buttons |= BUTTON_BITS[c];
        auto val = [&](Control c) { return raw[(size_t)c]; };
        std::tie(s.stick_x, s.stick_y) = shape_stick(val(Control::StickRight) - val(Control::StickLeft),
                                                     val(Control::StickUp) - val(Control::StickDown),
                                                     free_stick_ ? FREE_STICK_RADIUS : 1.0f);
        std::tie(px, py) = shape_stick(val(Control::PointerRight) - val(Control::PointerLeft),
                                       val(Control::PointerDown) - val(Control::PointerUp), 1.0f);
        if (px != 0 || py != 0)
            pointer_input_at_ = SDL_GetTicks();
    }
    // The pointer (in screen units, -1..1 across the window) moves at a rate set by its controls and
    // stays where it was left; moving the mouse over the window puts it under the mouse instead.
    pointer_x_ = std::clamp(pointer_x_ + px * 0.05f, -1.0f, 1.0f);
    pointer_y_ = std::clamp(pointer_y_ + py * 0.05f, -1.0f, 1.0f);
    if (use_devices && mouse_pointer_) {
        float mx = 0, my = 0;
        SDL_GetMouseState(&mx, &my);
        int w = 0, h = 0;
        if (SDL_Window* window = SDL_GetMouseFocus(); window && SDL_GetWindowSize(window, &w, &h) && w > 0 && h > 0 &&
                                                      (mx != mouse_x_ || my != mouse_y_)) {
            pointer_x_ = std::clamp(mx / (float)w * 2.0f - 1.0f, -1.0f, 1.0f);
            pointer_y_ = std::clamp(my / (float)h * 2.0f - 1.0f, -1.0f, 1.0f);
            pointer_input_at_ = SDL_GetTicks();
        }
        mouse_x_ = mx;
        mouse_y_ = my;
    }
    // A real Wii Remote: its buttons (HOME opens the settings menu), Nunchuk, motion and pointer.
    const Wiimote::State& wm = wiimote_.state();
    if (use_devices && wm.connected) {
        static const bool home_menu = std::getenv("PANDORA_HOME_MENU") != nullptr;  // HOME to the game instead
        s.buttons |= wm.buttons & (home_menu ? ~0u : ~WPAD_HOME);
        if (wm.nunchuk && (wm.stick_x != 0 || wm.stick_y != 0)) {
            s.stick_x = wm.stick_x;
            s.stick_y = wm.stick_y;
            if (const float mag = std::hypot(s.stick_x, s.stick_y); free_stick_ && mag > FREE_STICK_RADIUS) {
                s.stick_x *= FREE_STICK_RADIUS / mag;
                s.stick_y *= FREE_STICK_RADIUS / mag;
            }
        }
        s.motion = true;
        std::copy(std::begin(wm.acc), std::end(wm.acc), s.acc);
        std::copy(std::begin(wm.nunchuk_acc), std::end(wm.nunchuk_acc), s.nunchuk_acc);
        if (wm.pointer_valid) {
            // KPAD units already (a Wii reports past +-1 at the screen's sides).
            pointer_x_ = std::clamp(wm.pointer_x / POINTER_RANGE_X, -1.0f, 1.0f);
            pointer_y_ = std::clamp(wm.pointer_y / POINTER_RANGE_Y, -1.0f, 1.0f);
            s.horizon_x = wm.horizon_x;
            s.horizon_y = wm.horizon_y;
            s.dist = wm.dist;
        } else {
            // Not pointing at the screen: the cursor goes away, unless the mouse or bound inputs
            // moved it recently.
            s.pointer_valid = SDL_GetTicks() - pointer_input_at_ < 2000;
        }
    }
    s.pointer_x = pointer_x_ * POINTER_RANGE_X;
    s.pointer_y = pointer_y_ * POINTER_RANGE_Y;
    if (use_devices && wm.connected && wm.pointer_valid) {
        s.pointer_x = wm.pointer_x;
        s.pointer_y = wm.pointer_y;
    }

    for (const auto& p : script_) {
        if (frame < p.start || frame >= p.start + p.frames)
            continue;
        s.buttons |= p.buttons & ~(SCRIPT_STICK_UP | SCRIPT_STICK_DOWN | SCRIPT_STICK_LEFT | SCRIPT_STICK_RIGHT);
        if (p.buttons & SCRIPT_STICK_UP)
            s.stick_y = 1;
        if (p.buttons & SCRIPT_STICK_DOWN)
            s.stick_y = -1;
        if (p.buttons & SCRIPT_STICK_LEFT)
            s.stick_x = -1;
        if (p.buttons & SCRIPT_STICK_RIGHT)
            s.stick_x = 1;
    }
    state_ = s;

    // Rumble: refreshed every guest frame with a short duration, so it stops by itself when the
    // game pauses or stops calling in.
    SDL_Gamepad* pad = use_devices ? first_gamepad() : nullptr;
    const bool want = pad && rumble_on_ && rumble_intensity_ > 0.0f;
    if (want) {
        const auto strength = (Uint16)std::lround(std::clamp(rumble_intensity_, 0.0f, 1.0f) * 0xFFFF);
        SDL_RumbleGamepad(pad, strength, strength, 200);
    } else if (rumbling_) {
        if (SDL_Gamepad* p = first_gamepad())
            SDL_RumbleGamepad(p, 0, 0, 0);
    }
    rumbling_ = want;
    if (use_devices && rumble_on_ && rumble_intensity_ > 0.0f && wm.connected) {
        wiimote_.set_rumble(true);  // on/off only
        wiimote_rumble_until_ = SDL_GetTicks() + 200;
    } else {
        wiimote_.set_rumble(false);
    }
}
