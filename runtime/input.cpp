#include "input.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {

struct ButtonName {
    const char* name;
    u32 bit;
};
constexpr ButtonName BUTTONS[] = {
    {"A", WPAD_A},       {"B", WPAD_B},         {"1", WPAD_1},       {"2", WPAD_2},
    {"PLUS", WPAD_PLUS}, {"MINUS", WPAD_MINUS}, {"HOME", WPAD_HOME}, {"UP", WPAD_UP},
    {"DOWN", WPAD_DOWN}, {"LEFT", WPAD_LEFT},   {"RIGHT", WPAD_RIGHT}, {"Z", WPAD_Z},
    {"C", WPAD_C},
};

float axis(SDL_Gamepad* pad, SDL_GamepadAxis a) {
    const float v = SDL_GetGamepadAxis(pad, a) / 32767.0f;
    return std::fabs(v) < 0.2f ? 0.0f : std::clamp(v, -1.0f, 1.0f);  // dead zone
}

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

}  // namespace

bool Input::add_script(const std::string& spec) {
    const size_t at = spec.find('@');
    if (at == std::string::npos)
        return false;
    const std::string name = spec.substr(0, at);
    u32 bit = 0;
    for (const auto& b : BUTTONS)
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
    PadState s;
    float px = 0, py = 0;
    if (use_devices) {
        // Keyboard: Enter/Space = A, Backspace = B, arrows = D-pad, WASD = stick, 1/2, +/- (=/-),
        // H = Home, Z/C, IJKL = pointer.
        const bool* k = SDL_GetKeyboardState(nullptr);
        auto key = [&](SDL_Scancode sc, u32 bit) {
            if (k[sc])
                s.buttons |= bit;
        };
        key(SDL_SCANCODE_RETURN, WPAD_A);
        key(SDL_SCANCODE_SPACE, WPAD_A);
        key(SDL_SCANCODE_BACKSPACE, WPAD_B);
        key(SDL_SCANCODE_UP, WPAD_UP);
        key(SDL_SCANCODE_DOWN, WPAD_DOWN);
        key(SDL_SCANCODE_LEFT, WPAD_LEFT);
        key(SDL_SCANCODE_RIGHT, WPAD_RIGHT);
        key(SDL_SCANCODE_1, WPAD_1);
        key(SDL_SCANCODE_2, WPAD_2);
        key(SDL_SCANCODE_EQUALS, WPAD_PLUS);
        key(SDL_SCANCODE_MINUS, WPAD_MINUS);
        key(SDL_SCANCODE_H, WPAD_HOME);
        key(SDL_SCANCODE_Z, WPAD_Z);
        key(SDL_SCANCODE_C, WPAD_C);
        s.stick_x = (k[SDL_SCANCODE_D] ? 1.0f : 0.0f) - (k[SDL_SCANCODE_A] ? 1.0f : 0.0f);
        s.stick_y = (k[SDL_SCANCODE_W] ? 1.0f : 0.0f) - (k[SDL_SCANCODE_S] ? 1.0f : 0.0f);
        px = (k[SDL_SCANCODE_L] ? 1.0f : 0.0f) - (k[SDL_SCANCODE_J] ? 1.0f : 0.0f);
        py = (k[SDL_SCANCODE_K] ? 1.0f : 0.0f) - (k[SDL_SCANCODE_I] ? 1.0f : 0.0f);

        // Gamepad: south = A, east = B, west = 1, north = 2, start/back = +/-, guide = Home,
        // LB = C, LT = Z, RT/RB = B, left stick = Nunchuk, right stick = pointer.
        if (SDL_Gamepad* pad = first_gamepad()) {
            auto btn = [&](SDL_GamepadButton b, u32 bit) {
                if (SDL_GetGamepadButton(pad, b))
                    s.buttons |= bit;
            };
            btn(SDL_GAMEPAD_BUTTON_SOUTH, WPAD_A);
            btn(SDL_GAMEPAD_BUTTON_EAST, WPAD_B);
            btn(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, WPAD_B);
            btn(SDL_GAMEPAD_BUTTON_WEST, WPAD_1);
            btn(SDL_GAMEPAD_BUTTON_NORTH, WPAD_2);
            btn(SDL_GAMEPAD_BUTTON_START, WPAD_PLUS);
            btn(SDL_GAMEPAD_BUTTON_BACK, WPAD_MINUS);
            btn(SDL_GAMEPAD_BUTTON_GUIDE, WPAD_HOME);
            btn(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, WPAD_C);
            btn(SDL_GAMEPAD_BUTTON_DPAD_UP, WPAD_UP);
            btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN, WPAD_DOWN);
            btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT, WPAD_LEFT);
            btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, WPAD_RIGHT);
            if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000)
                s.buttons |= WPAD_Z;
            if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000)
                s.buttons |= WPAD_B;
            const float lx = axis(pad, SDL_GAMEPAD_AXIS_LEFTX), ly = axis(pad, SDL_GAMEPAD_AXIS_LEFTY);
            if (lx || ly) {
                s.stick_x = lx;
                s.stick_y = -ly;
            }
            px += axis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
            py += axis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
        }
    }
    // The pointer moves with the right stick (rate control) and stays where it was left.
    pointer_x_ = std::clamp(pointer_x_ + px * 0.05f, -1.0f, 1.0f);
    pointer_y_ = std::clamp(pointer_y_ + py * 0.05f, -1.0f, 1.0f);
    s.pointer_x = pointer_x_;
    s.pointer_y = pointer_y_;

    for (const auto& p : script_)
        if (frame >= p.start && frame < p.start + p.frames)
            s.buttons |= p.buttons;
    state_ = s;
}
