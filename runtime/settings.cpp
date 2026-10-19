#include "settings.h"

#include <fstream>

#include "log.h"

namespace {

std::string trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r"), e = s.find_last_not_of(" \t\r");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

bool parse_bool(const std::string& v) { return v == "1" || v == "true" || v == "yes" || v == "on"; }

}  // namespace

void Settings::load(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos)
            continue;
        const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (key == "fullscreen")
            fullscreen = parse_bool(value);
    }
}

bool Settings::save(const std::string& path) const {
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        LOG_ERROR("settings", "cannot write %s", path.c_str());
        return false;
    }
    out << "fullscreen=" << (fullscreen ? 1 : 0) << "\n";
    return true;
}
