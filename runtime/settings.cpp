#include "settings.h"

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

constexpr const char* SLOT_NAMES[2] = {"controller", "keyboard"};

}  // namespace

void Settings::load(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos)
            continue;
        const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (key == "fullscreen") {
            fullscreen = parse_bool(value);
        } else if (key == "volume") {
            volume = std::clamp(std::atoi(value.c_str()), 0, 100);
        } else if (key == "muted") {
            muted = parse_bool(value);
        } else if (key.rfind("bind.", 0) == 0) {
            // bind.<control>.<controller|keyboard> = <input>, empty for unbound
            const size_t dot = key.rfind('.');
            const std::string control = key.substr(5, dot - 5), slot = key.substr(dot + 1);
            for (size_t c = 0; c < kControlCount; c++)
                for (size_t s = 0; s < 2; s++)
                    if (control == control_info((Control)c).id && slot == SLOT_NAMES[s])
                        bindings.map[c][s] = PhysicalInput::parse(value);
        }
    }
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
    for (size_t c = 0; c < kControlCount; c++)
        for (size_t s = 0; s < 2; s++)
            out << "bind." << control_info((Control)c).id << "." << SLOT_NAMES[s] << "="
                << bindings.map[c][s].to_string() << "\n";
    return true;
}
