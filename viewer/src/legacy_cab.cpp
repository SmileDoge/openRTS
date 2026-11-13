#include "legacy_cab.hpp"

#include <glm/geometric.hpp>
#include <algorithm>
#include <cctype>
#include <cstdio>

#ifdef OPENRTS_HAS_GLFW
#include <GLFW/glfw3.h>
#endif
#ifdef OPENRTS_HAS_IMGUI
#include <imgui.h>
#endif

#include "ui_text.hpp"

namespace openrts::viewer {

namespace {

std::string lower(std::string_view s) {
    std::string r(s);
    for (auto& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

// The folder holding rts1.exe (bass.dll and CC3260MT.DLL, which some DLLs
// import, live there): the content root or its RTS subfolder.
std::filesystem::path rts_program_folder(const core::Vfs& vfs, const std::vector<LegacyCarRef>& cars) {
    for (const auto& car : cars) {
        if (!car.definition) continue;
        const auto path = formats::wagon_path(car.folder, car.definition->name, car.definition->engine.has_value());
        const auto real = vfs.real_path(path, std::filesystem::temp_directory_path() / "openrts");
        if (!real) continue;
        // <root>/TRAINS/TRAINSET/<folder>/<file>
        const auto root = real->parent_path().parent_path().parent_path().parent_path();
        std::error_code ec;
        for (const auto& candidate : {root, root / "RTS"}) {
            if (std::filesystem::exists(candidate / "rts1.exe", ec)) return candidate;
        }
        return root;
    }
    return {};
}

} // namespace

CabKey parse_cab_key(std::string_view text) {
    CabKey out;
#ifdef OPENRTS_HAS_GLFW
    std::string t = lower(text);
    // Modifiers: "shift+", "ctrl+" (and the "shiht+" some cabs carry).
    for (;;) {
        if (t.rfind("shift+", 0) == 0 || t.rfind("shiht+", 0) == 0) {
            out.shift = true;
            t.erase(0, 6);
        } else if (t.rfind("ctrl+", 0) == 0) {
            out.ctrl = true;
            t.erase(0, 5);
        } else {
            break;
        }
    }
    if (t == "space") {
        out.key = GLFW_KEY_SPACE;
    } else if (t.size() == 1) {
        const char c = t[0];
        if (c >= 'a' && c <= 'z') out.key = GLFW_KEY_A + (c - 'a');
        else if (c >= '0' && c <= '9') out.key = GLFW_KEY_0 + (c - '0');
        else if (c == '\'') out.key = GLFW_KEY_APOSTROPHE;
        else if (c == ';') out.key = GLFW_KEY_SEMICOLON;
        else if (c == '[') out.key = GLFW_KEY_LEFT_BRACKET;
        else if (c == ']') out.key = GLFW_KEY_RIGHT_BRACKET;
        else if (c == '/') out.key = GLFW_KEY_SLASH;
        else if (c == '\\') out.key = GLFW_KEY_BACKSLASH;
        else if (c == ',') out.key = GLFW_KEY_COMMA;
        else if (c == '.') out.key = GLFW_KEY_PERIOD;
        else if (c == '-') out.key = GLFW_KEY_MINUS;
        else if (c == '=') out.key = GLFW_KEY_EQUAL;
        else if (c == '`') out.key = GLFW_KEY_GRAVE_ACCENT;
        else if (c == '<') { out.key = GLFW_KEY_COMMA; out.shift = true; }
        else if (c == '>') { out.key = GLFW_KEY_PERIOD; out.shift = true; }
    }
#else
    (void)text;
#endif
    return out;
}

std::unique_ptr<LegacyCab> LegacyCab::attach(const core::Vfs& vfs, const std::vector<LegacyCarRef>& cars,
                                             simulation::Train& train, std::string& status, const int start_state,
                                             const core::VirtualPath* route, const bool fresh) {
    status.clear();
    bool any = false;
    for (const auto& c : cars)
        if (c.definition && c.definition->engine && !c.definition->engine->library_module.empty()) any = true;
    if (!any) return nullptr;
    std::vector<trainplugin::LegacyCar> legacy;
    for (const auto& c : cars) {
        trainplugin::LegacyCar l;
        l.definition = c.definition;
        l.uid = c.uid;
        l.key = c.key;
        if (c.definition) l.file = formats::wagon_path(c.folder, c.definition->name, c.definition->engine.has_value());
        legacy.push_back(std::move(l));
    }
    std::string error;
    auto consist = trainplugin::LegacyConsist::create(vfs, std::filesystem::temp_directory_path() / "openrts", legacy,
                                                      rts_program_folder(vfs, cars), &error, {}, route);
    if (!consist) {
        status = error;
        return nullptr;
    }
    std::unique_ptr<LegacyCab> cab(new LegacyCab());
    std::size_t player = SIZE_MAX;
    for (std::size_t i = 0; i < cars.size(); ++i) {
        if (!consist->has_dll(i)) continue;
        train.set_controller(i, consist->controller(i));
        if (player == SIZE_MAX) player = i;
    }
    // A train just made takes its air from Init, as in RTS.
    const bool entered = fresh ? trainplugin::enter_new_train(*consist, player, train, true, start_state)
                               : consist->enter(player, train, true, start_state);
    if (!entered) {
        status = "enter: " + consist->error() + "\n" + consist->take_messages();
        return nullptr;
    }
    for (const auto& m : consist->modules()) {
        if (m.status != 0) status += "refused: " + trainplugin::utf16_to_utf8(m.path) + "\n";
    }
    cab->consist_ = std::move(consist);
    cab->bind_keys(player);
    cab->log(cab->consist_->take_messages());
    return cab;
}

void LegacyCab::bind_keys(const std::size_t car) {
    bindings_.clear();
    const auto* sd = consist_ ? consist_->cab(car) : nullptr;
    if (!sd) return;
    for (const auto& s : sd->switches) {
        for (std::size_t k = 0; k < s.keys.size() && k < 2; ++k) {
            const auto key = parse_cab_key(s.keys[k]);
            if (!key.key) continue;
            const int direction = s.keys.size() >= 2 ? (k == 0 ? 1 : -1) : 0;
            bindings_.push_back({s.id, key.key, key.shift, key.ctrl, direction});
        }
    }
}

namespace {

std::vector<trainplugin::LegacyCar> legacy_cars(const std::vector<LegacyCarRef>& cars) {
    std::vector<trainplugin::LegacyCar> legacy;
    for (const auto& c : cars) {
        trainplugin::LegacyCar l;
        l.definition = c.definition;
        l.uid = c.uid;
        l.key = c.key;
        if (c.definition) l.file = formats::wagon_path(c.folder, c.definition->name, c.definition->engine.has_value());
        legacy.push_back(std::move(l));
    }
    return legacy;
}

} // namespace

bool LegacyCab::reconsist(const core::Vfs& vfs, const std::vector<std::vector<LegacyCarRef>>& trains,
                          const std::vector<simulation::Train*>& sims) {
    if (!consist_ || trains.empty() || sims.size() != trains.size() || !sims[0]) return false;
    std::vector<std::vector<trainplugin::LegacyCar>> legacy;
    for (const auto& cars : trains) legacy.push_back(legacy_cars(cars));
    std::vector<const simulation::Train*> views(sims.begin(), sims.end());
    const auto before = consist_->player();
    const bool ok = consist_->reconsist(vfs, std::move(legacy), views);
    log(consist_->take_messages());
    if (!ok) return false;
    auto& train = *sims[0];
    const auto& cars = trains[0];
    // What the mouse holds in the player's cab stays held (at its car's new
    // place); in another car it is let go.
    const auto player = consist_->player();
    if (held_ && held_->first == before) held_->first = player; else held_.reset();
    if (drag_ && drag_->car == before) drag_->car = player; else drag_.reset();
    std::map<std::uint64_t, std::uint8_t> going_down;
    for (const auto& [k, v] : going_down_)
        if ((k >> 32) == before) going_down[(static_cast<std::uint64_t>(player) << 32) | (k & 0xFFFFFFFFu)] = v;
    going_down_ = std::move(going_down);
    for (std::size_t i = 0; i < cars.size(); ++i)
        if (consist_->has_dll(i)) train.set_controller(i, consist_->controller(i));
    if (player < cars.size()) bind_keys(player);
    return true;
}

bool LegacyCab::enter(const std::size_t car, const simulation::Train& train, const int state) {
    if (!consist_ || !consist_->has_dll(car) || car == consist_->player()) return false;
    release_keys();
    held_.reset();
    drag_.reset();
    const bool ok = consist_->enter(car, train, state < 0, state);
    log(consist_->take_messages());
    if (!ok) return false;
    bind_keys(car);
    return true;
}

void LegacyCab::log(std::string line) {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    if (line.empty()) return;
    log_.push_back(std::move(line));
    while (log_.size() > 40) log_.pop_front();
}

const std::array<float, 8>* LegacyCab::sound_variables(std::size_t car) const {
    const auto* out = consist_ ? consist_->output(car) : nullptr;
    return out && out->has_engine ? &out->sms_ext : nullptr;
}

const std::array<float, 8>* LegacyCab::cab_sound_variables(std::size_t car) const {
    const auto* out = consist_ ? consist_->output(car) : nullptr;
    return out && out->has_engine ? &out->sms_cab : nullptr;
}

const std::array<float, 8>* LegacyCab::aux_sound_variables(std::size_t car, std::size_t library) const {
    return aux_sound_variables(0, car, library);
}

const std::array<float, 8>* LegacyCab::sound_variables(std::size_t train, std::size_t car) const {
    const auto* out = consist_ ? consist_->output(train, car) : nullptr;
    return out && out->has_engine ? &out->sms_ext : nullptr;
}

const std::array<float, 8>* LegacyCab::aux_sound_variables(std::size_t train, std::size_t car, std::size_t library) const {
    const auto* out = consist_ ? consist_->output(train, car) : nullptr;
    return out && library < out->aux_sound.size() ? &out->aux_sound[library].vars : nullptr;
}

void LegacyCab::step(const std::vector<const simulation::Train*>& trains, std::vector<Triggers>& posted) {
    posted.assign(trains.size(), {});
    if (!consist_) return;
    if (!consist_->step(simulation::train_step, trains, world_)) {
        log(consist_->take_messages());
        log(tr("the DLL host stopped: ") + consist_->error());
        return;
    }
    for (std::size_t t = 0; t < trains.size(); ++t) {
        if (!trains[t]) continue;
        auto& p = posted[t];
        const auto n = trains[t]->cars().size();
        p.exterior.resize(n);
        p.cab.resize(n);
        p.aux.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            const auto* out = consist_->output(t, i);
            if (!out) continue;
            for (const auto code : out->sound_ext) p.exterior[i].push_back(code);
            for (const auto code : out->sound_cab) p.cab[i].push_back(code);
            if (p.aux[i].size() < out->aux_sound.size()) p.aux[i].resize(out->aux_sound.size());
            for (std::size_t k = 0; k < out->aux_sound.size(); ++k)
                for (const auto code : out->aux_sound[k].triggers) p.aux[i][k].push_back(code);
            for (const auto& m : out->messages) {
                if (!m.text.empty()) log(trainplugin::utf16_to_utf8(m.text));
                // GM_SMS_POST id: that trigger in the sound of every car of the
                // train (RTS 0x424488 -> 0x46AF1C).
                constexpr std::uint16_t gm_sms_post = 5;
                if (m.action == gm_sms_post && m.id != 0)
                    for (auto& each : p.exterior) each.push_back(m.id);
            }
        }
    }
    log(consist_->take_messages());
}

void LegacyCab::step(const simulation::Train& train, std::vector<std::vector<std::uint32_t>>& triggers,
                     std::vector<std::vector<std::uint32_t>>& cab_triggers,
                     std::vector<std::vector<std::vector<std::uint32_t>>>& aux_triggers) {
    if (!consist_) return;
    if (!consist_->step(simulation::train_step, train, world_)) {
        log(consist_->take_messages());
        log(tr("the DLL host stopped: ") + consist_->error());
        return;
    }
    triggers.resize(train.cars().size());
    cab_triggers.resize(train.cars().size());
    aux_triggers.resize(train.cars().size());
    for (std::size_t i = 0; i < train.cars().size(); ++i) {
        const auto* out = consist_->output(i);
        if (!out) continue;
        for (const auto t : out->sound_ext) triggers[i].push_back(t);
        for (const auto t : out->sound_cab) cab_triggers[i].push_back(t);
        if (aux_triggers[i].size() < out->aux_sound.size()) aux_triggers[i].resize(out->aux_sound.size());
        for (std::size_t k = 0; k < out->aux_sound.size(); ++k)
            for (const auto t : out->aux_sound[k].triggers) aux_triggers[i][k].push_back(t);
        for (const auto& m : out->messages) {
            if (!m.text.empty()) log(trainplugin::utf16_to_utf8(m.text));
            // GM_SMS_POST id: that trigger in the sound of every car of the
            // train (RTS 0x424488 -> 0x46AF1C), e.g. the ED9M's door sounds
            // (105 opening, 106 closing).
            constexpr std::uint16_t gm_sms_post = 5;
            if (m.action == gm_sms_post && m.id != 0)
                for (auto& each : triggers) each.push_back(m.id);
        }
    }
    log(consist_->take_messages());
}

std::size_t LegacyCab::car_or_player(const std::size_t car) const {
    return car == SIZE_MAX && consist_ ? consist_->player() : car;
}

bool LegacyCab::has_cab(const std::size_t car) const {
    return consist_ && consist_->cab(car) && consist_->output(car) && consist_->has_dll(car);
}

std::uint32_t LegacyCab::next_state(std::uint32_t id, int direction, bool shift, std::size_t car) {
    car = car_or_player(car);
    const auto* out = consist_->output(car);
    const auto* sd = consist_->cab(car);
    const auto* spec = sd ? sd->find_switch(id) : nullptr;
    if (!out || id >= out->switches.size() || !spec) return 0;
    const auto& sw = out->switches[id];
    std::uint32_t s = sw.set_state;
    if (sw.flags & formats::cab_disabled) return s;
    const bool key = (sw.flags & formats::cab_key) != 0;
    const bool loop = (sw.flags & formats::cab_loop) != 0;
    const std::uint32_t n = spec->num_states + (key ? 1u : 0u);
    auto& down = going_down_[static_cast<std::uint64_t>(car) << 32 | id];
    const std::uint32_t low = key ? 1u : 0u;
    if (key) {
        // RTS 0x437AA4: the key goes in at InitialState + 1 and comes out
        // from there with Shift (the ED9M's reverser: at neutral).
        if (s == 0) return spec->initial_state + 1;           // insert the key
        if (s - 1 == spec->initial_state && shift) return 0;  // Shift: take it out
    }
    // A click ping-pongs (`down` remembers the way); a key of a pair goes its own way.
    if (direction < 0 || (direction == 0 && down)) {
        if (s > low) return s - 1;
        if (loop) return n - 1;
        if (direction == 0) {
            down = 0;
            return s + 1 < n ? s + 1 : s;
        }
        return s;
    }
    if (s + 1 < n) return s + 1;
    if (loop) return low;
    if (direction == 0) {
        down = 1;
        return s > low ? s - 1 : s;
    }
    return s;
}

void LegacyCab::click(const std::uint32_t id, const int direction, const bool shift, std::size_t car) {
    if (!consist_) return;
    car = car_or_player(car);
    const auto* out = consist_->output(car);
    if (!out || id >= out->switches.size()) return;
    const auto& sw = out->switches[id];
    // RTS 0x4121BC: a button (NONFIXED, not a lever you drag) goes one step
    // up and stays there while the mouse button is down.
    const bool button = (sw.flags & formats::cab_non_fixed) != 0 &&
                        (sw.flags & (formats::cab_controller_x | formats::cab_controller_y)) == 0 &&
                        (sw.flags & formats::cab_disabled) == 0;
    if (direction == 0 && button) {
        release();
        const auto next = next_state(id, 1, shift, car);
        if (next != sw.set_state) operate(id, next, car);
        held_ = std::pair{car, id};
        return;
    }
    const auto next = next_state(id, direction, shift, car);
    if (next != sw.set_state) operate(id, next, car);
}

bool LegacyCab::grab(const std::uint32_t id, const float frames_per_state, std::size_t car) {
    if (!consist_) return false;
    car = car_or_player(car);
    const auto* sd = consist_->cab(car);
    const auto* spec = sd ? sd->find_switch(id) : nullptr;
    if (!spec || (spec->flags & (formats::cab_controller_x | formats::cab_controller_y)) == 0
        || (spec->flags & formats::cab_disabled) != 0)
        return false;
    release();
    Drag d{.car = car, .id = id, .precision = {spec->precision[0], spec->precision[1]}, .flags = spec->flags};
    // Smoothly with a SwitchPrecision and an animated node (0x437E64): the
    // cursor's move in the node's frames, turned into positions.
    if ((d.precision[0] != 0.0f || d.precision[1] != 0.0f) && frames_per_state > 0.0f) d.frames_per_state = frames_per_state;
    drag_ = d;
    return true;
}

void LegacyCab::drag(const float dx, const float dy, const bool shift) {
    if (!drag_ || !consist_) return;
    const auto d = *drag_;
    if (d.frames_per_state > 0.0f) {
        const float delta = (dx * d.precision[0] + dy * d.precision[1]) / d.frames_per_state;
        if (delta != 0.0f) consist_->drag(d.car, d.id, delta, false);
        log(consist_->take_messages());
        return;
    }
    // One step in the move's direction, then the hold ends (CONTROLLERX
    // first; the cursor's y grows downwards, up is +1).
    float along = 0.0f;
    if ((d.flags & formats::cab_controller_x) != 0) along = dx;
    if (along == 0.0f && (d.flags & formats::cab_controller_y) != 0) along = -dy;
    if (along == 0.0f) return;
    drag_.reset();
    // With Shift a KEY switch's key comes out at its insert position.
    click(d.id, along > 0.0f ? 1 : -1, shift, d.car);
    // A NONFIXED one (the EP1M's windscreen blind joysticks) stays there
    // while the button is down and goes back to its FixedState when it is
    // let go (0x4121BC), as a button does.
    if ((d.flags & formats::cab_non_fixed) != 0) held_ = std::pair{d.car, d.id};
}

void LegacyCab::drag_end() {
    const auto d = std::exchange(drag_, std::nullopt);
    if (!d || !consist_) return;
    if (d->frames_per_state > 0.0f) consist_->drag(d->car, d->id, 0.0f, true);
    // A NONFIXED switch goes back to its FixedState (0x4121BC).
    if ((d->flags & formats::cab_non_fixed) != 0) {
        if (const auto* out = consist_->output(d->car); out && d->id < out->switches.size()) {
            const auto rest = out->switches[d->id].fixed_state;
            if (out->switches[d->id].set_state != rest) operate(d->id, rest, d->car);
        }
    }
    log(consist_->take_messages());
}

void LegacyCab::release() {
    const auto held = std::exchange(held_, std::nullopt);
    if (!held || !consist_) return;
    const auto [car, id] = *held;
    const auto* out = consist_->output(car);
    if (!out || id >= out->switches.size()) return;
    // Back to the rest position (RTS: FixedState, then SetState is forced there).
    const auto rest = out->switches[id].fixed_state;
    if (out->switches[id].set_state != rest) operate(id, rest, car);
}

void LegacyCab::operate(std::uint32_t id, std::uint32_t state, std::size_t car) {
    if (!consist_) return;
    if (!consist_->operate(car_or_player(car), id, state)) {
        // A refusal is the DLL's normal answer (an interlock); nothing to log.
    }
    log(consist_->take_messages());
}

void LegacyCab::release_keys() {
    if (!consist_) return;
    const auto car = consist_->player();
    const auto* out = consist_->output(car);
    const auto* sd = consist_->cab(car);
    if (!out || !sd) return;
    for (const auto& b : bindings_) {
        const auto* spec = sd->find_switch(b.id);
        if (!spec || (spec->flags & formats::cab_non_fixed) == 0 || b.id >= out->switches.size()) continue;
        const auto& sw = out->switches[b.id];
        if (sw.set_state != sw.fixed_state) operate(b.id, sw.fixed_state);
    }
    log(consist_->take_messages());
}

bool key_reaches(const formats::CabSwitch& control, const std::optional<glm::vec3>& eye) {
    if (!eye || (control.flags & formats::cab_check_distance) == 0 || control.radius <= 0.0f) return true;
    const glm::vec3 at{control.position[0], control.position[1], control.position[2]};
    return glm::distance(*eye, at) <= control.radius;
}

void LegacyCab::key(const KeyEvent& event, const std::optional<glm::vec3>& eye) {
    if (!consist_ || !keyboard()) return;
    const auto* out = consist_->output(consist_->player());
    for (const auto& b : bindings_) {
        if (b.key != event.key || b.shift != event.shift || b.ctrl != event.ctrl) {
            // A release counts whatever the modifiers are now.
            if (event.down || b.key != event.key) continue;
        }
        const auto* sd = consist_->cab(consist_->player());
        const auto* spec = sd ? sd->find_switch(b.id) : nullptr;
        if (!spec || !out || b.id >= out->switches.size()) continue;
        const bool non_fixed = (spec->flags & formats::cab_non_fixed) != 0;
        const auto& sw = out->switches[b.id];
        // A press works only the switches within reach (a release lets go
        // of what a press took, wherever he is).
        if (event.down && !key_reaches(*spec, eye)) continue;
        if (!event.down) {
            // RTS 0x414340: a NONFIXED switch returns to its FixedState when
            // its key is let go (RTS steps there one position per key delay).
            if (non_fixed && sw.set_state != sw.fixed_state) operate(b.id, sw.fixed_state);
            continue;
        }
        if (non_fixed && b.direction == 0) {
            // Held: one step up from a pressed position, else RTS's click step.
            const auto next = next_state(b.id, sw.set_state != 0 ? 1 : 0, event.shift);
            if (next != sw.set_state) operate(b.id, next);
            continue;
        }
        const auto next = next_state(b.id, b.direction, event.shift);
        if (next != out->switches[b.id].set_state) operate(b.id, next);
    }
}

void LegacyCab::draw_window(const std::function<void()>& extra) {
#ifdef OPENRTS_HAS_IMGUI
    if (!consist_) return;
    const auto player = consist_->player();
    ImGui::SetNextWindowSize(ImVec2(560.0f, 620.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(tr("Cab (locomotive DLL)###legacy_cab"))) {
        ImGui::End();
        return;
    }
    const auto* out = consist_->output(player);
    if (!consist_->alive()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s %s", tr("The DLL host stopped:"), consist_->error().c_str());
    }
    for (const auto& m : consist_->modules()) {
        const auto path = trainplugin::utf16_to_utf8(m.path);
        const auto slash = path.find_last_of("/\\");
        ImGui::TextDisabled("%s: %s, TS_VERSION %u", path.substr(slash == std::string::npos ? 0 : slash + 1).c_str(),
                            m.status == 0 ? tr("loaded") : tr("refused"), m.version[1]);
    }
    // On foot they are on whatever is chosen (the box shows it, greyed).
    bool keys_on = keyboard();
    ImGui::BeginDisabled(on_foot_);
    if (ImGui::Checkbox(tr("Cab keys (as in RTS; the camera keys are off; on foot always on)"), &keys_on)) keyboard_ = keys_on;
    ImGui::EndDisabled();
    if (extra) extra();
    if (out) {
        const auto& e = out->engine;
        ImGui::Text(tr("Force %.1f kN  notch %u  reverser %d  pantographs %u  main switch %u"), e.force / 1000.0f,
                    e.throttle, e.reverse, e.panto, e.main_switch);
        ImGui::Text(tr("Equalising reservoir %.2f atm  brake pipe rate %.3f  independent %.2f"), e.equalising,
                    e.train_pipe_rate, e.independent_brake);
    }
    const auto* sd = consist_->cab(player);
    if (sd && out && ImGui::CollapsingHeader(tr("Switches"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::InputText(tr("Filter"), filter_, sizeof(filter_));
        const auto filter = lower(filter_);
        if (ImGui::BeginTable("##switches", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                              ImVec2(0.0f, 260.0f))) {
            ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 34.0f);
            ImGui::TableSetupColumn(tr("Switch"));
            ImGui::TableSetupColumn(tr("State"), ImGuiTableColumnFlags_WidthFixed, 150.0f);
            ImGui::TableSetupColumn(tr("Keys"), ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableHeadersRow();
            for (const auto& s : sd->switches) {
                if (s.id >= out->switches.size()) continue;
                const auto label = s.name + " " + s.hint;
                if (!filter.empty() && lower(label).find(filter) == std::string::npos) continue;
                const auto& st = out->switches[s.id];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%u", s.id);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(label.c_str());
                const bool key = (s.flags & formats::cab_key) != 0;
                const std::uint32_t position = key ? (st.state ? st.state - 1 : 0) : st.state;
                if (position < s.state_hints.size() && !(key && st.state == 0)) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", s.state_hints[position].c_str());
                }
                ImGui::TableNextColumn();
                ImGui::PushID(static_cast<int>(s.id));
                if (ImGui::SmallButton("-")) operate(s.id, next_state(s.id, -1, false));
                ImGui::SameLine();
                if (key && st.state == 0)
                    ImGui::Text(tr("key out"));
                else
                    ImGui::Text("%u/%u%s", st.state, s.num_states + (key ? 1u : 0u), st.state != st.set_state ? " ..." : "");
                ImGui::SameLine();
                if (ImGui::SmallButton("+")) operate(s.id, next_state(s.id, 1, false));
                ImGui::PopID();
                ImGui::TableNextColumn();
                std::string keys;
                for (const auto& k : s.keys) keys += (keys.empty() ? "" : " ") + k;
                ImGui::TextUnformatted(keys.c_str());
            }
            ImGui::EndTable();
        }
    }
    if (sd && out && ImGui::CollapsingHeader(tr("Displays"))) {
        if (ImGui::BeginTable("##displays", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 200.0f))) {
            ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 34.0f);
            ImGui::TableSetupColumn(tr("Display"));
            ImGui::TableSetupColumn(tr("Value"), ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableHeadersRow();
            for (const auto& d : sd->displays) {
                if (d.id >= out->displays.size()) continue;
                const auto& v = out->displays[d.id];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%u", d.id);
                ImGui::TableNextColumn();
                ImGui::Text("%s %s", d.name.c_str(), d.hint.c_str());
                ImGui::TableNextColumn();
                if (d.type == formats::CabDisplayType::lamp)
                    ImGui::Text(v.state ? tr("on (%u)") : tr("off"), v.state);
                else
                    ImGui::Text("%.2f (%u)", v.value, v.state);
            }
            ImGui::EndTable();
        }
    }
    if (ImGui::CollapsingHeader(tr("DLL messages"), ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const auto& line : log_) ImGui::TextWrapped("%s", line.c_str());
    }
    ImGui::End();
#endif
}

} // namespace openrts::viewer
