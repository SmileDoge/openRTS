#include "realistic_air.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <mutex>
#include <set>

namespace openrts::simulation::detail {

namespace {

// Volumes, m³. The pipe is 1¼" (32 mm bore, 0.8 l per metre of car).
constexpr float pipe_area = 0.0008f;
// The pipe between two cars' centres as a line with the air's inertia and
// the pipe's friction (2026-11-17): the flow q between them changes as
//   dq/dt = (Δp - R·q) / I,   I = L / (c²·A),   R = r·L,
// L the distance between the centres. A change runs down the pipe no
// faster than sound (a closed end reflects it), and the friction spreads
// it; the distributors' quick service carries the brake wave on. Until
// then the flow was q = k·V·Δp (k = 200 1/s) alone: no front, a 16-car
// train's tail braked 0.25 s after its head (1366 m/s), a 100-car freight
// train's tail never released in II.
// r from a 100-car loaded freight train with ВР 483 (openrts-train-check
// --charge, docs/train.md): full service fills the first car's cylinder to
// 90 % in 18.6 s and the last's in about 48 s (Крылов, table 9: 18 and 48
// s to 0.35 MPa); II releases the last in about 41 s (60 s); its pipe
// charges from empty in II to 4.5 at the tail in about 525 s (614 before).
// Its brake wave is about 385 m/s (Крылов: 270), a 16-car passenger train's
// about 550 (120): over 342 m the pipe passes a change in about a second,
// and a stiffer pipe that slowed it would leave a long freight train
// uncharged for half an hour.
constexpr float sound_speed = 330.0f;      // m/s
constexpr float pipe_resistance = 0.025f;  // atm·s/m³ per metre of pipe
constexpr float max_substep = 1.0f / 600.0f; // s
constexpr float wave_speed = 280.0f;       // m/s, the emergency accelerators' front
constexpr float open_end_rate = 60.0f;     // 1/s, an open pipe end with its cock open
constexpr float accelerator_time = 3.0f;    // s an emergency accelerator vents the pipe
constexpr float emergency_rate = -2.5f;    // atm/s: only a pipe vented right there fires by itself; the rest wait for the front
// The 395 relay at the locomotive: flow from the main reservoir (or to the
// atmosphere) towards the equalising reservoir, l·atm/s, with its capacity.
// The 395's relay (its equalising piston) is stiff: a small difference
// opens its feed or exhaust valve wide, so the pipe at the engine stands by
// the УР within seconds and the train's charging is the pipe's flow and the
// distributors' (2026-11-13: at 0.02 m³/(s·atm) the ЭД9М's pipe lagged its
// УР by 2 atm while charging and took minutes).
constexpr float relay_gain = 0.40f;        // m³/(s·atm)
constexpr float relay_feed = 0.150f;       // m³·atm/s
constexpr float relay_vent = 0.080f;
constexpr float emergency_valve = 0.050f;  // VI: m³/(s·atm) to the atmosphere

float car_length(const CarState& car) {
    return car.definition && car.definition->length > 1.0f ? car.definition->length : 14.0f;
}

float reservoir_volume(const formats::EngineDefinition& engine) { return main_reservoir_volume(engine); }

// The load mode of a car's 483: as a hand set it, else by its mass.
DistributorLoad load_of(const CarState& car) {
    return effective_load(car.distributor.load, car.definition ? static_cast<float>(car_mass(car)) : 60000.0f);
}


// A script's trouble goes to the console once, not once a car or a step.
void report_once(const std::string& message) {
    static std::mutex mutex;
    static std::set<std::string> reported;
    const std::lock_guard lock(mutex);
    if (reported.insert(message).second) std::cerr << message << '\n';
}

} // namespace

float main_reservoir_volume(const formats::EngineDefinition& engine) {
    // RTS content gives AirBrakesMainResVolume in litres (175 .. 2020 in the
    // MaDaDev install) or, MSTS style, cubic feet (1 .. 84: the ВЛ80Т's 66,
    // the ЧС2т's 84). At a border of 50 the ВЛ80Т's 66 ft³ (1.9 m³) read as
    // 66 litres, and its compressors cycled every few seconds (2026-11-13).
    const float v = engine.main_reservoir_volume;
    if (v <= 0.0f) return 1.0f;
    return v > 100.0f ? v * 0.001f : v * 0.0283f;
}

void RealisticAir::save(StateWriter& out) const {
    for (std::size_t i = 0; i < air_.size(); ++i) {
        const auto& a = air_[i];
        out.put("a" + std::to_string(i) + ".state", a.braking, a.emergency, a.reference, a.last_pipe, a.wave, a.accelerator, a.ep_chamber,
                a.ep_side, a.pipe_low);
    }
    out.put_list("air.links", link_flow_);
}

void RealisticAir::restore(const StateReader& in) {
    for (std::size_t i = 0; i < air_.size(); ++i) {
        auto& a = air_[i];
        in.get("a" + std::to_string(i) + ".state", a.braking, a.emergency, a.reference, a.last_pipe, a.wave, a.accelerator, a.ep_chamber,
               a.ep_side, a.pipe_low);
    }
    std::vector<float> links;
    if (in.get_list("air.links", links) && links.size() == link_flow_.size()) {
        link_flow_ = std::move(links);
    } else {
        std::fill(link_flow_.begin(), link_flow_.end(), 0.0f);
    }
}

void RealisticAir::reset(const std::vector<CarState>& cars) {
    air_.assign(cars.size(), {});
    link_flow_.assign(cars.empty() ? 0 : cars.size() - 1, 0.0f);
    for (std::size_t i = 0; i < cars.size(); ++i) {
        if (cars[i].definition) air_[i].type = distributor_kind(*cars[i].definition);
        air_[i].reference = cars[i].pipe;
        air_[i].last_pipe = cars[i].pipe;
        // The car's own distributor in Luau (ORTSBrakeDistributor), each car
        // its own VM; a script that does not load is reported once and the
        // built-in one works.
        const auto* definition = cars[i].definition;
        if (!definition) continue;
        const auto& brakes = definition->brakes;
        if (brakes.distributor_script.empty()) continue;
        if (brakes.distributor_source.empty()) {
            report_once("brakes: " + definition->name + ": " + brakes.distributor_error);
            continue;
        }
        std::string error;
        air_[i].script = ScriptDistributor::load(brakes.distributor_source, brakes.distributor_script, error);
        if (!air_[i].script) report_once("brakes: " + definition->name + ": " + error);
    }
}

void RealisticAir::step(std::vector<CarState>& cars, const AirInputs& inputs, const float dt) {
    const std::size_t n = cars.size();
    if (air_.size() != n) reset(cars);
    if (n == 0) return;
    std::vector<float> volume(n);
    for (std::size_t i = 0; i < n; ++i) volume[i] = car_length(cars[i]) * pipe_area;
    const auto& demands = *inputs.demands;

    // The distributor each car carries now (the car menu may change it).
    for (std::size_t i = 0; i < n; ++i) {
        if (cars[i].definition) air_[i].type = distributor_kind(*cars[i].definition);
    }
    // Sub-steps of under 2 ms (the distributors' quick service and
    // thresholds; the line itself needs less than L / c).
    const int steps = (std::max)(1, static_cast<int>(std::ceil(dt / max_substep)));
    const float h = dt / static_cast<float>(steps);
    if (link_flow_.size() + 1 != n) link_flow_.assign(n > 0 ? n - 1 : 0, 0.0f);
    std::vector<float> flow(n);
    for (int s = 0; s < steps; ++s) {
        std::fill(flow.begin(), flow.end(), 0.0f);
        // The pipe between joined neighbours: the flow follows the pressure
        // difference against its inertia and friction (semi-implicit).
        for (std::size_t i = 0; i + 1 < n; ++i) {
            auto& q = link_flow_[i];
            if (!inputs.joined[i]) {
                q = 0.0f;
                continue;
            }
            const float length = 0.5f * (car_length(cars[i]) + car_length(cars[i + 1]));
            const float inertia = length / (sound_speed * sound_speed * pipe_area);
            const float resistance = pipe_resistance * length;
            q = (q + h * (cars[i].pipe - cars[i + 1].pipe) / inertia) / (1.0f + h * resistance / inertia);
            flow[i] -= q * h;
            flow[i + 1] += q * h;
        }
        for (std::size_t i = 0; i < n; ++i) {
            auto& car = cars[i];
            // Open ends vent; every car leaks a little.
            const bool front_open = (car.pipe_ends & pipe_hose_front) && !(car.pipe_ends & pipe_cock_front);
            const bool rear_open = (car.pipe_ends & pipe_hose_rear) && !(car.pipe_ends & pipe_cock_rear);
            if (front_open || rear_open) flow[i] -= open_end_rate * volume[i] * car.pipe * h;
            if (car.definition) flow[i] -= car.definition->brakes.train_pipe_leak_rate * volume[i] * h;
            // Its wear's own leak, as the pipe's pressure drives it.
            if (car.wear.pipe_leak > 0.0f) flow[i] -= car.wear.pipe_leak / 60.0f * (car.pipe / 5.0f) * volume[i] * h;
            // The accelerator of a car gone into emergency vents its pipe for
            // a while (not a 292 with its plug at УВ: the accelerator is off).
            const bool accelerator_off = air_[i].type != DistributorKind::vr483 &&
                                         car.distributor.plug == DistributorPlug::no_accelerator;
            if (air_[i].accelerator > 0.0f) {
                if (!accelerator_off) flow[i] -= open_end_rate * volume[i] * car.pipe * h;
                air_[i].accelerator -= h;
            }
            // The driver's valve of a locomotive working the pipe (395 relay).
            if (car.definition && car.definition->engine && i < inputs.has_controller->size() &&
                (*inputs.has_controller)[i] && demands[i].equalising >= 0.0f) {
                const auto& d = demands[i];
                const float vmr = reservoir_volume(*car.definition->engine);
                if (d.emergency) {
                    flow[i] -= emergency_valve * car.pipe * h;
                } else {
                    float q = relay_gain * (d.equalising - car.pipe);
                    q = std::clamp(q, -relay_vent, relay_feed);
                    // Feeding needs the main reservoir above the pipe, and
                    // the valve not in III (lap without feed: its leaks are
                    // not made up; until 2026-11-18 it held the pipe as IV).
                    if (q > 0.0f) q = d.pipe_feed ? (std::min)(q, relay_gain * (std::max)(car.main_reservoir - car.pipe, 0.0f)) : 0.0f;
                    // The relay holds the pipe at the УР: no overshoot in a sub-step.
                    const float most = (d.equalising - car.pipe) * volume[i];
                    q = q > 0.0f ? (std::min)(q * h, (std::max)(most, 0.0f)) : (std::max)(q * h, (std::min)(most, 0.0f));
                    flow[i] += q;
                    if (q > 0.0f) car.main_reservoir = (std::max)(car.main_reservoir - q / vmr, 0.0f);
                }
            }
        }
        for (std::size_t i = 0; i < n; ++i) cars[i].pipe = (std::max)(cars[i].pipe + flow[i] / volume[i], 0.0f);

        // Distributors.
        for (std::size_t i = 0; i < n; ++i) {
            auto& car = cars[i];
            auto& a = air_[i];
            const float p = car.pipe;
            const float rate = (p - a.last_pipe) / h;
            a.last_pipe = p;
            if (!car.definition || !(car.definition->brakes.system & 5)) continue;
            const bool was_emergency = a.emergency;
            // The emergency accelerator: a fast fall, or the wave arriving.
            if (a.wave >= 0.0f) {
                a.wave -= h;
                if (a.wave <= 0.0f) {
                    a.wave = -1.0f;
                    a.emergency = true;
                }
            }
            // The front starts where the pipe is opened: an open end (a
            // parted train, an uncoupled hose) or the driver's valve in VI;
            // it reaches the other cars at the wave speed (the flow model
            // alone would carry it faster than sound).
            const bool end_open = ((car.pipe_ends & pipe_hose_front) && !(car.pipe_ends & pipe_cock_front)) ||
                                  ((car.pipe_ends & pipe_hose_rear) && !(car.pipe_ends & pipe_cock_rear));
            if (!a.emergency && end_open && rate <= emergency_rate) a.emergency = true;
            // A hand-opened emergency valve fires at its car at once: the
            // accelerators along the train go with it.
            if (!a.emergency && (car.valves & valve_emergency)) a.emergency = true;
            // The driver's valve in VI starts the front at its own locomotive.
            if (!a.emergency && i < inputs.has_controller->size() && (*inputs.has_controller)[i] && demands[i].emergency)
                a.emergency = true;
            if (a.emergency && a.accelerator > 0.0f) {
                // The front moves on to the neighbours at the wave speed,
                // carried by the accelerators while they vent (a car still
                // in emergency once its accelerator has closed sends no new
                // front: a neighbour that charged again stays released).
                const float delay = car_length(car) / wave_speed;
                if (i > 0 && inputs.joined[i - 1] && !air_[i - 1].emergency && air_[i - 1].wave < 0.0f) air_[i - 1].wave = delay;
                if (i + 1 < n && inputs.joined[i] && !air_[i + 1].emergency && air_[i + 1].wave < 0.0f) air_[i + 1].wave = delay;
            }
            // The distributor: the car's script, else the built-in one.
            const auto kind = a.type;
            // The release valve pulled (the car menu) on a braked car: a 483
            // vents its РК and releases at once, its ЗР kept; a 292 vents its
            // ЗР until the pipe stands above it (ts.h Wagon Flags1; RTS bleeds
            // the cylinder only, which the distributor here would fill again).
            if ((car.valves & valve_release) && a.braking && !a.emergency) {
                if (kind == DistributorKind::vr483) a.reference = (std::min)(a.reference, p);
                else car.auxiliary = (std::max)(car.auxiliary - 0.5f * h, 0.0f);
            }
            const auto load = load_of(car);
            const DistributorCar device{
                .kind = kind,
                .full_cylinder = distributor_full_cylinder(kind, load),
                .ratio = car.definition->brakes.triple_valve_ratio,
                .pipe_volume = volume[i],
                .auxiliary_volume = distributor_auxiliary_volume(kind),
                .cylinder_volume = distributor_cylinder_volume,
                .mass = car_mass(car),
                .ep = (car.definition->brakes.system & 2) != 0,
                .ep_fill = car.ep_fill,
                .load = load,
                .mountain = car.distributor.mountain,
                .plug = car.distributor.plug,
                .pace = car.wear.pace,
                .sensitivity = car.wear.sensitivity,
            };
            DistributorState state{.pipe = car.pipe,
                                   .auxiliary = car.auxiliary,
                                   .cylinder = car.cylinder,
                                   .reference = a.reference,
                                   .braking = a.braking,
                                   .emergency = a.emergency,
                                   .zk = a.zk,
                                   .kdr = a.kdr,
                                   .ep_chamber = a.ep_chamber,
                                   .ep_side = a.ep_side,
                                   .pipe_low = a.pipe_low,
                                   .discharge = false,
                                   .channel = a.channel};
            bool stepped = false;
            // A built-in one named (the car menu, a mod) stands in for a script.
            if (a.script && car.definition->brakes.distributor == formats::BrakeDistributor::inferred) {
                std::string error;
                stepped = a.script->step(state, device, rate, car.ep, h, error, car.ep_command);
                if (!stepped) {
                    report_once("brakes: " + car.definition->name + ": " + error + " (the built-in distributor takes over)");
                    a.script.reset();
                }
            }
            if (!stepped) step_distributor(state, device, rate, car.ep, h, car.ep_command);
            car.pipe = state.pipe;
            car.auxiliary = state.auxiliary;
            car.cylinder = state.cylinder;
            a.reference = state.reference;
            a.braking = state.braking;
            a.zk = state.zk;
            a.kdr = state.kdr;
            a.ep_chamber = state.ep_chamber;
            a.ep_side = state.ep_side;
            a.pipe_low = state.pipe_low;
            a.channel = state.channel;
            // A script may start the emergency front itself.
            if (state.emergency) a.emergency = true;
            // Gone into emergency: its accelerator opens for 3 s.
            if (a.emergency && !was_emergency) a.accelerator = accelerator_time;
            // Recovery: once the pipe is charged again the car leaves emergency.
            if (a.emergency && p > 3.5f && rate > 0.0f) a.emergency = false;
            // Its cylinder leaks (wear). A 483 keeps it filled from its ЗР,
            // the ЗР from the pipe, so a braked train's pipe leaks more; a
            // 292 does not (its piston stays in lap: the cylinder falls), a
            // 305 on its side does (its relay, from the ЗР).
            if (car.wear.cylinder_leak > 0.0f && car.cylinder > 0.0f) {
                const float lost = (std::min)(car.cylinder, car.wear.cylinder_leak / 60.0f * (car.cylinder / 3.8f) * h);
                car.cylinder -= lost;
                if (kind != DistributorKind::vr483 && a.braking && !a.ep_side) {
                    const float ratio = car.definition->brakes.triple_valve_ratio > 0.5f ? car.definition->brakes.triple_valve_ratio : 3.0f;
                    a.reference -= lost / ratio;
                }
            }
        }
    }
    for (std::size_t i = 0; i < n; ++i) cars[i].control = air_[i].type == DistributorKind::vr483 ? air_[i].reference : cars[i].auxiliary;
}

} // namespace openrts::simulation::detail
