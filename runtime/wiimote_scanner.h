// Continuous scanning for Wii Remotes (Windows), as in Dolphin: while enabled and no remote is
// connected, a background thread runs Bluetooth inquiries. A Wii Remote in discoverable mode (1+2
// or the red sync button) gets its HID service enabled directly, so Windows creates the HID device
// without pairing it in the system's Bluetooth settings; runtime/wiimote then opens it.
#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

class WiimoteScanner {
public:
    enum class Status { Off, Searching, NoAdapter, Unsupported };

    ~WiimoteScanner() { stop(); }

    // Scans only while enabled and no remote is connected (both may change at any time).
    void set_enabled(bool enabled);
    void set_connected(bool connected);
    Status status() const { return status_; }
    // True once after a remote's HID service was enabled (its HID device appears shortly after).
    bool take_found() { return found_.exchange(false); }

    void stop();

private:
    void run();
    void scan_once();
    void wake();

    std::atomic<bool> enabled_{false}, connected_{false}, quit_{false};
    std::atomic<Status> status_{Status::Off};
    std::atomic<bool> found_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
};
