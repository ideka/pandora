#include "settings_menu.h"

#include <aurora/input.hpp>
#include <aurora/rmlui.hpp>
#include <SDL3/SDL_keyboard.h>

#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "../log.h"

namespace in = aurora::input;

namespace {

constexpr const char* TABS[] = {"controls", "sound", "general"};
constexpr auto CAPTURE_TIMEOUT = std::chrono::seconds(5);
constexpr float NAV_THRESHOLD = 0.5f;  // stick deflection that counts as a navigation press

}  // namespace

SettingsMenu::SettingsMenu(Settings& settings, std::function<void()> on_change)
    : settings_(settings), on_change_(std::move(on_change)) {}

SettingsMenu::~SettingsMenu() {
    if (layer_)
        in::unregister_layer(layer_);
}

bool SettingsMenu::init(const std::string& asset_dir) {
    if (!aurora::rmlui::is_initialized())
        return false;
    Rml::Context* ctx = aurora::rmlui::get_context();
    for (const char* font : {"LatoLatin-Regular.ttf", "LatoLatin-Bold.ttf"})
        if (!Rml::LoadFontFace(asset_dir + "/fonts/" + font))
            LOG_ERROR("ui", "cannot load font %s", font);

    Rml::DataModelConstructor c = ctx->CreateDataModel("settings");
    if (!c)
        return false;
    if (auto row = c.RegisterStruct<Row>()) {
        row.RegisterMember("label", &Row::label);
        row.RegisterMember("controller", &Row::controller);
        row.RegisterMember("keyboard", &Row::keyboard);
        row.RegisterMember("index", &Row::index);
    }
    c.RegisterArray<std::vector<Row>>();
    c.Bind("controls", &rows_);
    c.Bind("tab", &tab_);
    c.Bind("capturing", &capturing_);
    c.BindFunc(
        "volume", [this](Rml::Variant& v) { v = settings_.volume; },
        [this](const Rml::Variant& v) {
            const int volume = std::clamp((int)std::lround(v.Get<float>()), 0, 100);
            if (volume != settings_.volume) {
                settings_.volume = volume;
                changed();
            }
        });
    c.BindFunc(
        "muted", [this](Rml::Variant& v) { v = settings_.muted; },
        [this](const Rml::Variant& v) {
            if (v.Get<bool>() != settings_.muted) {
                settings_.muted = v.Get<bool>();
                changed();
            }
        });
    c.BindFunc(
        "fullscreen", [this](Rml::Variant& v) { v = settings_.fullscreen; },
        [this](const Rml::Variant& v) {
            if (v.Get<bool>() != settings_.fullscreen) {
                settings_.fullscreen = v.Get<bool>();
                changed();
            }
        });
    c.BindEventCallback("set_tab", [this](Rml::DataModelHandle h, Rml::Event&, const Rml::VariantList& args) {
        if (!args.empty()) {
            tab_ = args[0].Get<Rml::String>();
            h.DirtyVariable("tab");
        }
    });
    c.BindEventCallback("bind", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
        if (args.size() == 2)
            start_capture(args[0].Get<int>(), args[1].Get<int>());
    });
    c.BindEventCallback("reset_controls", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        settings_.bindings = Bindings::defaults();
        refresh_rows();
        changed();
    });
    c.BindEventCallback("close", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { close(); });
    model_ = c.GetModelHandle();
    refresh_rows();

    doc_ = ctx->LoadDocument(asset_dir + "/ui/menu.rml");
    if (!doc_) {
        LOG_ERROR("ui", "cannot load %s/ui/menu.rml", asset_dir.c_str());
        return false;
    }
    layer_ = in::register_layer({
        .label = "settings menu",
        .priority = in::kRmlUiLayerPriority,
        .onEvent = &SettingsMenu::on_event,
        .capturesSource = &SettingsMenu::captures,
        .userdata = this,
        .enabled = false,
    });
    return true;
}

void SettingsMenu::open() {
    if (!doc_ || open_)
        return;
    open_ = true;
    stick_[0] = stick_[1] = 0;
    refresh_rows();
    doc_->Show();
    if (Rml::Element* first = doc_->GetElementById("tab-" + tab_))
        first->Focus(true);
    in::set_layer_enabled(layer_, true);
    in::reconcile();
}

void SettingsMenu::close() {
    if (!open_)
        return;
    cancel_capture();
    open_ = false;
    doc_->Hide();
    in::set_layer_enabled(layer_, false);
    in::reconcile();
}

void SettingsMenu::update() {
    if (capturing_ >= 0 && std::chrono::steady_clock::now() > capture_deadline_)
        cancel_capture();
}

void SettingsMenu::refresh_rows() {
    rows_.clear();
    for (size_t c = 0; c < kControlCount; c++) {
        const auto& b = settings_.bindings.map[c];
        rows_.push_back({control_info((Control)c).label, b[0].label(), b[1].label(), (int)c});
    }
    model_.DirtyVariable("controls");
}

void SettingsMenu::changed() {
    if (on_change_)
        on_change_();
}

// --- Rebinding --------------------------------------------------------------------------------

void SettingsMenu::start_capture(int control, int slot) {
    if (control < 0 || control >= (int)kControlCount || slot < 0 || slot > 1)
        return;
    capturing_ = control * 2 + slot;
    LOG_DEBUG("ui", "rebinding %s (%s)", control_info((Control)control).id, slot ? "keyboard" : "controller");
    capture_deadline_ = std::chrono::steady_clock::now() + CAPTURE_TIMEOUT;
    model_.DirtyVariable("capturing");
}

void SettingsMenu::finish_capture(const PhysicalInput& input) {
    const int control = capturing_ / 2, slot = capturing_ % 2;
    settings_.bindings.map[control][slot] = input;
    LOG_DEBUG("ui", "bound %s (%s) to %s", control_info((Control)control).id, slot ? "keyboard" : "controller",
              input.to_string().c_str());
    cancel_capture();
    refresh_rows();
    changed();
}

void SettingsMenu::cancel_capture() {
    if (capturing_ < 0)
        return;
    capturing_ = -1;
    model_.DirtyVariable("capturing");
}

// The next press binds: a key for the keyboard column; a button or a stick/trigger direction for
// the controller column. Escape (or the controller's East button for the keyboard column) cancels.
bool SettingsMenu::handle_capture(const in::InputEvent& event) {
    const bool controller_slot = capturing_ % 2 == 0;
    return event.payload.match(
        [&](const in::InputEvent::KeyChanged& key) {
            if (!key.pressed || key.repeat)
                return true;
            if (key.scancode == SDL_SCANCODE_ESCAPE)
                cancel_capture();
            else if (!controller_slot)
                finish_capture(PhysicalInput::key(key.scancode));
            return true;
        },
        [&](const in::InputEvent::ButtonChanged& button) {
            if (!button.pressed)
                return true;
            if (controller_slot)
                finish_capture(PhysicalInput::button(button.button));
            else if (button.button == SDL_GAMEPAD_BUTTON_EAST)
                cancel_capture();
            return true;
        },
        [&](const in::InputEvent::AxisChanged& axis) {
            if (controller_slot && std::fabs(axis.value) >= 0.6f)
                finish_capture(PhysicalInput::axis(axis.axis, axis.value < 0 ? -1 : 1));
            return true;
        },
        [](const auto&) { return true; });
}

// --- Input ------------------------------------------------------------------------------------

in::EventResult SettingsMenu::on_event(const in::InputEvent& event, void* self) {
    return static_cast<SettingsMenu*>(self)->handle(event);
}

// While open, the menu takes every source so the paused game sees nothing.
bool SettingsMenu::captures(const in::InputSource&, void* self) { return static_cast<SettingsMenu*>(self)->open_; }

in::EventResult SettingsMenu::handle(const in::InputEvent& event) {
    if (!open_)
        return in::EventResult::Pass;
    if (capturing_ >= 0 && handle_capture(event))
        return in::EventResult::Consume;
    using Key = Rml::Input::KeyIdentifier;
    event.payload.match(
        [&](const in::InputEvent::ButtonChanged& b) {
            if (!b.pressed)
                return;
            switch (b.button) {
            case SDL_GAMEPAD_BUTTON_DPAD_UP: navigate(Key::KI_UP); break;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN: navigate(Key::KI_DOWN); break;
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT: navigate(Key::KI_LEFT); break;
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: navigate(Key::KI_RIGHT); break;
            case SDL_GAMEPAD_BUTTON_SOUTH: navigate(Key::KI_RETURN); break;
            case SDL_GAMEPAD_BUTTON_EAST:
            case SDL_GAMEPAD_BUTTON_START: close(); break;
            case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: switch_tab(-1); break;
            case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: switch_tab(1); break;
            default: break;  // Guide toggles the menu in the main loop
            }
        },
        [&](const in::InputEvent::AxisChanged& a) {
            const int i = a.axis == SDL_GAMEPAD_AXIS_LEFTX ? 0 : a.axis == SDL_GAMEPAD_AXIS_LEFTY ? 1 : -1;
            if (i < 0)
                return;
            const float before = stick_[i];
            stick_[i] = a.value;
            if (std::fabs(before) < NAV_THRESHOLD && std::fabs(a.value) >= NAV_THRESHOLD)
                navigate(i == 0 ? (a.value < 0 ? Key::KI_LEFT : Key::KI_RIGHT) : (a.value < 0 ? Key::KI_UP : Key::KI_DOWN));
        },
        [&](const in::InputEvent::KeyChanged& k) {
            if (k.keycode == SDLK_ESCAPE)
                return;  // toggles the menu in the main loop
            const int arrow = k.keycode == SDLK_LEFT    ? Key::KI_LEFT
                              : k.keycode == SDLK_RIGHT ? Key::KI_RIGHT
                              : k.keycode == SDLK_UP    ? Key::KI_UP
                              : k.keycode == SDLK_DOWN  ? Key::KI_DOWN
                                                        : -1;
            if (arrow >= 0) {
                if (k.pressed)
                    navigate(arrow);
                return;
            }
            aurora::rmlui::process_input(event);
        },
        [&](const auto&) { aurora::rmlui::process_input(event); });
    return in::EventResult::Consume;
}

// Sends a navigation key to the document. Left/right adjust a focused slider instead of moving.
void SettingsMenu::navigate(int key) {
    Rml::Context* ctx = aurora::rmlui::get_context();
    if (key == Rml::Input::KI_LEFT || key == Rml::Input::KI_RIGHT) {
        auto* input = dynamic_cast<Rml::ElementFormControlInput*>(ctx->GetFocusElement());
        if (input && input->GetAttribute<Rml::String>("type", "") == "range") {
            const float step = input->GetAttribute<float>("step", 1.0f);
            const float value = std::strtof(input->GetValue().c_str(), nullptr) + (key == Rml::Input::KI_LEFT ? -step : step);
            input->SetValue(Rml::ToString(std::clamp(value, input->GetAttribute<float>("min", 0.0f),
                                                     input->GetAttribute<float>("max", 100.0f))));
            return;
        }
    }
    if (!navigate_bindings(key)) {
        ctx->ProcessKeyDown((Rml::Input::KeyIdentifier)key, 0);
        ctx->ProcessKeyUp((Rml::Input::KeyIdentifier)key, 0);
    }
    if (Rml::Element* focus = ctx->GetFocusElement())
        LOG_DEBUG("ui", "key %d -> focus %s", key, focus->GetAddress(false, false).c_str());
}

// RmlUi's spatial navigation does not enter or leave scroll containers, so the bindings list
// (a grid of two columns) is navigated here: up/down by row, left/right by column, and out to the
// tabs above and the reset button below.
bool SettingsMenu::navigate_bindings(int key) {
    Rml::Context* ctx = aurora::rmlui::get_context();
    Rml::Element* focus = ctx->GetFocusElement();
    if (!focus || tab_ != "controls")
        return false;
    Rml::ElementList rows;
    doc_->GetElementsByClassName(rows, "binding");
    if (rows.empty())
        return false;
    auto bind_button = [](Rml::Element* row, int column) -> Rml::Element* {
        Rml::ElementList buttons;
        row->GetElementsByClassName(buttons, "bind");
        return column < (int)buttons.size() ? buttons[column] : nullptr;
    };
    auto go = [](Rml::Element* target) {
        if (!target)
            return false;
        target->Focus(true);
        target->ScrollIntoView(Rml::ScrollAlignment::Nearest);
        return true;
    };

    if (focus->IsClassSet("bind")) {
        const auto it = std::find(rows.begin(), rows.end(), focus->GetParentNode());
        if (it == rows.end())
            return false;
        const int row = (int)(it - rows.begin());
        const int column = bind_button(rows[row], 0) == focus ? 0 : 1;
        switch (key) {
        case Rml::Input::KI_UP:
            return row > 0 ? go(bind_button(rows[row - 1], column)) : go(doc_->GetElementById("tab-" + tab_));
        case Rml::Input::KI_DOWN:
            return row + 1 < (int)rows.size() ? go(bind_button(rows[row + 1], column))
                                              : go(doc_->GetElementById("reset"));
        case Rml::Input::KI_LEFT: return column == 1 ? go(bind_button(rows[row], 0)) : true;
        case Rml::Input::KI_RIGHT: return column == 0 ? go(bind_button(rows[row], 1)) : true;
        default: return false;
        }
    }
    if (key == Rml::Input::KI_DOWN && focus->IsClassSet("tab"))
        return go(bind_button(rows.front(), 0));
    if (key == Rml::Input::KI_UP && focus->GetId() == "reset")
        return go(bind_button(rows.back(), 0));
    return false;
}

void SettingsMenu::switch_tab(int delta) {
    constexpr int count = (int)std::size(TABS);
    int i = 0;
    while (i < count && tab_ != TABS[i])
        i++;
    tab_ = TABS[((i + delta) % count + count) % count];
    model_.DirtyVariable("tab");
    if (Rml::Element* tab = doc_->GetElementById("tab-" + tab_))
        tab->Focus(true);
}
