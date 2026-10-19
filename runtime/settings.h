// User settings that persist between runs (a key=value text file).
#pragma once

#include <string>

#include "input.h"

struct Settings {
    bool fullscreen = false;
    int volume = 100;  // percent
    bool muted = false;
    Bindings bindings = Bindings::defaults();

    // Missing or unreadable files leave the defaults; unknown keys are ignored.
    void load(const std::string& path);
    bool save(const std::string& path) const;
};
