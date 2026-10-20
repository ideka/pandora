#include "wiimote.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
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

// The pointer is computed as the game's own KPAD library does (read from this game's copy of
// KPAD: fn_805E7CA0 and fn_805E76C0 in the DOL), from the sensor bar's two dots in WPAD's
// coordinates, so a real remote behaves as on a Wii: the Wii's own settings (sensor bar position,
// IR sensitivity) are the only ones. KPAD's defaults: position scale 1.25, origin 0.2 towards the
// bar, no play radius; dist = (0.2 m / tan 21 degrees) / the dots' separation (camera 42 degrees
// wide); dots in the outer 5% of the image are ignored.
constexpr float KPAD_POS_SCALE = 1.25f / 0.95f, KPAD_BAR_OFFSET = 0.2f, KPAD_DIST_SCALE = 0.2f / 0.383864f;
constexpr float KPAD_AREA_X = 0.95f, KPAD_AREA_Y = 0.70f;
// KPAD's tracking limits: a pair is kept while its distance stays within 0.52..3 m and changes by
// less than 4 cm and 25 degrees per sample; one dot is followed within 0.1 of a previous end.
constexpr float KPAD_DIST_MIN = KPAD_DIST_SCALE, KPAD_DIST_MAX = 3.0f, KPAD_DIST_STEP = 0.04f, KPAD_DIR_MIN = 0.9f,
                KPAD_TRACK_RADIUS = 0.1f;

// IR camera sensitivity blocks for the Wii's five levels (WiiBrew).
constexpr u8 IR_BLOCK1[5][9] = {
    {0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0x64, 0x00, 0xFE}, {0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0x96, 0x00, 0xB4},
    {0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0xAA, 0x00, 0x64}, {0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0xC8, 0x00, 0x36},
    {0x07, 0x00, 0x00, 0x71, 0x01, 0x00, 0x72, 0x00, 0x20}};
constexpr u8 IR_BLOCK2[5][2] = {{0xFD, 0x05}, {0xB3, 0x04}, {0x63, 0x03}, {0x35, 0x03}, {0x1F, 0x03}};

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
    dpd_count_ = 0;
    known_len_ = 0;
    stable_ = 0;
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
    setup_ir();
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
// IR camera: basic mode (10 bytes for 4 dots) at the Wii's sensitivity level (1-5).
void Wiimote::setup_ir() {
    u8 r[2] = {OUT_IR_PIXEL_CLOCK, 0x04};
    send(r, sizeof r);
    r[0] = OUT_IR_LOGIC;
    send(r, sizeof r);
    const int level = std::clamp(ir_sensitivity_, 1, 5) - 1;
    const u8 enable = 0x08, basic = 0x01;
    write_register(IR_REGISTERS + 0x30, &enable, 1);
    write_register(IR_REGISTERS + 0x00, IR_BLOCK1[level], sizeof IR_BLOCK1[level]);
    write_register(IR_REGISTERS + 0x1A, IR_BLOCK2[level], sizeof IR_BLOCK2[level]);
    write_register(IR_REGISTERS + 0x33, &basic, 1);
    write_register(IR_REGISTERS + 0x30, &enable, 1);
    ir_level_set_ = ir_sensitivity_;
}

void Wiimote::set_ir_sensitivity(int level) {
    ir_sensitivity_ = level;
    if (dev_ && ir_level_set_ != level)
        setup_ir();
}

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
// WPAD's coordinates: x as the camera reports it (falls as the remote turns right), y flipped
// (767 - y: rises as the remote turns up); both measured with a real remote and bar.
//
// Then KPAD's tracker (fn_805E7CA0 and the functions it calls): the dots are normalised by 512,
// those in the outer 5% of the image or duplicated are dropped, and the bar's two ends are
// chosen by what was tracked last:
//   - from two ends: the pair whose distance changed by less than 4 cm and direction by less
//     than 25 degrees (fn_805E7150), else the one dot within 0.1 of a previous end, the other
//     end a bar's length away as before (fn_805E7540);
//   - from nothing: the pair best aligned with the horizon that is within 0.52..3 m
//     (fn_805E6F60), or one dot with the other end a remembered bar's length away wherever it
//     fits in the image (fn_805E7380);
// and pos = 1.25 / 0.95 * (origin - midpoint in the bar's frame), horizon = the bar's direction,
// dist from the separation (fn_805E76C0). KPAD orders the ends by its horizon reference, which
// here is the accelerometer's roll (an upside-down remote reverses them).
void Wiimote::parse_ir(const u8* ir) {
    static const bool log = std::getenv("PANDORA_IR_LOG") != nullptr;
    int rx[4], ry[4], raw = 0;
    for (int pair = 0; pair < 2; pair++) {
        const u8* p = ir + pair * 5;
        rx[raw] = p[0] | ((p[2] >> 4) & 3) << 8, ry[raw++] = p[1] | ((p[2] >> 6) & 3) << 8;
        rx[raw] = p[3] | (p[2] & 3) << 8, ry[raw++] = p[4] | ((p[2] >> 2) & 3) << 8;
    }
    // KPAD's objects: (x - 511.5) / 512, (y - 383.5) / 512 with y down; outer 5% and duplicates out.
    float ox[4], oy[4];
    int n = 0;
    for (int i = 0; i < 4; i++) {
        if (rx[i] == 0x3FF || ry[i] == 0x3FF)
            continue;
        const float x = ((float)rx[i] - 511.5f) / 512.0f, y = ((float)(767 - ry[i]) - 383.5f) / 512.0f;
        if (!(x > -KPAD_AREA_X && x < KPAD_AREA_X && y > -KPAD_AREA_Y && y < KPAD_AREA_Y))
            continue;
        bool dup = false;
        for (int j = 0; j < n; j++)
            dup |= ox[j] == x && oy[j] == y;
        if (!dup) {
            ox[n] = x;
            oy[n++] = y;
        }
    }
    // The horizon reference: level, reversed when the remote is upside down (its z axis, up
    // through the buttons, reads below zero: KPAD's y is its negative).
    const float ref_x = state_.acc[1] > 0 ? -1.0f : 1.0f;
    auto inside = [](float x, float y) { return x > -KPAD_AREA_X && x < KPAD_AREA_X && y > -KPAD_AREA_Y && y < KPAD_AREA_Y; };
    auto pair_dist = [](float x1, float y1, float x2, float y2) {
        const float len = std::hypot(x2 - x1, y2 - y1);
        return len > 0 ? KPAD_DIST_SCALE / len : 0.0f;
    };
    int found = 0;
    float ax = 0, ay = 0, bx = 0, by = 0;  // the ends, left to right
    auto track2 = [&] {  // fn_805E7150: the pair most like the last one
        float best = 2.0f;
        for (int i = 0; i < n; i++)
            for (int j = i + 1; j < n; j++) {
                const float len = std::hypot(ox[j] - ox[i], oy[j] - oy[i]);
                const float dist = pair_dist(ox[i], oy[i], ox[j], oy[j]);
                if (!(dist > KPAD_DIST_MIN && dist < KPAD_DIST_MAX))
                    continue;
                float score = std::fabs(dist - dpd_dist_) / KPAD_DIST_STEP;
                if (score >= 1.0f)
                    continue;
                float dot = ((ox[j] - ox[i]) * dpd_ux_ + (oy[j] - oy[i]) * dpd_uy_) / len;
                const bool swap = dot < 0;
                if (swap)
                    dot = -dot;
                if (dot <= KPAD_DIR_MIN)
                    continue;
                score += (1.0f - dot) / (1.0f - KPAD_DIR_MIN);
                if (score < best) {
                    best = score;
                    ax = swap ? ox[j] : ox[i], ay = swap ? oy[j] : oy[i];
                    bx = swap ? ox[i] : ox[j], by = swap ? oy[i] : oy[j];
                    found = 2;
                }
            }
    };
    auto acquire2 = [&] {  // fn_805E6F60: the pair best aligned with the horizon
        float best = 0.5f;  // KPAD: 0.9 against its accelerometer horizon; looser here (level only)
        for (int i = 0; i < n; i++)
            for (int j = i + 1; j < n; j++) {
                const float len = std::hypot(ox[j] - ox[i], oy[j] - oy[i]);
                const float dist = pair_dist(ox[i], oy[i], ox[j], oy[j]);
                if (!(dist > KPAD_DIST_MIN && dist < KPAD_DIST_MAX))
                    continue;
                float dot = (ox[j] - ox[i]) * ref_x / len;
                const bool swap = dot < 0;
                if (swap)
                    dot = -dot;
                if (dot > best) {
                    best = dot;
                    ax = swap ? ox[j] : ox[i], ay = swap ? oy[j] : oy[i];
                    bx = swap ? ox[i] : ox[j], by = swap ? oy[i] : oy[j];
                    found = 2;
                }
            }
    };
    auto track1 = [&] {  // fn_805E7540: the dot nearest (within 0.1) a previous end
        float best = KPAD_TRACK_RADIUS * KPAD_TRACK_RADIUS;
        int which = -1, end = 0;
        for (int e = 0; e < 2; e++) {
            const float ex = e ? dpd_bx_ : dpd_ax_, ey = e ? dpd_by_ : dpd_ay_;
            for (int i = 0; i < n; i++) {
                const float d = (ox[i] - ex) * (ox[i] - ex) + (oy[i] - ey) * (oy[i] - ey);
                if (d < best)
                    best = d, which = i, end = e;
            }
        }
        if (which < 0)
            return;
        const float sx = dpd_len_ * dpd_ux_, sy = dpd_len_ * dpd_uy_;
        if (end == 0)
            ax = ox[which], ay = oy[which], bx = ax + sx, by = ay + sy;
        else
            bx = ox[which], by = oy[which], ax = bx - sx, ay = by - sy;
        found = 1;
    };
    auto acquire1 = [&] {  // fn_805E7380: one dot, the other end a remembered length away
        if (known_len_ <= 0)
            return;
        const float sx = known_len_ * ref_x, sy = 0;
        for (int i = 0; i < n && !found; i++) {
            if (inside(ox[i] - sx, oy[i] - sy))  // as the right end
                ax = ox[i] - sx, ay = oy[i] - sy, bx = ox[i], by = oy[i], found = 1;
            else if (inside(ox[i] + sx, oy[i] + sy))  // as the left end
                ax = ox[i], ay = oy[i], bx = ox[i] + sx, by = oy[i] + sy, found = 1;
        }
    };
    if (dpd_count_ == 2) {
        if (n >= 2)
            track2();
        if (!found && n >= 1)
            track1();
    } else if (dpd_count_ == 1) {
        if (n >= 2)
            acquire2();
        if (!found && n >= 1)
            track1();
    } else {
        if (n >= 2)
            acquire2();
        if (!found && n == 1)
            acquire1();
    }
    if (found) {
        const float len = std::hypot(bx - ax, by - ay);
        dpd_ax_ = ax, dpd_ay_ = ay, dpd_bx_ = bx, dpd_by_ = by;
        dpd_len_ = len;
        dpd_ux_ = (bx - ax) / len, dpd_uy_ = (by - ay) / len;
        dpd_dist_ = KPAD_DIST_SCALE / len;
        // The bar's length is remembered for one-dot acquisition once seen whole for 200 samples.
        if (found == 2 && dpd_count_ == 2) {
            if (stable_ < 200)
                stable_++;
            else
                known_len_ = len;
        } else {
            stable_ = 0;
        }
        const float mx = (ax + bx) / 2, my = (ay + by) / 2;
        const float px = dpd_ux_ * mx + dpd_uy_ * my, py = dpd_ux_ * my - dpd_uy_ * mx;
        const float oy_origin = bar_above_ ? -KPAD_BAR_OFFSET : KPAD_BAR_OFFSET;
        state_.pointer_x = KPAD_POS_SCALE * (0.0f - px);
        state_.pointer_y = KPAD_POS_SCALE * (oy_origin - py);
        state_.horizon_x = dpd_ux_;
        state_.horizon_y = -dpd_uy_;
        state_.dist = dpd_dist_;
        state_.pointer_valid = true;
    } else {
        stable_ = 0;
        state_.pointer_valid = false;  // the game hides its cursor, as with a real Wii
    }
    dpd_count_ = found;
    if (log)
        LOG_INFO("ir", "dots %4d,%4d %4d,%4d %4d,%4d %4d,%4d  valid %d  ends %d  pos %6.3f %6.3f  dist %.2f  acc %5.2f %5.2f %5.2f",
                 rx[0], ry[0], rx[1], ry[1], rx[2], ry[2], rx[3], ry[3], n, found, state_.pointer_x, state_.pointer_y,
                 state_.dist, state_.acc[0], state_.acc[1], state_.acc[2]);
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
