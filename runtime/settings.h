// User settings that persist between runs (a key=value text file).
#pragma once

#include <string>

#include "input.h"

struct Settings {
    bool fullscreen = false;
    int volume = 100;  // percent
    bool muted = false;
    int rumble = 100;          // percent; 0 disables rumble
    bool mouse_pointer = true;  // the mouse aims the Wii Remote pointer
    Bindings bindings = Bindings::defaults();

    // Missing or unreadable files leave the defaults; unknown keys are ignored.
    void load(const std::string& path);
    bool save(const std::string& path) const;
};
