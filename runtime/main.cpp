#include <cstdlib>
#include <cstring>
#include <filesystem>

#include <SDL3/SDL.h>
#include <aurora/aurora.h>
#include <aurora/event.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "gpu/aurora_bridge.h"
#include "log.h"
#include "runtime.h"
#include "settings.h"

int g_log_level = 1;

static void usage() {
    std::fprintf(stderr,
                 "usage: pandora --disc <extracted partition> [--headless | --background] [--nand <dir>] [--map <file>]\n"
                 "               [--max-blocks N] [--audio-dump <wav>] [--mute] [--profile] [-v|-vv|-vvv]\n"
                 "               [--screenshot <frame>:<file.png>]... [--exit-after <frames>]\n"
                 "               [--press <button>@<frame>[+<frames>]]...\n"
                 "  debugging: [--watch <symbol>]... [--watch-write <hexaddr>]... [--dump <hexaddr>[:len]]...\n"
                 "             [--trace] [--shot-copy <frame>:<copy>:<file.png>]... [--shot-draw <frame>:<draw>:<file.png>]...\n"
                 "             (PANDORA_DEBUG_FRAME=<n> with -vv logs every draw and EFB copy of frame n)\n");
}

static void aurora_log(AuroraLogLevel level, const char* module, const char* message, unsigned int len) {
    static const int ours[] = {2, 1, 1, 0, 0};  // debug, info, warning, error, fatal
    LOG_AT(ours[level], module, "%.*s", (int)len, message);
    if (level == LOG_FATAL)
        std::abort();
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
    if (hwnd)
        SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
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

static int run_windowed(Runtime& rt, int argc, char** argv, bool background, bool mute) {
    const std::string settings_path = "build/aurora/settings.ini";
    Settings settings;
    settings.load(settings_path);

    AuroraConfig config{};
    config.appName = "Pandora's Tower";
    config.logCallback = &aurora_log;
    config.logLevel = g_log_level >= 2 ? LOG_DEBUG : LOG_INFO;
    config.vsync = true;
    config.windowWidth = 1280;
    config.windowHeight = 720;
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
    aurora_initialize(argc, argv, &config);
    if (background)
        send_window_to_background();

    rt.enable_rendering();
    if (!mute)
        rt.enable_audio_output();
    rt.start();
    bool running = true;
    while (running) {
        for (const AuroraEvent* e = aurora_update(); e && e->type != AURORA_NONE; ++e) {
            if (e->type == AURORA_EXIT)
                running = false;
            else if (e->type == AURORA_SDL_EVENT && is_fullscreen_toggle(e->sdl) && !background) {
                settings.fullscreen = !settings.fullscreen;
                SDL_SetWindowFullscreen(aurora_get_window(), settings.fullscreen);
                settings.save(settings_path);
            }
        }
        if (!running) {
            rt.halt("quit");
            break;
        }
        if (!aurora_begin_frame())
            continue;
        AuroraBridge& bridge = *rt.bridge();
        rt.input().update(bridge.frames(), !background);
        for (const auto& [frame, path] : rt.options().screenshots)
            if (bridge.frames() == frame)
                bridge.request_screenshot(path);
        for (const auto& c : rt.options().copy_shots)
            if (bridge.frames() == c.frame)
                bridge.request_copy_shot(c.frame, c.index, c.path);
        for (const auto& c : rt.options().draw_shots)
            if (bridge.frames() == c.frame)
                bridge.request_draw_shot(c.frame, c.index, c.path);
        running = rt.run_frame();
        aurora_end_frame();
        bridge.poll_screenshot();
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
    aurora_shutdown();
    return status;
}

int main(int argc, char** argv) {
    Options opts;
    bool headless = false, background = false, mute = false;
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

    Runtime rt(opts);
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
    return headless ? rt.run() : run_windowed(rt, argc, argv, background, mute || (background && !forced_driver));
}
