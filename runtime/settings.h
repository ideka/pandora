// User settings that persist between runs (a key=value text file).
#pragma once

#include <string>

struct Settings {
    bool fullscreen = false;

    // Missing or unreadable files leave the defaults; unknown keys are ignored.
    void load(const std::string& path);
    bool save(const std::string& path) const;
};
