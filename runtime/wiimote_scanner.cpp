#include "wiimote_scanner.h"

#include <chrono>
#include <cwchar>

#include "log.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// windows.h first
#include <bluetoothapis.h>
#endif

void WiimoteScanner::set_enabled(bool enabled) {
    enabled_ = enabled;
#ifdef _WIN32
    if (enabled && !thread_.joinable())
        thread_ = std::thread([this] { run(); });
#else
    if (enabled)
        status_ = Status::Unsupported;  // Linux/macOS: pair through the system (BlueZ, etc.)
#endif
    wake();
}

void WiimoteScanner::set_connected(bool connected) {
    if (connected_.exchange(connected) != connected)
        wake();
}

void WiimoteScanner::stop() {
    quit_ = true;
    wake();
    if (thread_.joinable())
        thread_.join();  // an inquiry in progress takes up to ~2.6 s to finish
}

void WiimoteScanner::wake() {
    std::lock_guard lock(mutex_);
    cv_.notify_all();
}

void WiimoteScanner::run() {
    while (!quit_) {
        if (!enabled_ || connected_) {
            status_ = Status::Off;
            std::unique_lock lock(mutex_);
            cv_.wait_for(lock, std::chrono::seconds(1), [this] { return quit_ || (enabled_ && !connected_); });
            continue;
        }
        scan_once();
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, std::chrono::seconds(1), [this] { return quit_.load(); });
    }
}

#ifdef _WIN32
namespace {

// Bluetooth HID service, {00001124-0000-1000-8000-00805F9B34FB}.
constexpr GUID HID_SERVICE = {0x00001124, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}};

bool is_wiimote(const BLUETOOTH_DEVICE_INFO& d) {
    return !std::wcscmp(d.szName, L"Nintendo RVL-CNT-01") || !std::wcscmp(d.szName, L"Nintendo RVL-CNT-01-TR");
}

// Calls fn(radio, device) for every device known to (or, with inquiry, discovered by) each radio.
// Returns false if there is no Bluetooth radio.
template <typename Fn>
bool for_each_device(bool inquiry, Fn&& fn) {
    BLUETOOTH_FIND_RADIO_PARAMS radio_params{sizeof radio_params};
    HANDLE radio = nullptr;
    HBLUETOOTH_RADIO_FIND radios = BluetoothFindFirstRadio(&radio_params, &radio);
    if (!radios)
        return false;
    do {
        BLUETOOTH_DEVICE_SEARCH_PARAMS search{sizeof search};
        search.fReturnAuthenticated = TRUE;
        search.fReturnRemembered = TRUE;
        search.fReturnUnknown = TRUE;
        search.fReturnConnected = TRUE;
        search.fIssueInquiry = inquiry;
        search.cTimeoutMultiplier = 2;  // x 1.28 s
        search.hRadio = radio;
        BLUETOOTH_DEVICE_INFO device{sizeof device};
        if (HBLUETOOTH_DEVICE_FIND devices = BluetoothFindFirstDevice(&search, &device)) {
            do {
                fn(radio, device);
                device = {sizeof device};
            } while (BluetoothFindNextDevice(devices, &device));
            BluetoothFindDeviceClose(devices);
        }
        CloseHandle(radio);
    } while (BluetoothFindNextRadio(radios, &radio));
    BluetoothFindRadioClose(radios);
    return true;
}

}  // namespace

void WiimoteScanner::scan_once() {
    // A remote Windows remembers from an earlier (unauthenticated) connection but that is not
    // connected now keeps it from reconnecting: forget it. Remotes paired with a PIN are left alone.
    for_each_device(false, [](HANDLE, BLUETOOTH_DEVICE_INFO& d) {
        if (is_wiimote(d) && d.fRemembered && !d.fConnected && !d.fAuthenticated) {
            BluetoothRemoveDevice(&d.Address);
            LOG_DEBUG("wiimote", "removed a stale Bluetooth entry for a Wii Remote");
        }
    });
    status_ = Status::Searching;
    const bool radio = for_each_device(true, [this](HANDLE radio, BLUETOOTH_DEVICE_INFO& d) {
        if (!is_wiimote(d) || d.fConnected)
            return;
        // Discoverable (1+2 or sync): enable its HID service; Windows then creates the HID device.
        const DWORD err = BluetoothSetServiceState(radio, &d, &HID_SERVICE, BLUETOOTH_SERVICE_ENABLE);
        if (err == ERROR_SUCCESS) {
            LOG_INFO("wiimote", "found a Wii Remote over Bluetooth; connecting");
            found_ = true;
        }
        else
            LOG_DEBUG("wiimote", "enabling the Wii Remote's HID service failed (%lu)", err);
    });
    if (!radio)
        status_ = Status::NoAdapter;
}
#else
void WiimoteScanner::scan_once() {}
#endif
