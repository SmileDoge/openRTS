#include <cctype>
#include <openrts/trainplugin/legacy_consist.hpp>

#include <openrts/core/profile.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <type_traits>

#include <openrts/formats/cab_description.hpp>
#include <openrts/formats/sound.hpp>
#include <openrts/simulation/distributor.hpp>
#include <openrts/simulation/driver_valve.hpp>
#include <openrts/simulation/klub_screen.hpp>
#include <openrts/simulation/speed_limits.hpp>
#include <openrts/formats/shape_binary.hpp>
#include <openrts/formats/shape_file.hpp>

namespace openrts::trainplugin {

namespace {

// VkKeyScanW's low byte on the US layout for a key's character.
std::uint8_t virtual_key(const char c) {
    const auto u = static_cast<unsigned char>(c);
    if (u >= 'a' && u <= 'z') return static_cast<std::uint8_t>(u - 'a' + 'A');
    if ((u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9')) return u;
    switch (c) {
    case ';': case ':': return 0xBA;
    case '=': case '+': return 0xBB;
    case ',': case '<': return 0xBC;
    case '-': case '_': return 0xBD;
    case '.': case '>': return 0xBE;
    case '/': case '?': return 0xBF;
    case '`': case '~': return 0xC0;
    case '[': case '{': return 0xDB;
    case '\\': case '|': return 0xDC;
    case ']': case '}': return 0xDD;
    case '\'': case '"': return 0xDE;
    case '!': return '1';
    case '@': return '2';
    case '#': return '3';
    case '$': return '4';
    case '%': return '5';
    case '^': return '6';
    case '&': return '7';
    case '*': return '8';
    case '(': return '9';
    case ')': return '0';
    default: return u;
    }
}

} // namespace

void switch_key_bytes(const std::vector<std::string>& keys, std::uint8_t (&out)[4]) {
    out[0] = out[1] = out[2] = out[3] = 0;
    for (std::size_t k = 0; k < keys.size() && k < 2; ++k) {
        std::uint8_t key = 0;
        std::uint8_t modifiers = 0;
        const std::string_view text = keys[k];
        std::size_t start = 0;
        for (;;) {
            const auto plus = text.find('+', start);
            auto part = text.substr(start, plus == std::string_view::npos ? std::string_view::npos : plus - start);
            while (!part.empty() && static_cast<unsigned char>(part.front()) <= 0x20) part.remove_prefix(1);
            while (!part.empty() && static_cast<unsigned char>(part.back()) <= 0x20) part.remove_suffix(1);
            std::string lower(part);
            for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lower == "shift") {
                modifiers |= 1;
            } else if (lower == "ctrl" || lower == "control") {
                modifiers |= 2;
            } else if (lower == "alt") {
                modifiers |= 4;
            } else if (lower == "space") {
                key = 0x20;
            } else if (!part.empty()) {
                key = virtual_key(part.front());
            }
            if (plus == std::string_view::npos) break;
            start = plus + 1;
        }
        out[2 * k] = key;
        out[2 * k + 1] = modifiers;
    }
}

using namespace dllhost;

// openRTS's КЛУБ on a car (LegacyConsist::set_own_klub).
struct LegacyConsist::KlubCar {
    KlubParameters parameters;
    bool on = false;      // working: Brakes.dll's КЛУБ switched off
    bool resume = false;  // a saved state put back: no fresh start
    simulation::KlubState state;
    simulation::KlubOutput out;
    std::vector<simulation::KlubScreen> screens; // a cab's each (its KLUB_*<n>)
    // САУТ's displays as its steps left them, a cab's each: a step redraws
    // them every 0.3 s and sets nothing between, and the cab keeps what was
    // drawn (as the КЛУБ's screens above). Until 2026-11-20 only the step's
    // own were put over the outputs: between its redraws the cab showed the
    // outputs' stale values, САУТ's speed blinking 45 / 120 and the speed
    // 0 / the train's (testers, the ЭД9М at Тимашевская).
    std::vector<simulation::SautScreen> saut_screens;
    std::vector<simulation::KlubPress> keys;     // pressed since the last step
    std::vector<simulation::SautKey> saut_keys;  // САУТ's, pressed since the last step
    std::vector<SwitchState> switches;           // the cab's after the last step
    // Engine::ALSNOn: the ЭПК key last turned, of any cab (Brakes.dll's Switched).
    bool alsn_on = false;
    float last_ur = 0.0f, last_pipe = 0.0f;      // the last step's (updateKLUB draws before the brakes)
    float last_gauge = 0.0f;                     // the ТМ gauge's, Brakes.dll's var + 28, the last step's
    simulation::EkState ek;                      // openRTS's map for its entered track (LegacyWorld::electronic_map)
    std::uint32_t ek_entries = 0;                // KlubState::track_entries it is for (a new entry: afresh)
    // What the log last said of it: the track, the ЭК or not, the target.
    std::uint32_t ek_logged_track = 0;
    int ek_logged_mode = -1;
    std::uint32_t ek_logged_object = ~0u;
};

namespace {
// LegacyConsist::set_without_brakes_dll's: for the consists made next.
std::atomic<bool> without_brakes_dll_choice{false};
} // namespace

struct LegacyConsist::RecorderCar {
    RecorderParameters parameters;
    bool on = false;
    bool started = false;
    simulation::RecorderState state;
    simulation::RecorderOutput out;
    std::vector<SwitchState> switches;  // the cab's after the last step
    std::uint32_t last_var4 = 0;        // as the step before left them (read first)
    float last_ep = 0.0f;
    std::uint32_t random = 1;           // the needle's error: an LCG as the C library's rand
};

namespace {

std::u16string u16(std::string_view s) { return utf8_to_utf16(s); }

// A screen sub-object for the host: RTS CloseUpSubObject's Pt by type (0x519B8C).
dllhost::ScreenObjectSpec screen_object_spec(const formats::ScreenObject& o) {
    dllhost::ScreenObjectSpec s;
    s.type = static_cast<std::uint32_t>(o.type);
    s.flags = o.flags;
    s.initial_state = o.initial_state;
    s.parent = o.parent;
    s.frames = static_cast<std::uint32_t>(o.frames.size());
    s.textures = static_cast<std::uint32_t>(o.textures.size());
    s.shader = o.shader;
    s.colour = o.colour;
    for (int k = 0; k < 4; ++k) {
        s.area[k] = o.area[k];
        s.uv[k] = o.uv[k];
    }
    s.anim_time = o.anim_time;
    s.fade = o.fade;
    s.blink = o.blink;
    using T = formats::ScreenObjectType;
    switch (o.type) {
    case T::arrow:
        s.pt[0] = o.center[0];
        s.pt[1] = o.center[1];
        for (int k = 0; k < 4; ++k) s.pt[2 + k] = o.range[k];
        break;
    case T::link:
        s.pt[0] = static_cast<float>(o.link);
        break;
    case T::label:
        if (o.char_width[1] > 0.0f && o.max_length > 0) {
            s.text_size = o.max_length + 1u;
            s.pt[1] = static_cast<float>(s.text_size);
        }
        break;
    case T::bar:
        s.pt[0] = static_cast<float>(o.bar_style);
        for (int k = 0; k < 4; ++k) s.pt[2 + k] = o.range[k];
        break;
    case T::grid:
        s.pt[0] = static_cast<float>(o.grid_rows[0]);
        s.pt[1] = static_cast<float>(o.grid_rows[1]);
        s.pt[2] = static_cast<float>(o.grid_columns[0]);
        s.pt[3] = static_cast<float>(o.grid_columns[1]);
        break;
    case T::sector:
        for (int k = 0; k < 4; ++k) s.pt[2 + k] = o.range[k];
        s.pt[6] = static_cast<float>(o.sector_shape + 10u * o.sector_fill);
        s.pt[7] = o.sector_granularity;
        break;
    case T::frame:
        break;
    }
    // RTS: a value above the range's minimum starts at the minimum.
    if (o.type == T::arrow || o.type == T::bar || o.type == T::sector) s.value = (std::min)(0.0f, o.range[0]);
    return s;
}

std::vector<Parameter> parameters(const std::vector<formats::EngineParameter>& list) {
    std::vector<Parameter> out;
    for (const auto& p : list) out.push_back({u16(p.name), p.value, u16(p.text)});
    return out;
}

std::uint16_t aux_event(const std::string& name) {
    if (name == "run") return aux_run;
    if (name == "alsn") return aux_alsn;
    if (name == "speedlimit") return aux_speed_limit;
    if (name == "switched") return aux_switched;
    if (name == "canswitch") return aux_can_switch;
    return 0;
}

// A DLL named in an .eng, as RTS looks for it (0x56D9C8): next to the .eng,
// then under TRAINS/TRAINSET and GLOBAL/LIB. Returns the real file.
std::optional<std::filesystem::path> find_module(const core::Vfs& vfs, const std::filesystem::path& cache_root,
                                                 const core::VirtualPath& file, const std::string& module) {
    std::vector<std::optional<core::VirtualPath>> candidates{
        file.parent().resolve(module),
        core::VirtualPath::parse("TRAINS/TRAINSET/" + module),
        core::VirtualPath::parse("GLOBAL/LIB/" + module),
    };
    for (const auto& c : candidates) {
        if (c && vfs.exists(*c)) return vfs.real_path(*c, cache_root);
    }
    return std::nullopt;
}

using formats::shape_description;

// The Shortcut frames of a cab's animation nodes as positions of the
// switches with those names. RTS (0x4B30DC): a position's frame is the
// node's first key frame + position x (its key span / (States - 1)), whole
// frames per position.
void add_shortcuts(const core::Vfs& vfs, const core::VirtualPath& cabin, const formats::CabDescription& cab,
                   std::vector<SwitchSpec>& switches) {
    if (cab.animation_nodes.empty()) return;
    core::VirtualPath shape = cabin;
    if (core::fold_case(cabin.extension()) == ".repaint") {
        const auto text = vfs.read(cabin);
        const auto repaint = text ? formats::parse_repaint(*text) : formats::Repaint{};
        const auto target = repaint.ok() ? cabin.parent().resolve(repaint.shape) : std::nullopt;
        if (!target) return;
        shape = *target;
    }
    const auto body = formats::load_binary_shape_body(vfs, shape);
    if (!body.ok()) return;
    const auto model = formats::parse_binary_shape_model(*body.body);
    if (!model.ok() || model.animations.empty()) return;
    for (const auto& node : cab.animation_nodes) {
        // The node's keys: first frame and span.
        std::optional<std::uint32_t> first, last;
        for (const auto& animation : model.animations) {
            for (const auto& track : animation.nodes) {
                if (core::fold_case(track.name) != core::fold_case(node.name)) continue;
                const auto see = [&](const std::uint32_t frame) {
                    first = first ? (std::min)(*first, frame) : frame;
                    last = last ? (std::max)(*last, frame) : frame;
                };
                for (const auto& key : track.positions) see(key.frame);
                for (const auto& key : track.rotations) see(key.frame);
            }
        }
        if (!first || !last) continue;
        for (auto& sw : switches) {
            if (core::fold_case(utf16_to_utf8(sw.name)) != core::fold_case(node.name) || sw.num_states < 2) continue;
            const std::uint32_t per_position = (*last - *first) / (sw.num_states - 1);
            if (per_position == 0) continue;
            const float key_out = (sw.flags & 2) != 0 ? 1.0f : 0.0f;
            for (const auto& frames : node.shortcuts) {
                std::vector<float> set;
                for (const auto frame : frames) {
                    set.push_back((static_cast<float>(frame) - static_cast<float>(*first)) / static_cast<float>(per_position) + key_out);
                }
                sw.shortcuts.push_back(std::move(set));
            }
        }
    }
}

class LegacyController final : public simulation::LocomotiveController {
public:
    LegacyController(std::weak_ptr<const LegacyConsist::Outputs> outputs, std::size_t car, std::shared_ptr<const DllAirSetting> air,
                     BrakeCabControls cab, BrakesParameters brakes = {}, int brakes_library = -1, std::uint32_t wagons = 0)
        : outputs_(std::move(outputs)), car_(car), air_(std::move(air)), cab_(std::move(cab)), brakes_(std::move(brakes)),
          brakes_library_(brakes_library), wagons_(wagons) {}
    void step(double dt, const simulation::EngineInputs& inputs, simulation::EngineDemands& demands) override {
        const auto outputs = outputs_.lock();
        if (!outputs || !outputs->valid || car_ >= outputs->cars.size() || !outputs->cars[car_].has_engine) {
            demands = {};
            return;
        }
        const auto& out = outputs->cars[car_];
        apply_engine_output(out.engine, demands);
        const auto mode = air_ ? air_->mode : DllAir::rts;
        if (mode == DllAir::rts) {
            valve_started_ = false;
            brakes_started_ = false;
            dll_ur_known_ = false;
            return;
        }
        // The main reservoir is the model's: the DLLs keep RTS's (its huge
        // volume never drains, their compressors may never run, and they
        // take their own air from it), so openRTS runs the compressors, by
        // the .eng's governor; started cold, not before the train has power.
        demands.main_reservoir_rate = outputs->cold ? 0.0f : simulation::compressor_rate(compressor_, inputs);
        // The realistic air works the pipe. The DLL's TrainPipeRate counts
        // only as a direct vent (its VI, the ЭПК, a stop valve): faster than
        // 0.6 atm/s and faster than its own valve would vent, which follows
        // the pipe's excess over the DLL's УР about one to one (the ЭД9М's
        // Brakes.dll: V -0.23 atm/s; with openRTS's valve over it, its УР
        // below the pipe after an overcharge, -0.54 at 0.54 apart); or its
        // УР falling faster than 0.6 atm/s (its VI). The ЭПК: about -0.9
        // with its УР standing at the pipe.
        const float dll_ur = out.engine.equalising;
        const float ur_rate = dll_ur_known_ && dt > 0.0 ? (dll_ur - last_dll_ur_) / static_cast<float>(dt) : 0.0f;
        last_dll_ur_ = dll_ur;
        dll_ur_known_ = true;
        const float excess = (std::max)(inputs.train_pipe - dll_ur, 0.0f);
        const float rate = out.engine.train_pipe_rate;
        // A driven cab whose DLL valve stands cut off (its УР about 0: the
        // 367 off, a cold start) vents nothing by its valve: a fall then is
        // its ЭПК (the ВЛ80С-1886 from cold, 2026-11-13: -1.2 at УР 0).
        const bool cut_off = (out.loco_flags & 1u) != 0 && dll_ur < 0.5f;
        // openRTS's КЛУБ in place of Brakes.dll's (set_own_klub): its ЭПК and
        // КОН; Brakes.dll's КЛУБ vents nothing then.
        const bool klub = car_ < outputs->klub_vent.size() && outputs->klub_vent[car_] != 0;
        const bool vent = (rate <= -0.6f && (rate < -(excess + 0.3f) || ur_rate < -0.6f || cut_off)) || klub;
        demands.emergency = vent;
        // openRTS's cab brake devices in place of Brakes.dll's
        // (DllAirSetting::own_brakes): Brakes.dll's brake outputs are not
        // used on any car it runs on. A car not driven (a multiple unit's
        // other cars, BR_MODE 1, take the ЭПТ in their independent brake
        // from the head car's Brakes.dll) gets the driven cab's ЭПТ through
        // the line and no brake of its own.
        const bool own_brakes = air_->own_brakes && brakes_.understood();
        if (!own_brakes) brakes_started_ = false;
        if (own_brakes && (out.loco_flags & 1u) == 0) {
            demands.independent_brake = 0.0f;
            demands.ep_value = -1.0f;
            demands.systems = static_cast<std::uint16_t>(demands.systems & ~2u);
            demands.release_train_brake = demands.release_independent_brake = false;
            brakes_started_ = false;
            // A locomotive's other section: the driven cab's 254 (its
            // independent brake and its I) through the sections' lines.
            if (car_ < outputs->section_master.size()) {
                const auto master = outputs->section_master[car_];
                if (master >= 0 && static_cast<std::size_t>(master) < outputs->section_brake.size()) {
                    const auto& b = outputs->section_brake[static_cast<std::size_t>(master)];
                    if (b.set) {
                        demands.independent_brake = b.independent;
                        demands.release_train_brake = b.release_train;
                    }
                }
            }
            return;
        }
        // Only the cab driven (LocoFlags 1) works the pipe: the DLLs of the
        // other cars of a multiple unit keep a УР of their own.
        if ((out.loco_flags & 1u) == 0) return;
        const auto* engine = inputs.definition && inputs.definition->engine ? &*inputs.definition->engine : nullptr;
        // The cab the driver works (several in one car: the one whose 367 is
        // on, as Brakes.dll finds it). Until 2026-11-18 the first cab's
        // handle was read: from the ЭП20's second cab openRTS's valve took
        // the first's, standing in VI, and the УР and pipe stayed at 0
        // (testers).
        const std::size_t cab = engine ? working_cab(engine->brakes_library, out.switches) : 0;
        const auto ids = engine ? (cab == 0 ? engine->brakes_library : engine->brakes_library.for_cab(cab))
                                : formats::EngineDefinition::BrakesLibrary{};
        if (cab != last_cab_) {
            last_cab_ = cab;
            valve_started_ = false; // the other cab's valve, from where the pipe stands
        }
        if (own_brakes) {
            step_own_brakes(dt, inputs, demands, out, ids, cab, vent);
            return;
        }
        const auto valve = ids.valve;
        if (mode == DllAir::own_valve && valve >= 0 && static_cast<std::size_t>(valve) < out.switches.size()) {
            // openRTS's 395 on Brakes.dll's handle, its states as the cab
            // names them (valve_states; VЭ as Va without the ЭПТ); a state
            // not known keeps the last position. Its ЭПТ demand stays the DLL's.
            // The charging pressure: the cab's reducer (BR_REDID1), else
            // BR_ZARPRESS, else 5.0.
            controls_ = cab_.of_cab(cab).read(ids, out.switches, controls_);
            if (controls_.position < 0) controls_.position = static_cast<int>(simulation::ValvePosition::running);
            const auto position = static_cast<simulation::ValvePosition>(controls_.position);
            const float charging = controls_.charging > 0.0f                    ? controls_.charging
                                   : engine->brakes_library.charging > 0.0f ? engine->brakes_library.charging
                                                                             : 5.0f;
            if (!valve_started_) {
                // From where the DLL's valve stood.
                state_.equalising = out.engine.equalising > 0.5f ? out.engine.equalising : (std::min)(inputs.train_pipe, charging);
                valve_started_ = true;
            }
            const simulation::ValveInputs in{
                .pipe = inputs.train_pipe,
                .main_reservoir = inputs.main_reservoir,
                .cylinder = inputs.cylinder,
                .charging_pressure = charging,
                .realistic = true,
                .ep_on = (out.engine.systems & 2u) != 0,
                .slow_service = true,
            };
            const auto valve_out = simulation::step_valve_395(state_, position, in, static_cast<float>(dt));
            // The combined valve: double traction cuts the valve off the pipe
            // (its УР goes on), emergency vents the pipe. The 367 out of its
            // operating state cuts it off too.
            // The separating cock (Кран тормозной магистрали) shut cuts it off too:
            // until 2026-11-18 openRTS's valve charged the pipe through a shut
            // cock (testers: shut, the pipe charged; open, Brakes.dll's own valve
            // vented it).
            const bool connected = controls_.combined != 1 && !controls_.blocked && !controls_.separated;
            if (connected) demands.equalising = valve_out.equalising;
            // Its gauge shows that УР all the same: until 2026-11-18 a valve
            // cut off showed the УР the DLL kept for itself (testers: after
            // an emergency, the main reservoir pipe's cock closed, the УР
            // leapt from 0 to the DLL's 1.71).
            demands.equalising_kept = valve_out.equalising;
            demands.emergency = (connected && valve_out.emergency) || controls_.combined == 2 || vent;
            demands.pipe_feed = valve_out.feed;
            demands.pipe_direct = connected && valve_out.direct;
            return;
        }
        // The DLL's valve: its УР, through the cab's cocks as openRTS's valve
        // goes: until 2026-11-18 the pipe followed it with the separating
        // cock and the 367 shut (testers: a cab changed, both cocks shut, the
        // pipe charged as the handle went to II).
        if (out.engine.equalising > 0.5f) {
            if (engine) controls_ = cab_.of_cab(cab).read(ids, out.switches, controls_);
            const bool connected = controls_.combined != 1 && !controls_.blocked && !controls_.separated;
            if (connected) demands.equalising = out.engine.equalising;
            demands.equalising_kept = out.engine.equalising;
        }
    }
    // A saved game: openRTS's valve over the DLL (its УР, its ЭПТ demand),
    // the compressor's governor and the DLL УР last seen. The DLL's own
    // state goes with LegacyConsist::save_state.
    void save(simulation::StateWriter& out, const std::string_view prefix) const override {
        const std::string p(prefix);
        out.put(p + "valve", state_.equalising, state_.ep_target, valve_started_, compressor_, last_dll_ur_, dll_ur_known_);
        const auto& b = brakes_state_;
        out.put(p + "cab_brakes", brakes_started_, b.valve.equalising, b.valve.ep_target, b.independent, b.cylinder_needle,
                b.pipe_needle, b.sensor418_time, b.sensor418, b.last_pipe, b.started);
    }
    void restore(const simulation::StateReader& in, const std::string_view prefix) override {
        const std::string p(prefix);
        in.get(p + "valve", state_.equalising, state_.ep_target, valve_started_, compressor_, last_dll_ur_, dll_ur_known_);
        auto& b = brakes_state_;
        in.get(p + "cab_brakes", brakes_started_, b.valve.equalising, b.valve.ep_target, b.independent, b.cylinder_needle,
               b.pipe_needle, b.sensor418_time, b.sensor418, b.last_pipe, b.started);
    }

private:
    // openRTS's cab brake devices (simulation/cab_brakes.hpp) on the
    // working cab's handles: they work the realistic air instead of
    // Brakes.dll's valve, its ЭПТ and its independent brake. Brakes.dll's
    // ЭПК trip (var + 32 bit 1), its direct vents and the train's DLL's
    // requests (var + 32 before its Run) reach them.
    void step_own_brakes(const double dt, const simulation::EngineInputs& inputs, simulation::EngineDemands& demands,
                         const dllhost::CarOutput& out, const formats::EngineDefinition::BrakesLibrary& ids, const std::size_t cab,
                         const bool vent) {
        const auto& table = cab_.of_cab(cab);
        controls_ = table.read(ids, out.switches, controls_);
        CabBrakes valve = controls_;
        // Without the cab's names for the handle's states, BR_FLIPCABS numbers them.
        if (table.valve.empty() && table.controller.empty()) valve.position = -1;
        const auto exchange = brakes_library_ >= 0 && static_cast<std::size_t>(brakes_library_) < out.aux_exchange.size()
                                  ? out.aux_exchange[static_cast<std::size_t>(brakes_library_)]
                                  : dllhost::AuxExchange{};
        auto controls = cab_brake_controls(brakes_, cab, out.switches, valve, exchange, out.engine.systems);
        // The ЭПК (Brakes.dll's trip, openRTS's КЛУБ's) opens the pipe as
        // the emergency valves do: the ЭПТ's demand gone, the air sound.
        controls.emergency = controls.emergency || vent;
        controls.wagons = wagons_;
        const auto o = outputs_.lock();
        std::vector<std::uint16_t>* sounds = nullptr;
        if (o) {
            if (o->own_air.size() <= car_) {
                o->own_air.resize(car_ + 1, 0);
                o->own_air_sounds.resize(car_ + 1);
            }
            o->own_air[car_] = 1;
            sounds = &o->own_air_sounds[car_];
        }
        if (!brakes_started_) {
            // From where the DLL's valve stood.
            brakes_state_ = {};
            brakes_state_.valve.equalising = out.engine.equalising > 0.5f ? out.engine.equalising
                                                                          : (std::min)(inputs.train_pipe, controls.charging);
            brakes_started_ = true;
            // Brakes.dll's air sounds stop (theirs are left out from now on):
            // every pair's off.
            if (sounds) {
                for (const std::uint16_t off : {std::uint16_t{125}, std::uint16_t{127}, std::uint16_t{129}, std::uint16_t{131},
                                                std::uint16_t{133}, std::uint16_t{135}, std::uint16_t{137}, std::uint16_t{123}})
                    sounds->push_back(static_cast<std::uint16_t>(brakes_.sounds_130 ? off + 3000 : off));
                sounds->push_back(static_cast<std::uint16_t>(brakes_.sounds_224 ? 3121 : 121));
            }
        }
        const simulation::CabBrakeAir air{
            .pipe = inputs.train_pipe,
            .main_reservoir = inputs.main_reservoir,
            .cylinder = inputs.cylinder,
            .independent = inputs.independent,
            .valve_feed = inputs.valve_feed,
            .distributor_known = true,
            .sensor418_fitted = inputs.definition && simulation::sensor418_fitted(*inputs.definition),
            .distributor_channel = inputs.distributor_channel,
            .distributor_discharge = inputs.distributor_discharge,
        };
        const auto numbers = air_->book_numbers ? simulation::CabBrakeNumbers::openrts() : simulation::CabBrakeNumbers::brakes_dll();
        const auto r = simulation::step_cab_brakes(brakes_state_, controls, air, numbers, static_cast<float>(dt));
        if (sounds) sounds->insert(sounds->end(), r.sounds.begin(), r.sounds.end());
        demands.equalising = r.equalising;
        demands.equalising_kept = r.equalising_kept;
        demands.emergency = r.emergency || vent;
        demands.pipe_feed = r.feed;
        demands.pipe_direct = r.direct;
        demands.ep_value = r.ep_value;
        demands.ep_command = r.ep_command;
        demands.systems = static_cast<std::uint16_t>((demands.systems & 4u) | r.systems);
        demands.independent_brake = r.independent_brake;
        // The train's DLL's own release requests (Engine::EngineFlags 1, 2:
        // the ЭП20's "Отпуск тормозов" button) beside the devices': until
        // 2026-11-21 the devices' replaced them (the testers: the button did
        // nothing on openRTS's brakes).
        const bool dll_release = (out.engine.engine_flags & 1u) != 0;
        demands.release_train_brake = r.release_train_brake || dll_release;
        demands.release_independent_brake = (out.engine.engine_flags & 2u) != 0;
        demands.cab_brakes = r;
        // For its other sections (Outputs::section_brake).
        if (o) {
            if (o->section_brake.size() <= car_) o->section_brake.resize(car_ + 1);
            o->section_brake[car_] = controls.multiple_unit ? LegacyConsist::Outputs::SectionBrake{}
                                                            : LegacyConsist::Outputs::SectionBrake{true, r.independent_brake,
                                                                                                   r.release_train_brake || dll_release};
        }
    }

    std::weak_ptr<const LegacyConsist::Outputs> outputs_;
    std::size_t car_;
    std::shared_ptr<const DllAirSetting> air_;
    BrakeCabControls cab_;
    CabBrakes controls_;
    simulation::ValveState state_;
    bool valve_started_ = false;
    std::size_t last_cab_ = 0; // the working cab at the last step
    bool compressor_ = false;
    float last_dll_ur_{};      // the DLL's УР at the last step
    bool dll_ur_known_ = false;
    BrakesParameters brakes_;      // its Brakes.dll's (own_brakes)
    int brakes_library_ = -1;      // that library in the host
    std::uint32_t wagons_ = 0;     // its train's wagons, not engines (RTSGetInteger 2: the ТМ needle)
    simulation::CabBrakeState brakes_state_;
    bool brakes_started_ = false;
};

} // namespace

void apply_engine_output(const EngineOutput& out, simulation::EngineDemands& demands) {
    demands.force = out.force;
    demands.brake_force = out.brake_force;
    demands.train_pipe_rate = out.train_pipe_rate;
    demands.independent_brake = out.independent_brake;
    demands.main_reservoir_rate = out.main_reservoir_rate;
    demands.charging_rate = out.charging_rate;
    demands.ep_value = out.ep_value;
    demands.handbrake = out.handbrake_percent;
    demands.sets_handbrake = true; // RTS: HandbrakeValue = HandbrakePercent / 100, negative 0
    demands.systems = out.systems;
    demands.release_train_brake = (out.engine_flags & 1) != 0;
    demands.release_independent_brake = (out.engine_flags & 2) != 0;
    demands.sanding = out.sanding != 0;
    demands.engine_force_count = out.engine_force_count;
    demands.engine_forces = out.engine_forces;
    // RTS 0x503CE4: the line current comes from ElectricEngine::PowerConsuming.
    demands.power = out.power_consuming;
}

namespace {

// The sound scripts' tables for the host (SoundTableSpec): NamedTriggersTable
// and the On codes of the trigger pairs of the group the player's train
// plays (group 7, else the first). One per file.
struct SoundTables {
    std::vector<SoundTableSpec>& out;
    std::map<std::string, std::int32_t>& by_path; // kept by the consist across couplings
    std::int32_t base = 0;                        // tables the host has already

    std::int32_t add(const core::Vfs& vfs, const std::vector<std::optional<core::VirtualPath>>& candidates) {
        for (const auto& path : candidates) {
            if (!path || !vfs.exists(*path)) continue;
            const std::string key(path->str());
            if (const auto it = by_path.find(key); it != by_path.end()) return it->second;
            const auto script = formats::load_sound_script(vfs, *path);
            SoundTableSpec t;
            if (script.ok()) {
                for (const auto& [name, code] : script.named_triggers) {
                    t.names.push_back(u16(name));
                    t.codes.push_back(static_cast<std::uint16_t>(code));
                }
                if (const auto* group = script.select_group(formats::SoundOwner::player_train)) {
                    for (const auto& pair : group->trigger_pairs) {
                        std::vector<std::uint16_t> on;
                        for (const auto c : pair.on) on.push_back(static_cast<std::uint16_t>(c));
                        t.pair_on.push_back(std::move(on));
                    }
                }
            }
            if (t.names.empty() && t.pair_on.empty()) return by_path[key] = -1;
            out.push_back(std::move(t));
            return by_path[key] = base + static_cast<std::int32_t>(out.size() - 1);
        }
        return -1;
    }
};

struct BuiltCar {
    CarSpec spec;
    formats::CabDescription cab;
    bool dll = false;
    bool stubbed = false; // its Brakes.dll not loaded (AuxLibrarySpec::stub)
    std::vector<std::int16_t> aux_index; // each AuxLibrary of the .eng as the host numbers them, -1 missing
};

std::optional<core::VirtualPath> in(const core::VirtualPath& folder, const std::string& file) {
    if (file.empty()) return std::nullopt;
    return folder.resolve(file);
}

// RTS's TrackItem and its kind's fields (0x482680) from the TDB item.
TrackItemSpec item_spec(const formats::TrackItem& item, const float route_speed_limit, const bool without_tolerance) {
    using formats::TrackItemKind;
    TrackItemSpec s;
    s.id = item.id;
    switch (item.kind) {
    case TrackItemKind::signal: s.type = 1; break;
    case TrackItemKind::speed_post: s.type = 2; break;
    case TrackItemKind::platform: s.type = 3; break;
    case TrackItemKind::siding: s.type = 4; break;
    case TrackItemKind::level_crossing: s.type = 5; break;
    case TrackItemKind::crossover: s.type = 6; break;
    case TrackItemKind::pickup: s.type = 7; break;
    case TrackItemKind::car_spawner: s.type = 8; break;
    default: s.type = 0; break;
    }
    s.node_offset = item.distance;
    s.data_flags = item.data_flags;
    // RTS axes: x negated.
    if (item.has_pdata) {
        s.pdata[0] = -item.pdata[0];
        s.pdata[1] = item.pdata[1];
        s.ptile[0] = static_cast<std::int16_t>(item.pdata_tile[0]);
        s.ptile[1] = static_cast<std::int16_t>(item.pdata_tile[1]);
    }
    if (item.has_rdata) {
        s.rdata[0] = -item.rdata[0];
        s.rdata[1] = item.rdata[1];
        s.rdata[2] = item.rdata[2];
        s.rtile[0] = static_cast<std::int16_t>(item.rdata_tile[0]);
        s.rtile[1] = static_cast<std::int16_t>(item.rdata_tile[1]);
    }
    switch (s.type) {
    case 2: {
        // SpeedpostTrItemData as 0x482680 reads it: flags; with flag 2 the
        // limit (flag 1: the route's, as it is) and with 0x10 a number;
        // without it a speed with flag 8, then the post's number (KM); the
        // sign's rotation (Off) last. km/h, mph with 0x100, to m/s unless
        // flag 1.
        const auto& v = item.speed_post_data;
        std::size_t i = 0;
        const auto next = [&] { return i < v.size() ? v[i++] : 0.0f; };
        const auto flags = static_cast<std::uint32_t>(static_cast<std::int32_t>(next()));
        s.flags = flags;
        if (flags & 2u) {
            s.value = (flags & 1u) ? route_speed_limit : next();
            if (flags & 0x10u) s.number = static_cast<std::int32_t>(next());
        } else {
            if (flags & 8u) s.value = next();
            s.km = next();
        }
        s.off = next();
        s.value = std::abs(s.value);
        if (!(flags & 1u)) {
            s.value *= (flags & 0x100u) ? 0.4469444444f : 1.0f / 3.6f;
            // The order's limit, as openRTS's own (the КЛУБ's map, testers).
            if (without_tolerance) s.value = simulation::without_tolerance(s.value);
        }
        break;
    }
    case 3:
        s.flags = item.item_flags;
        s.link = item.linked;
        s.name = u16(item.name);
        s.station = u16(item.station);
        s.min_wait = item.min_wait;
        s.passengers = item.passengers;
        break;
    case 4:
        s.flags = item.item_flags;
        s.link = item.linked;
        s.name = u16(item.name);
        break;
    case 6:
        s.link = item.crossover[0];
        s.section = item.crossover[1];
        break;
    case 7:
        s.value = item.pickup_amount;
        s.flags = item.pickup_flags;
        break;
    default:
        break;
    }
    return s;
}

// What the host needs of one car (CarSpec), with its cab and whether a DLL
// runs it. `tables` collects the sound scripts' tables.
// Every part of Brakes.dll this engine uses is openRTS's: its brakes
// understood, a КЛУБ openRTS takes, no speed recorder (its ALSN delay and
// the КПД-3, УКБМ, МУ blocks are Brakes.dll's).
bool brakes_dll_all_ours(const formats::EngineDefinition& e) {
    if (!brakes_parameters(e).understood()) return false;
    const auto k = klub_parameters(e);
    if (!k.present || k.setup_index < 0 || !(k.settings.setup & 2u)) return false;
    const auto r = recorder_parameters(e);
    return !r.present && !(r.settings.setup & 0x18u);
}

bool is_brakes_module(const std::string& module) {
    const auto m = core::fold_case(module);
    return m == "brakes.dll" || (m.size() > 11 && (m.ends_with("/brakes.dll") || m.ends_with("\\brakes.dll")));
}

BuiltCar build_spec(const core::Vfs& vfs, const std::filesystem::path& cache_root, const LegacyCar& car,
                    const core::VirtualPath* route, SoundTables& tables, std::string& problems, const bool without_brakes_dll) {
    BuiltCar out;
    CarSpec& spec = out.spec;
    spec.uid = car.uid;
    spec.key = car.key;
    if (!car.definition) return out;
    const auto global_sound = core::VirtualPath::parse("SOUND");
    const auto& d = *car.definition;
    spec.mass = d.mass;
    spec.name = u16(d.name);
    // The exterior and cab scripts, looked up as the viewer does
    // (TrainView::update_sound / update_cab_sound).
    const auto folder = car.file.parent();
    const auto sound_folder = folder.resolve("SOUND");
    if (!d.sound.empty())
        spec.sound_ext = tables.add(vfs, {sound_folder ? in(*sound_folder, d.sound) : std::nullopt,
                                          global_sound ? in(*global_sound, d.sound) : std::nullopt});
    if (!d.cab_sound.empty())
        spec.sound_cab = tables.add(vfs, {sound_folder ? in(*sound_folder, d.cab_sound) : std::nullopt, in(folder, d.cab_sound),
                                          global_sound ? in(*global_sound, d.cab_sound) : std::nullopt});
    if (d.engine) {
        const auto& e = *d.engine;
        spec.kind = e.kind == formats::EngineKind::electric ? car_electric
                  : e.kind == formats::EngineKind::diesel   ? car_diesel
                                                            : car_wagon;
        spec.series = u16(e.series);
        spec.lib_param = e.library_param;
        spec.stack_size = e.variable_stack;
        spec.num_engines = e.num_engines;
        spec.num_axles = e.num_axles;
        spec.parameters = parameters(e.parameters);
        if (!e.library_module.empty() && spec.kind != car_wagon) {
            if (const auto path = find_module(vfs, cache_root, car.file, e.library_module)) {
                spec.module = path->u16string();
                out.dll = true;
            } else {
                problems += "DLL not found: " + e.library_module + " (" + d.name + ")\n";
            }
        }
        for (const auto& a : e.aux_libraries) {
            AuxLibrarySpec aux;
            aux.name = u16(a.name);
            aux.stack_size = a.variable_stack;
            aux.parameters = parameters(a.parameters);
            aux.has_sound = !a.sound.empty();
            if (aux.has_sound) {
                // TrainView::update_aux_sounds: the route's SOUND first
                // for ROUTEFORWAGON, then next to the .eng.
                std::optional<core::VirtualPath> route_file;
                if (route && a.sound_type == formats::AuxSoundType::route_for_wagon)
                    if (const auto f = route->resolve("SOUND")) route_file = in(*f, a.sound);
                aux.sound_table = tables.add(vfs, {route_file, in(folder, a.sound)});
            }
            for (const auto& [fn, order] : a.calls)
                if (const auto ev = aux_event(fn)) aux.calls.push_back({ev, static_cast<std::int16_t>(order)});
            if (without_brakes_dll && is_brakes_module(a.module) && brakes_dll_all_ours(e)) {
                aux.stub = true;
                out.stubbed = true;
            }
            if (const auto path = find_module(vfs, cache_root, car.file, a.module)) {
                aux.module = path->u16string();
                out.aux_index.push_back(static_cast<std::int16_t>(spec.aux.size()));
                spec.aux.push_back(std::move(aux));
            } else {
                out.aux_index.push_back(-1);
                problems += "aux library not found: " + a.module + " (" + d.name + ")\n";
            }
        }
        // The cab: the .sd of the EngExtended Cabin shape (through a
        // .repaint file when the cab is a repaint of another one).
        if (!e.cabin.empty()) {
            std::optional<core::VirtualPath> sd;
            const auto cabin = car.file.parent().resolve(e.cabin);
            if (cabin) sd = shape_description(vfs, *cabin);
            if (sd) {
                if (const auto bytes = vfs.read(*sd)) {
                    auto cab = formats::parse_cab_description(*bytes);
                    if (cab.ok()) {
                        CabSpec& c = spec.cab;
                        c.present = true;
                        for (const auto& s : cab.switches) {
                            SwitchSpec ss;
                            ss.id = s.id;
                            ss.num_states = s.num_states;
                            ss.initial_state = s.initial_state;
                            ss.fixed_state = s.fixed_state;
                            ss.flags = s.flags;
                            ss.anim_time = s.anim_time;
                            ss.name = u16(s.name);
                            ss.precision[0] = s.precision[0];
                            ss.precision[1] = s.precision[1];
                            switch_key_bytes(s.keys, ss.keys);
                            ss.parent = static_cast<std::int16_t>(s.parent);
                            c.switches.push_back(std::move(ss));
                        }
                        add_shortcuts(vfs, *cabin, cab, c.switches);
                        for (const auto& dsp : cab.displays) {
                            DisplaySpec ds;
                            ds.id = dsp.id;
                            std::copy(dsp.range.begin(), dsp.range.end(), ds.range);
                            ds.granularity = dsp.granularity;
                            ds.flags = dsp.flags;
                            ds.name = u16(dsp.name);
                            ds.initial_state = dsp.initial_state;
                            if (dsp.screen.present) {
                                const auto& screen = dsp.screen;
                                ds.has_screen = true;
                                for (int k = 0; k < 3; ++k) {
                                    ds.corners[k] = screen.top_left[k];
                                    ds.corners[3 + k] = screen.top_right[k];
                                    ds.corners[6 + k] = screen.bottom_right[k];
                                }
                                ds.reference_screen = screen.reference_screen;
                                for (const auto& o : screen.objects) ds.screen.push_back(screen_object_spec(o));
                            }
                            c.displays.push_back(std::move(ds));
                        }
                        c.internal_lights = static_cast<std::uint32_t>(cab.lights.size());
                        c.tex_changes = static_cast<std::uint32_t>(cab.texture_changes.size());
                        c.doors = cab.interior_doors;
                        for (const auto& door : cab.doors) {
                            c.door_names.push_back(u16(door.name));
                            c.door_groups.push_back(door.global_id);
                        }
                        c.signal_lights = static_cast<std::uint32_t>(cab.signalization.lights.size());
                        c.signal_aspects = static_cast<std::uint32_t>(cab.signalization.aspects.size());
                        c.signal_scripted = cab.signalization.scripted;
                        c.switch_count = cab.switch_count;
                        for (const auto& lamp : cab.lights) c.internal_light_groups.push_back(lamp.global_id);
                        for (const auto& change : cab.texture_changes) {
                            c.tex_change_images.push_back(change.texture);
                            c.tex_change_groups.push_back(change.values[1]);
                        }
                        c.display_count = cab.display_count;
                        for (const auto& a : cab.animations) c.animations.push_back(u16(a.name));
                        out.cab = std::move(cab);
                    } else {
                        problems += "cab " + std::string(sd->str()) + ": " + cab.error + "\n";
                    }
                }
            }
        }
    }
    // Exterior lights by LightID: a DLL switches them (SwitchLight).
    spec.lights = static_cast<std::uint32_t>(d.lights.size());
    for (const auto& light : d.lights) {
        dllhost::LightSpec l;
        l.type = light.type;
        l.fade_in = light.fade_in;
        l.fade_out = light.fade_out;
        l.spot_exponent = light.spot_exponent;
        if (!light.states.empty()) {
            const auto& s = light.states.front();
            l.colour = s.colour;
            l.radius = s.radius;
            for (int k = 0; k < 3; ++k) l.position[k] = s.position[k];
        }
        spec.light_specs.push_back(l);
    }
    // The car's own shape: free animations and doors a DLL looks up by name.
    if (!d.shape.empty()) {
        if (const auto shape = car.file.parent().resolve(d.shape)) {
            if (const auto bytes = vfs.read(shape_description(vfs, *shape))) {
                const auto ext = formats::parse_exterior_details(*bytes);
                for (const auto& a : ext.animations) spec.animations.push_back(u16(a.name));
                spec.doors = static_cast<std::uint32_t>(ext.doors.size());
                for (const auto& door : ext.door_details) {
                    spec.door_times.push_back(door.anim_time);
                    spec.door_groups.push_back(door.global_id);
                }
            }
        }
    }
    return out;
}

} // namespace

std::unique_ptr<LegacyConsist> LegacyConsist::create(const core::Vfs& vfs, const std::filesystem::path& cache_root,
                                                     std::vector<LegacyCar> cars,
                                                     const std::filesystem::path& rts_root, std::string* error,
                                                     const std::filesystem::path& host_executable,
                                                     const core::VirtualPath* route) {
    std::unique_ptr<LegacyConsist> consist(new LegacyConsist());
    const bool without_brakes_dll_next = without_brakes_dll_choice.load();
    consist->without_brakes_dll_ = without_brakes_dll_next;
    SetupRequest setup;
    setup.rts_root = rts_root.u16string();
    setup.train_type = 1;
    bool any = false;
    std::string problems;
    SoundTables tables{setup.sound_tables, consist->sound_paths_, 0};
    consist->cabs_.resize(cars.size());
    consist->dll_.assign(cars.size(), false);
    consist->aux_index_.resize(cars.size());
    for (std::size_t i = 0; i < cars.size(); ++i) {
        auto built = build_spec(vfs, cache_root, cars[i], route, tables, problems, without_brakes_dll_next);
        consist->cabs_[i] = std::move(built.cab);
        consist->dll_[i] = built.dll;
        if (built.dll) any = true;
        consist->aux_index_[i] = std::move(built.aux_index);
        setup.cars.push_back(std::move(built.spec));
    }
    consist->sound_count_ = static_cast<std::int32_t>(setup.sound_tables.size());
    consist->train_first_ = {0, cars.size()};
    consist->cache_root_ = cache_root;
    consist->station_names_ = load_klub_station_names(vfs);
    if (route) consist->route_ = *route;
    if (!any) {
        if (error) *error = problems.empty() ? "no DLL in the consist" : problems;
        return nullptr;
    }
    std::string why;
    consist->host_ = LegacyHost::start(&why, host_executable);
    if (!consist->host_) {
        if (error) *error = why;
        return nullptr;
    }
    if (!consist->host_->setup(setup, consist->modules_)) {
        if (error) *error = "dllhost setup: " + consist->host_->error() + "\n" + consist->host_->take_messages();
        return nullptr;
    }
    consist->messages_ = problems + consist->host_->take_messages();
    consist->cars_ = std::move(cars);
    consist->refresh_stubbed();
    return consist;
}

LegacyConsist::~LegacyConsist() {
    outputs_->valid = false;
    outputs_->cars.clear();
}

std::vector<CarInput> LegacyConsist::inputs(const std::vector<const simulation::Train*>& trains,
                                            const LegacyWorld* world) const {
    std::vector<CarInput> out(cars_.size());
    for (std::size_t t = 0; t < trains.size() && t < this->trains(); ++t) {
    if (!trains[t]) continue;
    const auto& cars = trains[t]->cars();
    const std::size_t n = (std::min)(cars.size(), train_first_[t + 1] - train_first_[t]);
    for (std::size_t i = 0; i < n; ++i) {
        const auto& c = cars[i];
        CarInput& in = out[train_first_[t] + i];
        std::uint32_t flags = c.flipped ? 1u : 0u;
        flags |= static_cast<std::uint32_t>(c.pipe_ends) << 8;
        // Byte 3, WagonData::Cables: SMECable 0x10000 front / 0x20000 rear,
        // HVBus 0x40000 / 0x80000 (CarState::cables, the car menu's); byte 4
        // bits 1, 2: the bogies fastened (brake shoes).
        flags |= static_cast<std::uint32_t>(c.cables & 0x0Fu) << 16;
        flags |= static_cast<std::uint32_t>(c.chocks & 0x03u) << 24;
        in.flags = flags;
        in.coupling = c.coupler_open;
        in.mass = c.definition ? c.definition->mass : 0.0f;
        in.velocity = c.velocity;
        in.acceleration = c.acceleration;
        in.resistance = c.resistance;
        in.slope = c.grade;
        in.zord = c.offset;
        // The player's train's coordinate on each engine (RTS 0x50B690).
        if (t == 0 && world && world->milepost.known && c.definition && c.definition->engine)
            in.current_milepost = simulation::car_milepost(world->milepost, c.offset);
        in.brake_force = c.brake_force;
        in.brake_cylinder = c.cylinder;
        in.independent = c.independent;
        in.auxiliary = c.auxiliary;
        in.control = c.control;
        in.train_pipe = c.pipe;
        in.charging_pipe = c.charging_pipe;
        in.handbrake = c.handbrake;
        in.main_reservoir = c.main_reservoir;
        in.wheelslip = c.wheelslip;
        // openRTS's 395 over Brakes.dll (own_valve; its demands of the last
        // step): the DLL's own valve gets its УР, which it keeps in
        // Engine::UR. Left to itself it drifted (after an emergency the
        // ЭД9М's held 0) and, its cocks opened, vented the pipe towards it:
        // taken for its ЭПК, the pipe fell to 0 (testers 2026-11-18).
        if (const auto* asked = trains[t]->demands_of(i); asked && asked->equalising_kept >= 0.0f) in.equalising = asked->equalising_kept;
        // RTS (0x5031D8): a release bit of EngineFlags is cleared once its
        // cylinder has bled empty (docs/reverse/brakes.md, section 7).
        if (const auto at = train_first_[t] + i; outputs_ && outputs_->valid && at < outputs_->cars.size() && outputs_->cars[at].has_engine) {
            const auto engine_flags = outputs_->cars[at].engine.engine_flags;
            if ((engine_flags & 1u) && c.cylinder <= 0.001f) in.clear_engine_flags |= 1u;
            if ((engine_flags & 2u) && c.independent <= 0.001f) in.clear_engine_flags |= 2u;
        }
        if (c.definition && c.definition->engine && c.definition->engine->kind == formats::EngineKind::electric) {
            // The route's line (Electrification::apply) when modelled, else the world's.
            if (c.line_voltage >= 0.0f) {
                in.line_voltage = c.line_voltage;
                in.line_frequency = c.line_frequency;
                in.line_current = c.line_current;
                in.line_burnt = c.line_burnt ? 1 : 0;
            } else if (world) {
                in.line_voltage = world->line_voltage;
                in.line_frequency = world->line_frequency;
            }
        }
    }
    }
    return out;
}

namespace {

// A DLL has power: a pantograph up, a diesel running, its own compressor
// on, or traction (a cold start's compressors wait for it).
bool has_power(const std::vector<CarOutput>& cars) {
    return std::any_of(cars.begin(), cars.end(), [](const CarOutput& c) {
        const auto& e = c.engine;
        return c.has_engine && (e.panto != 0 || e.diesel_on != 0 || e.main_reservoir_rate > 0.0f || e.rpm > 0.0f || std::abs(e.force) > 1.0f);
    });
}

} // namespace


bool LegacyConsist::enter(std::size_t car, const simulation::Train& train, bool running, int state) {
    if (!host_ || !host_->alive()) return false;
    EnterRequest request;
    request.car = static_cast<std::uint32_t>(car);
    request.running = running;
    request.state = state;
    LegacyWorld world;
    request.cars = inputs(train, &world);
    StepReply reply;
    const bool ok = host_->enter(request, reply);
    messages_ += host_->take_messages();
    if (!ok) return false;
    player_ = car;
    entered_air_ = std::move(reply.air);
    outputs_->cars = std::move(reply.cars);
    outputs_->valid = true;
    if (outputs_->cold && has_power(outputs_->cars)) outputs_->cold = false;
    return true;
}

int LegacyConsist::aux_library(const std::size_t car, const std::string_view name) const {
    if (car >= cars_.size() || car >= aux_index_.size() || !cars_[car].definition || !cars_[car].definition->engine) return -1;
    const auto& libraries = cars_[car].definition->engine->aux_libraries;
    for (std::size_t k = 0; k < libraries.size() && k < aux_index_[car].size(); ++k) {
        const auto& own = libraries[k].name;
        const bool same = own.size() == name.size() && std::equal(own.begin(), own.end(), name.begin(), [](const char a, const char b) {
                              return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
                          });
        if (same) return aux_index_[car][k];
    }
    return -1;
}

bool LegacyConsist::save_state(std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>>& states) {
    states.clear();
    if (!host_ || !host_->alive()) return false;
    std::vector<CarDllState> saved;
    const bool ok = host_->save_state(saved);
    messages_ += host_->take_messages();
    if (!ok) return false;
    for (auto& s : saved) {
        if (s.car >= cars_.size() || cars_[s.car].key == 0) continue;
        klub_save(cars_[s.car].key, s.bytes); // openRTS's КЛУБ after the DLL's bytes
        states.emplace_back(cars_[s.car].key, std::move(s.bytes));
    }
    return true;
}

bool LegacyConsist::restore_state(const std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>>& states) {
    if (!host_ || !host_->alive()) return false;
    std::vector<CarDllState> by_index;
    for (const auto& [key, bytes] : states) {
        if (key == 0) continue;
        for (std::size_t i = 0; i < cars_.size(); ++i) {
            if (cars_[i].key != key) continue;
            auto dll = bytes;
            klub_restore(i, dll);
            by_index.push_back({.car = static_cast<std::uint32_t>(i), .bytes = std::move(dll)});
        }
    }
    if (by_index.empty()) return true;
    StepReply reply;
    const bool ok = host_->restore_state(by_index, reply);
    messages_ += host_->take_messages();
    if (!ok) return false;
    outputs_->cars = std::move(reply.cars);
    outputs_->valid = true;
    return true;
}

bool enter_new_train(LegacyConsist& consist, const std::size_t car, simulation::Train& train, const bool running, const int state) {
    const std::size_t count = train.cars().size();
    // The main reservoirs the train had: an engine whose DLL Init does not
    // run here (not in the player's group, no DLL) keeps its own (else a
    // ВЛ80Т section standing apart started with 0, 2026-11-13).
    std::vector<float> reservoirs(count);
    for (std::size_t i = 0; i < count; ++i) {
        reservoirs[i] = train.cars()[i].main_reservoir;
        train.set_air(i, {});
    }
    if (!consist.enter(car, train, running, state)) {
        for (std::size_t i = 0; i < count; ++i) train.set_air(i, {.main_reservoir = reservoirs[i]});
        return false;
    }
    const auto& air = consist.entered_air();
    if (car >= air.size() || air.size() != count) return true;
    const auto& from = air[car];
    // Started cold (TrainState 0) the main reservoirs are as Init left them,
    // empty unless a DLL fills them: openRTS put 8.5 back where Init left 0
    // (the ЭД4М's cold start had air in them; testers, 2026-11-16).
    const bool cold = state == 0;
    consist.set_cold_start(cold);
    // A pipe the DLL charged has its reservoirs charged with it: MaDaDev's
    // Brakes.dll sets the pipe in Init (the ЭП2Д 4.4) and leaves the ЗР and
    // the РК as given (0), so under the realistic air the whole train's ЗР
    // started empty and charged for minutes: the cylinders filled no higher
    // than it (the ЭП2Д's 0.43 in emergency, its V nothing; testers,
    // 2026-11-16: "затормозил ... а ни на блоке, ни манометрами не набралось тц").
    const auto charged = [&from](const float reservoir) {
        return from.pipe > 0.5f && reservoir + 0.2f < from.pipe ? from.pipe : reservoir;
    };
    for (std::size_t i = 0; i < count; ++i) {
        train.set_air(i, {.pipe = from.pipe,
                          .charging_pipe = from.charging_pipe,
                          .cylinder = from.cylinder,
                          .auxiliary = charged(from.auxiliary),
                          .control = charged(from.control),
                          .main_reservoir = cold || air[i].main_reservoir > 0.0f ? air[i].main_reservoir : reservoirs[i]});
    }
    return true;
}

bool LegacyConsist::step(double dt, const std::vector<const simulation::Train*>& trains, const LegacyWorld& world) {
    if (!host_ || !host_->alive()) {
        outputs_->valid = false;
        return false;
    }
    core::SequentialZone part;
    part.open("DLL: request");
    StepRequest request;
    request.day_time = world.day_time;
    request.dt = static_cast<float>(dt);
    request.air_temperature = world.air_temperature;
    request.night = world.night ? 1 : 0;
    // RTS's train direction (train +4 bit 0x40): the way the player's cab
    // faces, its car turned or its Cabin ( x 1 ) turned in it. The leading
    // cab gets the signal (Cabin::Signal), and DLLs read it (RTS_TRAINDIR,
    // Run's `reversed`): the ЭД9М's rear cab, driven with car 0's front
    // ahead, took its own end for the head of the group, saw no motor car
    // behind it and lit "Напряжение сети", its doors and ЭПТ dead (testers
    // 2026-11-18); with the train turned round it works as the front cab.
    request.train_direction = 1;
    if (!trains.empty() && trains[0] && player_ >= train_first_[0] && player_ < train_first_[1] && player_ < cars_.size()) {
        const auto& cars = trains[0]->cars();
        const std::size_t i = player_ - train_first_[0];
        const auto* definition = cars_[player_].definition;
        const bool cab_turned = definition && definition->engine && !definition->engine->cabin.empty() && definition->engine->cabin_param == 1;
        if (i < cars.size() && cars[i].flipped != cab_turned) request.train_direction = 0;
    }
    request.cars = inputs(trains, &world);
    request.route = world.route;
    request.signal = world.signal;
    request.event = world.event;
    request.signals_ahead = world.signals_ahead;
    request.alsn_ahead = world.alsn_ahead;
    request.alsn_behind = world.alsn_behind;
    request.alsn_passing = world.alsn_passing;
    request.next_limit = world.next_limit;
    request.next_limit_distance = world.next_limit_distance;
    // What the host has not been sent yet: the items the list names and the
    // route database's signals the SignalInputs name.
    request.track_items = world.track_items;
    request.items_range = world.items_range;
    request.tape_car = world.tape_car;
    request.want_aux_vars = world.record_aux_vars ? 1 : 0;
    // The libraries' sounds by the host's numbering of the libraries.
    for (const auto& p : world.sound_pairs) {
        auto q = p;
        if (q.object >= 0) {
            const auto& map = q.car < aux_index_.size() ? aux_index_[q.car] : std::vector<std::int16_t>{};
            if (static_cast<std::size_t>(q.object) >= map.size() || map[static_cast<std::size_t>(q.object)] < 0) continue;
            q.object = map[static_cast<std::size_t>(q.object)];
        }
        request.sound_pairs.push_back(q);
    }
    if (world.tdb) {
        for (const auto& e : world.track_items) {
            if (e.item >= world.tdb->items.size() || !items_sent_.insert(e.item).second) continue;
            request.items.push_back(item_spec(world.tdb->items[e.item], world.route_speed_limit, world.limits_without_tolerance));
        }
    }
    if (world.route_database) {
        const auto& units = world.route_database->signal_units;
        for (const auto* list : {&request.signals_ahead, &request.alsn_ahead, &request.alsn_behind, &request.alsn_passing}) {
            for (const auto& s : *list) {
                if (s.unit < 0 || static_cast<std::size_t>(s.unit) >= units.size() || !units_sent_.insert(s.unit).second) continue;
                const auto& unit = units[static_cast<std::size_t>(s.unit)];
                SignalUnitSpec spec;
                spec.id = s.unit;
                spec.tile[0] = static_cast<std::int16_t>(unit.position.tile_x);
                spec.tile[1] = static_cast<std::int16_t>(unit.position.tile_z);
                spec.position[0] = static_cast<float>(-unit.position.x);
                spec.position[1] = static_cast<float>(unit.position.y);
                spec.position[2] = static_cast<float>(unit.position.z);
                spec.name = u16(unit.name);
                request.signal_units.push_back(std::move(spec));
            }
        }
    }
    klub_before(request);
    recorder_before(request);
    stub_before(request, trains);
    part.close(nullptr);
    StepReply reply;
    const bool ok = host_->step(request, reply);
    messages_ += host_->take_messages();
    if (!ok) {
        outputs_->valid = false;
        return false;
    }
    outputs_->cars = std::move(reply.cars);
    outputs_->valid = true;
    sections_after();
    own_air_after();
    klub_after(request, world);
    recorder_after(request, world);
    stub_after();
    if (outputs_->cold && has_power(outputs_->cars)) outputs_->cold = false;
    items_wanted_ = reply.items_wanted;
    if (reply.tape_set) {
        tape_request_ = {.set = true, .restart = reply.tape_restart != 0, .car = reply.tape_car};
    }
    return true;
}

bool LegacyConsist::reconsist(const core::Vfs& vfs, std::vector<LegacyCar> cars, const simulation::Train& train) {
    if (player_ >= cars_.size()) return false;
    const auto player_key = cars_[player_].key;
    if (!player_key || std::none_of(cars.begin(), cars.end(), [&](const LegacyCar& c) { return c.key == player_key; })) return false;
    std::vector<std::vector<LegacyCar>> trains;
    trains.push_back(std::move(cars));
    return reconsist(vfs, std::move(trains), {&train});
}

bool LegacyConsist::reconsist(const core::Vfs& vfs, std::vector<std::vector<LegacyCar>> trains,
                              const std::vector<const simulation::Train*>& sims) {
    if (!host_ || !host_->alive() || trains.empty()) return false;
    const auto player_key = player_ < cars_.size() ? cars_[player_].key : 0u;
    std::unordered_map<std::uint32_t, std::size_t> before;
    for (std::size_t i = 0; i < cars_.size(); ++i)
        if (cars_[i].key) before.emplace(cars_[i].key, i);
    ReconsistRequest request;
    std::string problems;
    SoundTables tables{request.sound_tables, sound_paths_, sound_count_};
    std::vector<LegacyCar> cars;
    std::vector<std::size_t> first{0};
    for (auto& train : trains) {
        for (auto& car : train) cars.push_back(std::move(car));
        first.push_back(cars.size());
    }
    std::size_t player = SIZE_MAX;
    for (std::size_t i = 0; i < first[1] && player_key; ++i)
        if (cars[i].key == player_key) player = i;
    std::vector<formats::CabDescription> cabs(cars.size());
    std::vector<bool> dll(cars.size(), false);
    std::vector<std::vector<std::int16_t>> aux(cars.size());
    std::vector<bool> kept(cars_.size(), false);
    request.trains.resize(trains.size());
    for (std::size_t t = 0; t < trains.size(); ++t)
        request.trains[t].type = static_cast<std::uint8_t>(t < train_types_.size() ? train_types_[t] : t == 0 ? 1u : 3u); // the player's, LOOSE
    for (std::size_t i = 0; i < cars.size(); ++i) {
        const auto t = static_cast<std::size_t>(std::upper_bound(first.begin(), first.end(), i) - first.begin()) - 1;
        ReconsistCar entry;
        entry.spec.uid = cars[i].uid;
        entry.spec.key = cars[i].key;
        if (const auto it = before.find(cars[i].key); it != before.end() && !kept[it->second]) {
            const auto from = it->second;
            kept[from] = true;
            entry.from = static_cast<std::int32_t>(from);
            cabs[i] = std::move(cabs_[from]);
            dll[i] = dll_[from];
            aux[i] = std::move(aux_index_[from]);
        } else if (const auto p = parked_.find(cars[i].key); p != parked_.end()) {
            entry.from = reconsist_parked;
            cabs[i] = std::move(p->second.cab);
            dll[i] = p->second.dll;
            aux[i] = std::move(p->second.aux_index);
            if (dll[i]) ++restored_;
            parked_.erase(p);
        } else {
            auto built = build_spec(vfs, cache_root_, cars[i], route_ ? &*route_ : nullptr, tables, problems, without_brakes_dll_);
            entry.spec = std::move(built.spec);
            cabs[i] = std::move(built.cab);
            dll[i] = built.dll;
            aux[i] = std::move(built.aux_index);
        }
        request.trains[t].cars.push_back(std::move(entry));
    }
    // What left waits, as the host keeps its mirror.
    for (std::size_t i = 0; i < cars_.size(); ++i) {
        if (kept[i] || !cars_[i].key) continue;
        parked_[cars_[i].key] = Parked{cars_[i], std::move(cabs_[i]), dll_[i], std::move(aux_index_[i])};
    }
    sound_count_ += static_cast<std::int32_t>(request.sound_tables.size());
    cars_ = std::move(cars);
    cabs_ = std::move(cabs);
    dll_ = std::move(dll);
    aux_index_ = std::move(aux);
    train_first_ = std::move(first);
    refresh_stubbed();
    request.running = !sims.empty() && sims[0] && std::abs(sims[0]->speed()) > 0.01;
    request.inputs = inputs(sims, nullptr);
    StepReply reply;
    const bool ok = host_->reconsist(request, reply);
    messages_ += problems + host_->take_messages();
    if (!ok) {
        outputs_->valid = false;
        return false;
    }
    player_ = player;
    outputs_->cars = std::move(reply.cars);
    outputs_->valid = true;
    if (outputs_->cold && has_power(outputs_->cars)) outputs_->cold = false;
    return true;
}

bool LegacyConsist::operate(std::size_t car, std::uint32_t id, std::uint32_t state) {
    if (!host_ || !host_->alive()) return false;
    bool allowed = false;
    const bool ok = host_->can_switch({static_cast<std::uint32_t>(car), id, state}, allowed);
    messages_ += host_->take_messages();
    // A КЛУБ key acts at the click (Brakes.dll: CanSwitch to a state not 0).
    if (ok && state != 0) klub_key(flat(0, car), id);
    return ok && allowed;
}

bool LegacyConsist::drag(std::size_t car, std::uint32_t id, float delta, bool release) {
    if (!host_ || !host_->alive()) return false;
    bool moved = false;
    SwitchState state;
    const SwitchState before = outputs_->valid && car < outputs_->cars.size() && id < outputs_->cars[car].switches.size()
                                   ? outputs_->cars[car].switches[id]
                                   : SwitchState{};
    const bool ok = host_->drag_switch({static_cast<std::uint32_t>(car), id, delta, release ? 1u : 0u}, moved, state);
    messages_ += host_->take_messages();
    if (ok && moved && state.set_state != 0 && state.set_state != before.set_state) klub_key(flat(0, car), id);
    // The cab shows the dragged switch at once (RTS moves its node in the
    // mouse handler), not at the next 50 ms step.
    if (ok && outputs_->valid && car < outputs_->cars.size() && id < outputs_->cars[car].switches.size())
        outputs_->cars[car].switches[id] = state;
    return ok && moved;
}

const CarOutput* LegacyConsist::output(std::size_t car) const { return output(0, car); }

const CarOutput* LegacyConsist::output(std::size_t train, std::size_t car) const {
    const auto at = flat(train, car);
    return outputs_->valid && at < outputs_->cars.size() ? &outputs_->cars[at] : nullptr;
}

const formats::CabDescription* LegacyConsist::cab(std::size_t car) const {
    const auto at = flat(0, car);
    return at < cabs_.size() && !cabs_[at].switches.empty() ? &cabs_[at] : nullptr;
}

CabBrakes LegacyConsist::cab_brakes(const std::size_t train, const std::size_t car) const {
    const auto at = flat(train, car);
    if (!outputs_->valid || at >= outputs_->cars.size() || at >= cars_.size() || at >= cabs_.size()) return {};
    const auto* definition = cars_[at].definition;
    if (!definition || !definition->engine || !definition->engine->brakes_library.present) return {};
    // As the controller reads them (LegacyController).
    const auto& all = definition->engine->brakes_library;
    const auto cab = working_cab(all, outputs_->cars[at].switches);
    const auto ids = cab == 0 ? all : all.for_cab(cab);
    return brake_cab_controls(ids, cabs_[at]).read(ids, outputs_->cars[at].switches);
}

std::size_t working_cab(const formats::EngineDefinition::BrakesLibrary& ids, const std::vector<dllhost::SwitchState>& switches) {
    if (ids.cabs.size() <= 1 || ids.block_operating < 0) return 0;
    for (std::size_t k = 0; k < ids.cabs.size(); ++k) {
        const auto block = ids.cabs[k].block;
        if (block >= 0 && static_cast<std::size_t>(block) < switches.size()
            && static_cast<std::int32_t>(switches[static_cast<std::size_t>(block)].state) == ids.block_operating)
            return k;
    }
    return 0;
}

BrakeCabControls brake_cab_controls(const formats::EngineDefinition::BrakesLibrary& ids, const formats::CabDescription& cab) {
    BrakeCabControls controls;
    for (std::size_t k = 1; k < ids.cabs.size(); ++k) controls.more.push_back(brake_cab_controls(ids.for_cab(k), cab));
    const auto control = [&](const std::int32_t id) { return id >= 0 ? cab.find_switch(static_cast<std::uint32_t>(id)) : nullptr; };
    if (const auto* valve = control(ids.valve)) controls.valve = valve_states(*valve);
    if (const auto* controller = control(ids.controller)) controls.controller = valve_states(*controller);
    // The reducer: its hints are the pressures, "[4.0]" .. "[5.9]".
    if (const auto* reducer = control(ids.reducer)) {
        for (const auto& hint : reducer->state_hints) {
            std::string s;
            for (const char c : hint) {
                if ((c >= '0' && c <= '9') || c == '.') s += c;
                else if (c == ',') s += '.';
            }
            const float value = s.empty() ? 0.0f : static_cast<float>(std::atof(s.c_str()));
            controls.reducer.push_back(value >= 3.0f && value <= 8.0f ? value : 0.0f);
        }
    }
    // The combined valve усл. 114: "Двойная тяга", "Нормальное", "Экстренное".
    if (const auto* combined = control(ids.combined)) {
        for (const auto& hint : combined->state_hints) {
            const auto s = core::fold_case(hint);
            const bool twin = s.find("\xd0\xb4\xd0\xb2\xd0\xbe\xd0\xb9\xd0\xbd") != std::string::npos;     // двойн
            const bool emergency = s.find("\xd1\x8d\xd0\xba\xd1\x81\xd1\x82\xd1\x80") != std::string::npos; // экстр
            controls.combined.push_back(static_cast<std::int8_t>(twin ? 1 : emergency ? 2 : 0));
        }
    }
    return controls;
}

CabBrakes BrakeCabControls::read(const formats::EngineDefinition::BrakesLibrary& ids, const std::vector<dllhost::SwitchState>& switches,
                                 const CabBrakes& last) const {
    CabBrakes now = last;
    const auto state = [&](const std::int32_t id) -> int {
        return id >= 0 && static_cast<std::size_t>(id) < switches.size() ? static_cast<int>(switches[static_cast<std::size_t>(id)].state) : -1;
    };
    // The 395: the cab's hints, else 0..6 are I..VI. With a КПТ (BR_KPTID1,
    // the ЭП2Д) the handle moved last sets the position, the 395 at first:
    // until 2026-11-16 the КПТ did nothing to openRTS's valve, while
    // Brakes.dll works the pipe from it (testers braked by it, the ТЦ empty).
    const auto position_of = [](const std::vector<std::int8_t>& states, const int s) {
        if (states.empty()) return s <= 6 ? s : -1;
        return static_cast<std::size_t>(s) < states.size() ? static_cast<int>(states[static_cast<std::size_t>(s)]) : -1;
    };
    const int valve_state = state(ids.valve);
    const int controller_state = state(ids.controller);
    const bool valve_moved = valve_state >= 0 && valve_state != last.raw;
    const bool controller_moved = controller_state >= 0 && last.controller_raw >= 0 && controller_state != last.controller_raw;
    if (valve_state >= 0) now.raw = valve_state;
    if (controller_state >= 0) now.controller_raw = controller_state;
    int known = -1;
    if (controller_moved) known = position_of(controller, controller_state);
    else if (valve_moved) known = position_of(valve, valve_state);
    else if (last.position < 0) known = valve_state >= 0 ? position_of(valve, valve_state) : controller_state >= 0 ? position_of(controller, controller_state) : -1;
    if (known >= 0) now.position = known;
    if (const int s = state(ids.reducer); s >= 0 && static_cast<std::size_t>(s) < reducer.size() && reducer[static_cast<std::size_t>(s)] > 0.0f) {
        now.charging = reducer[static_cast<std::size_t>(s)];
    }
    if (const int s = state(ids.combined); s >= 0 && static_cast<std::size_t>(s) < combined.size()) {
        now.combined = combined[static_cast<std::size_t>(s)];
    }
    if (const int s = state(ids.block); s >= 0 && ids.block_operating >= 0) now.blocked = s != ids.block_operating;
    if (const int s = state(ids.separating); s >= 0) now.separated = s == 0;
    return now;
}

bool LegacyConsist::has_dll(std::size_t car) const { return has_dll(0, car); }

bool LegacyConsist::has_dll(std::size_t train, std::size_t car) const {
    const auto at = flat(train, car);
    return at < dll_.size() && dll_[at];
}

bool LegacyConsist::alive() const { return host_ && host_->alive(); }

std::string LegacyConsist::error() const { return host_ ? host_->error() : std::string("no host"); }

std::string LegacyConsist::take_messages() { return std::exchange(messages_, {}); }

std::unique_ptr<simulation::LocomotiveController> LegacyConsist::controller(std::size_t car) const { return controller(0, car); }

std::unique_ptr<simulation::LocomotiveController> LegacyConsist::controller(std::size_t train, std::size_t car) const {
    const auto at = flat(train, car);
    // The cab's Brakes.dll controls (the 395, the reducer, the combined valve)
    // and what their states are.
    BrakeCabControls cab;
    if (at < cars_.size() && at < cabs_.size() && cars_[at].definition && cars_[at].definition->engine) {
        cab = brake_cab_controls(cars_[at].definition->engine->brakes_library, cabs_[at]);
    }
    // Its Brakes.dll's parameters, for openRTS's cab brake devices.
    BrakesParameters brakes;
    if (at < cars_.size() && cars_[at].definition && cars_[at].definition->engine) brakes = brakes_parameters(*cars_[at].definition->engine);
    // Its train's wagons, as RTSGetInteger 2 counts them (not engines).
    std::uint32_t wagons = 0;
    if (train + 1 < train_first_.size())
        for (std::size_t k = train_first_[train]; k < train_first_[train + 1] && k < cars_.size(); ++k)
            if (cars_[k].definition && !cars_[k].definition->engine) ++wagons;
    return std::make_unique<LegacyController>(outputs_, at, air_, std::move(cab), std::move(brakes), aux_library(at, "BRAKES"), wagons);
}

std::vector<std::int8_t> valve_states(const formats::CabSwitch& control) {
    // The cabs name them (MaDaDev's, 2026-11-16): a Roman numeral first
    // ("[V торможение]", "[VЭ Положение]", "[Vа Торможение замедленным
    // темпом]"), alone ("[Va]"), or words only ("[торможение]",
    // "[Торможение ЭПТ]", "Торможение замедленным темпом/ЭПТ торможение").
    // The ЧС7's "[торможение]" and "[VЭ торможение]" were read as no
    // position, and openRTS's valve stayed in II.
    const auto position = [&](const std::string& hint) -> int {
        // Lower case; brackets, quotes and dots off; tokens at spaces and "/".
        std::string s;
        for (const char c : hint)
            if (c != '[' && c != ']' && c != '"' && c != '.') s += c;
        s = core::fold_case(s);
        std::vector<std::string> tokens;
        std::string token;
        for (const char c : s + " ") {
            if (c == ' ' || c == '/' || c == '\t') {
                if (!token.empty()) tokens.push_back(token);
                token.clear();
            } else {
                token += c;
            }
        }
        // A Roman numeral first; the Cyrillic а / э of "Vа" / "VЭ" read as Latin.
        if (!tokens.empty()) {
            auto roman = tokens.front();
            for (const auto& [from, to] : {std::pair<const char*, const char*>{"\xd0\xb0", "a"}, {"\xd1\x8d", "e"}, {"-", ""}}) {
                for (auto at = roman.find(from); at != std::string::npos; at = roman.find(from)) roman.replace(at, std::strlen(from), to);
            }
            if (roman == "i") return 0;
            if (roman == "ii") return 1;
            if (roman == "iii") return 2;
            if (roman == "iv") return 3;
            if (roman == "va" || roman == "ve") return 4;
            if (roman == "v") return 5;
            if (roman == "vi") return 6;
        }
        // Words.
        const auto has = [&](const char* part) { return s.find(part) != std::string::npos; };
        const bool braking = has("\xd1\x82\xd0\xbe\xd1\x80\xd0\xbc");                           // торм
        if (has("\xd1\x8d\xd0\xba\xd1\x81\xd1\x82\xd1\x80")) return 6;                       // экстр
        if (has("\xd0\xb7\xd0\xb0\xd0\xbc\xd0\xb5\xd0\xb4\xd0\xbb")) return 4;               // замедл: Va
        if (braking && has("\xd1\x8d\xd0\xbf\xd1\x82")) return 4;                            // торможение ЭПТ: VЭ
        if (has("\xd0\xbf\xd0\xbe\xd0\xb5\xd0\xb7\xd0\xb4")) return 1;                       // поезд
        if (has("\xd0\xbf\xd0\xb5\xd1\x80\xd0\xb5\xd0\xba\xd1\x80")) {                     // перекр
            return has("\xd0\xb1\xd0\xb5\xd0\xb7") ? 2 : 3;                                       // без питания: III
        }
        if (has("\xd0\xb7\xd0\xb0\xd1\x80\xd1\x8f\xd0\xb4") || has("\xd0\xbe\xd1\x82\xd0\xbf\xd1\x83\xd1\x81\xd0\xba")) return 0; // заряд, отпуск
        if (has("\xd1\x81\xd0\xbb\xd1\x83\xd0\xb6") || braking) return 5;                     // служебное, торможение: V
        return -1;
    };
    std::vector<std::int8_t> states;
    bool any = false;
    for (const auto& hint : control.state_hints) {
        const int p = position(hint);
        any = any || p >= 0;
        states.push_back(static_cast<std::int8_t>(p));
    }
    if (any) return states;
    states.clear();
    if (control.num_states == 7)
        for (std::int8_t s = 0; s < 7; ++s) states.push_back(s);
    return states;
}

std::unique_ptr<simulation::LocomotiveController> make_legacy_controller(std::weak_ptr<const LegacyConsist::Outputs> outputs,
                                                                         const std::size_t car,
                                                                         std::shared_ptr<const DllAirSetting> air,
                                                                         BrakeCabControls cab, BrakesParameters brakes,
                                                                         const int brakes_library) {
    return std::make_unique<LegacyController>(std::move(outputs), car, std::move(air), std::move(cab), std::move(brakes),
                                              brakes_library);
}


// --- openRTS's cab brake devices' air sounds (own_brakes; docs/brakes-dll-replacement.md step 1 of the rest) ---

namespace {

// Brakes.dll's air sounds (docs/reverse/brakes-dll.md §10): the valve's,
// the ЭПТ's, the pipe gauge's, the 4ВК's, and their 31xx forms.
bool air_sound(const std::uint16_t code) { return (code >= 120 && code <= 137) || (code >= 3120 && code <= 3137); }

} // namespace

void LegacyConsist::own_air_after() {
    auto& o = *outputs_;
    if (trans_sound_.size() != o.cars.size()) trans_sound_.assign(o.cars.size(), -1);
    for (std::size_t i = 0; i < o.cars.size() && i < o.own_air.size(); ++i) {
        if (!o.own_air[i]) continue;
        o.own_air[i] = 0;
        auto& out = o.cars[i];
        auto& ours = o.own_air_sounds[i];
        const auto drop = [](std::vector<std::uint16_t>& triggers) {
            triggers.erase(std::remove_if(triggers.begin(), triggers.end(), air_sound), triggers.end());
        };
        // On Brakes.sms in place of Brakes.dll's.
        const int library = aux_library(i, "BRAKES");
        if (library >= 0 && static_cast<std::size_t>(library) < out.aux_sound.size()) {
            auto& triggers = out.aux_sound[static_cast<std::size_t>(library)].triggers;
            drop(triggers);
            triggers.insert(triggers.end(), ours.begin(), ours.end());
        }
        // On the locomotive's SMS too with TRANS_SOUND 1 or the train's DLL
        // asking, as Brakes.dll posts them.
        if (trans_sound_[i] < 0) {
            trans_sound_[i] = 0;
            if (i < cars_.size() && cars_[i].definition && cars_[i].definition->engine)
                trans_sound_[i] = brakes_parameters(*cars_[i].definition->engine).trans_sound ? 1 : 0;
        }
        const bool to_loco = trans_sound_[i] == 1 || (library >= 0 && static_cast<std::size_t>(library) < out.aux_exchange.size() &&
                                                      (out.aux_exchange[static_cast<std::size_t>(library)].before & 0x100u));
        if (to_loco) {
            drop(out.sound_ext);
            drop(out.sound_cab);
            out.sound_ext.insert(out.sound_ext.end(), ours.begin(), ours.end());
            out.sound_cab.insert(out.sound_cab.end(), ours.begin(), ours.end());
        }
        ours.clear();
    }
}

// --- openRTS's КЛУБ in place of Brakes.dll's (set_own_klub; step 3.4) -----------------

namespace {

// The КЛУБ's sounds of Brakes.sms (docs/reverse/brakes-dll.md §7).
bool klub_sound(const std::uint16_t code) { return code == 56 || code == 57 || (code >= 3192 && code <= 3199) || code == 3956 || code == 3957; }

// A saved КЛУБ after the DLL's bytes: its state, its size, the magic.
constexpr std::uint32_t klub_magic = 0x42554C4Bu;
static_assert(std::is_trivially_copyable_v<simulation::KlubState>);

std::u16string cp1251(const std::string_view text) {
    std::u16string out;
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x80) out.push_back(c);
        else if (c >= 0xC0) out.push_back(static_cast<char16_t>(0x0410 + (c - 0xC0)));
        else if (c == 0xA8) out.push_back(u'Ё');
        else if (c == 0xB8) out.push_back(u'ё');
        else out.push_back(u'?');
    }
    return out;
}

std::u16string upper_names(std::u16string s) {
    for (auto& c : s) {
        if (c >= u'a' && c <= u'z') c = static_cast<char16_t>(c - 32);
        else if (c >= u'а' && c <= u'я') c = static_cast<char16_t>(c - 32);
        else if (c == u'ё') c = u'Ё';
    }
    return s;
}

} // namespace

std::vector<std::pair<std::u16string, std::u16string>> load_klub_station_names(const core::Vfs& vfs) {
    // Brakes.dll reads it from RTS's folder: ../TRAINS/TRAINSET/mdd_Libraries/Brakes.
    std::vector<std::pair<std::u16string, std::u16string>> names;
    const auto path = core::VirtualPath::parse("TRAINS/TRAINSET/mdd_Libraries/Brakes/ChangeStanc.bto");
    if (!path || !vfs.exists(*path)) return names;
    const auto text = vfs.read(*path);
    if (!text) return names;
    std::size_t at = 0;
    while (at < text->size()) {
        auto end = text->find('\n', at);
        if (end == std::string::npos) end = text->size();
        std::string_view line(text->data() + at, end - at);
        at = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        const auto tab = line.find('\t');
        if (tab == std::string_view::npos || tab == 0) continue;
        names.emplace_back(upper_names(cp1251(line.substr(0, tab))), upper_names(cp1251(line.substr(tab + 1))));
    }
    return names;
}

const simulation::KlubState* LegacyConsist::klub_state(const std::size_t car) const {
    const auto at = flat(0, car);
    if (at >= cars_.size()) return nullptr;
    const auto found = klub_.find(cars_[at].key);
    return found != klub_.end() && found->second && found->second->on ? &found->second->state : nullptr;
}

void LegacyConsist::klub_key(const std::size_t at, const std::uint32_t id) {
    if (at >= cars_.size()) return;
    const auto found = klub_.find(cars_[at].key);
    if (found == klub_.end() || !found->second || !found->second->on) return;
    // The working cab's keys only (Brakes.dll's SwitchKLUB).
    const auto& p = found->second->parameters;
    const int cab = at < outputs_->cars.size() ? p.working_cab(outputs_->cars[at].switches) : 0;
    if (cab < 0) return;
    if (const auto* press = p.cabs[static_cast<std::size_t>(cab)].key(static_cast<std::int32_t>(id))) found->second->keys.push_back(*press);
    // САУТ's (the working cab's).
    const auto& saut = p.cabs[static_cast<std::size_t>(cab)].saut_keys;
    for (std::size_t k = 0; k < saut.size(); ++k)
        if (saut[k] >= 0 && static_cast<std::uint32_t>(saut[k]) == id) found->second->saut_keys.push_back(static_cast<simulation::SautKey>(k));
}

LegacyConsist::KlubCar* LegacyConsist::klub_car(const std::size_t i) {
    if (i >= cars_.size()) return nullptr;
    const auto key = cars_[i].key;
    const auto* definition = cars_[i].definition;
    if (!key || !definition || !definition->engine) return nullptr;
    auto found = klub_.find(key);
    if (found == klub_.end()) {
        // A КЛУБ of Brakes.dll that openRTS's takes: PB_SETUP 2, in each
        // of its cabs (KLUB_*<n>).
        std::unique_ptr<KlubCar> made;
        auto parameters = klub_parameters(*definition->engine);
        if (parameters.present && (parameters.settings.setup & 2) && parameters.setup_index >= 0) {
            made = std::make_unique<KlubCar>();
            made->parameters = std::move(parameters);
        }
        found = klub_.emplace(key, std::move(made)).first;
    }
    return found->second.get();
}

void LegacyConsist::klub_before(StepRequest& request) {
    const bool want = own_klub_ && air_->mode != DllAir::rts;
    const std::size_t first = train_first_.size() > 1 ? train_first_[0] : 0;
    const std::size_t end = train_first_.size() > 1 ? train_first_[1] : 0;
    klub_working_ = 0;
    for (std::size_t i = 0; i < cars_.size(); ++i) {
        auto* car = klub_car(i);
        if (!car) continue;
        const int library = aux_library(i, "BRAKES");
        const bool on = want && library >= 0 && i >= first && i < end;
        const auto index = static_cast<std::uint32_t>(i);
        const auto lib = static_cast<std::uint16_t>(library);
        const auto setup = static_cast<std::uint16_t>(car->parameters.setup_index);
        if (!on) {
            if (car->on && library >= 0) // Brakes.dll's КЛУБ back
                request.aux_parameters.push_back({index, lib, setup, static_cast<float>(car->parameters.settings.setup)});
            car->on = false;
            continue;
        }
        if (!car->on && !car->resume) {
            car->state = {};
            car->screens.clear();
            car->switches.clear();
        }
        car->on = true;
        car->resume = false;
        ++klub_working_;
        // Brakes.dll without its КЛУБ (updateKLUB, SwitchKLUB, the ЭПК part of
        // Run); sent every step (a reconsist builds the parameters again).
        // Brakes.dll without its ТСКБМ and САУТ too:
        // openRTS's run in the КЛУБ's step.
        request.aux_parameters.push_back({index, lib, setup, static_cast<float>(car->parameters.settings.setup & ~7u)});
        // What its КЛУБ would have written in its Run, put in just before
        // that Run (the train's DLL reads it in its next; Brakes.dll's other
        // parts go on over it as they did): docs/reverse/brakes-dll.md §5, §6.
        if (!car->state.started) continue;
        const auto& s = car->state;
        // САУТ's coefficient where Brakes.dll's keeps it (Engine +120).
        if ((car->parameters.settings.setup & 1) && i < request.cars.size())
            request.cars[i].engine_flags_word = static_cast<std::int32_t>(s.saut_coefficient);
        const auto put = [&](const std::uint16_t offset, const std::uint32_t mask, const std::uint32_t bits,
                             const bool after_run = false) {
            request.aux_writes.push_back({index, lib, offset, mask, bits, after_run ? 1u : 0u});
        };
        const auto put_float = [&](const std::uint16_t offset, const float v, const bool after_run = false) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &v, sizeof bits);
            put(offset, ~0u, bits, after_run);
        };
        // Bit 4 every step (updateKLUB clears and sets it); bit 1 only when
        // the КЛУБ wrote it (its trip, an answer, the ЭПК key off): set by
        // Brakes.dll's other parts, it stays theirs.
        put(32, 4u, car->out.safety_powered ? 4u : 0u);
        if (car->out.epk_bit_written) put(32, 1u, s.epk_bit ? 1u : 0u);
        put(8, ~0u, s.flags); // ТСКБМ's request (0x10) too: openRTS's
        put(4, 0x600u | 0x8u | 0x2000u | 0x400000u, s.device); // КОН, its vent; ТСКБМ on, its whistle, its warning
        put_float(24, s.timer);
        put_float(12, s.tskbm_timer); // ТСКБМ's timer (the screens, the train's DLLs)
        // The ЭПК part's last aspect: a float, 0 unpowered, every step the
        // ALSN is on (docs/reverse/brakes-dll.md §6).
        put_float(48, static_cast<float>(s.epk_aspect));
        // The limit and the next target after that Run: САУТ (0x4022A0)
        // runs in it before the КЛУБ and reads them as _ALSN gave them this
        // step; Brakes.dll's КЛУБ puts its own there after.
        put_float(80, s.limit, true);
        put_float(116, s.target_speed, true);
        put_float(120, s.target_distance, true);
        put_float(140, s.permitted);
        put_float(144, s.signal_metres);
        put(148, ~0u, s.white_speed);
        put(160, ~0u, s.track);
        put(164, ~0u, s.driver);
        put(168, ~0u, s.train_number);
        put(172, ~0u, s.axles);
        put(176, ~0u, s.wagons);
        put(180, ~0u, s.mass);
        put(240, ~0u, s.alsen_table);
        put(244, ~0u, s.alsen_count);
        // САУТ's: its "on", its braking (the train's DLLs cut
        // traction), its flags beside the КЛУБ's,
        // timers, target, speed shown (the ЧС7's, 2ЭС6's, ЭП1М's screens) and
        // distance.
        if (car->parameters.settings.setup & 1) {
            put(4, 0x40u, s.device);
            put(32, 8u, s.saut_braking ? 8u : 0u);
            put(88, ~0u, s.limit_marks | s.saut_flags);
            put_float(92, s.saut_display_timer);
            put_float(96, s.saut_reminder);
            put_float(100, s.saut_target);
            put_float(104, s.saut_mode_distance);
            put_float(108, s.saut_mode_start);
            put_float(112, s.saut_mode_time);
            put(124, ~0u, s.saut_aspect);
            put_float(252, s.saut_coefficient_timer);
            put_float(256, s.saut_shown);
            put_float(260, s.saut_stop_timer);
            put_float(288, s.saut_clock);
            put_float(292, s.saut_caught);
            put_float(308, s.saut_distance);
        }
    }
    // Its screens read what Brakes.dll's other parts keep (ТСКБМ's timer,
    // САУТ's distance, the ЭП20's var + 296): the library's variables back.
    if (klub_working_) request.want_aux_vars = 1;
}

// Each car's driven car of its locomotive's sections: next to a driven
// one (LocoFlags 1) the cars its DLL took (LocoFlags 0x10 without 1), in
// its train (Outputs::section_master).
void LegacyConsist::sections_after() {
    const auto& cars = outputs_->cars;
    auto& master = outputs_->section_master;
    master.assign(cars.size(), -1);
    if (outputs_->section_brake.size() != cars.size()) outputs_->section_brake.assign(cars.size(), {});
    const auto taken = [&](const std::size_t k) { return cars[k].has_engine && (cars[k].loco_flags & 0x11u) == 0x10u; };
    for (std::size_t t = 0; t + 1 < train_first_.size(); ++t) {
        const std::size_t first = train_first_[t], end = (std::min)(train_first_[t + 1], cars.size());
        for (std::size_t i = first; i < end; ++i) {
            if (!cars[i].has_engine || !(cars[i].loco_flags & 1u)) continue;
            for (std::size_t k = i + 1; k < end && taken(k); ++k) master[k] = static_cast<std::int32_t>(i);
            for (std::size_t k = i; k-- > first && taken(k);) master[k] = static_cast<std::int32_t>(i);
        }
    }
}

void LegacyConsist::klub_after(const StepRequest& request, const LegacyWorld& world) {
    outputs_->klub_vent.assign(outputs_->cars.size(), 0);
    if (!klub_working_) return;
    const std::size_t first = train_first_.size() > 1 ? train_first_[0] : 0;
    const std::size_t end = train_first_.size() > 1 ? train_first_[1] : 0;
    // The leading car (dllhost Host::signals: the train's front as it runs).
    const std::size_t leading = request.train_direction ? first : (end > first ? end - 1 : first);
    std::vector<simulation::KlubSignal> signals;
    for (const auto& g : request.alsn_ahead) {
        simulation::KlubSignal k;
        k.distance = g.distance;
        std::memcpy(k.aspect, g.aspect, sizeof k.aspect);
        k.flags = g.flags;
        k.speed_limit = g.speed_limit;
        signals.push_back(k);
    }
    float limit = -1.0f;
    for (const float v : {request.route.limit, request.signal.limit, request.event.limit})
        if (v >= 0.0f && (limit < 0.0f || v < limit)) limit = v;
    // The screen's track items (RTSGetTrackItems: under the train, then ahead
    // and behind outward) and the target signal's name.
    std::vector<simulation::KlubTrackItem> items;
    if (world.tdb) {
        for (const auto& e : world.track_items) {
            if (e.item >= world.tdb->items.size()) continue;
            const auto spec = item_spec(world.tdb->items[e.item], world.route_speed_limit, world.limits_without_tolerance);
            if (spec.type < 2 || spec.type > 5) continue;
            simulation::KlubTrackItem item;
            item.type = static_cast<std::uint8_t>(spec.type);
            item.distance = e.distance;
            item.pos = e.pos;
            item.name = spec.type == 3 ? spec.station : spec.name;
            item.flags = spec.flags;
            item.value = spec.value;
            items.push_back(std::move(item));
        }
        std::stable_sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
            const int ra = (a.pos & 4) ? 0 : 1, rb = (b.pos & 4) ? 0 : 1;
            return ra != rb ? ra < rb : a.distance < b.distance;
        });
    }
    std::u16string signal_name;
    if (!request.alsn_ahead.empty() && world.route_database) {
        const auto unit = request.alsn_ahead.front().unit;
        if (unit >= 0 && static_cast<std::size_t>(unit) < world.route_database->signal_units.size())
            signal_name = u16(world.route_database->signal_units[static_cast<std::size_t>(unit)].name);
    }
    for (std::size_t i = first; i < end && i < cars_.size() && i < outputs_->cars.size() && i < request.cars.size(); ++i) {
        const auto found = klub_.find(cars_[i].key);
        if (found == klub_.end() || !found->second || !found->second->on) continue;
        auto& car = *found->second;
        auto& out = outputs_->cars[i];
        const auto& input = request.cars[i];
        const auto& p = car.parameters;
        const bool first_step = car.switches.empty();
        if (first_step) car.switches = out.switches; // the first step: as they stand
        const auto state_in = [](const std::vector<SwitchState>& list, const std::int32_t id) {
            return id >= 0 && static_cast<std::size_t>(id) < list.size() ? list[static_cast<std::size_t>(id)].state : 0u;
        };
        const auto on = [&](const std::int32_t id) { return state_in(car.switches, id) != 0; };
        const auto moved = [&](const std::int32_t id) { return id >= 0 && state_in(car.switches, id) != state_in(out.switches, id); };
        const bool flipped = (input.flags & 1u) != 0;
        simulation::KlubInputs in;
        in.dt = request.dt;
        in.speed = flipped ? -input.velocity : input.velocity;
        in.signals = signals;
        if (i == leading && !request.signals_ahead.empty()) {
            const auto& g = request.signals_ahead.front();
            in.cab_signal.distance = g.distance;
            std::memcpy(in.cab_signal.aspect, g.aspect, sizeof in.cab_signal.aspect);
            in.cab_signal.flags = g.flags;
            in.cab_signal.speed_limit = g.speed_limit;
        } else {
            in.cab_signal = {};
            in.cab_signal.aspect[0] = i == leading ? 0u : 8u;
            in.cab_signal.speed_limit = -1.0f;
        }
        in.cars_ahead = i != leading;
        in.limit = limit;
        in.next_limit = request.next_limit;
        in.next_limit_distance = request.next_limit_distance;
        // Powered unless one of its switches is there and at 0 (Brakes.dll
        // 0x4015A4: a switch the cab has not counts as on).
        in.powered = true;
        for (const auto id : p.power) {
            const bool there = id >= 0 && static_cast<std::size_t>(id) < car.switches.size();
            in.powered = in.powered && (!there || on(id));
        }
        // Brakes.dll's variables after this step (klub_before asks for them):
        // what its other parts keep (САУТ, ТСКБМ).
        const int brakes = aux_library(i, "BRAKES");
        const auto var_at = [&](const std::size_t offset, const float otherwise) {
            if (brakes < 0 || static_cast<std::size_t>(brakes) >= out.aux_vars.size()) return otherwise;
            const auto& var = out.aux_vars[static_cast<std::size_t>(brakes)].var;
            return offset / 4 < var.size() ? var[offset / 4] : otherwise;
        };
        const auto var_bits = [&](const std::size_t offset) {
            std::uint32_t u = 0;
            const float v = var_at(offset, 0.0f);
            std::memcpy(&u, &v, sizeof u);
            return u;
        };
        // The cabs: the working one's devices; the РБ, the РБС and the ЭПК
        // key of any (Brakes.dll's Switched goes through them all), the
        // ЭПК key last turned setting Engine::ALSNOn.
        const int working = p.working_cab(car.switches);
        const auto& wc = p.cabs[static_cast<std::size_t>(working < 0 ? 0 : working)];
        if (first_step) {
            car.alsn_on = false;
            for (const auto& c : p.cabs) car.alsn_on = car.alsn_on || on(c.epk_key);
        }
        in.epk_key = car.alsn_on;
        in.rb_held = false;
        for (const auto& c : p.cabs) {
            in.rb_held = in.rb_held || on(c.rb);
            in.rb = in.rb || moved(c.rb);
            in.rbs = in.rbs || moved(c.rbs);
            if (moved(c.epk_key)) {
                in.epk_key_turned = true;
                in.epk_key_to = state_in(out.switches, c.epk_key) != 0;
            }
        }
        if (in.epk_key_turned) car.alsn_on = in.epk_key_to;
        // ТСКБМ's switch of any cab.
        for (const auto& c : p.cabs) in.tskbm = in.tskbm || on(c.tskbm);
        in.alsn_on = out.engine.alsn_on;
        // Any switch of the car moved (Switched): ТСКБМ's timer again.
        for (std::size_t k = 0; k < out.switches.size() && !in.switched; ++k)
            in.switched = k < car.switches.size() && car.switches[k].state != out.switches[k].state;
        // САУТ goes on in Brakes.dll: its own "on" and its permitted
        // speed.
        in.saut = (var_bits(4) & 0x40u) != 0;
        in.saut_permitted = var_at(256, 0.0f);
        // САУТ's "announced": the РБ lets it go in Brakes.dll's
        // vigilance, still loaded; openRTS's
        // САУТ puts it there before Run and only sets it.
        if ((p.settings.setup & 1) && !(var_bits(88) & 0x20000u)) car.state.saut_flags &= ~0x20000u;
        // openRTS's САУТ (PB_SETUP 1) in the КЛУБ's step: what it reads.
        std::vector<simulation::SautInputs::Cab> saut_cabs;
        if (p.settings.setup & 1) {
            auto& u = in.saut_in;
            u.own = true;
            u.powered = true;
            for (const auto id : p.saut_power) u.powered = u.powered && (id < 0 || static_cast<std::size_t>(id) >= car.switches.size() || on(id));
            u.high = p.saut_mode || (p.saut_mode_switch >= 0 && state_in(car.switches, p.saut_mode_switch) == p.saut_mode_on);
            u.clock = (p.settings.setup & 16) != 0;
            u.acceleration = input.acceleration;
            u.cylinder = input.brake_cylinder;
            u.ept = (out.engine.systems & 2u) != 0;
            u.brake_bits = var_bits(4) & 0x180u;
            u.reverser = out.engine.reverse;
            u.traction = out.engine.throttle != 0;
            u.equalising = out.engine.equalising;
            u.milepost = out.engine.milepost; // Engine::CurrentMilepost (simulation/milepost.hpp)
            u.signal_name = signal_name; // Cabin::Signal's (+136) RTSSignalData name
            u.items = items;
            u.flipped = (input.flags & 1u) != 0;
            u.keys = car.saut_keys;
            for (std::size_t c = 0; c < p.cabs.size(); ++c)
                saut_cabs.push_back({.working = static_cast<int>(c) == working,
                                     .on = on(p.cabs[c].saut),
                                     .os = on(p.cabs[c].saut_os),
                                     .cassette = on(p.cabs[c].cassette),
                                     .turned_on = moved(p.cabs[c].saut) && state_in(out.switches, p.cabs[c].saut) != 0,
                                     .rb_moved = moved(p.cabs[c].rb)});
            u.cabs = saut_cabs;
            u.flip = p.flip;
        }
        in.wheelslip = input.wheelslip != 0;
        in.cylinder = (std::max)(input.brake_cylinder, input.independent);
        in.day_time = request.day_time;
        in.cassette = on(wc.cassette);
        in.keys = car.keys;
        // openRTS's map in the leading cab: the entered track's target.
        std::u16string target_name = signal_name;
        if (world.electronic_map && i == leading) {
            if (car.state.track_entries != car.ek_entries) {
                car.ek = {};
                car.ek_entries = car.state.track_entries;
            }
            const auto target = world.electronic_map(car.state.track & 0x7Fu, car.ek);
            in.ek = {.given = true, .matched = target.matched, .has_target = target.has_target, .distance = static_cast<float>(target.distance)};
            if (target.matched && target.has_target && world.route_database && target.unit >= 0
                && static_cast<std::size_t>(target.unit) < world.route_database->signal_units.size())
                target_name = u16(world.route_database->signal_units[static_cast<std::size_t>(target.unit)].name);
            else if (target.matched && target.has_target)
                target_name.clear();
            // The log: the map's answer when it changes (a track entered).
            const std::uint32_t track = car.state.track & 0x7Fu;
            const int mode = target.matched ? 1 : 0;
            const std::uint32_t object = target.has_target ? target.object : ~0u;
            if (track && (track != car.ek_logged_track || mode != car.ek_logged_mode || (mode && object != car.ek_logged_object))) {
                const std::string name = utf16_to_utf8(target_name);
                std::cout << "КЛУБ ЭК, track " << track << ": "
                          << (!target.matched     ? std::string("без ЭК")
                              : target.has_target ? "target " + (name.empty() ? std::string("signal ") + std::to_string(object) : name) + " in "
                                                        + std::to_string(static_cast<int>(target.distance)) + " m"
                                                  : std::string("no signal ahead"))
                          << std::endl;
            }
            car.ek_logged_track = track;
            car.ek_logged_mode = mode;
            car.ek_logged_object = object;
        }
        // Switched on with the ЭПК key in (its Init's Switched, or the
        // setting turned on): the КЛУБ's beep.
        if (!car.state.started && in.epk_key) in.epk_key_turned = in.epk_key_to = true;
        simulation::step_klub(car.state, p.settings, in, car.out);
        car.keys.clear();
        car.saut_keys.clear();
        // The screens and the lamps: each cab's (updateKLUB draws them all).
        car.screens.resize(p.cabs.size());
        simulation::KlubScreenInputs si;
        si.powered = in.powered;
        si.epk_key = in.epk_key;
        si.speed = in.speed;
        // The ТМ gauge as Brakes.dll's last step left it, else the pipe.
        si.pipe = car.last_gauge > 0.0f ? car.last_gauge : (car.last_pipe > 0.0f ? car.last_pipe : input.train_pipe);
        si.cylinder = in.cylinder;
        si.equalising = car.last_ur;
        si.acceleration = input.acceleration;
        si.line_frequency = input.line_frequency;
        si.wheelslip = in.wheelslip;
        si.day_time = request.day_time;
        si.signal_name = target_name;
        si.items = items;
        si.station_names = station_names_;
        si.objects_line = world.klub_objects;
        for (std::size_t c = 0; c < p.cabs.size(); ++c) {
            si.working_cab = static_cast<int>(c) == working;
            si.direction = p.flip == static_cast<std::int32_t>(c) ? -1 : 1; // PB_FLIP: the cab (from 0) the other way
            si.cassette = on(p.cabs[c].cassette);
            si.cassette_given = p.cabs[c].cassette >= 0;
            // САУТ's braking coefficient, its SAUT_SWID<n> given and on
            // (Brakes.dll's САУТ goes on running: Engine::Flags / 100).
            si.saut_coefficient = p.cabs[c].saut >= 0 && on(p.cabs[c].saut)
                                      ? static_cast<float>((p.settings.setup & 1) ? car.state.saut_coefficient : out.engine.flags) * 0.01f
                                      : -1.0f;
            si.tskbm_timer = car.state.tskbm_shown; // openRTS's ТСКБМ
            si.tskbm_switch = on(p.cabs[c].tskbm);
            si.block_value = var_at(296, 0.0f);
            si.saut_distance = (p.settings.setup & 1) ? car.state.saut_distance : var_at(308, 0.0f);
            si.milepost = out.engine.milepost;
            si.saut_caught = (((p.settings.setup & 1) ? car.state.saut_flags : var_bits(88)) & 0x100u) != 0;
            simulation::draw_klub(car.state, p.settings, si, car.screens[c]);
        }
        car.last_ur = out.engine.equalising;
        car.last_pipe = input.train_pipe;
        car.last_gauge = var_at(28, 0.0f);
        car.switches = out.switches;
        // Its drawing over the outputs (the screen keeps what it drew).
        const auto* cab = i < cabs_.size() ? &cabs_[i] : nullptr;
        const auto display_of = [&](const std::int32_t id) -> const formats::CabDisplay* {
            if (!cab || id < 0) return nullptr;
            for (const auto& dd : cab->displays)
                if (static_cast<std::int32_t>(dd.id) == id) return &dd;
            return nullptr;
        };
        const auto apply = [&](const std::int32_t id, std::span<const simulation::KlubScreenObject> objects) {
            if (id < 0) return;
            const auto* display = display_of(id);
            for (auto& screen : out.screens) {
                if (static_cast<std::int32_t>(screen.display) != id) continue;
                for (std::size_t k = 0; k < objects.size() && k < screen.objects.size(); ++k) {
                    const auto& o = objects[k];
                    auto& g = screen.objects[k];
                    if (o.state_set) g.state = o.state;
                    if (o.value_set) g.value = o.value;
                    if (o.blink_set) {
                        g.fade[1] = 0.0f;
                        g.fade[2] = o.blink;
                    }
                    // A label RTS gives no text buffer (no character width or
                    // length in the cab, screen_object_spec) takes no text.
                    const auto* def = display && k < display->screen.objects.size() ? &display->screen.objects[k] : nullptr;
                    if (o.text_set && k < screen.texts.size() && (!def || (def->char_width[1] > 0.0f && def->max_length > 0))) {
                        auto text = o.text;
                        if (def && text.size() > def->max_length) text.resize(def->max_length);
                        screen.texts[k] = std::move(text);
                    }
                }
            }
        };
        // САУТ's displays (SAUT_DBID, SAUT_DFID) as its step drew them.
        const auto apply_saut = [&](const std::int32_t id, std::span<const simulation::SautObject> objects) {
            std::vector<simulation::KlubScreenObject> as_klub(objects.size());
            for (std::size_t k = 0; k < objects.size(); ++k) {
                as_klub[k].state = objects[k].state;
                as_klub[k].state_set = objects[k].state_set;
                as_klub[k].blink = objects[k].blink;
                as_klub[k].blink_set = objects[k].blink_set;
                as_klub[k].text = objects[k].text;
                as_klub[k].text_set = objects[k].text_set;
            }
            apply(id, as_klub);
        };
        // What this step set over what the cab keeps, then all of it.
        const auto keep = [](auto& kept, const auto& now) {
            for (std::size_t k = 0; k < kept.size() && k < now.size(); ++k) {
                const auto& o = now[k];
                auto& g = kept[k];
                if (o.state_set) {
                    g.state = o.state;
                    g.state_set = true;
                }
                if (o.blink_set) {
                    g.blink = o.blink;
                    g.blink_set = true;
                }
                if (o.text_set) {
                    g.text = o.text;
                    g.text_set = true;
                }
            }
        };
        if (car.saut_screens.size() != car.out.saut_screens.size()) car.saut_screens.assign(car.out.saut_screens.size(), {});
        for (std::size_t c = 0; c < car.out.saut_screens.size(); ++c) {
            keep(car.saut_screens[c].db, car.out.saut_screens[c].db);
            keep(car.saut_screens[c].df, car.out.saut_screens[c].df);
        }
        for (std::size_t c = 0; c < p.cabs.size() && c < car.saut_screens.size(); ++c) {
            apply_saut(p.cabs[c].saut_db, car.saut_screens[c].db);
            apply_saut(p.cabs[c].saut_df, car.saut_screens[c].df);
        }
        for (std::size_t c = 0; c < p.cabs.size(); ++c) {
            const auto& ids = p.cabs[c];
            const auto& drawn = car.screens[c];
            apply(ids.screen, drawn.bil);
            apply(ids.dals, drawn.dals);
            apply(ids.dpom, drawn.dpom);
            for (std::size_t k = 0; k < ids.lamps.size(); ++k) {
                const auto id = ids.lamps[k];
                if (id >= 0 && static_cast<std::size_t>(id) < out.displays.size())
                    out.displays[static_cast<std::size_t>(id)].state = static_cast<std::uint32_t>(drawn.lamps[k]);
            }
            // ТСКБМ's display and lamps, L2's blink (its Fade).
            if (p.settings.setup & 4) {
                apply(ids.tskbm_screen, drawn.tskbm);
                const auto lamp = [&](const std::int32_t id, const std::int8_t lit) {
                    if (id >= 0 && static_cast<std::size_t>(id) < out.displays.size())
                        out.displays[static_cast<std::size_t>(id)].state = static_cast<std::uint32_t>(lit);
                };
                lamp(ids.tskbm_power, drawn.tskbm_power);
                for (std::size_t k = 0; k < 3; ++k) lamp(ids.tskbm_lamps[k], drawn.tskbm_lamps[k]);
                if (const auto l2 = ids.tskbm_lamps[1]; l2 >= 0 && static_cast<std::size_t>(l2) < out.displays.size()) {
                    out.displays[static_cast<std::size_t>(l2)].fade[1] = 0.0f;
                    out.displays[static_cast<std::size_t>(l2)].fade[2] = drawn.tskbm_l2_blink;
                }
            }
        }
        const auto& shown = car.screens.front();
        if (p.lamp >= 0 && static_cast<std::size_t>(p.lamp) < out.displays.size())
            out.displays[static_cast<std::size_t>(p.lamp)].state = static_cast<std::uint32_t>(shown.lamp);
        if (p.backlight >= 0 && static_cast<std::size_t>(p.backlight) < out.tex_changes.size())
            out.tex_changes[static_cast<std::size_t>(p.backlight)] = static_cast<std::uint16_t>(shown.backlight);
        out.cab_aspect = car.state.signal.aspect[0];
        // Its sounds on Brakes.sms in place of Brakes.dll's КЛУБ's.
        const int library = aux_library(i, "BRAKES");
        if (library >= 0 && static_cast<std::size_t>(library) < out.aux_sound.size()) {
            auto& triggers = out.aux_sound[static_cast<std::size_t>(library)].triggers;
            triggers.erase(std::remove_if(triggers.begin(), triggers.end(), klub_sound), triggers.end());
            triggers.insert(triggers.end(), car.out.sounds.begin(), car.out.sounds.end());
        }
        // On the locomotive's SMS too with TRANS_SOUND 1, or the train's DLL
        // asking for it (var + 32 bit 0x100), as Brakes.dll does (0x404B10).
        const bool to_loco = p.trans_sound || (library >= 0 && static_cast<std::size_t>(library) < out.aux_exchange.size() &&
                                               (out.aux_exchange[static_cast<std::size_t>(library)].before & 0x100u));
        if (to_loco) {
            out.sound_ext.insert(out.sound_ext.end(), car.out.sounds.begin(), car.out.sounds.end());
            out.sound_cab.insert(out.sound_cab.end(), car.out.sounds.begin(), car.out.sounds.end());
        }
        outputs_->klub_vent[i] = car.out.vent ? 1 : 0;
    }
}

// --- openRTS's speed recorder and ALSN vigilance (docs/brakes-dll-replacement.md "The rest", 4) ---

namespace {

// Brakes.dll's recorder sounds (the whistle, a better aspect, the clock's key).
bool recorder_sound(const std::uint16_t c) { return c == 56 || c == 57 || c == 106 || c == 970 || c == 171; }

} // namespace

LegacyConsist::RecorderCar* LegacyConsist::recorder_car(const std::size_t i) {
    if (i >= cars_.size()) return nullptr;
    const auto key = cars_[i].key;
    const auto* definition = cars_[i].definition;
    if (!key || !definition || !definition->engine) return nullptr;
    auto found = recorder_.find(key);
    if (found == recorder_.end()) {
        std::unique_ptr<RecorderCar> made;
        auto parameters = recorder_parameters(*definition->engine);
        if (parameters.own && parameters.speed1_index >= 0) {
            made = std::make_unique<RecorderCar>();
            made->parameters = std::move(parameters);
        }
        found = recorder_.emplace(key, std::move(made)).first;
    }
    return found->second.get();
}

void LegacyConsist::recorder_before(StepRequest& request) {
    const bool want = own_klub_ && air_->mode != DllAir::rts;
    const std::size_t first = train_first_.size() > 1 ? train_first_[0] : 0;
    const std::size_t end = train_first_.size() > 1 ? train_first_[1] : 0;
    for (std::size_t i = 0; i < cars_.size(); ++i) {
        auto* car = recorder_car(i);
        if (!car) continue;
        const int library = aux_library(i, "BRAKES");
        const bool on = want && library >= 0 && i >= first && i < end;
        const auto index = static_cast<std::uint32_t>(i);
        const auto lib = static_cast<std::uint16_t>(library);
        const auto speed1 = static_cast<std::uint16_t>(car->parameters.speed1_index);
        if (!on) {
            if (car->on && library >= 0) request.aux_parameters.push_back({index, lib, speed1, car->parameters.speed1}); // Brakes.dll's back
            car->on = false;
            car->started = false;
            continue;
        }
        car->on = true;
        // Brakes.dll without its recorder (SL_SPEED1 −1: its part of Run
        // skipped); sent every step.
        request.aux_parameters.push_back({index, lib, speed1, -1.0f});
        request.want_aux_vars = 1;
        if (!car->started) continue;
        const auto& s = car->state;
        const auto put = [&](const std::uint16_t offset, const std::uint32_t mask, const std::uint32_t bits, const bool after_run = false) {
            request.aux_writes.push_back({index, lib, offset, mask, bits, after_run ? 1u : 0u});
        };
        const auto put_float = [&](const std::uint16_t offset, const float v) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &v, sizeof bits);
            put(offset, ~0u, bits);
        };
        // What its Run would have left there, for Brakes.dll's Switched and
        // other parts and the train's DLLs: the timers, the aspect seen, the
        // whistle, its flags; the ЭПК's bit after the Run (it clears it first).
        put_float(16, s.kon_timer);
        put_float(20, s.vented);
        put_float(36, s.needle_timer);
        put_float(44, s.periodic);
        put_float(48, s.aspect_seen);
        put_float(52, s.whistle);
        put_float(56, s.blink);
        put(4, simulation::recorder_flag::mask, s.flags);
        put(8, 0x10000000u, s.whistle_flag ? 0x10000000u : 0u);
        put(32, 1u, s.epk_bit ? 1u : 0u, true);
        // The ВК on К: the cab's aspect 2 for the rest of that step.
        if (car->out.cab_aspect >= 0)
            request.aux_writes.push_back({index, lib, 0, 0, static_cast<std::uint32_t>(car->out.cab_aspect), 1u, 1u});
        if (i < request.cars.size()) request.cars[i].alsn_on_word = s.alsn_on ? 1 : 0;
    }
}

void LegacyConsist::recorder_after(const StepRequest& request, const LegacyWorld& world) {
    (void)world;
    if (outputs_->klub_vent.size() != outputs_->cars.size()) outputs_->klub_vent.assign(outputs_->cars.size(), 0);
    for (std::size_t i = 0; i < cars_.size() && i < outputs_->cars.size() && i < request.cars.size(); ++i) {
        auto* found = recorder_car(i);
        if (!found || !found->on) continue;
        auto& car = *found;
        auto& out = outputs_->cars[i];
        const auto& input = request.cars[i];
        const auto& p = car.parameters;
        const int brakes = aux_library(i, "BRAKES");
        const auto var_at = [&](const std::size_t offset, const float otherwise) {
            if (brakes < 0 || static_cast<std::size_t>(brakes) >= out.aux_vars.size()) return otherwise;
            const auto& var = out.aux_vars[static_cast<std::size_t>(brakes)].var;
            return offset / 4 < var.size() ? var[offset / 4] : otherwise;
        };
        const auto var_bits = [&](const std::size_t offset) {
            std::uint32_t u = 0;
            const float v = var_at(offset, 0.0f);
            std::memcpy(&u, &v, sizeof u);
            return u;
        };
        const bool flipped = (input.flags & 1u) != 0;
        const float speed = flipped ? -input.velocity : input.velocity;
        if (!car.started) {
            // Brakes.dll's Init's: its random odometer (it ran on this step).
            car.started = true;
            car.state = {};
            car.state.distance = static_cast<float>(var_at(224, 0.0f) - std::abs(static_cast<double>(speed)) * request.dt);
            car.last_var4 = var_bits(4);
            car.last_ep = var_at(40, 0.0f);
            car.switches = out.switches;
        }
        const auto state_in = [](const std::vector<SwitchState>& list, const std::int32_t id) {
            return id >= 0 && static_cast<std::size_t>(id) < list.size() ? list[static_cast<std::size_t>(id)].state : 0u;
        };
        const auto on = [&](const std::int32_t id) { return state_in(car.switches, id) != 0; };
        const auto moved = [&](const std::int32_t id) { return id >= 0 && state_in(car.switches, id) != state_in(out.switches, id); };
        std::vector<simulation::RecorderCabInputs> cabs;
        for (const auto& c : p.settings.cabs) {
            simulation::RecorderCabInputs ci;
            ci.epk_key = on(c.epk_key);
            ci.als = on(c.als);
            ci.rb = on(c.rb);
            ci.rbs = on(c.rbs);
            ci.h = on(c.h);
            ci.dvij = on(c.dvij);
            ci.kid = on(c.kid);
            ci.rb_pressed = moved(c.rb) && state_in(out.switches, c.rb) != 0;
            ci.rbs_pressed = moved(c.rbs) && state_in(out.switches, c.rbs) != 0;
            ci.vk_pressed = moved(c.vk) && state_in(out.switches, c.vk) != 0;
            ci.zavod_moved = moved(c.zavod);
            ci.epk_key_moved = moved(c.epk_key);
            ci.epk_key_to = state_in(out.switches, c.epk_key) != 0;
            cabs.push_back(ci);
        }
        // The needle's error: Brakes.dll's (rand / 4096 − 6) × 0.2778 each step.
        car.random = car.random * 22695477u + 1u;
        const auto r = static_cast<int>((car.random >> 16) & 0x7FFFu);
        simulation::RecorderInputs ri;
        ri.dt = request.dt;
        ri.speed = speed;
        ri.cylinder = input.brake_cylinder;
        ri.independent = input.independent;
        ri.main_reservoir = input.main_reservoir;
        ri.reverser = out.engine.reverse;
        ri.traction = out.engine.throttle != 0;
        ri.aspect = var_bits(72); // the cab's after Brakes.dll's ALSN delay
        ri.powered = (var_bits(32) & 4u) != 0;
        ri.tskbm_on = (car.last_var4 & 8u) != 0;
        ri.bypass = (car.last_var4 & 0x200u) != 0;
        ri.ep_pressure = car.last_ep;
        ri.day_time = request.day_time;
        ri.random = static_cast<float>(static_cast<double>(r / 4096 - 6) * 0.2777777777777777709);
        ri.cabs = cabs;
        simulation::step_recorder(car.state, p.settings, ri, car.out);
        simulation::recorder_switched(car.state, p.settings, ri, car.out);
        // Brakes.dll's odometer goes on in its Run all the same: the same.
        car.last_var4 = var_bits(4);
        car.last_ep = var_at(40, 0.0f);
        car.switches = out.switches;
        // Its cab objects.
        for (const auto& [id, v] : car.out.values)
            if (id >= 0 && static_cast<std::size_t>(id) < out.displays.size()) out.displays[static_cast<std::size_t>(id)].value = v;
        for (const auto& [id, v] : car.out.states)
            if (id >= 0 && static_cast<std::size_t>(id) < out.displays.size()) out.displays[static_cast<std::size_t>(id)].state = v;
        if (car.out.cab_aspect >= 0) out.cab_aspect = static_cast<std::uint32_t>(car.out.cab_aspect);
        // Its sounds on Brakes.sms in place of Brakes.dll's (its Switched's
        // too: they go on Brakes.dll's state as openRTS's put it).
        const auto drop = [](std::vector<std::uint16_t>& triggers) {
            triggers.erase(std::remove_if(triggers.begin(), triggers.end(), recorder_sound), triggers.end());
        };
        if (brakes >= 0 && static_cast<std::size_t>(brakes) < out.aux_sound.size()) {
            auto& triggers = out.aux_sound[static_cast<std::size_t>(brakes)].triggers;
            drop(triggers);
            triggers.insert(triggers.end(), car.out.sounds.begin(), car.out.sounds.end());
        }
        const bool to_loco = p.trans_sound || (brakes >= 0 && static_cast<std::size_t>(brakes) < out.aux_exchange.size() &&
                                               (out.aux_exchange[static_cast<std::size_t>(brakes)].before & 0x100u));
        if (to_loco) {
            drop(out.sound_ext);
            drop(out.sound_cab);
            out.sound_ext.insert(out.sound_ext.end(), car.out.sounds.begin(), car.out.sounds.end());
            out.sound_cab.insert(out.sound_cab.end(), car.out.sounds.begin(), car.out.sounds.end());
        }
        if (car.out.vent) outputs_->klub_vent[i] = 1;
    }
}

// --- Brakes.dll not loaded (docs/brakes-dll-replacement.md "The rest", 7) ---

void LegacyConsist::set_without_brakes_dll(const bool on) noexcept { without_brakes_dll_choice = on; }

void LegacyConsist::refresh_stubbed() {
    stubbed_.assign(cars_.size(), 0);
    stubbed_count_ = 0;
    if (!without_brakes_dll_) return;
    for (std::size_t i = 0; i < cars_.size(); ++i) {
        const auto* d = cars_[i].definition;
        if (!d || !d->engine || aux_library(i, "BRAKES") < 0 || !brakes_dll_all_ours(*d->engine)) continue;
        stubbed_[i] = 1;
        ++stubbed_count_;
    }
    if (stubbed_count_ == 0) return;
    // Brakes.dll is not there to go back to: openRTS's КЛУБ and brakes on
    // the realistic air for good.
    own_klub_ = true;
    air_->own_brakes = true;
    if (air_->mode == DllAir::rts) air_->mode = DllAir::own_valve;
}

// What Brakes.dll's Run kept for the train's DLL there, before its Run:
// the "it runs" bit, the distance, the ЭПТ's pressure, the 418's bit, its
// braking positions' bits (САУТ's coefficient); Engine::ALSNOn by the ЭПК key.
void LegacyConsist::stub_before(StepRequest& request, const std::vector<const simulation::Train*>& trains) {
    if (!outputs_) return;
    // The 418 (the train's DLLs cut traction) as openRTS's cab brakes
    // have it, after Brakes.dll's step, on every car they work.
    for (std::size_t i = 0; i < cars_.size() && i < request.cars.size(); ++i) {
        const simulation::EngineDemands* asked = nullptr;
        for (std::size_t t = 0; t + 1 < train_first_.size() && t < trains.size(); ++t)
            if (trains[t] && i >= train_first_[t] && i < train_first_[t + 1]) asked = trains[t]->demands_of(i - train_first_[t]);
        const int library = aux_library(i, "BRAKES");
        if (!asked || !asked->cab_brakes || library < 0) continue;
        request.aux_writes.push_back({static_cast<std::uint32_t>(i), static_cast<std::uint16_t>(library), 32, 2u,
                                      asked->cab_brakes->lamp_pipe ? 2u : 0u, 1u});
    }
    if (stubbed_count_ == 0) return;
    for (std::size_t i = 0; i < cars_.size() && i < stubbed_.size() && i < request.cars.size(); ++i) {
        if (!stubbed_[i]) continue;
        const int library = aux_library(i, "BRAKES");
        if (library < 0) continue;
        const auto index = static_cast<std::uint32_t>(i);
        const auto lib = static_cast<std::uint16_t>(library);
        auto& car = stub_cars_[cars_[i].key];
        const auto put = [&](const std::uint16_t offset, const std::uint32_t mask, const std::uint32_t bits) {
            request.aux_writes.push_back({index, lib, offset, mask, bits, 0u});
        };
        const auto put_float = [&](const std::uint16_t offset, const float v) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &v, sizeof bits);
            put(offset, ~0u, bits);
        };
        put(4, 0x01000000u, 0x01000000u);
        const auto& input = request.cars[i];
        car.distance = static_cast<float>(std::abs(static_cast<double>(input.velocity)) * request.dt + car.distance);
        put_float(224, car.distance);
        // openRTS's cab brakes' last step (the train's demands of the car).
        const simulation::EngineDemands* asked = nullptr;
        for (std::size_t t = 0; t + 1 < train_first_.size() && t < trains.size(); ++t)
            if (trains[t] && i >= train_first_[t] && i < train_first_[t + 1]) asked = trains[t]->demands_of(i - train_first_[t]);
        if (asked && asked->cab_brakes) {
            const auto& r = *asked->cab_brakes;
            put_float(40, r.ep_value >= 0.0f ? r.ep_value : 0.0f);
            if (r.braking) car.braking = 0x180u;
            else if (car.braking & 0x80u) car.braking &= ~0x80u;
            else car.braking &= ~0x100u;
            put(4, 0x180u, car.braking);
        }
        if (const auto found = klub_.find(cars_[i].key); found != klub_.end() && found->second)
            request.cars[i].alsn_on_word = found->second->alsn_on ? 1 : 0;
    }
}

// Its Switched's sounds: the 395's and 254's handles, the РБ, the РБС, the
// ЭПК key, on Brakes.sms and with TRANS_SOUND the
// locomotive's.
void LegacyConsist::stub_after() {
    if (stubbed_count_ == 0 || !outputs_) return;
    for (std::size_t i = 0; i < cars_.size() && i < stubbed_.size() && i < outputs_->cars.size(); ++i) {
        if (!stubbed_[i]) continue;
        auto& out = outputs_->cars[i];
        auto& car = stub_cars_[cars_[i].key];
        const auto* d = cars_[i].definition;
        if (!d || !d->engine) continue;
        if (car.switches.empty()) car.switches = out.switches;
        const auto bp = brakes_parameters(*d->engine);
        const auto kp = klub_parameters(*d->engine);
        const auto state_of = [](const std::vector<SwitchState>& list, const std::int32_t id) -> std::int64_t {
            return id >= 0 && static_cast<std::size_t>(id) < list.size() ? static_cast<std::int64_t>(list[static_cast<std::size_t>(id)].state) : -1;
        };
        std::vector<std::uint16_t> sounds;
        for (std::size_t k = 0; k < out.switches.size(); ++k) {
            const auto id = static_cast<std::int32_t>(k);
            const auto now = state_of(out.switches, id);
            if (now == state_of(car.switches, id)) continue;
            for (std::size_t c = 0; c < bp.cabs.size(); ++c) {
                const auto& cab = bp.cabs[c];
                if (cab.valve == id) sounds.push_back((now && now != 6) ? (bp.sounds_130 ? 3917 : 917) : (bp.sounds_130 ? 3918 : 918));
                else if (cab.independent == id || cab.independent2 == id) sounds.push_back(bp.sounds_224 ? 3919 : 919);
                const auto* kc = c < kp.cabs.size() ? &kp.cabs[c] : nullptr;
                if (kc && (kc->rb == id || kc->rbs == id)) sounds.push_back(now ? 901 : 902);
                else if (kc && kc->epk_key == id) sounds.push_back(bp.epk_sounds ? 3903 : 903);
            }
        }
        car.switches = out.switches;
        if (sounds.empty()) continue;
        const int brakes = aux_library(i, "BRAKES");
        if (brakes >= 0 && static_cast<std::size_t>(brakes) < out.aux_sound.size()) {
            auto& triggers = out.aux_sound[static_cast<std::size_t>(brakes)].triggers;
            triggers.insert(triggers.end(), sounds.begin(), sounds.end());
        }
        if (bp.trans_sound) {
            out.sound_ext.insert(out.sound_ext.end(), sounds.begin(), sounds.end());
            out.sound_cab.insert(out.sound_cab.end(), sounds.begin(), sounds.end());
        }
    }
}

void LegacyConsist::klub_save(const std::uint32_t key, std::vector<std::uint8_t>& bytes) const {
    const auto found = klub_.find(key);
    if (found == klub_.end() || !found->second || !found->second->on) return;
    const auto& state = found->second->state;
    const auto at = bytes.size();
    bytes.resize(at + sizeof state + 8);
    std::memcpy(bytes.data() + at, &state, sizeof state);
    const auto size = static_cast<std::uint32_t>(sizeof state);
    std::memcpy(bytes.data() + at + sizeof state, &size, 4);
    std::memcpy(bytes.data() + at + sizeof state + 4, &klub_magic, 4);
}

void LegacyConsist::klub_restore(const std::size_t i, std::vector<std::uint8_t>& bytes) {
    std::uint32_t magic = 0, size = 0;
    if (bytes.size() < 8) return;
    std::memcpy(&magic, bytes.data() + bytes.size() - 4, 4);
    std::memcpy(&size, bytes.data() + bytes.size() - 8, 4);
    if (magic != klub_magic || size + 8 > bytes.size()) return;
    const auto at = bytes.size() - 8 - size;
    if (size == sizeof(simulation::KlubState)) {
        if (auto* car = klub_car(i)) {
            std::memcpy(&car->state, bytes.data() + at, size);
            car->screens.clear();
            car->switches.clear();
            car->resume = true;
        }
    }
    bytes.resize(at);
}

} // namespace openrts::trainplugin
