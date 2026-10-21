#include "settings.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>

#include "log.h"

namespace {

std::string trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r"), e = s.find_last_not_of(" \t\r");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

bool parse_bool(const std::string& v) { return v == "1" || v == "true" || v == "yes" || v == "on"; }

constexpr const char* DEVICE_NAMES[kDeviceCount] = {"gamepad", "kbm"};

// "<prefix>.<id>.<device>.<1|2>" -> the mapping it names, or null.
PhysicalInput* find_mapping(Bindings& b, const std::string& key) {
    const size_t d1 = key.find('.'), d3 = key.rfind('.'), d2 = d3 == std::string::npos ? d3 : key.rfind('.', d3 - 1);
    if (d1 == std::string::npos || d2 == std::string::npos || d2 <= d1)
        return nullptr;
    const std::string prefix = key.substr(0, d1), id = key.substr(d1 + 1, d2 - d1 - 1),
                      device = key.substr(d2 + 1, d3 - d2 - 1), slot = key.substr(d3 + 1);
    size_t dev = kDeviceCount;
    for (size_t i = 0; i < kDeviceCount; i++)
        if (device == DEVICE_NAMES[i])
            dev = i;
    const size_t s = slot == "1" ? 0 : slot == "2" ? 1 : kMappingsPerDevice;
    if (dev == kDeviceCount || s == kMappingsPerDevice)
        return nullptr;
    if (prefix == "bind") {
        for (size_t c = 0; c < kControlCount; c++)
            if (id == control_info((Control)c).id)
                return &b.controls[c][dev][s];
    } else if (prefix == "menu") {
        for (size_t a = 0; a < kMenuActionCount; a++)
            if (id == menu_action_info((MenuAction)a).id)
                return &b.menu[a][dev][s];
    }
    return nullptr;
}

// Version 2 moved some gamepad defaults: the Classic Controller's buttons by position (A east, B
// south, X north, Y west) with ZL/ZR on the triggers and L/R on the shoulders, and the settings
// menu's Accept/Cancel to east/south. A file from before keeps its mappings, except those still
// at the old default, which move to the new one.
constexpr int BINDINGS_VERSION = 2;

void migrate_v1_defaults(Bindings& b) {
    using P = PhysicalInput;
    const Bindings now = Bindings::defaults();
    constexpr size_t PAD = 0;  // the gamepad's first mapping
    const std::pair<Control, P> controls[] = {
        {Control::ClA, P::button(SDL_GAMEPAD_BUTTON_SOUTH)},
        {Control::ClB, P::button(SDL_GAMEPAD_BUTTON_EAST)},
        {Control::ClX, P::button(SDL_GAMEPAD_BUTTON_WEST)},
        {Control::ClY, P::button(SDL_GAMEPAD_BUTTON_NORTH)},
        {Control::ClL, P::axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1)},
        {Control::ClR, P::axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1)},
        {Control::ClZL, P::button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)},
        {Control::ClZR, P::button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)},
    };
    for (const auto& [c, old] : controls)
        if (b.controls[(size_t)c][PAD][0] == old)
            b.controls[(size_t)c][PAD][0] = now.controls[(size_t)c][PAD][0];
    const std::pair<MenuAction, P> menu[] = {
        {MenuAction::Accept, P::button(SDL_GAMEPAD_BUTTON_SOUTH)},
        {MenuAction::Cancel, P::button(SDL_GAMEPAD_BUTTON_EAST)},
    };
    for (const auto& [a, old] : menu)
        if (b.menu[(size_t)a][PAD][0] == old)
            b.menu[(size_t)a][PAD][0] = now.menu[(size_t)a][PAD][0];
}

}  // namespace

void Settings::load(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    int bindings_version = 1;
    bool any_binding = false;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos)
            continue;
        std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        if (key == "fullscreen") {
            fullscreen = parse_bool(value);
        } else if (key == "volume") {
            volume = std::clamp(std::atoi(value.c_str()), 0, 100);
        } else if (key == "muted") {
            muted = parse_bool(value);
        } else if (key == "rumble") {
            rumble = std::clamp(std::atoi(value.c_str()), 0, 100);
        } else if (key == "mouse_pointer") {
            mouse_pointer = parse_bool(value);
        } else if (key == "sensor_bar_above") {
            sensor_bar_above = parse_bool(value);
        } else if (key == "ir_sensitivity") {
            ir_sensitivity = std::clamp(std::atoi(value.c_str()), 1, 5);
        } else if (key == "wiimote_scan") {
            wiimote_scan = parse_bool(value);
        } else if (key == "fps60") {
            fps60 = parse_bool(value);
        } else if (key == "free_stick") {
            free_stick = parse_bool(value);
        } else if (key == "classic_controller") {
            classic_controller = parse_bool(value);
        } else if (key == "anisotropy") {
            const int a = std::atoi(value.c_str());
            anisotropy = a >= 16 ? 16 : a >= 8 ? 8 : a >= 4 ? 4 : a >= 2 ? 2 : 1;
        } else if (key == "msaa") {
            msaa = std::atoi(value.c_str()) >= 4 ? 4 : 1;
        } else if (key == "bindings_version") {
            bindings_version = std::atoi(value.c_str());
        } else {
            // Files from before the two-mapping layout: bind.<id>.controller / .keyboard.
            const size_t dot = key.rfind('.');
            if (key.rfind("bind.", 0) == 0 && dot != std::string::npos) {
                const std::string old = key.substr(dot + 1);
                if (old == "controller")
                    key = key.substr(0, dot) + ".gamepad.1";
                else if (old == "keyboard")
                    key = key.substr(0, dot) + ".kbm.1";
            }
            if (PhysicalInput* m = find_mapping(bindings, key)) {
                *m = PhysicalInput::parse(value);
                any_binding = true;
            }
        }
    }
    if (any_binding && bindings_version < 2)
        migrate_v1_defaults(bindings);
}

bool Settings::save(const std::string& path) const {
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        LOG_ERROR("settings", "cannot write %s", path.c_str());
        return false;
    }
    out << "fullscreen=" << (fullscreen ? 1 : 0) << "\n";
    out << "volume=" << volume << "\n";
    out << "muted=" << (muted ? 1 : 0) << "\n";
    out << "rumble=" << rumble << "\n";
    out << "mouse_pointer=" << (mouse_pointer ? 1 : 0) << "\n";
    out << "sensor_bar_above=" << (sensor_bar_above ? 1 : 0) << "\n";
    out << "ir_sensitivity=" << ir_sensitivity << "\n";
    out << "wiimote_scan=" << (wiimote_scan ? 1 : 0) << "\n";
    out << "fps60=" << (fps60 ? 1 : 0) << "\n";
    out << "free_stick=" << (free_stick ? 1 : 0) << "\n";
    out << "classic_controller=" << (classic_controller ? 1 : 0) << "\n";
    out << "anisotropy=" << anisotropy << "\n";
    out << "msaa=" << msaa << "\n";
    out << "bindings_version=" << BINDINGS_VERSION << "\n";
    auto write = [&](const char* prefix, const char* id, const Mappings& m) {
        for (size_t d = 0; d < kDeviceCount; d++)
            for (size_t s = 0; s < kMappingsPerDevice; s++)
                out << prefix << "." << id << "." << DEVICE_NAMES[d] << "." << s + 1 << "=" << m[d][s].to_string()
                    << "\n";
    };
    for (size_t c = 0; c < kControlCount; c++)
        write("bind", control_info((Control)c).id, bindings.controls[c]);
    for (size_t a = 0; a < kMenuActionCount; a++)
        write("menu", menu_action_info((MenuAction)a).id, bindings.menu[a]);
    return true;
}
