#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

#include <SDL3/SDL.h>
#include <aurora/aurora.h>
#include <aurora/event.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "gpu/aurora_bridge.h"
#include "crash.h"
#include "log.h"
#include "memory.h"
#include "runtime.h"
#include "png_write.h"
#include "settings.h"
#include "ui/settings_menu.h"
#include "version.h"

int g_log_level = 1;
extern "C" int pandora_fp_selftest(void);

static void usage() {
    std::fprintf(stderr,
                 "usage: pandora --disc <extracted partition> [--headless | --background] [--nand <dir>] [--map <file>]\n"
                 "               [--max-blocks N] [--audio-dump <wav>] [--mute] [--unthrottled] [--window <w>x<h>]\n"
                 "               [--profile] [-v|-vv|-vvv]\n"
                 "               [--screenshot <frame>:<file.png>]... [--exit-after <frames>]\n"
                 "               [--press <button>@<frame>[+<frames>]]... (A B 1 2 PLUS MINUS UP DOWN LEFT RIGHT Z C,\n"
                 "                or StickUp StickDown StickLeft StickRight for the Nunchuk stick)\n"
                 "  debugging: [--watch <symbol>]... [--watch-write <hexaddr>]... [--dump <hexaddr>[:len]]...\n"
                 "             [--trace] [--shot-copy <frame>:<copy>:<file.png>]... [--shot-draw <frame>:<draw>:<file.png>]...\n"
                 "             [--menu-test <frame>:<file.png>] (open the settings menu, capture the window, exit)\n"
                 "             [--menu-keys <key>,<key>...] (keys pressed in the menu before the capture; Key*N holds it N frames)\n"
                 "             (PANDORA_DEBUG_FRAME=<n> with -vv logs every draw and EFB copy of frame n)\n");
}

static void aurora_log(AuroraLogLevel level, const char* module, const char* message, unsigned int len) {
    static const int ours[] = {2, 1, 1, 0, 0};  // debug, info, warning, error, fatal
    LOG_AT(ours[level], module, "%.*s", (int)len, message);
    if (level == LOG_FATAL) {
        crash::report("fatal error in aurora (above)");
        std::abort();
    }
}

// Windowed: aurora owns the window and the WebGPU device; each host frame runs the guest until
// it presents (display copy) and then lets aurora render and present what it was sent.
// Keep the window from taking focus or covering other windows (for automated test runs).
// Rendering continues, so in-process screenshots still work.
static void send_window_to_background() {
#ifdef _WIN32
    SDL_Window* window = aurora_get_window();
    if (!window)
        return;
    auto hwnd = static_cast<HWND>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!hwnd)
        return;
    // Past the right edge of the virtual desktop rather than just behind other windows: Windows
    // throttles presents of covered windows (gameplay measured ~15% slower), and this way it is
    // still not visible.
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) + 64;
    SetWindowPos(hwnd, HWND_BOTTOM, x, 0, 0, 0, SWP_NOACTIVATE | SWP_NOSIZE);
#endif
}

// Debugging: capture our own window's contents (including the UI overlay, which the in-process
// EFB readback does not see). PrintWindow copies only this window, never other screen content.
static bool capture_window(const std::string& path) {
#ifdef _WIN32
    SDL_Window* window = aurora_get_window();
    auto hwnd = window ? static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                                                  SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr))
                       : nullptr;
    RECT rc{};
    if (!hwnd || !GetClientRect(hwnd, &rc) || rc.right <= 0 || rc.bottom <= 0)
        return false;
    const int w = rc.right, h = rc.bottom;
    HDC wdc = GetDC(hwnd), mdc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, w, h);
    HGDIOBJ old = SelectObject(mdc, bmp);
    const bool ok = PrintWindow(hwnd, mdc, PW_CLIENTONLY | PW_RENDERFULLCONTENT);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    std::vector<u8> px((size_t)w * h * 4);
    GetDIBits(mdc, bmp, 0, (UINT)h, px.data(), &bi, DIB_RGB_COLORS);
    SelectObject(mdc, old);
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(hwnd, wdc);
    for (size_t i = 0; i < px.size(); i += 4) {
        std::swap(px[i], px[i + 2]);  // BGRA -> RGBA
        px[i + 3] = 255;
    }
    return ok && write_png_rgba(path, px.data(), (u32)w, (u32)h);
#else
    (void)path;
    return false;
#endif
}

// F11 or Alt+Enter toggles fullscreen.
static bool is_fullscreen_toggle(const SDL_Event& e) {
    if (e.type != SDL_EVENT_KEY_DOWN || e.key.repeat)
        return false;
    if (e.key.key == SDLK_F11)
        return true;
    return (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) && (e.key.mod & SDL_KMOD_ALT);
}

// Keeps emulated time in step with the wall clock: sleeps while the guest is ahead, and
// re-anchors when it falls behind (slow frames are not made up by running fast afterwards).
class Pacer {
public:
    explicit Pacer(u64 timebase) { reset(timebase); }
    void reset(u64 timebase) {
        tb_ = timebase;
        wall_ = std::chrono::steady_clock::now();
    }
    void wait(u64 timebase) {
        const double emu = (double)(timebase - tb_) / 60750000.0;
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_).count();
        if (emu > wall)
            SDL_DelayPrecise((Uint64)((emu - wall) * 1e9));
        else if (wall - emu > 0.1)
            reset(timebase);
    }
    // Shift the schedule: positive slows the guest down, negative lets it catch up.
    void nudge(double seconds) { wall_ += std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds)); }

private:
    u64 tb_ = 0;
    std::chrono::steady_clock::time_point wall_;
};

static int run_windowed(Runtime& rt, int argc, char** argv, bool background, bool mute, bool unthrottled, u32 win_w,
                        u32 win_h, u64 menu_test_frame, const std::string& menu_test_path,
                        const std::vector<std::string>& menu_test_keys) {
    const std::string settings_path = "build/aurora/settings.ini";
    Settings settings;
    settings.load(settings_path);
    if (const char* fps60 = std::getenv("PANDORA_FPS60"))  // test runs: force the 60 FPS mode on/off
        settings.fps60 = *fps60 != '0';

    LOG_INFO("main", "pandora %s, built %s", PANDORA_VERSION, PANDORA_BUILD_TIME);
    AuroraConfig config{};
    config.appName = "Pandora's Tower";
    config.logCallback = &aurora_log;
    config.logLevel = g_log_level >= 2 ? LOG_DEBUG : LOG_INFO;
    config.vsync = std::getenv("PANDORA_NO_VSYNC") == nullptr;  // test runs: PANDORA_NO_VSYNC=1
    config.windowPosX = SDL_WINDOWPOS_CENTERED;  // aurora takes 0, 0 literally (title bar off screen)
    config.windowPosY = SDL_WINDOWPOS_CENTERED;
    config.windowWidth = win_w;
    config.windowHeight = win_h;
    // Keep aurora's caches (compiled pipelines, Dawn cache, imgui.ini) with the build, not in the
    // user profile.
    std::filesystem::create_directories("build/aurora");
    config.userPath = "build/aurora";
    config.cachePath = "build/aurora";
    config.mem1Size = 0;  // guest memory lives in the runtime
    config.mem2Size = 0;
    config.startFullscreen = settings.fullscreen && !background;
    if (background)
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_WII, "0");  // Wii Remotes are driven directly (runtime/wiimote)
    aurora_initialize(argc, argv, &config);
    if (background)
        send_window_to_background();

    rt.enable_rendering();
    if (!mute)
        rt.enable_audio_output();
    rt.start();

    rt.input().set_bindings(&settings.bindings);
    auto apply_settings = [&] {
        SDL_Window* window = aurora_get_window();
        const bool fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
        if (!background && fullscreen != settings.fullscreen)
            SDL_SetWindowFullscreen(window, settings.fullscreen);
        rt.set_audio_volume(settings.muted ? 0.0f : (float)settings.volume / 100.0f);
        rt.input().set_rumble_intensity((float)settings.rumble / 100.0f);
        rt.input().set_mouse_pointer(settings.mouse_pointer);
        rt.input().set_free_stick(settings.free_stick);
        rt.input().set_sensor_bar_above(settings.sensor_bar_above);
        rt.input().set_ir_sensitivity(settings.ir_sensitivity);
        rt.input().set_wiimote_scanning(settings.wiimote_scan && (!background || std::getenv("PANDORA_POLL_WIIMOTE")));
    };
    apply_settings();
    SettingsMenu menu(settings, [&] {
        apply_settings();
        if (!background)
            settings.save(settings_path);
    });
    const char* base = SDL_GetBasePath();
    if (!menu.init(std::string(base ? base : "") + "assets"))
        LOG_ERROR("ui", "settings menu unavailable");
    bool paused = false, user_paused = false;
    int menu_test_wait = -1;  // host frames until the --menu-test capture
    int menu_test_elapsed = 0;
    struct ScriptedKey {
        int frame;
        SDL_Scancode scancode;
        bool down;
    };
    std::vector<ScriptedKey> menu_test_schedule;

    // --profile: where host time goes, logged every 60 frames.
    using clock = std::chrono::steady_clock;
    auto secs = [](clock::time_point a, clock::time_point b) { return std::chrono::duration<double>(b - a).count(); };
    struct {
        double total = 0, run = 0, end = 0, other = 0;
        u32 frames = 0;
    } prof;
    u64 prof_tb = rt.cpu.timebase, prof_cycles = rt.executed_cycles();
    // Host CPU cycles of this (the emulation) thread, for a host-cycles-per-guest-cycle figure that
    // does not depend on waits or clock speed.
    auto thread_cycles = [] {
#ifdef _WIN32
        ULONG64 c = 0;
        QueryThreadCycleTime(GetCurrentThread(), &c);
        return (u64)c;
#else
        return (u64)0;
#endif
    };
    u64 prof_host_cycles = thread_cycles(), prof_blocks = rt.blocks();
    auto prof_start = clock::now();

    Pacer pacer(rt.cpu.timebase);
    bool running = true;
    while (running) {
        const auto t0 = clock::now();
        for (const AuroraEvent* e = aurora_update(); e && e->type != AURORA_NONE; ++e) {
            if (e->type == AURORA_EXIT)
                running = false;
            else if (e->type == AURORA_SDL_EVENT && is_fullscreen_toggle(e->sdl) && !background) {
                settings.fullscreen = !settings.fullscreen;
                SDL_SetWindowFullscreen(aurora_get_window(), settings.fullscreen);
                if (!background)
                    settings.save(settings_path);
                menu.refresh();
            } else if (e->type == AURORA_SDL_EVENT && e->sdl.type == SDL_EVENT_KEY_DOWN && e->sdl.key.key == SDLK_F5 &&
                       !e->sdl.key.repeat) {
                user_paused = !user_paused;  // F5 pauses the game (the menu does not)
                menu.set_paused(user_paused);
            }
        }
        if (!running) {
            rt.halt("quit");
            break;
        }
        AuroraBridge& bridge = *rt.bridge();
        if (menu_test_frame && bridge.frames() >= menu_test_frame && menu_test_wait < 0) {
            menu.open();
            // "Key" taps a key; "Key*N" holds it for N host frames. Steps are at least 8 frames apart.
            int t = 0;
            for (const std::string& spec : menu_test_keys) {
                const size_t star = spec.find('*');
                const int hold = star == std::string::npos ? 1 : std::max(1, std::atoi(spec.c_str() + star + 1));
                const SDL_Scancode sc = SDL_GetScancodeFromName(spec.substr(0, star).c_str());
                menu_test_schedule.push_back({t, sc, true});
                menu_test_schedule.push_back({t + hold, sc, false});
                t += std::max(8, hold + 4);
            }
            menu_test_wait = t + 30;
        }
        aurora_set_preserve_frame(user_paused);  // keep showing the paused game
        if (!aurora_begin_frame())
            continue;
        // --menu-test: play the scripted keys through SDL's event queue, then capture the window and exit.
        if (menu_test_wait > 0) {
            for (const auto& k : menu_test_schedule)
                if (k.frame == menu_test_elapsed) {
                    SDL_Event ev{};
                    ev.type = k.down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                    ev.key.windowID = SDL_GetWindowID(aurora_get_window());
                    ev.key.scancode = k.scancode;
                    ev.key.key = SDL_GetKeyFromScancode(k.scancode, SDL_KMOD_NONE, false);
                    ev.key.down = k.down;
                    SDL_PushEvent(&ev);
                }
            menu_test_elapsed++;
            if (--menu_test_wait == 0) {
                if (capture_window(menu_test_path))
                    LOG_INFO("shot", "wrote %s (window)", menu_test_path.c_str());
                else
                    LOG_ERROR("shot", "window capture failed");
                rt.halt("quit");
                running = false;
                aurora_end_frame();
                continue;
            }
        }
        // A real Wii Remote: polled every host frame; its buttons also drive the menu. Background
        // (test) runs leave remotes alone unless PANDORA_POLL_WIIMOTE is set.
        static const bool poll_in_background = std::getenv("PANDORA_POLL_WIIMOTE") != nullptr;
        if (!background || poll_in_background) {
            rt.input().poll_wiimote();
            const Wiimote::State& wm = rt.input().wiimote().state();
            static const bool home_menu = std::getenv("PANDORA_HOME_MENU") != nullptr;  // HOME to the game, not the menu
            menu.set_remote_buttons(wm.connected ? wm.buttons & (home_menu ? ~WPAD_HOME : ~0u) : 0);
            char status[96];
            const auto scan = rt.input().wiimote_scan_status();
            if (!wm.connected && scan == WiimoteScanner::Status::Searching)
                std::snprintf(status, sizeof status, "Searching... press 1 and 2 on the Wii Remote");
            else if (!wm.connected && scan == WiimoteScanner::Status::NoAdapter)
                std::snprintf(status, sizeof status, "No Bluetooth adapter found");
            else if (!wm.connected)
                std::snprintf(status, sizeof status, "Not connected (pair it over Bluetooth, or turn on scanning)");
            else
                std::snprintf(status, sizeof status, "Connected%s, battery %d%%%s", wm.nunchuk ? " with Nunchuk" : "",
                              wm.battery * 100 / 200 > 100 ? 100 : wm.battery * 100 / 200,
                              wm.pointer_valid ? ", pointing at the screen" : "");
            menu.set_remote_status(status);
        }
        menu.update();
        // While paused (F5) the guest does not run; aurora keeps presenting the last frame.
        if (user_paused) {
            paused = true;
            aurora_end_frame();
            continue;
        }
        if (paused) {
            paused = false;
            pacer.reset(rt.cpu.timebase);  // don't try to catch up on the paused time
        }
        bridge.begin_frame();
        // 60 FPS mode: the game's frame-rate target (a word in its frame manager, 30 normally), as the
        // community code "04774AB0 0000003C" sets it, re-applied every frame like a cheat code.
        // Switching it off puts 30 back.
        constexpr u32 FRAME_RATE_TARGET = 0x80774AB0;
        // Test runs: PANDORA_FPS60_UNTIL=<frame> leaves 60 FPS mode from that frame on (to
        // compare the modes along an identical path into gameplay).
        static const u64 fps60_until = std::getenv("PANDORA_FPS60_UNTIL") ? std::strtoull(std::getenv("PANDORA_FPS60_UNTIL"), nullptr, 10) : 0;
        if (fps60_until && bridge.frames() >= fps60_until)
            settings.fps60 = false;
        if (settings.fps60)
            mem::write32(&rt.cpu, FRAME_RATE_TARGET, 60);
        else if (mem::read32(&rt.cpu, FRAME_RATE_TARGET) == 60)
            mem::write32(&rt.cpu, FRAME_RATE_TARGET, 30);
        static const u64 profile_from = std::getenv("PANDORA_PROFILE_FROM") ? std::strtoull(std::getenv("PANDORA_PROFILE_FROM"), nullptr, 10) : 0;
        if (profile_from && rt.options().profile)
            rt.set_profiling_paused(bridge.frames() < profile_from);
        rt.input().update(bridge.frames(), !background && !menu.is_open());  // the menu takes input while open
        for (const auto& [frame, path] : rt.options().screenshots)
            if (bridge.frames() == frame)
                bridge.request_screenshot(path);
        for (const auto& c : rt.options().copy_shots)
            if (bridge.frames() == c.frame)
                bridge.request_copy_shot(c.frame, c.index, c.path);
        for (const auto& c : rt.options().draw_shots)
            if (bridge.frames() == c.frame)
                bridge.request_draw_shot(c.frame, c.index, c.path);
        const auto t1 = clock::now();
        running = rt.run_frame();
        const auto t2 = clock::now();
        if (!unthrottled) {
            // Follow the audio device's clock: keep 90-170 ms queued (just after a frame's burst).
            if (const int queued = rt.audio_queued_ms(); queued > 170)
                pacer.nudge(0.002);
            else if (queued >= 0 && queued < 90)
                pacer.nudge(-0.002);
            pacer.wait(rt.cpu.timebase);
        }
        aurora_end_frame();
        const auto t3 = clock::now();
        bridge.poll_screenshot();
        if (rt.options().profile) {
            prof.other += secs(t0, t1);
            prof.run += secs(t1, t2);
            prof.end += secs(t2, t3);
            if (++prof.frames == 60) {
                const auto now = clock::now();
                const double wall = secs(prof_start, now), emu = (double)(rt.cpu.timebase - prof_tb) / 60750000.0;
                // Share of emulated time the guest CPU was busy (not idle): near 100% means the game is
                // limited by the emulated CPU's speed, whatever the host's.
                const double busy = (double)(rt.executed_cycles() - prof_cycles) / ((double)(rt.cpu.timebase - prof_tb) * 12.0);
                const auto bt = bridge.take_timing();
                const double ms = 1000.0 / prof.frames;
                const u64 host_cycles = thread_cycles();
                const double guest_cycles = (double)(rt.executed_cycles() - prof_cycles);
                LOG_INFO("perf",
                         "%.1f fps, emulated %.2fx real time, guest CPU %.0f%% busy, %.1f host cycles/guest cycle "
                         "(%.1fM/frame, %.0fk dispatches/frame) | per frame ms: guest %.2f (bridge %.2f, draw-done "
                         "wait %.2f x%.1f), pacing+present %.2f, events %.2f | %.0f draws/frame",
                         prof.frames / wall, emu / wall, 100.0 * busy, (double)(host_cycles - prof_host_cycles) / guest_cycles,
                         guest_cycles / prof.frames / 1e6, (double)(rt.blocks() - prof_blocks) / prof.frames / 1e3,
                         (prof.run - bt.forward - bt.drain) * ms, bt.forward * ms, bt.drain * ms,
                         (double)bt.drains / prof.frames, prof.end * ms, prof.other * ms, (double)bt.draws / prof.frames);
                prof_host_cycles = host_cycles;
                prof_blocks = rt.blocks();
                prof = {};
                prof_tb = rt.cpu.timebase;
                prof_cycles = rt.executed_cycles();
                prof_start = now;
            }
        }
        if (rt.options().exit_after_frames && bridge.frames() >= rt.options().exit_after_frames) {
            rt.halt("quit");
            // let pending screenshots finish reading back
            for (int i = 0; i < 30; i++) {
                aurora_update();
                if (aurora_begin_frame())
                    aurora_end_frame();
                bridge.poll_screenshot();
            }
            running = false;
        }
    }
    int status = rt.finish();
    rt.disable_audio_output();  // before aurora shuts SDL down
    aurora_shutdown();
    return status;
}

int main(int argc, char** argv) {
    // PANDORA_WAIT_DEBUGGER=1: wait for a debugger to attach before anything runs (test runs started
    // with the test's environment and arguments, then attached from x64dbg).
    if (const char* w = std::getenv("PANDORA_WAIT_DEBUGGER"); w && *w == '1') {
        std::fprintf(stderr, "waiting for a debugger (pid %lu)\n", (unsigned long)GetCurrentProcessId());
        while (!IsDebuggerPresent())
            Sleep(100);
    }
    // PANDORA_HOST_PATCH=<rva>=<hex bytes>[;...]: byte patches to this executable's code (test runs:
    // open or close a recompiled branch without rebuilding; RVAs from the PDB, tools/lines2rva.py).
    if (const char* spec = std::getenv("PANDORA_HOST_PATCH")) {
        auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleA(nullptr));
        for (const char* p = spec; *p;) {
            char* end;
            const unsigned long rva = std::strtoul(p, &end, 16);
            if (*end != '=') {
                std::fprintf(stderr, "PANDORA_HOST_PATCH: bad entry at '%s'\n", p);
                return 1;
            }
            std::vector<unsigned char> bytes;
            for (p = end + 1; std::isxdigit((unsigned char)p[0]) && std::isxdigit((unsigned char)p[1]); p += 2)
                bytes.push_back((unsigned char)std::strtoul(std::string(p, 2).c_str(), nullptr, 16));
            DWORD old;
            VirtualProtect(base + rva, bytes.size(), PAGE_EXECUTE_READWRITE, &old);
            std::memcpy(base + rva, bytes.data(), bytes.size());
            VirtualProtect(base + rva, bytes.size(), old, &old);
            FlushInstructionCache(GetCurrentProcess(), base + rva, bytes.size());
            std::fprintf(stderr, "host patch: %zu bytes at rva %lx\n", bytes.size(), rva);
            if (*p == ';')
                p++;
            else if (*p) {
                std::fprintf(stderr, "PANDORA_HOST_PATCH: bad entry at '%s'\n", p);
                return 1;
            }
        }
    }
    Options opts;
    bool headless = false, background = false, mute = false, unthrottled = false;
    u64 menu_test_frame = 0;
    std::string menu_test_path;
    std::vector<std::string> menu_test_keys;
    u32 win_w = 1280, win_h = 720;
    std::vector<std::string> presses;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (!std::strcmp(a, "--disc") && i + 1 < argc)
            opts.disc = next();
        else if (!std::strcmp(a, "--headless"))
            headless = true;
        else if (!std::strcmp(a, "--background"))
            background = true;
        else if (!std::strcmp(a, "--mute"))
            mute = true;
        else if (!std::strcmp(a, "--fp-selftest"))  // compare fastfp.h with DolRecomp's FP code
            return pandora_fp_selftest();
        else if (!std::strcmp(a, "--unthrottled"))
            unthrottled = true;
        else if (!std::strcmp(a, "--menu-test") && i + 1 < argc) {
            char* end;
            menu_test_frame = std::strtoull(next(), &end, 10);
            if (*end != ':' || !menu_test_frame) {
                usage();
                return 2;
            }
            menu_test_path = end + 1;
        } else if (!std::strcmp(a, "--menu-keys") && i + 1 < argc) {
            // Comma-separated SDL scancode names, e.g. "Right,Down,Return".
            std::string list = next();
            for (size_t pos = 0; pos <= list.size();) {
                const size_t comma = std::min(list.find(',', pos), list.size());
                if (comma > pos)
                    menu_test_keys.push_back(list.substr(pos, comma - pos));
                pos = comma + 1;
            }
        } else if (!std::strcmp(a, "--window") && i + 1 < argc) {
            if (std::sscanf(next(), "%ux%u", &win_w, &win_h) != 2 || !win_w || !win_h) {
                usage();
                return 2;
            }
        }
        else if (!std::strcmp(a, "--press") && i + 1 < argc)
            presses.push_back(next());
        else if (!std::strcmp(a, "--nand") && i + 1 < argc)
            opts.nand = next();
        else if (!std::strcmp(a, "--map") && i + 1 < argc)
            opts.map = next();
        else if (!std::strcmp(a, "--max-blocks") && i + 1 < argc)
            opts.max_blocks = std::strtoull(next(), nullptr, 0);
        else if (!std::strcmp(a, "--audio-dump") && i + 1 < argc)
            opts.audio_dump = next();
        else if (!std::strcmp(a, "--watch") && i + 1 < argc)
            opts.watch.push_back(next());
        else if (!std::strcmp(a, "--dump") && i + 1 < argc) {
            char* end;
            u32 addr = (u32)std::strtoul(next(), &end, 16);
            opts.dumps.emplace_back(addr, *end == ':' ? (u32)std::strtoul(end + 1, nullptr, 0) : 64u);
        } else if (!std::strcmp(a, "--dump-file") && i + 1 < argc) {
            char* end;
            u32 addr = (u32)std::strtoul(next(), &end, 16);
            u32 len = *end == ':' ? (u32)std::strtoul(end + 1, &end, 0) : 0;
            if (*end != ':' || !len) {
                usage();
                return 2;
            }
            opts.file_dumps.push_back({addr, len, end + 1});
        } else if (!std::strcmp(a, "--watch-write") && i + 1 < argc)
            opts.watch_writes.push_back((u32)std::strtoul(next(), nullptr, 16));
        else if (!std::strcmp(a, "--screenshot") && i + 1 < argc) {
            char* end;
            u64 frame = std::strtoull(next(), &end, 10);
            if (*end != ':') {
                usage();
                return 2;
            }
            opts.screenshots.emplace_back(frame, end + 1);
        } else if ((!std::strcmp(a, "--shot-copy") || !std::strcmp(a, "--shot-draw")) && i + 1 < argc) {
            const bool draw = a[7] == 'd';
            char* end;
            u64 frame = std::strtoull(next(), &end, 10);
            u32 index = *end == ':' ? (u32)std::strtoul(end + 1, &end, 10) : 0;
            if (*end != ':') {
                usage();
                return 2;
            }
            (draw ? opts.draw_shots : opts.copy_shots).push_back({frame, index, end + 1});
        } else if (!std::strcmp(a, "--exit-after") && i + 1 < argc)
            opts.exit_after_frames = std::strtoull(next(), nullptr, 10);
        else if (!std::strcmp(a, "--profile"))
            opts.profile = true;
        else if (!std::strcmp(a, "--trace"))
            opts.trace_calls = true;
        else if (!std::strncmp(a, "-v", 2) && std::strspn(a + 1, "v") == std::strlen(a + 1))
            g_log_level = 1 + (int)std::strlen(a + 1);
        else {
            usage();
            return 2;
        }
    }
    if (opts.disc.empty()) {
        usage();
        return 2;
    }

    // Everything logged also goes to a file (test runs use their own, to keep the player's).
    std::filesystem::create_directories("build/aurora");
    const char* log_path = background ? "build/aurora/pandora-background.log" : "build/aurora/pandora.log";
    if (!log_open_file(log_path))
        std::fprintf(stderr, "cannot write %s\n", log_path);

    Runtime rt(opts);
    crash::install("build/aurora", &rt);
    for (const auto& spec : presses)
        if (!rt.input().add_script(spec)) {
            std::fprintf(stderr, "bad --press %s (use e.g. A@150+10)\n", spec.c_str());
            return 2;
        }
    if (!rt.boot())
        return 1;
    // Background (test) runs are silent unless an SDL audio driver is forced, e.g.
    // SDL_AUDIO_DRIVER=disk, which writes the output to a file instead of a device.
    const bool forced_driver = std::getenv("SDL_AUDIO_DRIVER") != nullptr;
    return headless ? rt.run() : run_windowed(rt, argc, argv, background, mute || (background && !forced_driver), unthrottled,
                                                win_w, win_h, menu_test_frame, menu_test_path,
                                                menu_test_keys);
}
