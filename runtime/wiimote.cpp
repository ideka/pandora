#include "wiimote.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// windows.h first
#include <hidsdi.h>
#endif

#include "log.h"

// Wii Remote HID protocol (wiibrew.org/wiki/Wiimote). Every output report's first payload byte
// carries the rumble bit; reports are sent with their id as the first byte.
namespace {

constexpr unsigned short NINTENDO = 0x057E;
constexpr unsigned short RVL_CNT_01 = 0x0306, RVL_CNT_01_TR = 0x0330;
constexpr auto SCAN_INTERVAL = std::chrono::seconds(3);
constexpr size_t REPORT_SIZE = 22;  // output report length over Bluetooth

constexpr u8 OUT_LEDS = 0x11, OUT_REPORTING = 0x12, OUT_IR_PIXEL_CLOCK = 0x13, OUT_STATUS = 0x15,
             OUT_WRITE = 0x16, OUT_READ = 0x17, OUT_IR_LOGIC = 0x1A;
constexpr u8 IN_STATUS = 0x20, IN_READ_DATA = 0x21, IN_ACK = 0x22, IN_CORE_ACC_IR10_EXT6 = 0x37;

constexpr u32 IR_REGISTERS = 0xB00000, EXT_REGISTERS = 0xA40000;

// Camera image: 1024 x 768. The pointer covers the screen when the bar's midpoint moves this far
// (as a fraction of the half-image) from where it sits when pointing at the screen's centre:
// roughly a 1.2 m wide 16:9 screen seen from 2.5 m, through a camera with a ~33 x 23 degree view.
constexpr float IR_SCALE_X = 1.22f, IR_SCALE_Y = 1.5f;

float axis(float raw, float zero, float one) { return one != zero ? (raw - zero) / (one - zero) : 0.0f; }

}  // namespace

void Wiimote::rescan_soon() {
    rescan_until_ = Clock::now() + std::chrono::seconds(10);
    next_scan_ = {};
}

void Wiimote::poll() {
    if (!dev_) {
        const auto now = Clock::now();
        if (now < next_scan_)
            return;
        next_scan_ = now + (now < rescan_until_ ? std::chrono::seconds(1) : SCAN_INTERVAL);
        static const bool hid_ready = SDL_hid_init() == 0;
        if (!hid_ready)
            return;
        // Enumerating costs a few milliseconds; only do it when the system's device list changed.
        const Uint32 changes = SDL_hid_device_change_count();
        if (changes == device_changes_ && scanned_once_ && now > rescan_until_)
            return;
        device_changes_ = changes;
        scanned_once_ = true;
        for (unsigned short product : {RVL_CNT_01, RVL_CNT_01_TR}) {
            SDL_hid_device_info* list = SDL_hid_enumerate(NINTENDO, product);
            for (SDL_hid_device_info* d = list; d && !dev_; d = d->next)
                connect(d->path);
            SDL_hid_free_enumeration(list);
            if (dev_)
                break;
        }
        return;
    }
    u8 buf[32];
    for (int i = 0; i < 64; i++) {
        const int n = SDL_hid_read_timeout(dev_, buf, sizeof buf, 0);
        if (n < 0) {
            LOG_INFO("wiimote", "disconnected");
            disconnect();
            return;
        }
        if (n == 0)
            break;
        handle(buf, n);
    }
    // An extension that could not be identified yet: try again now and then.
    if (ext_connected_ && !state_.nunchuk && Clock::now() > next_ext_retry_) {
        setup_extension();
        if (state_.nunchuk)
            LOG_INFO("wiimote", "Nunchuk connected");
        set_reporting();
    }
}

bool Wiimote::connect(const char* path) {
    dev_ = SDL_hid_open_path(path);
    if (!dev_)
        return false;
    path_ = path;
    SDL_hid_set_nonblocking(dev_, 0);
    state_ = {};
    state_.connected = true;
    ext_connected_ = false;
    setup();
    LOG_INFO("wiimote", "connected%s", state_.nunchuk ? " with Nunchuk" : "");
    return true;
}

void Wiimote::disconnect() {
    if (dev_)
        SDL_hid_close(dev_);
    dev_ = nullptr;
    scanned_once_ = false;  // look again (e.g. the remote reconnects)
#ifdef _WIN32
    if (win_handle_)
        CloseHandle(static_cast<HANDLE>(win_handle_));
#endif
    win_handle_ = nullptr;
    state_ = {};
}

// Some Windows Bluetooth stacks reject WriteFile on the Wii Remote (notably with the -TR model);
// HidD_SetOutputReport on our own handle works there, as in Dolphin.
bool Wiimote::send(const u8* data, size_t len) {
    u8 report[REPORT_SIZE] = {};
    std::memcpy(report, data, std::min(len, sizeof report));
    if (len > 1 && rumble_)
        report[1] |= 1;
    if (!win_handle_ && SDL_hid_write(dev_, report, len) >= 0)
        return true;
#ifdef _WIN32
    if (!win_handle_) {
        HANDLE h = CreateFileA(path_.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return false;
        win_handle_ = h;
    }
    return HidD_SetOutputReport(static_cast<HANDLE>(win_handle_), report, (ULONG)sizeof report);
#else
    return false;
#endif
}

bool Wiimote::wait_for(u8 id, u8* out, int timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    u8 buf[32];
    while (Clock::now() < deadline) {
        const int n = SDL_hid_read_timeout(dev_, buf, sizeof buf, 10);
        if (n < 0)
            return false;
        if (n == 0)
            continue;
        if (buf[0] == id) {
            if (out)
                std::memcpy(out, buf, sizeof buf);
            return true;
        }
        handle(buf, n);
    }
    return false;
}

bool Wiimote::write_register(u32 addr, const u8* data, u8 size) {
    u8 r[REPORT_SIZE] = {OUT_WRITE, 0x04, (u8)(addr >> 16), (u8)(addr >> 8), (u8)addr, size};
    std::memcpy(r + 6, data, std::min<size_t>(size, 16));
    return send(r, sizeof r) && wait_for(IN_ACK, nullptr, 200);
}

bool Wiimote::read_memory(u32 addr, u16 size, u8* out) {
    const bool reg = addr >= 0x400000;
    const u8 r[] = {OUT_READ, (u8)(reg ? 0x04 : 0x00), (u8)(addr >> 16), (u8)(addr >> 8), (u8)addr,
                    (u8)(size >> 8), (u8)size};
    if (!send(r, sizeof r))
        return false;
    for (u16 got = 0; got < size;) {
        u8 resp[32];
        if (!wait_for(IN_READ_DATA, resp, 300) || (resp[3] & 0x0F))
            return false;
        const int n = (resp[3] >> 4) + 1;
        std::memcpy(out + got, resp + 6, std::min<int>(n, size - got));
        got += (u16)n;
    }
    return true;
}

void Wiimote::setup() {
    const u8 leds[] = {OUT_LEDS, 0x10};  // player 1
    send(leds, sizeof leds);
    // Accelerometer calibration: zero-g x, y, z, low bits, one-g x, y, z, low bits.
    u8 cal[10];
    if (read_memory(0x16, sizeof cal, cal)) {
        for (int i = 0; i < 3; i++) {
            acc_zero_[i] = (float)(cal[i] << 2 | ((cal[3] >> (2 * (2 - i))) & 3));
            acc_one_[i] = (float)(cal[4 + i] << 2 | ((cal[7] >> (2 * (2 - i))) & 3));
        }
    }
    // IR camera: basic mode (10 bytes for 4 dots), Wii sensitivity level 3.
    u8 r[2] = {OUT_IR_PIXEL_CLOCK, 0x04};
    send(r, sizeof r);
    r[0] = OUT_IR_LOGIC;
    send(r, sizeof r);
    const u8 enable = 0x08, block1[] = {0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0x64, 0x00, 0xFE}, block2[] = {0xFD, 0x05},
             basic = 0x01;
    write_register(IR_REGISTERS + 0x30, &enable, 1);
    write_register(IR_REGISTERS + 0x00, block1, sizeof block1);
    write_register(IR_REGISTERS + 0x1A, block2, sizeof block2);
    write_register(IR_REGISTERS + 0x33, &basic, 1);
    write_register(IR_REGISTERS + 0x30, &enable, 1);
    // The status report says whether an extension is plugged in.
    const u8 status[] = {OUT_STATUS, 0x00};
    send(status, sizeof status);
    u8 resp[32];
    if (wait_for(IN_STATUS, resp, 300)) {
        state_.battery = resp[6];
        ext_connected_ = resp[3] & 0x02;
        if (ext_connected_)
            setup_extension();
    }
    set_reporting();
}

// Unencrypted extension access (0x55 to F0, 0x00 to FB), then its id: a Nunchuk ends in A4 20 00 00.
void Wiimote::setup_extension() {
    const u8 init1 = 0x55, init2 = 0x00;
    write_register(EXT_REGISTERS + 0xF0, &init1, 1);
    write_register(EXT_REGISTERS + 0xFB, &init2, 1);
    // The extension may still be busy right after initialisation (read error 7): retry briefly.
    u8 id[6] = {};
    bool read = false;
    for (int attempt = 0; attempt < 5 && !read; attempt++) {
        if (attempt)
            SDL_Delay(30);
        read = read_memory(EXT_REGISTERS + 0xFA, sizeof id, id);
    }
    LOG_DEBUG("wiimote", "extension id %s %02X%02X%02X%02X%02X%02X", read ? "" : "(read failed)", id[0], id[1],
              id[2], id[3], id[4], id[5]);
    state_.nunchuk = read && id[2] == 0xA4 && id[3] == 0x20 && id[4] == 0x00 && id[5] == 0x00;
    next_ext_retry_ = Clock::now() + std::chrono::seconds(1);
    u8 cal[8];
    if (state_.nunchuk && read_memory(EXT_REGISTERS + 0x20, sizeof cal, cal) && cal[0] != 0xFF && cal[0] != 0x00) {
        for (int i = 0; i < 3; i++) {
            nunchuk_zero_[i] = (float)(cal[i] << 2 | ((cal[3] >> (2 * (2 - i))) & 3));
            nunchuk_one_[i] = (float)(cal[4 + i] << 2 | ((cal[7] >> (2 * (2 - i))) & 3));
        }
    }
    if (read && !state_.nunchuk)
        LOG_INFO("wiimote", "extension %02X%02X%02X%02X%02X%02X is not a Nunchuk", id[0], id[1], id[2], id[3], id[4],
                 id[5]);
}

// Continuous core buttons + accelerometer + 10 IR bytes + 6 extension bytes.
void Wiimote::set_reporting() {
    const u8 r[] = {OUT_REPORTING, 0x04, IN_CORE_ACC_IR10_EXT6};
    send(r, sizeof r);
}

void Wiimote::set_rumble(bool on) {
    if (!dev_ || on == rumble_)
        return;
    rumble_ = on;
    const u8 r[] = {OUT_LEDS, 0x10};  // any output report carries the rumble bit
    send(r, sizeof r);
}

void Wiimote::handle(const u8* r, int len) {
    switch (r[0]) {
    case IN_STATUS:
        // Sent when an extension is plugged in or out; the remote then waits for a reporting mode.
        if (len >= 7) {
            state_.battery = r[6];
            const bool ext = r[3] & 0x02;
            if (ext != ext_connected_) {
                ext_connected_ = ext;
                state_.nunchuk = false;
                if (ext)
                    setup_extension();
                if (state_.nunchuk || !ext)
                    LOG_INFO("wiimote", "Nunchuk %s", ext ? "connected" : "disconnected");
            }
            set_reporting();
        }
        break;
    case IN_CORE_ACC_IR10_EXT6:
        if (len >= 22) {
            parse_core(r + 1, r + 3);
            parse_ir(r + 6);
            if (state_.nunchuk)
                parse_nunchuk(r + 16);
        }
        break;
    default:
        if (r[0] >= 0x30 && r[0] <= 0x3F && len >= 3)
            parse_core(r + 1, nullptr);  // before the reporting mode takes effect
        break;
    }
}

void Wiimote::parse_core(const u8* b, const u8* a) {
    state_.buttons = (u32)(b[0] & 0x1F) | (u32)(b[1] & 0x9F) << 8;
    if (!a)
        return;
    // 10-bit x from the high byte and two bits in the button bytes; y and z get one extra bit.
    const float x = (float)(a[0] << 2 | ((b[0] >> 5) & 3));
    const float y = (float)(a[1] << 2 | ((b[1] >> 4) & 2));
    const float z = (float)(a[2] << 2 | ((b[1] >> 5) & 2));
    const float ax = axis(x, acc_zero_[0], acc_one_[0]), ay = axis(y, acc_zero_[1], acc_one_[1]),
                az = axis(z, acc_zero_[2], acc_one_[2]);
    // Remote axes: x left, y towards the camera, z up through the buttons. KPAD: x right, y down
    // (gravity reads -1 lying flat, buttons up), z towards the camera.
    state_.acc[0] = -ax;
    state_.acc[1] = -az;
    state_.acc[2] = ay;
}

// Basic IR: two dots per 5 bytes (x and y low bytes, then the high bits of both), 0x3FF = none.
void Wiimote::parse_ir(const u8* ir) {
    float dx[4], dy[4];
    int dots = 0;
    for (int pair = 0; pair < 2; pair++) {
        const u8* p = ir + pair * 5;
        const int x1 = p[0] | ((p[2] >> 4) & 3) << 8, y1 = p[1] | ((p[2] >> 6) & 3) << 8;
        const int x2 = p[3] | (p[2] & 3) << 8, y2 = p[4] | ((p[2] >> 2) & 3) << 8;
        if (y1 != 0x3FF && x1 != 0x3FF) {
            dx[dots] = (float)x1;
            dy[dots++] = (float)y1;
        }
        if (y2 != 0x3FF && x2 != 0x3FF) {
            dx[dots] = (float)x2;
            dy[dots++] = (float)y2;
        }
    }
    if (dots < 2) {
        state_.pointer_valid = false;  // the game hides its cursor, as with a real Wii
        return;
    }
    // The sensor bar's two ends: the first two dots, ordered left to right in the image.
    float x1 = dx[0], y1 = dy[0], x2 = dx[1], y2 = dy[1];
    if (x1 > x2) {
        std::swap(x1, x2);
        std::swap(y1, y2);
    }
    // Undo the remote's roll, then centre on the image (the camera sees the scene mirrored).
    const float angle = std::atan2(y2 - y1, x2 - x1);
    const float mx = (x1 + x2) / 2 - 512, my = (y1 + y2) / 2 - 384;
    const float c = std::cos(-angle), s = std::sin(-angle);
    const float rx = mx * c - my * s, ry = mx * s + my * c;
    // Pointing at the bar puts the cursor on the screen edge next to it.
    const float edge = bar_above_ ? -1.0f : 1.0f;
    state_.pointer_x = std::clamp(-rx / 512.0f * IR_SCALE_X, -1.0f, 1.0f);
    state_.pointer_y = std::clamp(edge - ry / 384.0f * IR_SCALE_Y, -1.0f, 1.0f);
    state_.pointer_valid = true;
}

// Nunchuk (unencrypted): stick x, y, then accelerometer x, y, z high bytes, then Z and C (active
// low) and the accelerometer's low bits.
void Wiimote::parse_nunchuk(const u8* e) {
    state_.stick_x = std::clamp(((float)e[0] - 128.0f) / 100.0f, -1.0f, 1.0f);
    state_.stick_y = std::clamp(((float)e[1] - 128.0f) / 100.0f, -1.0f, 1.0f);
    if (std::fabs(state_.stick_x) < 0.08f)
        state_.stick_x = 0;
    if (std::fabs(state_.stick_y) < 0.08f)
        state_.stick_y = 0;
    const float x = (float)(e[2] << 2 | ((e[5] >> 2) & 3));
    const float y = (float)(e[3] << 2 | ((e[5] >> 4) & 3));
    const float z = (float)(e[4] << 2 | ((e[5] >> 6) & 3));
    const float ax = axis(x, nunchuk_zero_[0], nunchuk_one_[0]), ay = axis(y, nunchuk_zero_[1], nunchuk_one_[1]),
                az = axis(z, nunchuk_zero_[2], nunchuk_one_[2]);
    state_.nunchuk_acc[0] = -ax;
    state_.nunchuk_acc[1] = -az;
    state_.nunchuk_acc[2] = ay;
    // Z and C are reported as the Nunchuk buttons KPAD expects.
    if (!(e[5] & 1))
        state_.buttons |= 0x2000;  // WPAD_Z
    if (!(e[5] & 2))
        state_.buttons |= 0x4000;  // WPAD_C
}
