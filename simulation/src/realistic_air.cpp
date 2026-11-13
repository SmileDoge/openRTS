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
// Flow between neighbouring pipe volumes: q = k · V · Δp. k sets the service
// wave (a 100-car train feels a 1.5 atm reduction at the tail in about 8 s).
constexpr float link_rate = 200.0f;        // 1/s
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
    return effective_load(car.distributor.load, car.definition ? car.definition->mass : 60000.0f);
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

void RealisticAir::reset(const std::vector<CarState>& cars) {
    air_.assign(cars.size(), {});
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

    // Sub-steps: explicit flow needs dt < 1 / (2k).
    const int steps = static_cast<int>(std::ceil(dt * link_rate * 3.0f));
    const float h = dt / static_cast<float>(steps);
    std::vector<float> flow(n);
    for (int s = 0; s < steps; ++s) {
        std::fill(flow.begin(), flow.end(), 0.0f);
        // Pipe flow between joined neighbours (volume-weighted pressures).
        for (std::size_t i = 0; i + 1 < n; ++i) {
            if (!inputs.joined[i]) continue;
            const float v = (std::min)(volume[i], volume[i + 1]);
            const float q = link_rate * v * (cars[i].pipe - cars[i + 1].pipe) * h;
            flow[i] -= q;
            flow[i + 1] += q;
        }
        for (std::size_t i = 0; i < n; ++i) {
            auto& car = cars[i];
            // Open ends vent; every car leaks a little.
            const bool front_open = (car.pipe_ends & pipe_hose_front) && !(car.pipe_ends & pipe_cock_front);
            const bool rear_open = (car.pipe_ends & pipe_hose_rear) && !(car.pipe_ends & pipe_cock_rear);
            if (front_open || rear_open) flow[i] -= open_end_rate * volume[i] * car.pipe * h;
            if (car.definition) flow[i] -= car.definition->brakes.train_pipe_leak_rate * volume[i] * h;
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
                    // Feeding needs the main reservoir above the pipe.
                    if (q > 0.0f) q = (std::min)(q, relay_gain * (std::max)(car.main_reservoir - car.pipe, 0.0f));
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
                .mass = car.definition->mass,
                .ep = (car.definition->brakes.system & 2) != 0,
                .load = load,
                .mountain = car.distributor.mountain,
                .plug = car.distributor.plug,
            };
            DistributorState state{car.pipe, car.auxiliary, car.cylinder, a.reference, a.braking, a.emergency};
            bool stepped = false;
            if (a.script) {
                std::string error;
                stepped = a.script->step(state, device, rate, car.ep, h, error);
                if (!stepped) {
                    report_once("brakes: " + car.definition->name + ": " + error + " (the built-in distributor takes over)");
                    a.script.reset();
                }
            }
            if (!stepped) step_distributor(state, device, rate, car.ep, h);
            car.pipe = state.pipe;
            car.auxiliary = state.auxiliary;
            car.cylinder = state.cylinder;
            a.reference = state.reference;
            a.braking = state.braking;
            // A script may start the emergency front itself.
            if (state.emergency) a.emergency = true;
            // Gone into emergency: its accelerator opens for 3 s.
            if (a.emergency && !was_emergency) a.accelerator = accelerator_time;
            // Recovery: once the pipe is charged again the car leaves emergency.
            if (a.emergency && p > 3.5f && rate > 0.0f) a.emergency = false;
        }
    }
    for (std::size_t i = 0; i < n; ++i) cars[i].control = air_[i].type == DistributorKind::vr483 ? air_[i].reference : cars[i].auxiliary;
}

} // namespace openrts::simulation::detail
