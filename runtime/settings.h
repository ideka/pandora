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
    // Real Wii Remote: the sensor bar's offset from the screen's centre in hundredths of KPAD image
    // units, y down: the Wii's "Sensor Bar Position" is -20 (above) or 20 (below).
    int sensor_bar_offset = 20;
    int ir_sensitivity = 3;         // real Wii Remote: the Wii's IR sensitivity setting (1-5)
    int ir_merge = 0;               // real Wii Remote: IR dots closer than this (camera pixels) merge; 0 = off
    bool wiimote_scan = true;       // scan for Wii Remotes over Bluetooth while none is connected
    bool fps60 = true;              // the game's 60 FPS mode (community code)
    bool free_stick = true;         // stick in any direction (no snapping to the four cardinals)
    bool classic_controller = false;  // a Classic Controller instead of the Nunchuk
    int anisotropy = 16;            // anisotropic filtering forced on the game's textures: 1 (off), 2, 4, 8, 16
    int msaa = 1;                   // anti-aliasing samples: 1 (off) or 4 (applies at startup)
    Bindings bindings = Bindings::defaults();

    // Missing or unreadable files leave the defaults; unknown keys are ignored.
    void load(const std::string& path);
    bool save(const std::string& path) const;
};
