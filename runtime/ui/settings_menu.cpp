#include "settings_menu.h"

#include <aurora/input.hpp>
#include <aurora/rmlui.hpp>

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
// Holding a direction repeats it after REPEAT_DELAY, every REPEAT_INTERVAL.
constexpr auto REPEAT_DELAY = std::chrono::milliseconds(500);
constexpr auto REPEAT_INTERVAL = std::chrono::milliseconds(50);
constexpr float AXIS_PRESS = 0.5f, AXIS_RELEASE = 0.35f;  // stick/trigger deflection, with hysteresis

constexpr int WII_TABLE = 0, MENU_TABLE = 1;

bool is_direction(MenuAction a) {
    return a == MenuAction::Up || a == MenuAction::Down || a == MenuAction::Left || a == MenuAction::Right;
}

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
        row.RegisterMember("first", &Row::first);
        row.RegisterMember("second", &Row::second);
        row.RegisterMember("clear_first", &Row::clear_first);
        row.RegisterMember("clear_second", &Row::clear_second);
        row.RegisterMember("reset_first", &Row::reset_first);
        row.RegisterMember("index", &Row::index);
    }
    c.RegisterArray<std::vector<Row>>();
    c.Bind("wii_rows", &wii_rows_);
    c.Bind("menu_rows", &menu_rows_);
    c.Bind("tab", &tab_);
    c.Bind("device", &device_);
    c.Bind("capturing", &capturing_);
    c.BindFunc("prompt", [this](Rml::Variant& v) {
        v = Rml::String(device_ == "gamepad" ? "Press a button..." : "Press a key...");
    });
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
        "rumble", [this](Rml::Variant& v) { v = settings_.rumble; },
        [this](const Rml::Variant& v) {
            const int rumble = std::clamp((int)std::lround(v.Get<float>()), 0, 100);
            if (rumble != settings_.rumble) {
                settings_.rumble = rumble;
                changed();
            }
        });
    c.BindFunc(
        "mouse_pointer", [this](Rml::Variant& v) { v = settings_.mouse_pointer; },
        [this](const Rml::Variant& v) {
            if (v.Get<bool>() != settings_.mouse_pointer) {
                settings_.mouse_pointer = v.Get<bool>();
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
    c.BindEventCallback("set_device", [this](Rml::DataModelHandle h, Rml::Event&, const Rml::VariantList& args) {
        if (!args.empty()) {
            cancel_capture();
            device_ = args[0].Get<Rml::String>();
            h.DirtyVariable("device");
            h.DirtyVariable("prompt");
            refresh_rows();
        }
    });
    c.BindEventCallback("bind", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
        if (args.size() == 3)
            start_capture(args[0].Get<int>(), args[1].Get<int>(), args[2].Get<int>());
    });
    c.BindEventCallback("clear", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
        if (args.size() == 3)
            clear_mapping(args[0].Get<int>(), args[1].Get<int>(), args[2].Get<int>());
    });
    c.BindEventCallback("reset_controls", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        // Resets the device shown.
        const Bindings defaults = Bindings::defaults();
        const size_t dev = device_ == "gamepad" ? 0 : 1;
        for (size_t i = 0; i < kControlCount; i++)
            settings_.bindings.controls[i][dev] = defaults.controls[i][dev];
        for (size_t i = 0; i < kMenuActionCount; i++)
            settings_.bindings.menu[i][dev] = defaults.menu[i][dev];
        cancel_capture();
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
    pause_doc_ = ctx->LoadDocument(asset_dir + "/ui/pause.rml");
    // The layer sees every event (to open the menu with its toggle binding); it only captures
    // sources while the menu is open.
    layer_ = in::register_layer({
        .label = "settings menu",
        .priority = in::kRmlUiLayerPriority,
        .onEvent = &SettingsMenu::on_event,
        .capturesSource = &SettingsMenu::captures,
        .pointerMode = &SettingsMenu::pointer_mode,
        .userdata = this,
    });
    return true;
}

void SettingsMenu::open() {
    if (!doc_ || open_)
        return;
    open_ = true;
    // Nothing held before opening acts in the menu until it is pressed again.
    down_.clear();
    held_ = {};
    refresh_rows();
    doc_->Show();
    if (Rml::Element* first = doc_->GetElementById("tab-" + tab_))
        first->Focus(true);
    in::reconcile();
}

void SettingsMenu::close() {
    if (!open_)
        return;
    cancel_capture();
    open_ = false;
    doc_->Hide();
    in::reconcile();
}

void SettingsMenu::refresh() {
    model_.DirtyAllVariables();
    refresh_rows();
}

void SettingsMenu::set_paused(bool paused) {
    if (!pause_doc_)
        return;
    if (paused)
        pause_doc_->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    else
        pause_doc_->Hide();
}

void SettingsMenu::update() {
    const auto now = Clock::now();
    if (capturing_ >= 0 && now > capture_deadline_)
        cancel_capture();
    if (!open_ || capturing_ >= 0)
        return;
    for (size_t a = 0; a < kMenuActionCount; a++)
        if (held_[a] > 0 && is_direction((MenuAction)a) && now >= next_repeat_[a]) {
            action((MenuAction)a);
            next_repeat_[a] = std::max(next_repeat_[a] + REPEAT_INTERVAL, now);
        }
}

// --- Bindings tables --------------------------------------------------------------------------

Mappings& SettingsMenu::mappings(int table, int index) {
    return table == MENU_TABLE ? settings_.bindings.menu[index] : settings_.bindings.controls[index];
}

// Required actions (open/close, accept) cannot lose their first mapping: its button resets it to
// the default instead of clearing it.
bool SettingsMenu::is_reset(int table, int index, int slot) const {
    return table == MENU_TABLE && slot == 0 && menu_action_info((MenuAction)index).required;
}

// Whether the clear (or reset) button would change anything.
bool SettingsMenu::can_clear(int table, int index, int slot) {
    const size_t dev = device_ == "gamepad" ? 0 : 1;
    const PhysicalInput& current = mappings(table, index)[dev][slot];
    if (is_reset(table, index, slot))
        return current != Bindings::defaults().menu[index][dev][slot];
    return (bool)current;
}

void SettingsMenu::refresh_rows() {
    const size_t dev = device_ == "gamepad" ? 0 : 1;
    auto label = [](const PhysicalInput& p) { return p ? p.label() : Rml::String("-"); };
    auto build = [&](std::vector<Row>& rows, int table, size_t count) {
        rows.clear();
        for (size_t i = 0; i < count; i++) {
            const auto& m = mappings(table, (int)i)[dev];
            rows.push_back({table == MENU_TABLE ? menu_action_info((MenuAction)i).label : control_info((Control)i).label,
                            label(m[0]), label(m[1]), can_clear(table, (int)i, 0), can_clear(table, (int)i, 1),
                            is_reset(table, (int)i, 0), (int)i});
        }
    };
    build(wii_rows_, WII_TABLE, kControlCount);
    build(menu_rows_, MENU_TABLE, kMenuActionCount);
    model_.DirtyVariable("wii_rows");
    model_.DirtyVariable("menu_rows");
}

void SettingsMenu::changed() {
    if (on_change_)
        on_change_();
}

void SettingsMenu::start_capture(int table, int index, int slot) {
    const size_t count = table == MENU_TABLE ? kMenuActionCount : kControlCount;
    if (index < 0 || index >= (int)count || slot < 0 || slot >= (int)kMappingsPerDevice)
        return;
    capturing_ = table * 1000 + index * 10 + slot;
    capture_deadline_ = Clock::now() + CAPTURE_TIMEOUT;
    model_.DirtyVariable("capturing");
}

void SettingsMenu::finish_capture(const PhysicalInput& input) {
    const int table = capturing_ / 1000, index = capturing_ % 1000 / 10, slot = capturing_ % 10;
    mappings(table, index)[device_ == "gamepad" ? 0 : 1][slot] = input;
    LOG_DEBUG("ui", "bound %s %s mapping %d to %s", table == MENU_TABLE ? "menu" : "wii",
              table == MENU_TABLE ? menu_action_info((MenuAction)index).id : control_info((Control)index).id, slot + 1,
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

void SettingsMenu::clear_mapping(int table, int index, int slot) {
    if (!can_clear(table, index, slot))
        return;
    cancel_capture();
    const size_t dev = device_ == "gamepad" ? 0 : 1;
    mappings(table, index)[dev][slot] = is_reset(table, index, slot) ? Bindings::defaults().menu[index][dev][slot]
                                                                     : PhysicalInput{};
    refresh_rows();
    changed();
}

// --- Input ------------------------------------------------------------------------------------

in::EventResult SettingsMenu::on_event(const in::InputEvent& event, void* self) {
    return static_cast<SettingsMenu*>(self)->handle(event);
}

// While open, the menu takes every source so the game sees nothing through the router.
bool SettingsMenu::captures(const in::InputSource&, void* self) { return static_cast<SettingsMenu*>(self)->open_; }

// The OS cursor shows in the menu; in the game it is hidden while the mouse aims the pointer (the
// game draws its own).
in::PointerMode SettingsMenu::pointer_mode(void* self) {
    auto* menu = static_cast<SettingsMenu*>(self);
    if (menu->open_)
        return in::PointerMode::Visible;
    return menu->settings_.mouse_pointer ? in::PointerMode::Hidden : in::PointerMode::AutoHide;
}

std::vector<SettingsMenu::Edge> SettingsMenu::edges(const in::InputEvent& event) {
    std::vector<Edge> out;
    event.payload.match(
        [&](const in::InputEvent::KeyChanged& k) {
            if (!k.repeat)  // held keys repeat through the menu's own timer
                out.push_back({PhysicalInput::key(k.scancode), k.pressed});
        },
        [&](const in::InputEvent::ButtonChanged& b) { out.push_back({PhysicalInput::button(b.button), b.pressed}); },
        [&](const in::InputEvent::AxisChanged& a) {
            if (a.axis < 0 || a.axis >= SDL_GAMEPAD_AXIS_COUNT)
                return;
            // Each direction keeps its own pressed state: pressed past AXIS_PRESS, released below
            // AXIS_RELEASE. (Deriving it from the previous value lost releases when the stick came
            // back slowly through the band between the two.)
            for (int dir = 0; dir < 2; dir++) {
                const int sign = dir ? 1 : -1;
                bool& on = axis_on_[a.axis][dir];
                const bool now = a.value * (float)sign >= (on ? AXIS_RELEASE : AXIS_PRESS);
                if (now != on) {
                    on = now;
                    out.push_back({PhysicalInput::axis(a.axis, sign), now});
                }
            }
        },
        [&](const in::InputEvent::PointerChanged& p) {
            // The left button stays RmlUi's (clicking); the others can be bound.
            if (event.source.kind == in::InputSource::Kind::Mouse && p.button != 0 && p.button != SDL_BUTTON_LEFT &&
                (p.phase == in::InputEvent::PointerChanged::Phase::Down ||
                 p.phase == in::InputEvent::PointerChanged::Phase::Up))
                out.push_back({PhysicalInput::mouse(p.button), p.phase == in::InputEvent::PointerChanged::Phase::Down});
        },
        [](const auto&) {});
    return out;
}

// While rebinding, the next press of the shown device's kind becomes the mapping (including the
// left mouse button). Escape or the gamepad's Guide button cancels (they cannot be bound here; the
// reset button restores the defaults that use them).
bool SettingsMenu::capture(const in::InputEvent& event, const std::vector<Edge>& es) {
    if (const auto* k = event.payload.get_if<in::InputEvent::KeyChanged>(); k && k->pressed && k->scancode == SDL_SCANCODE_ESCAPE) {
        cancel_capture();
        return true;
    }
    if (const auto* b = event.payload.get_if<in::InputEvent::ButtonChanged>();
        b && b->pressed && b->button == SDL_GAMEPAD_BUTTON_GUIDE) {
        cancel_capture();
        return true;
    }
    const InputDevice device = device_ == "gamepad" ? InputDevice::Gamepad : InputDevice::KeyboardMouse;
    // edges() leaves the left button to RmlUi; here it can be bound.
    if (const auto* p = event.payload.get_if<in::InputEvent::PointerChanged>();
        p && device == InputDevice::KeyboardMouse && event.source.kind == in::InputSource::Kind::Mouse &&
        p->phase == in::InputEvent::PointerChanged::Phase::Down && p->button == SDL_BUTTON_LEFT) {
        finish_capture(PhysicalInput::mouse(SDL_BUTTON_LEFT));
        return true;
    }
    for (const Edge& e : es)
        if (e.pressed && e.input.device() == device) {
            finish_capture(e.input);
            break;
        }
    return true;
}

// Records presses and releases, then updates which actions are held (running an action when it
// is first pressed, if fire). True if any edge belongs to an action.
bool SettingsMenu::apply_actions(const std::vector<Edge>& es, bool fire) {
    bool matched = false;
    for (const Edge& e : es) {
        const auto it = std::find(down_.begin(), down_.end(), e.input);
        if (e.pressed && it == down_.end())
            down_.push_back(e.input);
        else if (!e.pressed && it != down_.end())
            down_.erase(it);
        for (const auto& action : settings_.bindings.menu) {
            const auto& m = action[(size_t)e.input.device()];
            matched = matched || std::find(m.begin(), m.end(), e.input) != m.end();
        }
    }
    update_held(fire);
    return matched;
}

void SettingsMenu::update_held(bool fire) {
    for (size_t a = 0; a < kMenuActionCount; a++) {
        int count = 0;
        for (const PhysicalInput& p : down_) {
            const auto& m = settings_.bindings.menu[a][(size_t)p.device()];
            count += (int)std::count(m.begin(), m.end(), p);
        }
        const bool pressed = held_[a] == 0 && count > 0;
        held_[a] = count;
        if (pressed && fire) {
            next_repeat_[a] = Clock::now() + REPEAT_DELAY;
            action((MenuAction)a);
        }
    }
}

// The router cancels a source when another layer starts or stops capturing it; its releases may
// never arrive (e.g. the key that opened the menu), so forget what that source held.
void SettingsMenu::release_source(in::InputSource::Kind kind) {
    std::erase_if(down_, [kind](const PhysicalInput& p) {
        switch (p.kind) {
        case PhysicalInput::Kind::Key: return kind == in::InputSource::Kind::Keyboard;
        case PhysicalInput::Kind::MouseButton: return kind == in::InputSource::Kind::Mouse;
        case PhysicalInput::Kind::Button:
        case PhysicalInput::Kind::Axis: return kind == in::InputSource::Kind::Controller;
        case PhysicalInput::Kind::None: break;
        }
        return false;
    });
    if (kind == in::InputSource::Kind::Controller)
        axis_on_ = {};
    update_held(false);
}

void SettingsMenu::action(MenuAction a) {
    using Key = Rml::Input::KeyIdentifier;
    if (!open_) {
        if (a == MenuAction::Toggle)
            open();
        return;
    }
    switch (a) {
    case MenuAction::Toggle: close(); break;
    case MenuAction::Cancel:  // back out to the current tab's button (the menu stays open)
        if (Rml::Element* tab = doc_->GetElementById("tab-" + tab_))
            tab->Focus(true);
        break;
    case MenuAction::Accept: navigate(Key::KI_RETURN); break;
    case MenuAction::Up: navigate(Key::KI_UP); break;
    case MenuAction::Down: navigate(Key::KI_DOWN); break;
    case MenuAction::Left: navigate(Key::KI_LEFT); break;
    case MenuAction::Right: navigate(Key::KI_RIGHT); break;
    case MenuAction::PrevTab: switch_tab(-1); break;
    case MenuAction::NextTab: switch_tab(1); break;
    case MenuAction::Count: break;
    }
}

in::EventResult SettingsMenu::handle(const in::InputEvent& event) {
    if (event.payload.is<in::InputEvent::Cancelled>()) {
        release_source(event.source.kind);
        if (open_)
            aurora::rmlui::process_input(event);
        return open_ ? in::EventResult::Consume : in::EventResult::Pass;
    }
    const std::vector<Edge> es = edges(event);
    if (open_ && capturing_ >= 0) {
        apply_actions(es, false);  // keep held state current without running actions
        capture(event, es);
        return in::EventResult::Consume;
    }
    const bool matched = apply_actions(es, true);
    if (!open_)
        return in::EventResult::Pass;
    const bool gamepad = event.payload.is<in::InputEvent::ButtonChanged>() || event.payload.is<in::InputEvent::AxisChanged>();
    if (!gamepad && !matched)
        aurora::rmlui::process_input(event);  // mouse, text, unbound keys
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
}

// RmlUi's spatial navigation does not enter or leave scroll containers, so the bindings tables
// (rows of four cells: mapping, clear, mapping, clear) are navigated here, and out to the device
// switch above and the reset button below.
bool SettingsMenu::navigate_bindings(int key) {
    Rml::Context* ctx = aurora::rmlui::get_context();
    Rml::Element* focus = ctx->GetFocusElement();
    if (!focus || tab_ != "controls")
        return false;
    Rml::ElementList rows;
    doc_->GetElementsByClassName(rows, "binding");
    // data-for keeps its template element (hidden) next to the rows it generates.
    std::erase_if(rows, [](Rml::Element* row) { return !row->IsVisible(); });
    if (rows.empty())
        return false;
    auto cell = [](Rml::Element* row, int column) -> Rml::Element* {
        Rml::ElementList cells;
        row->GetElementsByClassName(cells, "cell");
        if (cells.empty())
            return nullptr;
        column = std::clamp(column, 0, (int)cells.size() - 1);
        // A clear button that cannot clear is skipped in favour of its mapping.
        if (cells[column]->IsClassSet("disabled") && column > 0)
            column--;
        return cells[column];
    };
    auto go = [](Rml::Element* target) {
        if (!target)
            return false;
        target->Focus(true);
        target->ScrollIntoView(Rml::ScrollAlignment::Nearest);
        return true;
    };

    if (focus->IsClassSet("cell")) {
        Rml::Element* row_element = focus->GetParentNode();
        const auto it = std::find(rows.begin(), rows.end(), row_element);
        if (it == rows.end())
            return false;
        const int row = (int)(it - rows.begin());
        Rml::ElementList cells;
        row_element->GetElementsByClassName(cells, "cell");
        const int column = (int)(std::find(cells.begin(), cells.end(), focus) - cells.begin());
        switch (key) {
        case Rml::Input::KI_UP:
            return row > 0 ? go(cell(rows[row - 1], column)) : go(doc_->GetElementById("dev-" + device_));
        case Rml::Input::KI_DOWN:
            return row + 1 < (int)rows.size() ? go(cell(rows[row + 1], column)) : go(doc_->GetElementById("reset"));
        case Rml::Input::KI_LEFT:
            for (int c = column - 1; c >= 0; c--)
                if (!cells[c]->IsClassSet("disabled"))
                    return go(cells[c]);
            return true;
        case Rml::Input::KI_RIGHT:
            for (int c = column + 1; c < (int)cells.size(); c++)
                if (!cells[c]->IsClassSet("disabled"))
                    return go(cells[c]);
            return true;
        default: return false;
        }
    }
    if (key == Rml::Input::KI_DOWN && focus->IsClassSet("device"))
        return go(cell(rows.front(), 0));
    if (key == Rml::Input::KI_DOWN && focus->IsClassSet("tab"))
        return go(doc_->GetElementById("dev-" + device_));
    if (key == Rml::Input::KI_UP && focus->GetId() == "reset")
        return go(cell(rows.back(), 0));
    return false;
}

void SettingsMenu::switch_tab(int delta) {
    constexpr int count = (int)std::size(TABS);
    int i = 0;
    while (i < count && tab_ != TABS[i])
        i++;
    cancel_capture();
    tab_ = TABS[((i + delta) % count + count) % count];
    model_.DirtyVariable("tab");
    if (Rml::Element* tab = doc_->GetElementById("tab-" + tab_))
        tab->Focus(true);
}
