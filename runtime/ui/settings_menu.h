// In-game settings menu (RmlUi through aurora): controls, sound and general settings. It works with
// mouse, keyboard and controller; its own actions (open/close, accept, cancel, directions, tabs)
// are bindings like the game's. The layout and style live in assets/ui (menu.rml, menu.rcss).
#pragma once

#include <array>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include <RmlUi/Core/DataModelHandle.h>
#include <aurora/input.hpp>

#include "../settings.h"

namespace Rml {
class ElementDocument;
}

class SettingsMenu {
public:
    // on_change runs after any setting changes (to apply and save it).
    SettingsMenu(Settings& settings, std::function<void()> on_change);
    ~SettingsMenu();

    // Loads fonts and the documents from asset_dir. False if RmlUi is unavailable.
    bool init(const std::string& asset_dir);

    bool is_open() const { return open_; }
    void open();
    void close();
    void toggle() { open_ ? close() : open(); }

    // Once per host frame (held-direction repeat, rebinding timeout).
    void update();
    // Settings changed outside the menu (e.g. the fullscreen hotkey).
    void refresh();
    // Shows the "paused" indicator (the game is paused with F5, independently of the menu).
    void set_paused(bool paused);

    // One row of a bindings table, as the document sees it.
    struct Row {
        Rml::String label, first, second;
        bool clear_first = false, clear_second = false;  // whether the clear/reset button does anything
        bool reset_first = false;  // the first mapping's button resets it to the default instead
        int index = 0;
    };

private:
    using Clock = std::chrono::steady_clock;

    // A press or release of one physical input, from an input event.
    struct Edge {
        PhysicalInput input;
        bool pressed;
    };

    static aurora::input::EventResult on_event(const aurora::input::InputEvent& event, void* self);
    static bool captures(const aurora::input::InputSource& source, void* self);
    aurora::input::EventResult handle(const aurora::input::InputEvent& event);
    std::vector<Edge> edges(const aurora::input::InputEvent& event);
    bool capture(const aurora::input::InputEvent& event, const std::vector<Edge>& edges);
    bool apply_actions(const std::vector<Edge>& edges, bool fire);
    void update_held(bool fire);
    void release_source(aurora::input::InputSource::Kind kind);
    void action(MenuAction a);

    void navigate(int key);  // an Rml::Input::KeyIdentifier
    bool navigate_bindings(int key);
    void switch_tab(int delta);

    Mappings& mappings(int table, int index);
    bool can_clear(int table, int index, int slot);
    bool is_reset(int table, int index, int slot) const;
    void start_capture(int table, int index, int slot);
    void finish_capture(const PhysicalInput& input);
    void cancel_capture();
    void clear_mapping(int table, int index, int slot);
    void refresh_rows();
    void changed();

    Settings& settings_;
    std::function<void()> on_change_;
    Rml::ElementDocument* doc_ = nullptr;
    Rml::ElementDocument* pause_doc_ = nullptr;
    Rml::DataModelHandle model_;
    std::vector<Row> wii_rows_, menu_rows_;
    Rml::String tab_ = "controls";
    Rml::String device_ = "gamepad";  // bindings shown: "gamepad" or "kbm"
    int capturing_ = -1;  // table * 1000 + index * 10 + slot while waiting for an input to bind
    Clock::time_point capture_deadline_;
    bool open_ = false;
    uint64_t layer_ = 0;

    std::vector<PhysicalInput> down_;            // physical inputs currently held
    std::array<int, kMenuActionCount> held_{};  // how many of them hold each action
    std::array<Clock::time_point, kMenuActionCount> next_repeat_{};
    std::array<std::array<bool, 2>, SDL_GAMEPAD_AXIS_COUNT> axis_on_{};  // [axis][negative, positive] pressed
};
