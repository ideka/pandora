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

constexpr const char* TABS[] = {"controls", "sound", "graphics", "general"};
constexpr auto CAPTURE_TIMEOUT = std::chrono::seconds(5);
// Holding a direction repeats it after REPEAT_DELAY, every REPEAT_INTERVAL.
constexpr auto REPEAT_DELAY = std::chrono::milliseconds(500);
constexpr auto REPEAT_INTERVAL = std::chrono::milliseconds(50);
constexpr float AXIS_PRESS = 0.5f, AXIS_RELEASE = 0.35f;  // stick/trigger deflection, with hysteresis

constexpr int WII_TABLE = 0, MENU_TABLE = 1;

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
    if (auto dot = c.RegisterStruct<Dot>()) {
        dot.RegisterMember("left", &Dot::left);
        dot.RegisterMember("top", &Dot::top);
        dot.RegisterMember("kind", &Dot::kind);
        dot.RegisterMember("size", &Dot::size);
        dot.RegisterMember("offset", &Dot::offset);
    }
    c.RegisterArray<std::vector<Dot>>();
    c.Bind("ir_dots", &ir_dots_);
    c.Bind("ir_status", &ir_status_);
    c.BindFunc(
        "ir_merge", [this](Rml::Variant& v) { v = settings_.ir_merge; },
        [this](const Rml::Variant& v) {
            const int px = std::clamp((int)std::lround(v.Get<float>()), 0, 200);
            if (px != settings_.ir_merge) {
                settings_.ir_merge = px;
                changed();
            }
        });
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
    c.Bind("remote_status", &remote_status_);
    // Anisotropic filtering as a slider position 0-4 (1x-16x).
    c.BindFunc(
        "anisotropy_step",
        [this](Rml::Variant& v) {
            int step = 0;
            while (step < 4 && (2 << step) <= settings_.anisotropy)
                step++;
            v = step;
        },
        [this](const Rml::Variant& v) {
            const int level = 1 << std::clamp((int)std::lround(v.Get<float>()), 0, 4);
            if (level != settings_.anisotropy) {
                settings_.anisotropy = level;
                model_.DirtyVariable("anisotropy");
                changed();
            }
        });
    c.BindFunc("anisotropy", [this](Rml::Variant& v) { v = settings_.anisotropy; });
    c.BindFunc(
        "msaa", [this](Rml::Variant& v) { v = settings_.msaa > 1; },
        [this](const Rml::Variant& v) {
            const int samples = v.Get<bool>() ? 4 : 1;
            if (samples != settings_.msaa) {
                settings_.msaa = samples;
                changed();
            }
        });
    c.BindFunc(
        "keep_quicksaves", [this](Rml::Variant& v) { v = settings_.keep_quicksaves; },
        [this](const Rml::Variant& v) {
            if (v.Get<bool>() != settings_.keep_quicksaves) {
                settings_.keep_quicksaves = v.Get<bool>();
                changed();
            }
        });
    c.BindFunc(
        "ghost_filter", [this](Rml::Variant& v) { v = settings_.ghost_filter; },
        [this](const Rml::Variant& v) {
            if (v.Get<bool>() != settings_.ghost_filter) {
                settings_.ghost_filter = v.Get<bool>();
                changed();
            }
        });
    c.BindFunc(
        "classic_controller", [this](Rml::Variant& v) { v = settings_.classic_controller; },
        [this](const Rml::Variant& v) {
            if (v.Get<bool>() != settings_.classic_controller) {
                settings_.classic_controller = v.Get<bool>();
                cancel_capture();
                refresh_rows();
                changed();
            }
        });
    c.BindFunc(
        "free_stick", [this](Rml::Variant& v) { v = settings_.free_stick; },
        [this](const Rml::Variant& v) {
            if (v.Get<bool>() != settings_.free_stick) {
                settings_.free_stick = v.Get<bool>();
                changed();
            }
        });
    c.BindFunc(
        "fps60", [this](Rml::Variant& v) { v = settings_.fps60; },
        [this](const Rml::Variant& v) {
            if (v.Get<bool>() != settings_.fps60) {
                settings_.fps60 = v.Get<bool>();
                changed();
            }
        });
    c.BindFunc(
        "wiimote_scan", [this](Rml::Variant& v) { v = settings_.wiimote_scan; },
        [this](const Rml::Variant& v) {
            if (v.Get<bool>() != settings_.wiimote_scan) {
                settings_.wiimote_scan = v.Get<bool>();
                changed();
            }
        });
    c.BindFunc(
        "sensor_bar_above", [this](Rml::Variant& v) { v = settings_.sensor_bar_above; },
        [this](const Rml::Variant& v) {
            if (v.Get<bool>() != settings_.sensor_bar_above) {
                settings_.sensor_bar_above = v.Get<bool>();
                changed();
            }
        });
    c.BindFunc(
        "ir_sensitivity", [this](Rml::Variant& v) { v = settings_.ir_sensitivity; },
        [this](const Rml::Variant& v) {
            const int s = std::clamp((int)std::lround(v.Get<float>()), 1, 5);
            if (s != settings_.ir_sensitivity) {
                settings_.ir_sensitivity = s;
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
    c.BindEventCallback("open_calibration",
                        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { open_calibration(); });
    c.BindEventCallback("close_calibration",
                        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { close_calibration(); });
    model_ = c.GetModelHandle();
    refresh_rows();

    doc_ = ctx->LoadDocument(asset_dir + "/ui/menu.rml");
    if (!doc_) {
        LOG_ERROR("ui", "cannot load %s/ui/menu.rml", asset_dir.c_str());
        return false;
    }
    pause_doc_ = ctx->LoadDocument(asset_dir + "/ui/pause.rml");
    calibration_doc_ = ctx->LoadDocument(asset_dir + "/ui/calibration.rml");
    if (!calibration_doc_)
        LOG_ERROR("ui", "cannot load %s/ui/calibration.rml", asset_dir.c_str());
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
    for (size_t a = 0; a < kMenuActionCount; a++)
        held_[a] = remote_held_[a] ? 1 : 0;
    refresh_rows();
    doc_->Show();
    // Back where the cursor was when the menu closed, if that element still exists (rows are only
    // regenerated when their number changes); else the current tab.
    Rml::Element* target = nullptr;
    if (last_focus_) {
        std::vector<Rml::Element*> stack{doc_};
        while (!stack.empty() && !target) {
            Rml::Element* e = stack.back();
            stack.pop_back();
            if (e == last_focus_)
                target = e;
            for (int i = 0; i < e->GetNumChildren(); i++)
                stack.push_back(e->GetChild(i));
        }
    }
    if (target && target->IsVisible()) {
        target->Focus(true);
        target->ScrollIntoView(Rml::ScrollAlignment::Nearest);
    } else if (Rml::Element* first = doc_->GetElementById("tab-" + tab_)) {
        first->Focus(true);
    }
    in::reconcile();
}

void SettingsMenu::close() {
    if (!open_)
        return;
    cancel_capture();
    if (calibration_doc_ && calibration_doc_->IsVisible())
        close_calibration();
    open_ = false;
    Rml::Element* focus = aurora::rmlui::get_context()->GetFocusElement();
    last_focus_ = focus && focus != doc_ && focus->GetOwnerDocument() == doc_ ? focus : nullptr;
    doc_->Hide();
    in::reconcile();
}

void SettingsMenu::refresh() {
    model_.DirtyAllVariables();
    refresh_rows();
}

void SettingsMenu::set_remote_buttons(u32 buttons) {
    static constexpr std::pair<MenuAction, u32> MAP[] = {
        {MenuAction::Toggle, WPAD_HOME},  {MenuAction::Accept, WPAD_A},   {MenuAction::Cancel, WPAD_B},
        {MenuAction::Up, WPAD_UP},        {MenuAction::Down, WPAD_DOWN},  {MenuAction::Left, WPAD_LEFT},
        {MenuAction::Right, WPAD_RIGHT},  {MenuAction::PrevTab, WPAD_MINUS}, {MenuAction::NextTab, WPAD_PLUS},
    };
    bool changed = false;
    for (const auto& [a, bit] : MAP) {
        const bool held = buttons & bit;
        changed = changed || held != remote_held_[(size_t)a];
        remote_held_[(size_t)a] = held;
    }
    if (changed && capturing_ < 0)
        update_held(true);
}

void SettingsMenu::set_remote_status(std::string status) {
    if (status != remote_status_) {
        remote_status_ = std::move(status);
        model_.DirtyVariable("remote_status");
    }
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
    const float seconds = last_update_ == Clock::time_point{} ? 0.0f
                                                              : std::chrono::duration<float>(now - last_update_).count();
    last_update_ = now;
    if (open_) {
        mark_focused_row();
        scroll(std::min(seconds, 0.1f));
    }
    if (!open_ || capturing_ >= 0)
        return;
    for (size_t a = 0; a < kMenuActionCount; a++)
        if (held_[a] > 0 && is_menu_direction((MenuAction)a) && now >= next_repeat_[a]) {
            action((MenuAction)a);
            next_repeat_[a] = std::max(next_repeat_[a] + REPEAT_INTERVAL, now);
        }
}

// The settings row holding the focused control gets the "focused" class (RmlUi has no
// :focus-within), so sliders and checkboxes show where the cursor is.
void SettingsMenu::mark_focused_row() {
    Rml::Element* row = aurora::rmlui::get_context()->GetFocusElement();
    while (row && !row->IsClassSet("setting"))
        row = row->GetParentNode();
    Rml::ElementList marked;
    doc_->GetElementsByClassName(marked, "focused");
    if (calibration_doc_)
        calibration_doc_->GetElementsByClassName(marked, "focused");
    for (Rml::Element* e : marked)
        if (e != row)
            e->SetClass("focused", false);
    if (row)
        row->SetClass("focused", true);
}

// The right stick scrolls the bindings table (faster the further it is pushed); the focus stays.
void SettingsMenu::scroll(float seconds) {
    constexpr float DEAD_ZONE = 0.2f, SPEED = 1400.0f;  // dp per second at full deflection
    const float a = std::abs(scroll_axis_);
    if (a < DEAD_ZONE || tab_ != "controls")
        return;
    if (Rml::Element* bindings = doc_->GetElementById("bindings")) {
        const float t = (a - DEAD_ZONE) / (1.0f - DEAD_ZONE);
        const float dp = aurora::rmlui::get_context()->GetDensityIndependentPixelRatio();
        bindings->SetScrollTop(bindings->GetScrollTop() + std::copysign(t * t * SPEED * dp * seconds, scroll_axis_));
    }
}

// --- Pointer calibration ----------------------------------------------------------------------

void SettingsMenu::open_calibration() {
    if (!calibration_doc_)
        return;
    calibration_doc_->Show(Rml::ModalFlag::Modal, Rml::FocusFlag::None);
    if (Rml::Element* first = calibration_doc_->GetElementById("calibration-merge"))
        first->Focus(true);
}

void SettingsMenu::close_calibration() {
    if (!calibration_doc_ || !calibration_doc_->IsVisible())
        return;
    calibration_doc_->Hide();
    if (Rml::Element* button = doc_->GetElementById("calibrate"))
        button->Focus(true);
}

// The camera's view as dots on a 4:3 box (CAMERA_DP wide, as calibration.rml sizes it): the raw
// dots with the merge distance around each, the dots after merging (when merging changed
// anything) and the two bar ends tracking chose.
void SettingsMenu::set_ir_view(bool connected, const Wiimote::IrView& view) {
    if (!calibration_doc_ || !calibration_doc_->IsVisible())
        return;
    constexpr float CAMERA_DP = 400.0f;
    std::vector<Dot> dots;
    auto add = [&](float x, float y, const char* kind, float size_dp) {
        if (x < 0 || x > 1 || y < 0 || y > 1)
            return;  // an inferred end off the image
        dots.push_back({Rml::ToString(x * 100.0f) + "%", Rml::ToString(y * 100.0f) + "%", kind,
                        Rml::ToString(size_dp) + "dp", Rml::ToString(-size_dp / 2.0f) + "dp"});
    };
    if (connected) {
        if (view.merge > 0)
            for (int i = 0; i < view.raw; i++)
                add(view.raw_x[i], view.raw_y[i], "radius", 2.0f * view.merge * CAMERA_DP);
        for (int i = 0; i < view.raw; i++)
            add(view.raw_x[i], view.raw_y[i], "raw", 12.0f);
        if (view.merged != view.raw)
            for (int i = 0; i < view.merged; i++)
                add(view.merged_x[i], view.merged_y[i], "merged", 18.0f);
        for (int i = 0; i < view.ends; i++)
            add(view.end_x[i], view.end_y[i], "end", 26.0f);
    }
    std::string status;
    if (!connected)
        status = "No Wii Remote connected";
    else if (view.raw == 0)
        status = "No dots in view: point the Wii Remote at the sensor bar";
    else
        status = std::to_string(view.raw) + (view.raw == 1 ? " dot" : " dots") + " seen" +
                 (view.merged != view.raw ? ", " + std::to_string(view.merged) + " after merging" : "") +
                 (view.ends == 2 ? "; tracking both ends" : view.ends == 1 ? "; tracking one end" : "; not tracking");
    if (status != ir_status_) {
        ir_status_ = status;
        model_.DirtyVariable("ir_status");
    }
    const bool same = dots.size() == ir_dots_.size() &&
                      std::equal(dots.begin(), dots.end(), ir_dots_.begin(), [](const Dot& a, const Dot& b) {
                          return a.left == b.left && a.top == b.top && a.kind == b.kind && a.size == b.size;
                      });
    if (!same) {
        ir_dots_ = std::move(dots);
        model_.DirtyVariable("ir_dots");
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
            // The bindings table shows the controller in use (Nunchuk or Classic).
            if (table == WII_TABLE && is_classic_control((Control)i) != settings_.classic_controller)
                continue;
            if (table == MENU_TABLE && is_menu_direction((MenuAction)i))
                continue;
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
    model_.DirtyVariable("classic_controller");
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
        count += remote_held_[a] ? 1 : 0;
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
    if (kind == in::InputSource::Kind::Controller) {
        axis_on_ = {};
        scroll_axis_ = 0;
    }
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
        if (calibration_doc_ && calibration_doc_->IsVisible()) {
            close_calibration();
            break;
        }
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
    if (const auto* a = event.payload.get_if<in::InputEvent::AxisChanged>(); a && a->axis == SDL_GAMEPAD_AXIS_RIGHTY)
        scroll_axis_ = a->value;
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
    Rml::Element* before = ctx->GetFocusElement();
    auto in_row = [](Rml::Element* e, const char* cls) { return e && e->IsClassSet(cls); };
    // Left/right along the tabs and the device switch stop at the row's ends (spatial navigation
    // would leave it, e.g. from the last tab to the Back to game button).
    for (const char* cls : {"tab", "device"})
        if ((key == Rml::Input::KI_LEFT || key == Rml::Input::KI_RIGHT) && in_row(before, cls)) {
            Rml::ElementList row;
            before->GetParentNode()->GetElementsByClassName(row, cls);
            const auto it = std::find(row.begin(), row.end(), before);
            const int i = (int)(it - row.begin()) + (key == Rml::Input::KI_LEFT ? -1 : 1);
            if (it != row.end() && i >= 0 && i < (int)row.size())
                row[i]->Focus(true);
            return;
        }
    // Up from Back to game goes to the page's last control (spatial navigation would pick whatever
    // is nearest above it, often the tabs).
    if (key == Rml::Input::KI_UP && before && before->GetId() == "back") {
        if (Rml::Element* last = last_page_control()) {
            last->Focus(true);
            last->ScrollIntoView(Rml::ScrollAlignment::Nearest);
        }
        return;
    }
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
    // Entering the tabs or the device switch from elsewhere lands on the active one, not the nearest.
    Rml::Element* after = ctx->GetFocusElement();
    if (in_row(after, "tab") && !in_row(before, "tab")) {
        if (Rml::Element* tab = doc_->GetElementById("tab-" + tab_))
            tab->Focus(true);
    } else if (in_row(after, "device") && !in_row(before, "device")) {
        if (Rml::Element* device = doc_->GetElementById("dev-" + device_))
            device->Focus(true);
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
    Rml::Element* table = doc_->GetElementById("bindings");
    auto go = [&](Rml::Element* target) {
        if (!target)
            return false;
        target->Focus(true);
        // A row keeps some distance from the table's visible edges (20% of its height), so the
        // view scrolls ahead of the cursor: near the top or bottom it reaches the table's ends.
        if (table && target->IsClassSet("cell")) {
            constexpr float MARGIN = 0.2f;
            Rml::Element* row = target->GetParentNode();
            const float view = table->GetClientHeight(), margin = view * MARGIN;
            const float y = row->GetAbsoluteOffset(Rml::BoxArea::Border).y -
                            table->GetAbsoluteOffset(Rml::BoxArea::Padding).y;  // within the view
            const float h = row->GetOffsetHeight();
            if (y < margin)
                table->SetScrollTop(table->GetScrollTop() - (margin - y));
            else if (y + h > view - margin)
                table->SetScrollTop(table->GetScrollTop() + (y + h - (view - margin)));
            return true;
        }
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

// The current page's last focusable control (the reset button on Controls).
Rml::Element* SettingsMenu::last_page_control() {
    if (tab_ == "controls")
        return doc_->GetElementById("reset");
    Rml::ElementList pages;
    doc_->GetElementsByClassName(pages, "page");
    for (Rml::Element* page : pages) {
        if (!page->IsVisible())
            continue;
        Rml::Element* last = nullptr;
        std::vector<Rml::Element*> stack{page};
        while (!stack.empty()) {  // depth first, in document order
            Rml::Element* e = stack.back();
            stack.pop_back();
            if (e != page && (e->GetTagName() == "input" || e->GetTagName() == "button") && e->IsVisible())
                last = e;
            for (int i = e->GetNumChildren() - 1; i >= 0; i--)
                stack.push_back(e->GetChild(i));
        }
        return last;
    }
    return nullptr;
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
