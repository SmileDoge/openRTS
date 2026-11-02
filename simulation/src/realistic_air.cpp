#include "realistic_air.hpp"

#include <algorithm>
#include <cmath>

namespace openrts::simulation::detail {

namespace {

// Volumes, m³. The pipe is 1¼" (32 mm bore, 0.8 l per metre of car).
constexpr float pipe_area = 0.0008f;
constexpr float zr483 = 0.078f;  // ЗР of a freight car (ВР 483)
constexpr float zr292 = 0.055f;  // ЗР of a passenger car (ВР 292)
constexpr float cylinder_volume = 0.008f;
// Flow between neighbouring pipe volumes: q = k · V · Δp. k sets the service
// wave (a 100-car train feels a 1.5 atm reduction at the tail in about 8 s).
constexpr float link_rate = 200.0f;        // 1/s
constexpr float wave_speed = 280.0f;       // m/s, the emergency accelerators' front
constexpr float open_end_rate = 60.0f;     // 1/s, an open pipe end with its cock open
// Distributor values (docs/train.md, the research table).
constexpr float s483 = 0.3f / 60.0f;       // stability, atm/s
constexpr float s292 = 0.5f / 60.0f;
constexpr float apply_483 = 0.15f;         // РК − pipe that starts an application
constexpr float apply_292 = 0.10f;
constexpr float quick_service = 0.12f;     // atm the quick-service valve takes locally
constexpr float release_483 = 0.06f;       // flat mode: release once the pipe is back within this of РК
constexpr float release_292 = 0.12f;       // a rise this far above the ЗР releases fully
constexpr float tau_zr483 = 45.0f;         // s, ЗР charging time constant
constexpr float tau_zr292 = 35.0f;
constexpr float tau_rk = 60.0f;            // s, РК charging
constexpr float emergency_rate = -2.5f;    // atm/s: only a pipe vented right there fires by itself; the rest wait for the front
// The 395 relay at the locomotive: flow from the main reservoir (or to the
// atmosphere) towards the equalising reservoir, l·atm/s, with its capacity.
constexpr float relay_gain = 0.020f;       // m³/(s·atm)
constexpr float relay_feed = 0.050f;       // m³·atm/s
constexpr float relay_vent = 0.020f;
constexpr float emergency_valve = 0.050f;  // VI: m³/(s·atm) to the atmosphere

float car_length(const CarState& car) {
    return car.definition && car.definition->length > 1.0f ? car.definition->length : 14.0f;
}

float reservoir_volume(const formats::EngineDefinition& engine) {
    // RTS content gives AirBrakesMainResVolume in litres (750) or, MSTS
    // style, cubic feet (a value below 50).
    const float v = engine.main_reservoir_volume;
    if (v <= 0.0f) return 1.0f;
    return v > 50.0f ? v * 0.001f : v * 0.0283f;
}

// The cylinder pressure a full application gives: by the car's load for 483
// (empty / medium / loaded), 3.8 for passenger cars.
float full_cylinder(const CarState& car, Distributor type) {
    if (type != Distributor::vr483) return 3.8f;
    const float mass = car.definition ? car.definition->mass : 60000.0f;
    return mass > 60000.0f ? 3.8f : mass > 35000.0f ? 3.0f : 1.5f;
}

} // namespace

Distributor distributor_for(const formats::WagonDefinition& definition) {
    if (definition.brakes.equipment & 4 || definition.brakes.system & 2) return Distributor::evr305;
    if (definition.kind == formats::WagonKind::freight) return Distributor::vr483;
    return Distributor::vr292;
}

void RealisticAir::reset(const std::vector<CarState>& cars) {
    air_.assign(cars.size(), {});
    for (std::size_t i = 0; i < cars.size(); ++i) {
        if (cars[i].definition) air_[i].type = distributor_for(*cars[i].definition);
        air_[i].reference = cars[i].pipe;
        air_[i].last_pipe = cars[i].pipe;
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
            // The accelerator of a car in emergency keeps its pipe open.
            if (air_[i].emergency) flow[i] -= open_end_rate * volume[i] * car.pipe * h;
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
            const float zr_volume = a.type == Distributor::vr483 ? zr483 : zr292;
            const float tc_max = full_cylinder(car, a.type);
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
            if (a.emergency) {
                a.braking = true;
                // The front moves on to the neighbours at the wave speed.
                const float delay = car_length(car) / wave_speed;
                if (i > 0 && inputs.joined[i - 1] && !air_[i - 1].emergency && air_[i - 1].wave < 0.0f) air_[i - 1].wave = delay;
                if (i + 1 < n && inputs.joined[i] && !air_[i + 1].emergency && air_[i + 1].wave < 0.0f) air_[i + 1].wave = delay;
                const float fill = (a.type == Distributor::vr483 ? 0.45f : 0.5f) * h;
                const float rise = (std::min)(tc_max - car.cylinder, fill);
                if (rise > 0.0f && car.auxiliary > car.cylinder) {
                    car.cylinder += rise;
                    car.auxiliary = (std::max)(car.auxiliary - rise * cylinder_volume / zr_volume, 0.0f);
                }
                // Recovery: once the pipe is charged again the accelerator closes.
                if (p > 3.5f && rate > 0.0f) a.emergency = false;
                continue;
            }
            // ЗР charging from the pipe (the air comes out of the pipe).
            if (p > car.auxiliary && !(a.braking && a.type == Distributor::vr292)) {
                const float tau = a.type == Distributor::vr483 ? tau_zr483 : tau_zr292;
                const float charge = (p - car.auxiliary) * h / tau;
                car.auxiliary += charge;
                car.pipe = (std::max)(car.pipe - charge * zr_volume / volume[i], 0.0f);
            }
            // ЭВР 305 with the EP brake working: the cylinder follows the wire.
            if (a.type == Distributor::evr305 && (car.definition->brakes.system & 2) && car.ep >= 0.0f) {
                const float target = (std::min)(car.ep, tc_max);
                const float change = std::clamp(target - car.cylinder, -0.35f * h, 1.0f * h);
                car.cylinder = (std::max)(car.cylinder + change, 0.0f);
                a.braking = car.cylinder > 0.05f;
                a.reference = car.auxiliary;
                continue;
            }
            if (a.type == Distributor::vr483) {
                // РК: charges from the pipe while released; follows a slow fall.
                if (!a.braking) {
                    if (p > a.reference) a.reference += (p - a.reference) * h / tau_rk;
                    else a.reference = (std::max)(p, a.reference - s483 * h);
                }
                const float drop = a.reference - p;
                if (!a.braking && drop >= apply_483) {
                    a.braking = true;
                    car.pipe = (std::max)(car.pipe - quick_service, 0.0f); // quick service
                }
                if (a.braking) {
                    const float target = (std::min)(tc_max, tc_max / 1.5f * drop);
                    if (car.cylinder < target && car.auxiliary > car.cylinder) {
                        const float rise = (std::min)(target - car.cylinder, tc_max * 0.9f / 18.0f * h);
                        car.cylinder += rise;
                        car.auxiliary = (std::max)(car.auxiliary - rise * cylinder_volume / zr_volume, 0.0f);
                    }
                    // Flat mode: no graduated release; back within 0.025 of РК releases fully.
                    if (p >= a.reference - release_483) a.braking = false;
                } else if (car.cylinder > 0.0f) {
                    car.cylinder = (std::max)(car.cylinder - tc_max / 35.0f * h, 0.0f);
                }
            } else {
                // ВР 292: the ЗР is the reference; it flows back on a slow fall.
                if (!a.braking) {
                    if (p < car.auxiliary) {
                        const float back = (std::min)(car.auxiliary - p, s292 * h);
                        car.auxiliary -= back;
                    }
                    if (car.auxiliary - p >= apply_292 && p < 4.9f) {
                        a.braking = true;
                        a.reference = car.auxiliary;
                        car.pipe = (std::max)(car.pipe - quick_service * 0.5f, 0.0f);
                    }
                }
                if (a.braking) {
                    const float ratio = car.definition->brakes.triple_valve_ratio > 0.5f
                                            ? car.definition->brakes.triple_valve_ratio
                                            : 3.0f;
                    const float target = (std::min)(tc_max, ratio * (a.reference - p));
                    if (car.cylinder < target && car.auxiliary > car.cylinder) {
                        const float rise = (std::min)(target - car.cylinder, 0.5f * h);
                        car.cylinder += rise;
                        car.auxiliary = (std::max)(car.auxiliary - rise * cylinder_volume / zr_volume, 0.0f);
                    }
                    // A rise above the ЗР releases fully.
                    if (p >= car.auxiliary + release_292) a.braking = false;
                } else if (car.cylinder > 0.0f) {
                    car.cylinder = (std::max)(car.cylinder - 0.35f * h, 0.0f);
                }
            }
        }
    }
    for (std::size_t i = 0; i < n; ++i) cars[i].control = air_[i].type == Distributor::vr483 ? air_[i].reference : cars[i].auxiliary;
}

} // namespace openrts::simulation::detail
