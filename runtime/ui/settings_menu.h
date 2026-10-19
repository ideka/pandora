// In-game settings menu (RmlUi through aurora): controls, sound and general settings. It opens
// over the paused game and works with mouse, keyboard and controller. The layout and style live in
// assets/ui (menu.rml, menu.rcss).
#pragma once

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

    // Loads fonts and the document from asset_dir. False if RmlUi is unavailable.
    bool init(const std::string& asset_dir);

    bool is_open() const { return open_; }
    void open();
    void close();
    void toggle() { open_ ? close() : open(); }

    // Once per host frame.
    void update();
    // Settings changed outside the menu (e.g. the fullscreen hotkey).
    void refresh() { model_.DirtyAllVariables(); }

    // One row of the controls table, as the document sees it.
    struct Row {
        Rml::String label, controller, keyboard;
        int index = 0;
    };

private:
    static aurora::input::EventResult on_event(const aurora::input::InputEvent& event, void* self);
    static bool captures(const aurora::input::InputSource& source, void* self);
    aurora::input::EventResult handle(const aurora::input::InputEvent& event);
    bool handle_capture(const aurora::input::InputEvent& event);
    void navigate(int key);  // an Rml::Input::KeyIdentifier
    bool navigate_bindings(int key);
    void switch_tab(int delta);
    void start_capture(int control, int slot);
    void finish_capture(const PhysicalInput& input);
    void cancel_capture();
    void refresh_rows();
    void changed();

    Settings& settings_;
    std::function<void()> on_change_;
    Rml::ElementDocument* doc_ = nullptr;
    Rml::DataModelHandle model_;
    std::vector<Row> rows_;
    Rml::String tab_ = "controls";
    int capturing_ = -1;  // control * 2 + slot while waiting for an input to bind
    std::chrono::steady_clock::time_point capture_deadline_;
    bool open_ = false;
    uint64_t layer_ = 0;
    float stick_[2] = {};  // left stick position, for navigation edges
};
