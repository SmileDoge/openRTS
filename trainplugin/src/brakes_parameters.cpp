// MaDaDev's Brakes.dll parameters as openRTS's cab brake devices take them
// (legacy_consist.hpp, BrakesParameters; docs/brakes-dll-replacement.md,
// step 2; the parameters: docs/reverse/brakes-dll.md §2, §10).
#include <openrts/trainplugin/legacy_consist.hpp>

#include <algorithm>
#include <cstdlib>
#include <string_view>

#include <openrts/core/virtual_path.hpp>

namespace openrts::trainplugin {

namespace {

using Cab = BrakesParameters::Cab;

// Each cab's switches and displays, BR_<name><k> with k from 1.
constexpr std::pair<std::string_view, std::int32_t Cab::*> cab_ids[] = {
    {"br_395id", &Cab::valve},
    {"br_334id", &Cab::valve334},
    {"br_kptid", &Cab::controller},
    {"br_pptid", &Cab::ppt},
    {"br_redid", &Cab::reducer},
    {"br_kkid", &Cab::combined},
    {"br_367id", &Cab::block},
    {"br_razid", &Cab::separating},
    {"br_254id", &Cab::independent},
    {"br_254rid", &Cab::independent_cock},
    {"br_254did", &Cab::independent2},
    {"br_254drid", &Cab::independent2_cock},
    {"br_4vkid", &Cab::vk4},
    {"br_4vkrid", &Cab::vk4_cock},
    {"br_otid", &Cab::release},
    {"br_etid", &Cab::emergency},
    {"br_eptapl", &Cab::ep_apply},
    {"br_eptrel", &Cab::ep_release},
    {"br_418pid", &Cab::sensor418},
    {"br_manc", &Cab::gauge_cylinder},
    {"br_manp", &Cab::gauge_pipe},
    {"br_manm", &Cab::gauge_main_reservoir},
    {"br_manu", &Cab::gauge_equalising},
    {"br_lampo", &Cab::lamp_ep_on},
    {"br_lampp", &Cab::lamp_ep_hold},
    {"br_lampt", &Cab::lamp_ep_brake},
    {"br_lamptm", &Cab::lamp_pipe},
    {"br_volt", &Cab::ep_volts},
    {"br_amp", &Cab::ep_amps},
};

// BR_KID is not a brake device: only the speed recorder reads it (its cock
// on, no ЭПК for running with the ALSN off; simulation/recorder.hpp).

// Names Brakes.dll never reads (a typo in content): nothing to do. The ЧС2's
// BR_OTIDID1 (Brakes.dll asks for BR_OTID<cab>).
constexpr std::string_view never_read[] = {"br_otidid"};

// Global parameters openRTS takes or that only Brakes.dll's other parts
// (its sounds, the ЭПК's mode) read, which go on running. BR_AUTO 1 (the
// ДТ116's): Brakes.dll takes no slave sections; openRTS's devices work each
// car's own air anyway.
constexpr std::string_view global_known[] = {"br_numcabs", "br_flipcabs", "br_mode",    "br_zarpress", "br_367oppol",
                                             "br_razobshk", "br_buferkvt", "br_ept",     "br_sknum",    "br_130mode",
                                             "br_224mode",  "br_epkmode",  "br_ampdbl",   "br_auto"};

bool is_brakes_dll(const std::string& module) {
    const auto m = core::fold_case(module);
    return m == "brakes.dll" || (m.size() > 11 && (m.ends_with("/brakes.dll") || m.ends_with("\\brakes.dll")));
}

} // namespace

BrakesParameters brakes_parameters(const formats::EngineDefinition& engine) {
    BrakesParameters p;
    const formats::AuxLibraryDefinition* library = nullptr;
    for (const auto& a : engine.aux_libraries) {
        if (is_brakes_dll(a.module)) {
            library = &a;
            break;
        }
    }
    if (!library) return p;
    p.present = true;
    std::size_t cabs = 1;
    int stop_valves = -1;
    std::vector<std::pair<int, std::int32_t>> stops;
    const auto not_known = [&](const std::string& name) {
        if (p.unknown.empty()) p.unknown = name;
    };
    for (const auto& parameter : library->parameters) {
        const auto name = core::fold_case(parameter.name);
        if (name == "trans_sound") p.trans_sound = parameter.value == 1.0f;
        if (!name.starts_with("br_")) continue; // the КЛУБ's and the rest: Brakes.dll's own
        std::size_t digits = name.size();
        while (digits > 0 && name[digits - 1] >= '0' && name[digits - 1] <= '9') --digits;
        const std::string_view base(name.data(), digits);
        const int index = digits < name.size() ? std::atoi(name.c_str() + digits) : 0;
        const auto value = static_cast<std::int32_t>(parameter.value);
        if (base == "br_sk" && index > 0) {
            stops.emplace_back(index, value);
            continue;
        }
        const auto field = std::find_if(std::begin(cab_ids), std::end(cab_ids), [&](const auto& c) { return c.first == base; });
        if (field != std::end(cab_ids)) {
            // Without its cab's number (the ВЛ82м's BR_OTID): Brakes.dll asks
            // for the name with the cab's appended and never finds it (no
            // such device), as openRTS.
            if (index < 1 || index > 8) continue;
            const auto k = static_cast<std::size_t>(index - 1);
            if (p.cabs.size() <= k) p.cabs.resize(k + 1);
            p.cabs[k].*(field->second) = value;
            continue;
        }
        if (base == "br_kid") continue; // the speed recorder's
        if (std::find(std::begin(never_read), std::end(never_read), base) != std::end(never_read)) continue;
        // The engine's own 292 (docs/reverse/brakes-dll.md §10): on
        // openRTS's air the car's distributor is a real one already, a 292
        // as its type gives it; nothing more to take over.
        if (name == "br_292custom") {
            p.custom292 = parameter.value != 0.0f;
            continue;
        }
        if (std::find(std::begin(global_known), std::end(global_known), name) == std::end(global_known)) {
            not_known(parameter.name);
            continue;
        }
        if (name == "br_numcabs") cabs = static_cast<std::size_t>(std::clamp(value, 1, 8));
        else if (name == "br_flipcabs") p.flipped = value != 0;
        else if (name == "br_mode") p.multiple_unit = value == 1;
        else if (name == "br_zarpress") p.charging = parameter.value;
        else if (name == "br_367oppol") p.block_operating = value;
        else if (name == "br_razobshk") p.separating_fitted = value != 0;
        else if (name == "br_epkmode") p.epk_sounds = parameter.value != 0.0f;
        else if (name == "br_buferkvt") p.buffer_kvt = value != 0;
        else if (name == "br_sknum") stop_valves = value;
        else if (name == "br_130mode") p.sounds_130 = parameter.value != 0.0f;
        else if (name == "br_224mode") p.sounds_224 = parameter.value != 0.0f;
    }
    if (p.cabs.size() < cabs) p.cabs.resize(cabs);
    if (!p.separating_fitted)
        for (auto& c : p.cabs) c.separating = -1;
    std::sort(stops.begin(), stops.end());
    for (const auto& [n, id] : stops)
        if ((stop_valves < 0 || n <= stop_valves) && id >= 0) p.stop_valves.push_back(id);
    return p;
}

std::vector<BrakeSpring> brake_springs(const BrakesParameters& p) {
    std::vector<BrakeSpring> springs;
    const auto add = [&](const std::int32_t id, const std::uint32_t last) {
        if (id < 0) return;
        springs.push_back(p.flipped ? BrakeSpring{.id = id, .from = 0, .to = 1, .direction = -1}
                                    : BrakeSpring{.id = id, .from = last, .to = last - 1, .direction = 1});
    };
    for (const auto& cab : p.cabs) {
        add(cab.independent, 5);
        add(cab.controller, 6);
        add(cab.independent2, 5);
    }
    return springs;
}

simulation::CabBrakeControls cab_brake_controls(const BrakesParameters& p, const std::size_t cab,
                                                const std::vector<dllhost::SwitchState>& switches, const CabBrakes& valve,
                                                const dllhost::AuxExchange exchange, const std::uint16_t systems) {
    simulation::CabBrakeControls c;
    const Cab ids = cab < p.cabs.size() ? p.cabs[cab] : p.cabs.empty() ? Cab{} : p.cabs.front();
    const auto state = [&](const std::int32_t id) -> int {
        return id >= 0 && static_cast<std::size_t>(id) < switches.size() ? static_cast<int>(switches[static_cast<std::size_t>(id)].state) : -1;
    };
    const auto on = [&](const std::int32_t id) { return state(id) > 0; };
    if (ids.valve334 >= 0) {
        // The 334 (the ЭР2's and ЭР9's valve): its state 0 .. 5 as it is.
        c.valve = simulation::ValvePosition::running;
        const int k = state(ids.valve334);
        c.valve334 = k >= 0 ? k : 4; // not read: train
    } else if (ids.valve >= 0 || ids.controller >= 0) {
        std::optional<simulation::ValvePosition> position;
        if (valve.position >= 0 && valve.position <= 6) position = static_cast<simulation::ValvePosition>(valve.position);
        else position = simulation::valve_position_of(state(ids.valve), p.flipped);
        c.valve = position.value_or(simulation::ValvePosition::running);
        // VЭ and V lower the УР unless the handle is passing through them
        // to a further position: Brakes.dll goes on while the handle leaves
        // them back (the golden runs: 0.6 atm in 2.8 s of V).
        if (ids.valve >= 0 && static_cast<std::size_t>(ids.valve) < switches.size()) {
            const auto& s = switches[static_cast<std::size_t>(ids.valve)];
            const int now = simulation::direct_state(static_cast<int>(s.state), 6, p.flipped);
            const int going = simulation::direct_state(static_cast<int>(s.set_state), 6, p.flipped);
            c.valve_resting = going <= now;
        }
        c.by_controller = valve.by_controller && ids.controller >= 0;
        // The train's DLL takes the handle as IV (var + 32 0x8000, the 2ЭС6).
        if (exchange.before & 0x8000u) c.valve = simulation::ValvePosition::lap_feed;
    } else {
        c.valve.reset();
    }
    c.charging = valve.charging > 0.0f ? valve.charging : p.charging > 0.0f ? p.charging : 5.0f;
    c.cut_off = valve.combined == 1 || valve.blocked || valve.separated;
    c.unfed = valve.blocked;
    // The pipe opens at the ЭПК's own trip (var + 8 0x01000000, its whistle's
    // 12 s; Brakes.dll's TrainPipeRate -1.2), not at var + 32 bit 1, which
    // only cuts the train DLL's traction and has more sources than the ЭПК.
    c.emergency = valve.combined == 2 || on(ids.emergency) || (exchange.flags & 0x01000000u) != 0 ||
                  std::any_of(p.stop_valves.begin(), p.stop_valves.end(), on);
    c.keep_equalising = (exchange.before & 0x4000u) != 0;
    const auto independent = [&](const std::int32_t id, const std::int32_t cock) -> std::optional<simulation::IndependentPosition> {
        if (id < 0 || (cock >= 0 && state(cock) == 0)) return std::nullopt;
        return simulation::independent_position_of(state(id), p.flipped);
    };
    c.independent = independent(ids.independent, ids.independent_cock);
    c.independent2 = independent(ids.independent2, ids.independent2_cock);
    if (ids.vk4 >= 0 && !(ids.vk4_cock >= 0 && state(ids.vk4_cock) == 0)) c.vk4 = simulation::vk4_position_of(state(ids.vk4), p.flipped);
    c.buffer_kvt = p.buffer_kvt;
    c.release_train_brake = on(ids.release);
    c.ep_on = (systems & 2u) != 0;
    c.ep_apply_button = on(ids.ep_apply);
    c.ep_release_button = on(ids.ep_release);
    c.ep_hold_asked = (exchange.before & 0x10u) != 0;
    c.ep_apply_asked = (exchange.before & 0x20u) != 0;
    // The ЭП2Д's ППТ at 0 with the ЭПТ on: its КПТ commands the ЭПТ by
    // itself (docs/reverse/brakes-dll.md, the КПТ): VI, V, VЭ hold and
    // apply, IV and III hold.
    if (ids.controller >= 0 && ids.ppt >= 0 && state(ids.ppt) == 0 && c.ep_on && state(ids.controller) >= 0) {
        const int position = simulation::direct_state(state(ids.controller), 6, p.flipped); // 0 is I
        if (position >= 4) c.ep_hold_asked = c.ep_apply_asked = true;
        else if (position >= 2) c.ep_hold_asked = true;
    }
    c.multiple_unit = p.multiple_unit;
    c.sensor418 = on(ids.sensor418);
    c.sounds_130 = p.sounds_130;
    c.sounds_224 = p.sounds_224;
    c.independent_sounds = !p.cabs.empty() && p.cabs.front().independent >= 0;
    return c;
}

void show_cab_brakes(const BrakesParameters& p, const std::size_t working, const simulation::CabBrakeOutput& shown,
                     std::vector<dllhost::DisplayState>& displays) {
    const auto value = [&](const std::int32_t id, const float v) {
        if (id >= 0 && static_cast<std::size_t>(id) < displays.size()) displays[static_cast<std::size_t>(id)].value = v;
    };
    const auto lamp = [&](const std::int32_t id, const bool lit) {
        if (id >= 0 && static_cast<std::size_t>(id) < displays.size()) displays[static_cast<std::size_t>(id)].state = lit ? 1u : 0u;
    };
    for (std::size_t k = 0; k < p.cabs.size(); ++k) {
        const auto& ids = p.cabs[k];
        value(ids.gauge_cylinder, shown.gauge_cylinder);
        value(ids.gauge_pipe, shown.gauge_pipe);
        value(ids.gauge_main_reservoir, shown.gauge_main_reservoir);
        if (k == working) value(ids.gauge_equalising, shown.gauge_equalising);
        lamp(ids.lamp_ep_on, shown.lamp_ep_on);
        lamp(ids.lamp_ep_hold, shown.lamp_ep_hold);
        lamp(ids.lamp_ep_brake, shown.lamp_ep_brake);
        lamp(ids.lamp_pipe, shown.lamp_pipe);
        value(ids.ep_volts, shown.ep_volts);
        value(ids.ep_amps, shown.ep_amps);
    }
}

} // namespace openrts::trainplugin
