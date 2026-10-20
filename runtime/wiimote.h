// A real Wii Remote (with Nunchuk), connected over Bluetooth (or a DolphinBar in mode 4), driven
// directly through SDL's HID API: buttons, accelerometers, IR camera (pointer), Nunchuk, rumble and
// the player LED. Its buttons map straight to the emulated ones (no rebinding).
#pragma once

#include <chrono>
#include <string>

#include "guest.h"

struct SDL_hid_device;

class Wiimote {
public:
    ~Wiimote() { disconnect(); }

    // Reads pending reports; looks for a newly paired remote every few seconds. Call every host frame.
    void poll();

    struct State {
        bool connected = false;
        u32 buttons = 0;              // WPAD_* bits (same layout as the remote's core buttons)
        float acc[3] = {0, -1, 0};    // KPAD axes, in g
        bool nunchuk = false;
        float stick_x = 0, stick_y = 0;  // -1..1, y up
        float nunchuk_acc[3] = {0, -1, 0};
        bool pointer_valid = false;   // the sensor bar is in view
        float pointer_x = 0, pointer_y = 0;  // screen units, -1..1 (y down)
        int battery = 0;              // 0..255
    };
    const State& state() const { return state_; }

    void set_rumble(bool on);
    void set_sensor_bar_above(bool above) { bar_above_ = above; }

private:
    using Clock = std::chrono::steady_clock;

    bool connect(const char* path);
    void disconnect();
    bool send(const u8* data, size_t len);
    bool write_register(u32 addr, const u8* data, u8 size);
    bool read_memory(u32 addr, u16 size, u8* out);
    void setup();
    void setup_extension();
    void set_reporting();
    // Waits up to timeout_ms for a report with the given id, handling the others meanwhile.
    bool wait_for(u8 id, u8* out, int timeout_ms);
    void handle(const u8* r, int len);
    void parse_core(const u8* buttons, const u8* accel);
    void parse_ir(const u8* ir);
    void parse_nunchuk(const u8* ext);

    SDL_hid_device* dev_ = nullptr;
    void* win_handle_ = nullptr;  // Windows fallback for output reports (some Bluetooth stacks)
    std::string path_;
    Clock::time_point next_scan_{};
    unsigned device_changes_ = 0;  // SDL_hid_device_change_count at the last scan
    bool scanned_once_ = false;
    State state_;
    bool rumble_ = false;
    bool bar_above_ = false;
    bool ext_connected_ = false;
    Clock::time_point next_ext_retry_{};
    // Accelerometer calibration (10-bit units): zero g and one g per axis.
    float acc_zero_[3] = {512, 512, 512}, acc_one_[3] = {616, 616, 616};
    float nunchuk_zero_[3] = {512, 512, 512}, nunchuk_one_[3] = {716, 716, 716};
};
