#include <openrts/simulation/train.hpp>

#include <openrts/simulation/electrification.hpp>

#include "realistic_air.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

#include <openrts/formats/track_database.hpp>
#include <openrts/formats/track_sections.hpp>

// Ported from the rts1.exe train update (decompiled; docs/reverse/*):
// - forces and the active/dissipative split: 0x424488;
// - coupler solver: 0x505414 and the velocity equalisation 0x504648;
// - brakes: 0x424488 and 0x5031D8 (docs/reverse/brakes.md).
// RTS runs this with the frame time; here it runs with a fixed step.

namespace openrts::simulation {
namespace {

constexpr float gravity = 10.0f;        // RTS uses g = 10
constexpr float pipe_area = 0.0024f;    // brake pipe volume per metre of car
constexpr float atm_epsilon = 1e-6f;

std::atomic<std::uint64_t> next_train_id{1};

using formats::Coupling;
using formats::WagonDefinition;

// Coupling block a car presents at the joint behind it (towards the end of
// the train) and ahead of it: [0] behind and [1] ahead, swapped when flipped.
[[nodiscard]] const Coupling& coupling_behind(const CarState& car) {
    return car.definition->couplings[car.flipped ? 1 : 0];
}
[[nodiscard]] const Coupling& coupling_ahead(const CarState& car) {
    return car.definition->couplings[car.flipped ? 0 : 1];
}

[[nodiscard]] float length_of(const CarState& car) {
    return car.definition->length;
}

[[nodiscard]] float mass_of(const CarState& car) {
    return (std::max)(car.definition->mass, 1.0f);
}

// Gap of the joint between car j and car j+1 with extra displacements:
// centre distance minus half lengths and half r0 of both ends (r0 once more
// for a rigid end); 0 at rest.
[[nodiscard]] double joint_gap(const CarState& ahead, const CarState& behind, const double ahead_moved,
                               const double behind_moved) {
    const auto& a = coupling_behind(ahead);
    const auto& b = coupling_ahead(behind);
    double gap = (ahead.offset + ahead_moved) - (behind.offset + behind_moved);
    gap -= (length_of(ahead) + length_of(behind)) * 0.5;
    gap -= (a.r0_tension + b.r0_tension) * 0.5;
    if (a.rigid) gap -= a.r0_tension * 0.5;
    if (b.rigid) gap -= b.r0_tension * 0.5;
    return gap;
}

// MSTS Friction law as rts1.exe evaluates it (including its formula above the
// transition speed, which adds rather than multiplies the first term).
[[nodiscard]] float rolling_resistance(const WagonDefinition& wagon, const float speed) {
    const auto& f = wagon.friction;
    if (speed > f.transition) {
        const double low = std::pow(f.transition, f.e1) + f.c1;
        return static_cast<float>((std::pow(speed - f.transition, f.e2) + f.transition) * f.c2 + low);
    }
    const double scale = speed <= 1.0f ? 1.0 : std::pow(speed, f.e1);
    return static_cast<float>(scale * f.c1);
}


// Velocity equalisation between car `a` and car `b = a + 1` (rts1.exe
// 0x504648). The relative displacement of the pair over the sub-step is
// resolved towards the train front: at each joint part goes into the coupler
// (its slack, by stiffness against damping), the rest is a plastic momentum
// exchange between the car and everything behind it, limited by the static
// friction left on braked cars. `gaps` is scratch space.
void equalise(std::vector<CarState>& cars, const std::size_t a, const std::size_t b, std::vector<double>& velocity,
              std::vector<double>& gaps, std::vector<double>& dissipative, const std::vector<double>& moved,
              const double h) {
    const double h2 = h * h;
    const bool opening = velocity[a] >= velocity[b];
    const bool slowing = (velocity[b] < 0.0 && velocity[a] >= velocity[b])
        || (velocity[b] >= 0.0 && velocity[a] < velocity[b]);
    for (std::size_t i = b; i > 0; --i) {
        double ahead_extra = moved[i - 1];
        double behind_extra = moved[i];
        if (i != b) {
            ahead_extra += velocity[i - 1] * h;
            behind_extra += velocity[i] * h;
        }
        gaps[i - 1] = joint_gap(cars[i - 1], cars[i], ahead_extra, behind_extra);
    }
    double relative = (velocity[a] - velocity[b]) * h;
    for (std::size_t j = a + 1; j-- > 0;) {
        const auto& car = cars[j];
        const double mass = mass_of(car);
        double behind_mass = 0.0;
        double behind_friction = 0.0;
        for (std::size_t k = j + 1; k <= b; ++k) {
            behind_mass += mass_of(cars[k]);
            if (!slowing) behind_friction += dissipative[k];
        }
        const auto& front_end = coupling_behind(car);
        const auto& back_end = coupling_ahead(cars[j + 1]);
        const double stiffness = opening ? front_end.stiffness_tension + back_end.stiffness_tension
                                         : front_end.stiffness_compression + back_end.stiffness_compression;
        const double damping = opening ? front_end.damping_tension + back_end.damping_tension
                                       : front_end.damping_compression + back_end.damping_compression;
        double slack = 0.0;
        if (!front_end.rigid) slack += front_end.r0_tension * 0.5;
        if (!back_end.rigid) slack += back_end.r0_tension * 0.5;
        double gap = gaps[j];
        if (opening) {
            if (gaps[j] < 0.0) {
                if (-gaps[j] >= relative) return;
                relative += gaps[j];
                gap = 0.0;
            }
        } else if (gaps[j] > 0.0) {
            if (-relative <= gaps[j]) return;
            relative += gaps[j];
            gap = 0.0;
        }
        if (relative == 0.0) break;
        double taken = 0.0;
        if (slack == 0.0) {
            taken = -gaps[j];
        } else {
            const double denominator = damping * relative / h;
            double share = denominator == 0.0 ? 1.0 : std::abs(stiffness / denominator);
            share = (std::min)(share, 1.0);
            taken = relative * share / (share + 1.0);
            if (opening) {
                if (taken + gap > slack) taken = slack - gap;
            } else if (-taken - gap > slack) {
                taken = -slack - gap;
            }
        }
        const double exchanged = (relative - taken) * behind_mass / (mass + behind_mass);
        const double rest = relative - taken - exchanged;
        double car_force = -mass * exchanged / h2;
        double behind_force = -car_force;
        double friction_factor = 1.0;
        if (!slowing || dissipative[j] == 0.0) {
            if (!slowing && behind_friction != 0.0) {
                double held = 0.0;
                if (behind_force < 0.0) {
                    if (-behind_force > behind_friction) {
                        friction_factor = 0.0;
                        held = rest * behind_friction / behind_force;
                    } else {
                        friction_factor = behind_force / behind_friction + 1.0;
                        held = -rest;
                    }
                } else if (behind_force > behind_friction) {
                    friction_factor = 0.0;
                    held = -rest * behind_friction / behind_force;
                } else {
                    friction_factor = 1.0 - behind_force / behind_friction;
                    held = -rest;
                }
                car_force = -mass * (exchanged - held) / h2;
                behind_force = behind_mass * (held + rest) / h2;
            }
        } else {
            double held = 0.0;
            if (car_force < 0.0) {
                if (-car_force > dissipative[j]) {
                    held = -exchanged * dissipative[j] / car_force;
                    dissipative[j] = 0.0;
                } else {
                    dissipative[j] += car_force;
                    held = -exchanged;
                }
            } else if (car_force > dissipative[j]) {
                held = exchanged * dissipative[j] / car_force;
                dissipative[j] = 0.0;
            } else {
                dissipative[j] -= car_force;
                held = -exchanged;
            }
            car_force = -mass * (held + exchanged) / h2;
            behind_force = behind_mass * (rest - held) / h2;
        }
        for (std::size_t m = j + 1; m <= b; ++m) {
            velocity[m] += behind_force / behind_mass * h;
            if (!slowing) dissipative[m] *= friction_factor;
        }
        const double change = car_force / mass * h;
        velocity[j] += change;
        gaps[j] = gap + taken;
        relative = -change * h;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Keyboard controller

KeyboardController::KeyboardController(std::shared_ptr<DriverHandles> handles, const bool master)
    : handles_(std::move(handles)), master_(master) {}

std::shared_ptr<DriverHandles> attach_keyboard_driver(Train& train, std::size_t master) {
    const auto engine = [&](const std::size_t i) {
        const auto* definition = i < train.cars().size() ? train.cars()[i].definition : nullptr;
        return definition && definition->engine ? &*definition->engine : nullptr;
    };
    if (!engine(master)) {
        master = SIZE_MAX;
        for (std::size_t i = 0; i < train.cars().size() && master == SIZE_MAX; ++i)
            if (engine(i)) master = i;
    }
    if (master == SIZE_MAX) return nullptr;
    auto handles = std::make_shared<DriverHandles>();
    if (engine(master)->throttle_notches > 0) handles->notches = engine(master)->throttle_notches;
    for (std::size_t i = 0; i < train.cars().size(); ++i) {
        if (!engine(i)) continue;
        train.set_controller(i, std::make_unique<KeyboardController>(handles, i == master));
    }
    return handles;
}

void KeyboardController::step(const double dt, const EngineInputs& inputs, EngineDemands& demands) {
    demands = {};
    const auto& handles = *handles_;
    const auto notches = (std::max)(handles.notches, 1u);
    const auto throttle = (std::min)(handles.throttle, notches);
    const int reverser = handles.reverser * (inputs.flipped ? -1 : 1);
    const auto brake = handles.brake;
    const auto* engine = inputs.definition && inputs.definition->engine ? &*inputs.definition->engine : nullptr;
    // Traction: MaxForce up to the power hyperbola, scaled by the notch. An
    // electric engine needs a line it works on (WorksOnLineVoltages) when the
    // route's electrification is modelled.
    const bool powered = !engine || engine->kind != formats::EngineKind::electric || inputs.line_voltage < 0.0f
        || Electrification::works_on(*engine, {inputs.line_voltage, inputs.line_frequency});
    if (engine && powered && reverser != 0 && throttle > 0 && brake != BrakeHandle::emergency) {
        const float max_force = engine->max_force > 0.0f ? engine->max_force : 300000.0f;
        const float power = engine->max_power > 0.0f ? engine->max_power : max_force * 10.0f;
        const float speed = std::abs(inputs.speed);
        float force = (std::min)(max_force, power / (std::max)(speed, 0.5f));
        force *= static_cast<float>(throttle) / static_cast<float>(notches);
        if (engine->max_velocity > 0.0f && speed > engine->max_velocity) force = 0.0f;
        // Stay within adhesion: MaxForce of DLL engines is often far above it
        // (RTS ignores it), and slipping would make keyboard driving jumpy.
        // The sander raises the limit as it raises adhesion (Adheasion's third value).
        const float sand = handles.sanding && inputs.definition->adhesion[2] > 0.0f ? inputs.definition->adhesion[2] : 1.0f;
        const float mu = inputs.definition->adhesion[1] * sand;
        if (mu > 0.0f) force = (std::min)(force, inputs.definition->mass * 10.0f * mu * 0.9f);
        demands.force = force * static_cast<float>(reverser);
    }
    // Compressor with hysteresis (every engine).
    if (inputs.main_reservoir < 7.5f) compressor_ = true;
    if (inputs.main_reservoir > 9.0f) compressor_ = false;
    demands.main_reservoir_rate = compressor_ ? 0.05f : 0.0f;
    if (inputs.realistic_air && compressor_ && engine) {
        // A КТ-6 delivers about 5.3 m³ of free air a minute (0.088 m³·atm/s)
        // into the real main reservoir volume.
        const float v = engine->main_reservoir_volume;
        const float volume = v <= 0.0f ? 1.0f : v > 50.0f ? v * 0.001f : v * 0.0283f;
        demands.main_reservoir_rate = 0.088f / volume;
    }
    // The independent brake acts on every engine (multiple-unit line).
    demands.independent_brake = std::clamp(handles.independent, 0.0f, 4.0f);
    demands.sanding = handles.sanding;
    if (!master_) return;
    // Brake valve 395, as the ВЛ10к DLL (vl10k_main.cpp) works it. The pipe
    // follows the equalising reservoir; positive rates feed from the main
    // reservoir (a factor), negative ones vent, atm/s.
    constexpr float ur_charge_rate = 0.25f;   // BRAKE_UR_RATE_CHARGE
    constexpr float pipe_charge_div = 1.8f;   // BRAKE_PIPE_RATE_CHARGE
    constexpr float pipe_vent_max = 0.5f;     // BRAKE_PIPE_RATE
    constexpr float service_pipe_rate = 0.25f; // BRAKE_PIPE_APPL_RATE
    constexpr float emergency_rate = -1.2f;   // BRAKE_PIPE_EMERGENCY
    constexpr float lap_leak = -0.005f;       // PIPE_DISCHARGE_SLOW
    auto& ur = handles_->equalising;
    const float p = inputs.train_pipe;
    const float mr = inputs.main_reservoir;
    const float time = static_cast<float>(dt);
    float rate = 0.0f;
    switch (brake) {
    case BrakeHandle::charge:
        ur += ur_charge_rate * time;
        ur = (std::min)(ur, mr);
        if (p < ur) rate = (ur - p) / pipe_charge_div;
        break;
    case BrakeHandle::running:
        if (ur < handles.charging_pressure) {
            ur += std::clamp((mr - ur) * 2.0f, 0.0f, ur_charge_rate) * time;
        } else if (!inputs.realistic_air && inputs.cylinder > 0.0f && ur - p < 0.1f) {
            // The DLL's own overcharge while the locomotive is still braked:
            // it pushes the release through the RTS triple valve.
            ur += 0.15f * time;
        }
        ur = (std::min)(ur, mr);
        // Stabiliser: overcharge removal (the DLL's 0.003 atm/s; the real
        // valve's 0.2 atm in 80-120 s with realistic air).
        if (ur > handles.charging_pressure && (inputs.realistic_air || ur > p)) {
            ur = (std::max)(ur - (inputs.realistic_air ? 0.002f : 0.003f) * time, handles.charging_pressure);
        } else if (!inputs.realistic_air && ur > p) {
            ur -= 0.003f * time;
        }
        if (p < ur - 0.01f) {
            rate = (ur - p) / pipe_charge_div;
        } else if (p > ur) {
            rate = (std::max)(ur - p, -pipe_vent_max);
        }
        break;
    case BrakeHandle::lap:
        ur = (std::min)(ur, mr);
        if (p > ur) rate = ur - p;
        rate = rate < -pipe_vent_max ? -pipe_vent_max : (std::min)(rate, lap_leak);
        break;
    case BrakeHandle::ep_service:
        // ЭР9Т: the pipe only vents down to the УР (no leak, no feed).
        ur = (std::min)(ur, mr);
        if (p > ur) rate = (std::max)(ur - p, -pipe_vent_max);
        break;
    case BrakeHandle::lap_feed:
        ur = (std::min)(ur, mr);
        if (p > ur) {
            rate = ur - p;
        } else if (ur - p > 0.1f) {
            rate = 0.05f;
        }
        rate = (std::max)(rate, -pipe_vent_max);
        break;
    case BrakeHandle::service:
        ur = std::clamp(ur - 0.2f * time, 0.0f, mr);
        rate = -service_pipe_rate;
        break;
    case BrakeHandle::emergency:
        ur = std::clamp(ur + emergency_rate * 1.2f * time, 0.0f, mr);
        rate = emergency_rate;
        break;
    }
    demands.train_pipe_rate = rate;
    demands.equalising = ur;
    demands.emergency = brake == BrakeHandle::emergency;
    // ЭПТ (ЭР9Т rates): EPT_APPLY_RATE 0.5, EPT_RELEASE_RATE 0.5 (II) and
    // EPT_RELEASE_RATE1 0.8 (I), at most 6 atm; none in VI (pneumatic).
    auto& target = handles_->ep_target;
    switch (brake) {
    case BrakeHandle::charge:
        target = (std::max)(target - 0.8f * time, 0.0f);
        break;
    case BrakeHandle::running:
        target = (std::max)(target - 0.5f * time, 0.0f);
        break;
    case BrakeHandle::ep_service:
    case BrakeHandle::service:
        if (handles.ep_brake) target = (std::min)(target + 0.5f * time, 6.0f);
        break;
    default:
        break;
    }
    if (handles.ep_brake && brake != BrakeHandle::emergency) {
        demands.ep_value = target;
        demands.systems = 1 | 2;
        return;
    }
    demands.systems = 1;
}

// ---------------------------------------------------------------------------
// Train

struct Train::Impl {
    std::optional<TrackTraveller> head; // front end of car 0
    std::optional<TrackTraveller> tail; // rear end of the last car
    std::vector<std::unique_ptr<LocomotiveController>> controllers;
    std::vector<EngineDemands> demands;
    // Per car and bogie: the node and offset along it at the last update.
    struct BogieTrack {
        std::uint32_t node = ~0u;
        double offset{};
    };
    std::vector<std::array<BogieTrack, 2>> last_bogie;
    // Per car: this step's rail joint impacts (RTS v118 / v119, 0..1).
    std::vector<std::array<float, 2>> impacts;
    std::vector<TrainEvent> events;
    bool stopped_at_end = false;
    // Junctions the tail passed facing, still held for their ClearanceDist.
    std::vector<std::uint32_t> clearing;
    bool moving_backward = false;
    detail::RealisticAir air;
};

Train::Train(std::vector<CarSetup> cars, const TrainSettings settings)
    : impl_(std::make_unique<Impl>()), settings_(settings), id_(next_train_id++) {
    settings_.coupler_substeps = std::clamp<std::uint32_t>(settings_.coupler_substeps, 1, 8);
    for (const auto& setup : cars) {
        CarState car{.definition = setup.definition, .flipped = setup.flipped, .handbrake = setup.handbrake};
        // Brakes start charged (RTS's initial state is not traced).
        car.pipe = 5.0f;
        car.auxiliary = 4.8f;
        car.control = 4.8f;
        car.charging_pipe = 0.0f;
        if (setup.definition && setup.definition->engine) {
            car.main_reservoir = 8.5f;
            car.sand_left = setup.definition->engine->max_sanding_time;
        }
        cars_.push_back(car);
    }
    if (!cars_.empty()) {
        cars_.front().pipe_ends |= pipe_hose_front | charging_hose_front | ep_cable_front | pipe_cock_front;
        cars_.back().pipe_ends |= pipe_hose_rear | charging_hose_rear | ep_cable_rear | pipe_cock_rear;
    }
    // Multiple-unit cables start connected where both touching ends have
    // one (SMECable / HVBus front, rear of the .eng; a flipped car's own
    // front faces the train rear). RTS's start state is not traced; the car
    // menu connects and disconnects them.
    const auto end_has = [](const CarState& c, const bool train_front, const bool hv) {
        if (!c.definition || !c.definition->engine) return false;
        const auto& e = *c.definition->engine;
        const bool own_front = train_front != c.flipped;
        return (hv ? e.hv_bus : e.sme_cable)[own_front ? 0 : 1] != 0;
    };
    for (std::size_t i = 0; i + 1 < cars_.size(); ++i) {
        auto& ahead = cars_[i];
        auto& behind = cars_[i + 1];
        for (const bool hv : {false, true}) {
            if (!end_has(ahead, false, hv) || !end_has(behind, true, hv)) continue;
            ahead.cables |= hv ? cable_hv_rear : cable_sme_rear;
            behind.cables |= hv ? cable_hv_front : cable_sme_front;
        }
    }
    // Centres laid back from the last car with every joint at gap 0.
    for (std::size_t i = cars_.size(); i-- > 1;) {
        auto& ahead = cars_[i - 1];
        const auto& behind = cars_[i];
        ahead.offset = behind.offset;
        ahead.offset = behind.offset - joint_gap(ahead, behind, 0.0, 0.0);
    }
    impl_->controllers.resize(cars_.size());
    impl_->demands.resize(cars_.size());
    impl_->last_bogie.resize(cars_.size());
    impl_->impacts.resize(cars_.size());
}

Train::~Train() = default;

double Train::length() const noexcept {
    if (cars_.empty()) return 0.0;
    return cars_.front().offset + length_of(cars_.front()) * 0.5 + length_of(cars_.back()) * 0.5;
}

std::optional<TrackTraveller> Train::front_end() const {
    return impl_->head;
}

std::optional<TrackTraveller> Train::rear_end() const {
    return impl_->tail;
}

std::optional<TrackTraveller> Train::trailing_end() const {
    if (impl_->moving_backward) return impl_->head;
    if (!impl_->tail) return std::nullopt;
    auto trailing = *impl_->tail;
    trailing.reverse();
    return trailing;
}

std::optional<TrackTraveller> Train::leading_end() const {
    if (!impl_->moving_backward) return impl_->head;
    if (!impl_->tail) return std::nullopt;
    auto lead = *impl_->tail;
    lead.reverse();
    return lead;
}

void Train::set_brake_model(const BrakeModel model) noexcept {
    if (settings_.brake_model != model && model == BrakeModel::realistic) impl_->air.reset(cars_);
    settings_.brake_model = model;
}

BrakeModel Train::brake_model() const noexcept {
    if (settings_.brake_model != BrakeModel::realistic) return BrakeModel::rts;
    for (const auto& c : impl_->controllers)
        if (c && c->rts_brakes()) return BrakeModel::rts;
    return BrakeModel::realistic;
}

void Train::set_coupler_substeps(const std::uint32_t substeps) noexcept {
    settings_.coupler_substeps = std::clamp<std::uint32_t>(substeps, 1, 8);
}

void Train::set_car_flags(const std::size_t car, const CarFlags which, const std::uint8_t mask, const bool on) {
    if (car >= cars_.size()) return;
    auto& state = cars_[car];
    std::uint8_t* bits = nullptr;
    switch (which) {
    case CarFlags::pipe_ends: bits = &state.pipe_ends; break;
    case CarFlags::coupler_open: bits = &state.coupler_open; break;
    case CarFlags::valves: bits = &state.valves; break;
    case CarFlags::chocks: bits = &state.chocks; break;
    case CarFlags::cables: bits = &state.cables; break;
    }
    if (!bits) return;
    *bits = on ? static_cast<std::uint8_t>(*bits | mask) : static_cast<std::uint8_t>(*bits & ~mask);
    asleep_ = false;
}

void Train::set_handbrake(const std::size_t car, const float value) {
    if (car >= cars_.size()) return;
    cars_[car].handbrake = std::clamp(value, 0.0f, 1.0f);
    asleep_ = false;
}

void Train::set_bogies(const std::size_t car, const float ahead, const float behind) {
    if (car >= cars_.size()) return;
    cars_[car].bogie_ahead = (std::max)(ahead, 0.0f);
    cars_[car].bogie_behind = (std::max)(behind, 0.0f);
}

void Train::set_controller(const std::size_t car, std::unique_ptr<LocomotiveController> controller) {
    if (car < impl_->controllers.size()) impl_->controllers[car] = std::move(controller);
}

LocomotiveController* Train::controller(const std::size_t car) {
    return car < impl_->controllers.size() ? impl_->controllers[car].get() : nullptr;
}

std::vector<TrainEvent> Train::take_events() {
    return std::exchange(impl_->events, {});
}

void Train::vacate() {
    if (!network_ || !impl_->tail) return;
    // Junctions under the train, from the tail to the front.
    TrackTraveller walker = *impl_->tail;
    const auto under = walker.move(length(), JunctionMode::plain);
    for (const auto& pass : under.junctions) network_->release_junction(pass.junction, id_);
    // Junctions behind it that the tail still keeps (clearance).
    for (std::uint32_t node = 0; node < network_->node_count(); ++node) {
        auto* state = network_->junction(node);
        if (state && state->clearance.contains(id_)) network_->release_junction(node, id_);
    }
}

bool Train::turn_around() {
    if (!network_ || !impl_->head || !impl_->tail || cars_.empty()) return false;
    for (const auto& car : cars_) {
        if (std::abs(car.velocity) > 0.1f) return false;
    }
    // The ends swap and face the other way.
    auto head = *impl_->tail;
    auto tail = *impl_->head;
    head.reverse();
    tail.reverse();
    impl_->head = head;
    impl_->tail = tail;
    reverse_cars();
    impl_->moving_backward = false;
    impl_->stopped_at_end = false;
    move_and_update(0.0, 0.0);
    return true;
}

// The car order and every car's orientation reverse; the ends of the
// travellers are the caller's business.
void Train::reverse_cars() {
    const auto swap_ends = [](const std::uint8_t bits, const std::uint8_t front, const std::uint8_t rear) {
        std::uint8_t result = bits & static_cast<std::uint8_t>(~(front | rear));
        if (bits & front) result |= rear;
        if (bits & rear) result |= front;
        return result;
    };
    // Offsets are measured forward from the last car: the old first car is
    // the new last one, and forward is the old backward.
    const double first = cars_.front().offset;
    for (auto& car : cars_) {
        car.offset = first - car.offset;
        car.flipped = !car.flipped;
        car.velocity = -car.velocity;
        car.acceleration = -car.acceleration;
        car.pitch = -car.pitch;
        car.coupler_force = 0.0f;
        car.travelled = -car.travelled;
        std::swap(car.bogie_ahead, car.bogie_behind);
        car.body[2] = -car.body[2];
        car.body_rate[2] = -car.body_rate[2];
        car.pipe_ends = swap_ends(car.pipe_ends, pipe_hose_front, pipe_hose_rear);
        car.pipe_ends = swap_ends(car.pipe_ends, charging_hose_front, charging_hose_rear);
        car.pipe_ends = swap_ends(car.pipe_ends, ep_cable_front, ep_cable_rear);
        car.pipe_ends = swap_ends(car.pipe_ends, pipe_cock_front, pipe_cock_rear);
        car.coupler_open = swap_ends(car.coupler_open, coupler_open_front, coupler_open_rear);
        car.coupler_open = swap_ends(car.coupler_open, coupler_broken_front, coupler_broken_rear);
        car.cables = swap_ends(car.cables, cable_sme_front, cable_sme_rear);
        car.cables = swap_ends(car.cables, cable_hv_front, cable_hv_rear);
        car.chocks = swap_ends(car.chocks, chock_front, chock_rear);
    }
    std::reverse(cars_.begin(), cars_.end());
    std::reverse(impl_->controllers.begin(), impl_->controllers.end());
    std::reverse(impl_->demands.begin(), impl_->demands.end());
    std::reverse(impl_->last_bogie.begin(), impl_->last_bogie.end());
    std::reverse(impl_->impacts.begin(), impl_->impacts.end());
    for (auto& bogies : impl_->last_bogie) std::swap(bogies[0], bogies[1]);
}

void Train::occupy_under() {
    if (!network_ || !impl_->tail) return;
    TrackTraveller walker = *impl_->tail;
    walker.move(length(), JunctionMode::occupy, id_);
}

bool Train::derailed() const noexcept {
    return std::any_of(cars_.begin(), cars_.end(), [](const CarState& car) { return car.derailed; });
}

double Train::mass() const noexcept {
    double total = 0.0;
    for (const auto& car : cars_) total += mass_of(car);
    return total;
}

void Train::hit(const std::size_t index, const float force, const float (&direction)[3], const float time) {
    if (index >= cars_.size() || force <= 0.0f) return;
    auto& car = cars_[index];
    const float m = mass_of(car);
    const float shove = force / m * time;
    if (!car.derailed) {
        if (settings_.derailment && force > car.definition->derail_buffer_force) {
            car.derailed = true;
            car.derail = DerailKind::sliding;
            car.slide_speed = car.velocity;
            car.slide = 0.0f;
            car.slide_origin_set = false;
            impl_->events.push_back({.kind = TrainEventKind::derailment, .car = static_cast<std::uint32_t>(index)});
            return;
        }
        for (int axis = 0; axis < 3; ++axis) {
            car.body[axis] = std::clamp(car.body[axis] + direction[axis] * shove, -0.3f, 0.3f);
        }
        return;
    }
    // Derailed and sliding: a hit along it folds it a little more.
    if (car.derail == DerailKind::sliding && std::abs(direction[2]) > 0.7f) {
        const float side = car.yaw_target < 0.0f ? -1.0f : 1.0f;
        car.yaw_target = side * (std::min)(std::abs(car.yaw_target) + shove * 0.02f, 0.8f);
    }
    // Derailed: a side hit rolls it (the 0x589A14 branch for a hit near the
    // middle of the car).
    if (std::abs(direction[0]) > 0.3f) {
        const float side = direction[0] > 0.0f ? 1.0f : -1.0f;
        const float target = (std::min)(shove * 0.04f + 0.1f, 1.8f);
        if (target > std::abs(car.tilt_target)) {
            car.tilt_target = target * side;
            car.tilt_rate = std::abs(shove / (std::max)(car.definition->width, 1.0f)) * side;
            car.tilt_friction = m * 10.0f * (target < 1.57f ? target * 0.637f : 1.0f);
        }
    }
}

void Train::stop_slide(const std::size_t index) {
    if (index < cars_.size()) cars_[index].slide_speed = 0.0f;
}

void Train::add_velocity(const float change) {
    for (auto& car : cars_) car.velocity += change;
}

void Train::shift(const double metres) {
    if (!network_ || !impl_->head || metres == 0.0) return;
    // The cars roll the way they are moved: their wheels turn.
    for (auto& car : cars_) car.travelled += metres;
    move_and_update(metres, metres);
}

std::unique_ptr<Train> Train::uncouple(const std::size_t index, const bool close_cocks, const bool keep_open) {
    if (!network_ || !impl_->tail || index == 0 || index >= cars_.size()) return nullptr;
    vacate();
    impl_->clearing.clear();
    auto rear = std::make_unique<Train>(std::vector<CarSetup>{}, settings_);
    // The new ends, measured from the tail (offsets count from the last car).
    const double last_half = length_of(cars_.back()) * 0.5;
    TrackTraveller rear_head = *impl_->tail;
    rear_head.move(cars_[index].offset + length_of(cars_[index]) * 0.5 + last_half);
    TrackTraveller front_tail = *impl_->tail;
    front_tail.move(cars_[index - 1].offset - length_of(cars_[index - 1]) * 0.5 + last_half);
    rear->network_ = network_;
    rear->impl_->head = rear_head;
    rear->impl_->tail = *impl_->tail;
    rear->impl_->moving_backward = impl_->moving_backward;
    impl_->tail = front_tail;

    const auto split = static_cast<std::ptrdiff_t>(index);
    const auto move_tail = [split](auto& from, auto& to) {
        to.assign(std::make_move_iterator(from.begin() + split), std::make_move_iterator(from.end()));
        from.erase(from.begin() + split, from.end());
    };
    move_tail(cars_, rear->cars_);
    move_tail(impl_->controllers, rear->impl_->controllers);
    move_tail(impl_->demands, rear->impl_->demands);
    move_tail(impl_->last_bogie, rear->impl_->last_bogie);
    move_tail(impl_->impacts, rear->impl_->impacts);
    // This train's new last car is the reference.
    const double base = cars_.back().offset;
    for (auto& car : cars_) car.offset -= base;

    auto& behind = cars_.back();
    auto& ahead = rear->cars_.front();
    behind.pipe_ends |= pipe_hose_rear | charging_hose_rear | ep_cable_rear;
    ahead.pipe_ends |= pipe_hose_front | charging_hose_front | ep_cable_front;
    // RTS 0x469858: the control cable and the HV bus part at the cut too.
    behind.cables &= static_cast<std::uint8_t>(~(cable_sme_rear | cable_hv_rear));
    ahead.cables &= static_cast<std::uint8_t>(~(cable_sme_front | cable_hv_front));
    if (close_cocks) {
        behind.pipe_ends |= pipe_cock_rear;
        ahead.pipe_ends |= pipe_cock_front;
    }
    if (keep_open) {
        behind.coupler_open |= coupler_open_rear;
        ahead.coupler_open |= coupler_open_front;
    }
    for (auto* train : {this, rear.get()}) {
        for (auto& car : train->cars_) car.coupler_force = 0.0f;
        train->occupy_under();
        train->move_and_update(0.0, 0.0);
    }
    return rear;
}

bool Train::couple(Train& other, const bool at_front, const bool other_front, const double gap) {
    if (&other == this || !network_ || !other.network_ || !impl_->tail || !other.impl_->tail || other.cars_.empty()) {
        return false;
    }
    vacate();
    other.vacate();
    // The other train must touch with its rear when it goes in front, with
    // its front when it goes behind.
    if (other_front == at_front) {
        auto head = *other.impl_->tail;
        auto tail = *other.impl_->head;
        head.reverse();
        tail.reverse();
        other.impl_->head = head;
        other.impl_->tail = tail;
        other.reverse_cars();
    }
    auto& joint_behind = at_front ? other.cars_.back() : cars_.back();
    auto& joint_ahead = at_front ? cars_.front() : other.cars_.front();
    joint_behind.pipe_ends &= static_cast<std::uint8_t>(~(pipe_hose_rear | charging_hose_rear | ep_cable_rear | pipe_cock_rear));
    joint_ahead.pipe_ends &= static_cast<std::uint8_t>(~(pipe_hose_front | charging_hose_front | ep_cable_front | pipe_cock_front));
    joint_behind.coupler_open &= static_cast<std::uint8_t>(~coupler_open_rear);
    joint_ahead.coupler_open &= static_cast<std::uint8_t>(~coupler_open_front);

    const auto join = [](auto& first, auto& second) {
        // first = first + second
        first.insert(first.end(), std::make_move_iterator(second.begin()), std::make_move_iterator(second.end()));
        second.clear();
    };
    if (at_front) {
        // Other ahead: its last car's centre is this front car's front end,
        // the gap and its half length ahead of this train's front car centre.
        const double distance = cars_.front().offset + length_of(cars_.front()) * 0.5 + gap + length_of(other.cars_.back()) * 0.5;
        for (auto& car : other.cars_) car.offset += distance;
        join(other.cars_, cars_);
        join(other.impl_->controllers, impl_->controllers);
        join(other.impl_->demands, impl_->demands);
        join(other.impl_->last_bogie, impl_->last_bogie);
        join(other.impl_->impacts, impl_->impacts);
        std::swap(cars_, other.cars_);
        std::swap(impl_->controllers, other.impl_->controllers);
        std::swap(impl_->demands, other.impl_->demands);
        std::swap(impl_->last_bogie, other.impl_->last_bogie);
        std::swap(impl_->impacts, other.impl_->impacts);
        impl_->head = other.impl_->head;
    } else {
        // Other behind: its last car becomes the reference.
        const double distance = other.cars_.front().offset + length_of(other.cars_.front()) * 0.5 + gap + length_of(cars_.back()) * 0.5;
        for (auto& car : cars_) car.offset += distance;
        join(cars_, other.cars_);
        join(impl_->controllers, other.impl_->controllers);
        join(impl_->demands, other.impl_->demands);
        join(impl_->last_bogie, other.impl_->last_bogie);
        join(impl_->impacts, other.impl_->impacts);
        impl_->tail = other.impl_->tail;
    }
    other.network_ = nullptr;
    other.impl_->head.reset();
    other.impl_->tail.reset();
    impl_->clearing.clear();
    impl_->stopped_at_end = false;
    occupy_under();
    move_and_update(0.0, 0.0);
    return true;
}

bool Train::place(TrackNetwork& network, const std::uint32_t node, const double offset, const bool forward,
                  std::string* error) {
    vacate();
    if (cars_.empty()) {
        if (error) *error = "empty train";
        return false;
    }
    TrackTraveller head(network, node, offset, forward);
    TrackTraveller tail = head;
    const auto laid = tail.move(-length());
    if (!laid.ok()) {
        if (error) *error = "not enough track behind the front for the train";
        return false;
    }
    network_ = &network;
    impl_->head = head;
    impl_->tail = tail;
    // Mark the junctions under the train as occupied by walking it once.
    TrackTraveller walker = tail;
    walker.move(length(), JunctionMode::occupy, id_);
    impl_->stopped_at_end = false;
    for (auto& car : cars_) {
        car.derailed = false;
        car.derail = DerailKind::none;
        car.tilt = car.tilt_target = car.tilt_rate = car.tilt_friction = 0.0f;
        car.fallen = false;
        car.slide = car.slide_speed = 0.0f;
        car.slide_origin_set = false;
        car.yaw = car.yaw_target = 0.0f;
        car.velocity = 0.0f;
        car.acceleration = 0.0f;
    }
    for (std::size_t i = 0; i < cars_.size(); ++i) {
        impl_->last_bogie[i] = {};
    }
    // Pitch, radius and bogie positions for the first step.
    move_and_update(0.0, 0.0);
    return true;
}

std::vector<CarPlacement> Train::placements() const {
    std::vector<CarPlacement> result;
    if (!impl_->tail || cars_.empty()) return result;
    const auto& last = cars_.back();
    result.resize(cars_.size());
    for (std::size_t i = 0; i < cars_.size(); ++i) {
        const auto& car = cars_[i];
        // Bogies at 35 % of the length from the centre (RTS takes the bogie
        // positions from the shape; the renderer refines the look).
        const double ahead = car.bogie_ahead > 0.0f ? car.bogie_ahead : length_of(car) * 0.35;
        const double behind = car.bogie_behind > 0.0f ? car.bogie_behind : length_of(car) * 0.35;
        const double centre = length_of(last) * 0.5 + car.offset;
        TrackTraveller rear = *impl_->tail;
        rear.move(centre - behind);
        TrackTraveller front = rear;
        front.move(ahead + behind);
        result[i] = {front.pose(), rear.pose()};
    }
    return result;
}

std::vector<TrackTraveller> Train::car_centres() const {
    std::vector<TrackTraveller> result;
    if (!impl_->tail || cars_.empty()) return result;
    const auto& last = cars_.back();
    result.reserve(cars_.size());
    for (const auto& car : cars_) {
        TrackTraveller centre = *impl_->tail;
        centre.move(length_of(last) * 0.5 + car.offset);
        result.push_back(centre);
    }
    return result;
}

void Train::set_line_supply(const std::size_t car, const float voltage, const float frequency) {
    if (car >= cars_.size()) return;
    cars_[car].line_voltage = voltage;
    cars_[car].line_frequency = frequency;
}

void Train::set_line_load(const std::size_t car, const float current, const float heat, const bool burnt) {
    if (car >= cars_.size()) return;
    cars_[car].line_current = current;
    cars_[car].line_heat = heat;
    cars_[car].line_burnt = burnt;
}

void Train::set_asleep(const bool asleep) noexcept {
    asleep_ = asleep;
    if (!asleep) return;
    for (auto& car : cars_) {
        car.velocity = 0.0f;
        car.acceleration = 0.0f;
    }
}

void Train::step() {
    if (!network_ || cars_.empty() || asleep_) return;
    const auto n = cars_.size();
    const auto dt = static_cast<float>(train_step);
    auto& impl = *impl_;

    const bool realistic_air = brake_model() == BrakeModel::realistic;
    // 1. Locomotive controllers and their engine part (0x5031D8).
    for (std::size_t i = 0; i < n; ++i) {
        auto& car = cars_[i];
        car.force = 0.0f;
        car.ep = -1.0f;
        auto& demands = impl.demands[i];
        demands = {};
        if (!car.definition->engine || !impl.controllers[i]) continue;
        const float direction = car.flipped ? -1.0f : 1.0f;
        const EngineInputs inputs{
            .speed = car.velocity * direction,
            .train_pipe = car.pipe,
            .main_reservoir = car.main_reservoir,
            .cylinder = car.cylinder,
            .independent = car.independent,
            .flipped = car.flipped,
            .definition = car.definition,
            .realistic_air = realistic_air,
            .line_voltage = car.line_voltage,
            .line_frequency = car.line_frequency,
        };
        impl.controllers[i]->step(dt, inputs, demands);
        const auto& brakes = car.definition->brakes;
        // Independent brake follows its target.
        if (demands.release_independent_brake) {
            car.independent = (std::max)(car.independent - brakes.max_release_rate * dt, 0.0f);
        } else if (demands.independent_brake >= 0.0f) {
            if (demands.independent_brake < car.independent) {
                car.independent = (std::max)(car.independent - brakes.max_release_rate * dt, demands.independent_brake);
            } else {
                car.independent = (std::min)(car.independent + brakes.max_application_rate * dt, demands.independent_brake);
            }
        }
        car.main_reservoir = (std::max)(car.main_reservoir + demands.main_reservoir_rate * dt, 0.0f);
        // Traction with the adhesion limit (RTS: m * 10 * mu, Adheasion's
        // second value). RTS cuts a slipping force to a random 10..40 %;
        // here it is 25 % so runs are deterministic.
        float force = demands.force;
        float mu = car.definition->adhesion[1];
        // Sanding (RTS 0x424488): the sand runs out over MaxSandingTime; the
        // sanded rail lasts the engine's length of travel after it stops.
        // Adhesion x the .eng's sanding factor (Adheasion's third value).
        if (demands.sanding && car.sand_left != 0.0f) {
            if (car.sand_left > 0.0f) car.sand_left = (std::max)(car.sand_left - dt, 0.0f);
            car.sanding = true;
            car.sand_travel = 0.0f;
        } else {
            car.sanding = false;
            if (car.sand_travel >= 0.0f) {
                car.sand_travel += std::abs(car.velocity) * dt;
                if (car.sand_travel > car.definition->length) car.sand_travel = -1.0f;
            }
        }
        if ((car.sanding || car.sand_travel >= 0.0f) && car.definition->adhesion[2] > 0.0f) mu *= car.definition->adhesion[2];
        const float limit = mass_of(car) * gravity * mu;
        car.wheelslip = mu > 0.0f && std::abs(force) > limit;
        if (car.wheelslip) force *= 0.25f;
        car.force = force * direction;
        car.power = demands.power >= 0.0f ? demands.power : std::abs(force * car.velocity);
        car.handbrake = demands.sets_handbrake ? std::clamp(0.01f * demands.handbrake, 0.0f, 1.0f)
                                               : (std::max)(car.handbrake, 0.01f * demands.handbrake);
    }

    // EPT: an engine's EPTvalue spreads to the neighbouring cars with EP
    // brakes, as far as the EP line runs (RTS: every other car gets -1).
    for (std::size_t i = 0; i < n; ++i) {
        const auto& demands = impl.demands[i];
        if (!(demands.systems & 2) || demands.ep_value < 0.0f || !(cars_[i].definition->brakes.system & 2)) continue;
        cars_[i].ep = demands.ep_value;
        // As far as the EP line runs: EP cars with the cables connected.
        const auto ep_joined = [&](const std::size_t k) {
            return !(cars_[k].pipe_ends & ep_cable_rear) && !(cars_[k + 1].pipe_ends & ep_cable_front);
        };
        for (std::size_t k = i + 1; k < n && (cars_[k].definition->brakes.system & 2) && ep_joined(k - 1); ++k) cars_[k].ep = demands.ep_value;
        for (std::size_t k = i; k-- > 0 && (cars_[k].definition->brakes.system & 2) && ep_joined(k);) cars_[k].ep = demands.ep_value;
    }

    // 2. Brake force from the cylinders (before this step's air update, as RTS).
    for (auto& car : cars_) {
        const auto& brakes = car.definition->brakes;
        const float cylinder = (std::max)(car.cylinder, car.independent);
        car.brake_force = brakes.cylinder_pressure_for_max_force > 0.0f
            ? cylinder / brakes.cylinder_pressure_for_max_force * brakes.max_brake_force
            : 0.0f;
        car.brake_force += car.handbrake * brakes.max_handbrake_force;
        // Brake shoes under its bogies (RTS: 10 kN each).
        if (car.chocks & chock_front) car.brake_force += chock_force;
        if (car.chocks & chock_rear) car.brake_force += chock_force;
    }
    for (std::size_t i = 0; i < n; ++i) cars_[i].brake_force += impl.demands[i].brake_force;

    // 3. Air: realistic pneumatics (openRTS), or RTS's pipe and triple valves.
    if (realistic_air) {
        detail::AirInputs air{};
        air.demands = &impl.demands;
        std::vector<bool> has(n);
        for (std::size_t i = 0; i < n; ++i) has[i] = impl.controllers[i] != nullptr;
        air.has_controller = &has;
        air.joined.resize(n > 0 ? n - 1 : 0);
        for (std::size_t i = 0; i + 1 < n; ++i) {
            air.joined[i] = !(cars_[i].pipe_ends & (pipe_hose_rear | pipe_cock_rear))
                && !(cars_[i + 1].pipe_ends & (pipe_hose_front | pipe_cock_front));
        }
        impl.air.step(cars_, air, dt);
        // The valves a hand opened, as in the RTS model.
        for (auto& car : cars_) {
            if (car.valves & valve_emergency) car.pipe = (std::max)(car.pipe - 18.0f * dt, 0.0f);
            if (car.valves & valve_release) car.cylinder = (std::max)(car.cylinder - 0.5f * dt, 0.0f);
        }
    } else {
        // 3. Air: pipe from the locomotives, equalisation, triple valves.
        std::vector<float> pipe_start(n);
        for (std::size_t i = 0; i < n; ++i) pipe_start[i] = cars_[i].pipe;
        const auto pipe_volume = [&](const std::size_t i) { return length_of(cars_[i]) * pipe_area; };
        // Air flows between car i and i + 1 only through connected hoses and
        // open end cocks; a pipe end with an open cock and nothing coupled vents
        // at 10 atm/s (RTS; the hiss is sound variable 80).
        const auto joined = [&](const std::size_t i) {
            return !(cars_[i].pipe_ends & (pipe_hose_rear | pipe_cock_rear))
                && !(cars_[i + 1].pipe_ends & (pipe_hose_front | pipe_cock_front));
        };
        for (auto& car : cars_) {
            const bool front_open = (car.pipe_ends & pipe_hose_front) && !(car.pipe_ends & pipe_cock_front);
            const bool rear_open = (car.pipe_ends & pipe_hose_rear) && !(car.pipe_ends & pipe_cock_rear);
            if (front_open || rear_open) car.pipe = (std::max)(car.pipe - 10.0f * dt, 0.0f);
            if (car.valves & valve_emergency) car.pipe = (std::max)(car.pipe - 18.0f * dt, 0.0f);
            if (car.valves & valve_release) car.cylinder = (std::max)(car.cylinder - 0.5f * dt, 0.0f);
        }
        for (std::size_t j = 0; j < n; ++j) {
            const auto& demands = impl.demands[j];
            auto& loco = cars_[j];
            if (!loco.definition->engine || !impl.controllers[j] || !(demands.systems & 1)) continue;
            // Only engines that ask for it touch the pipe (the master's valve).
            const float rate = demands.train_pipe_rate;
            if (rate == 0.0f) continue;
            const float reservoir_volume = loco.definition->engine->main_reservoir_volume;
            if (rate < 0.0f) {
                // Vent at the locomotive, the rest follows (lowering only).
                const float vented = (std::max)(loco.pipe + rate, 0.0f);
                loco.pipe += (vented - loco.pipe) * dt;
                float distance = 0.0f;
                for (std::size_t m = j + 1; m < n && joined(m - 1); ++m) {
                    distance += length_of(cars_[m - 1]);
                    const float factor = (std::min)(1.0f, 30.0f / distance) * dt;
                    cars_[m].pipe += (std::min)((vented - cars_[m].pipe) * factor, 0.0f);
                }
                distance = 0.0f;
                for (std::size_t m = j; m-- > 0 && joined(m);) {
                    distance += length_of(cars_[m + 1]);
                    const float factor = (std::min)(1.0f, 30.0f / distance) * dt;
                    cars_[m].pipe += (std::min)((vented - cars_[m].pipe) * factor, 0.0f);
                }
                continue;
            }
            if (reservoir_volume <= 0.0f) continue;
            // Fed from the main reservoir: volume-weighted equalisation, slower
            // with the distance from the locomotive.
            const auto feed = [&](CarState& car, const float distance, const float volume) {
                const float factor = (std::min)(1.0f, (std::min)(1.0f, 30.0f / (std::max)(distance, 1.0f)) * dt * rate);
                const float average = (loco.main_reservoir * reservoir_volume + car.pipe * volume) / (reservoir_volume + volume);
                const float change = (average - car.pipe) * factor;
                car.pipe += change;
                loco.main_reservoir = (std::max)(loco.main_reservoir - change * volume / reservoir_volume, 0.0f);
            };
            feed(loco, 1.0f, pipe_volume(j));
            float distance = 0.0f;
            for (std::size_t m = j + 1; m < n && joined(m - 1); ++m) {
                distance += length_of(cars_[m - 1]);
                feed(cars_[m], distance, pipe_volume(m - 1));
            }
            distance = 0.0f;
            for (std::size_t m = j; m-- > 0 && joined(m);) {
                distance += length_of(cars_[m + 1]);
                feed(cars_[m], distance, pipe_volume(m));
            }
        }
        // Every car equalises with each car ahead of it, slower with distance.
        for (std::size_t i = 1; i < n; ++i) {
            auto& car = cars_[i];
            const float volume = pipe_volume(i);
            for (std::size_t k = i; k-- > 0 && joined(k);) {
                auto& ahead = cars_[k];
                if (ahead.pipe == car.pipe) continue;
                const float ahead_volume = pipe_volume(k);
                const double distance = ahead.offset - car.offset;
                const float factor = (distance == 0.0 ? 1.0f : (std::min)(1.0f, static_cast<float>(30.0 / std::abs(distance)))) * dt;
                const float average = (ahead.pipe * ahead_volume + car.pipe * volume) / (ahead_volume + volume);
                car.pipe += (average - car.pipe) * factor;
                ahead.pipe += (average - ahead.pipe) * factor;
            }
        }
        for (std::size_t i = 0; i < n; ++i) {
            auto& car = cars_[i];
            const auto& brakes = car.definition->brakes;
            float pipe_change = car.pipe - pipe_start[i];
            const float ratio = (std::max)(brakes.triple_valve_ratio, 0.01f);
            if (car.definition->engine && impl.controllers[i] && impl.demands[i].release_train_brake) {
                car.cylinder = (std::max)(car.cylinder - brakes.max_release_rate * dt, 0.0f);
            }
            // EP brake.
            if ((brakes.system & 2) && car.ep >= 0.0f) {
                if (car.cylinder < car.ep) {
                    float equalised = (car.auxiliary - car.cylinder) / (ratio + 0.625f);
                    equalised = (std::max)(equalised, 0.0f);
                    float rise = (std::min)(car.ep - car.cylinder, brakes.max_application_rate * dt);
                    rise = (std::min)(rise, equalised * ratio);
                    car.cylinder += rise;
                    car.auxiliary = (std::max)(car.auxiliary - rise / ratio * 0.625f, 0.0f);
                } else if (car.cylinder > car.ep) {
                    car.cylinder = (std::max)(car.cylinder - brakes.max_release_rate * dt, car.ep);
                }
                pipe_change = 0.0f;
            }
            if (!(brakes.system & 5)) continue;
            // Control chamber.
            if (brakes.system & 4) {
                if (car.charging_pipe > car.auxiliary) {
                    float charge = (car.charging_pipe - car.auxiliary) * 0.05f;
                    if (charge / dt > brakes.max_auxiliary_charging_rate) charge = brakes.max_auxiliary_charging_rate * dt;
                    car.auxiliary += charge;
                    car.charging_pipe = (std::max)(car.charging_pipe - 10.0f * charge, 0.0f);
                }
                if (car.cylinder == 0.0f && pipe_change >= 0.0f) {
                    const float target = car.pipe - 0.2f;
                    if (car.control < target) {
                        car.control = (std::min)(car.control + car.pipe * 0.04f * dt, target);
                    } else if (car.control > target) {
                        car.control = (std::max)(car.control - car.pipe * 0.04f * dt, target);
                    }
                }
            } else {
                car.control = car.auxiliary;
            }
            if (pipe_change >= 0.0f) {
                // Charge the auxiliary reservoir from the pipe.
                if (car.auxiliary + 0.2f < car.pipe) {
                    float charge = (car.pipe - car.auxiliary - 0.2f) * 0.05f;
                    if (charge / dt > brakes.max_auxiliary_charging_rate) charge = brakes.max_auxiliary_charging_rate * dt;
                    if ((brakes.system & 4) && car.charging_pipe > car.pipe) charge = 0.0f;
                    car.auxiliary += charge;
                    car.pipe = (std::max)(car.pipe - 10.0f * charge, 0.0f);
                    pipe_change -= 10.0f * charge;
                }
                // Graduated release with the pipe rise.
                if (pipe_change > 0.0f) car.cylinder = (std::max)(car.cylinder - pipe_change * ratio, 0.0f);
            } else if (car.control > car.pipe) {
                // Application in proportion to the pipe drop, up to equalisation
                // with the auxiliary reservoir.
                float rise = -pipe_change * ratio;
                float drop = pipe_change * 0.625f;
                if (car.cylinder + rise > car.auxiliary + drop) {
                    drop = (std::min)(-(car.auxiliary - car.cylinder) / (ratio + 0.625f), 0.0f);
                    rise = -drop * ratio;
                    drop *= 0.625f;
                }
                car.cylinder += rise;
                car.auxiliary = (std::max)(car.auxiliary + drop, 0.0f);
                car.cylinder = (std::min)(car.cylinder, car.auxiliary);
            }
        }

    }

    // 4. Resistance, grade, curve.
    for (auto& car : cars_) {
        const auto& wagon = *car.definition;
        const float speed = std::abs(car.velocity);
        car.resistance = rolling_resistance(wagon, speed);
        if (wagon.engine && wagon.engine->max_velocity > 0.0f && speed > wagon.engine->max_velocity) {
            const float limit = wagon.engine->max_velocity;
            if (limit * 1.2f >= speed) {
                car.resistance *= (speed - limit) * 9.0f / (limit * 0.2f) + 1.0f;
            } else {
                car.resistance *= static_cast<float>(std::pow(speed - limit * 1.2f, 2.0) * 0.2 + 10.0);
            }
        }
        car.grade = car.pitch == 0.0f ? 0.0f : static_cast<float>(std::sin(car.pitch)) * -mass_of(car) * gravity;
        if (car.radius != 0.0f) {
            car.resistance += 7.0f / car.radius * mass_of(car);
            if (settings_.derailment && !car.derailed
                && mass_of(car) * car.velocity * car.velocity / car.radius > wagon.derail_rail_force) {
                start_tipping(static_cast<std::size_t>(&car - cars_.data()), nullptr);
            }
        }
    }

    // 5. Active and dissipative forces; the dissipative force first eats the
    // active force (0x424488).
    std::vector<double> dissipative(n), active(n), velocity(n), gaps(n), scratch(n), moved(n);
    bool moving = false;
    for (std::size_t i = 0; i < n; ++i) {
        auto& car = cars_[i];
        float force = car.force - car.grade;
        float friction = car.brake_force + car.resistance;
        if (car.derail == DerailKind::tipping && car.tilt_target != 0.0f) {
            friction += std::abs(car.tilt / car.tilt_target) * car.tilt_friction;
        } else if (car.derail == DerailKind::sliding) {
            friction += mass_of(car) * 5.0f; // RTS: the slid body's friction, mass * 5
        }
        const float magnitude = std::abs(force);
        if (magnitude <= friction) {
            friction -= magnitude;
            force = 0.0f;
        } else {
            force -= car.velocity <= 0.0f ? -friction : friction;
            friction = 0.0f;
        }
        car.acceleration = 0.0f;
        dissipative[i] = friction;
        active[i] = force;
        if (std::abs(car.velocity) < atm_epsilon) car.velocity = 0.0f;
        if (i + 1 < n) {
            gaps[i] = joint_gap(car, cars_[i + 1], 0.0, 0.0);
            if (std::abs(gaps[i]) < 1e-6) gaps[i] = 0.0;
        }
        if (active[i] != 0.0 || gaps[i] != 0.0 || car.velocity != 0.0f) moving = true;
    }
    if (!moving) {
        move_and_update(0.0, 0.0);
        derailment();
        return;
    }

    // 6. Coupler solver (0x505414): springs, integration, static friction,
    // velocity equalisation; `coupler_substeps` passes per step.
    const auto substeps = settings_.coupler_substeps;
    const double h = train_step / substeps;
    const std::vector<double> dissipative_frame = dissipative;
    const std::vector<double> active_frame = active;
    for (std::size_t i = 0; i < n; ++i) {
        velocity[i] = cars_[i].velocity;
        cars_[i].coupler_force = 0.0f;
    }
    for (std::uint32_t pass = 0; pass < substeps; ++pass) {
        for (std::size_t j = 0; j < n; ++j) {
            dissipative[j] = dissipative_frame[j];
            active[j] = active_frame[j];
        }
        for (std::size_t j = 0; j + 1 < n; ++j) {
            const auto& ahead = cars_[j];
            const auto& behind = cars_[j + 1];
            const double gap = joint_gap(ahead, behind, moved[j], moved[j + 1]);
            gaps[j] = gap;
            const auto& a = coupling_behind(ahead);
            const auto& b = coupling_ahead(behind);
            const double force = gap < 0.0 ? (a.stiffness_compression + b.stiffness_compression) * gap
                                           : (a.stiffness_tension + b.stiffness_tension) * gap;
            active[j] -= force * 0.5;
            active[j + 1] += force * 0.5;
            if (pass == 0) cars_[j].coupler_force = static_cast<float>(force);
        }
        for (std::size_t k = 0; k < n; ++k) {
            const double mass = mass_of(cars_[k]);
            velocity[k] += active[k] / mass * h;
            const double slowdown = dissipative[k] / mass * h;
            if (std::abs(velocity[k]) - slowdown > 0.0) {
                velocity[k] += velocity[k] <= 0.0 ? slowdown : -slowdown;
                dissipative[k] = 0.0;
            } else {
                dissipative[k] -= std::abs(velocity[k]) / h * mass;
                velocity[k] = 0.0;
            }
            active[k] = 0.0;
        }
        for (std::size_t m = 0; m + 1 < n; ++m) {
            if (velocity[m] != velocity[m + 1]) equalise(cars_, m, m + 1, velocity, scratch, dissipative, moved, h);
        }
        for (std::size_t k = 0; k < n; ++k) moved[k] += velocity[k] * h;
    }
    // Average velocity over the step; acceleration from it.
    for (std::size_t i = 0; i < n; ++i) {
        auto& car = cars_[i];
        const double average = moved[i] / train_step;
        car.acceleration = static_cast<float>((average - car.velocity) / train_step);
        car.velocity = static_cast<float>(average);
        if (std::abs(car.velocity) < 0.002f * dt) car.velocity = 0.0f;
        if (std::abs(car.acceleration) < 0.002f) car.acceleration = 0.0f;
    }
    // Offsets advance, rebased so the last car stays at 0; nearly closed
    // joints snap to exactly 0.
    const double last_moved = cars_.back().velocity * train_step;
    for (std::size_t i = n; i-- > 0;) {
        auto& car = cars_[i];
        car.offset += car.velocity * train_step - last_moved;
        car.travelled += car.velocity * train_step;
        if (i + 1 < n) {
            const double gap = joint_gap(car, cars_[i + 1], 0.0, 0.0);
            if (std::abs(gap) < 0.001 * train_step) {
                for (std::size_t k = 0; k <= i; ++k) cars_[k].offset -= gap;
            }
        }
    }
    // Coupler break: the first sub-step force against the weaker end.
    if (settings_.coupler_break) {
        for (std::size_t i = 0; i + 1 < n; ++i) {
            const auto& a = coupling_behind(cars_[i]);
            const auto& b = coupling_ahead(cars_[i + 1]);
            const float force = cars_[i].coupler_force;
            const float tension = (std::min)(a.break_tension, b.break_tension);
            float limit = (std::min)(a.break_compression, b.break_compression);
            if (a.rigid || b.rigid || (force > 0.0f) == (cars_[i + 1].velocity >= 0.0f)) limit = tension;
            if (std::abs(force) > limit) {
                impl.events.push_back({.kind = TrainEventKind::coupler_break, .car = static_cast<std::uint32_t>(i)});
            }
        }
    }
    move_and_update(cars_.front().velocity * train_step, last_moved);
    sway();
    derailment();
}

// A car starts overturning (RTS 0x580B30, its "not sliding" branch): the
// roll aims at 0.1 + 0.04 |v| rad (a tipped neighbour: 0.75 of its roll,
// at 0.8 of its rate), at most 1.8, and grows at |v| / 33 rad/s; the drag
// at the full roll is m * 10, times 0.637 * target below 1.57 rad. It rolls
// outwards of the curve, or the way the neighbour rolls.
void Train::start_tipping(const std::size_t index, const CarState* neighbour) {
    auto& car = cars_[index];
    if (car.derailed) return;
    car.derailed = true;
    car.derail = DerailKind::tipping;
    const float speed = std::abs(neighbour ? neighbour->velocity : car.velocity);
    float target = neighbour ? std::abs(neighbour->tilt_target) * 0.75f : speed * 0.04f + 0.1f;
    target = (std::min)(target, 1.8f);
    car.tilt_friction = mass_of(car) * 10.0f;
    if (target < 1.57f) car.tilt_friction *= target * 0.637f;
    float rate = neighbour ? std::abs(neighbour->tilt_rate) * 0.8f : std::abs(car.velocity) * (1.0f / 33.0f);
    // Outwards: against the side the car turns to.
    const float side = neighbour ? (neighbour->tilt_target < 0.0f ? -1.0f : 1.0f) : (car.curvature > 0.0f ? -1.0f : 1.0f);
    car.tilt_target = target * side;
    car.tilt_rate = rate * side;
    impl_->events.push_back({.kind = TrainEventKind::derailment, .car = static_cast<std::uint32_t>(index)});
}

// Per step after the move (RTS 0x581BC4 and 0x581694):
// - a tipping car rolls on, by rate * dt * (|target - roll| / |target| + 0.2),
//   until it reaches the target;
// - past 0.8 rad it has fallen: its hoses are torn off (both ends, and the
//   neighbours' ends facing it), so the pipe vents and the train brakes;
// - it pulls its neighbours with 0.637 * |roll| * m: above a neighbour's
//   DerailBufferForce the neighbour overturns too; past 0.9 rad the coupler
//   breaks;
// - a sliding car slides on along its heading, slowing by its friction.
void Train::derailment() {
    const auto dt = static_cast<float>(train_step);
    const auto n = cars_.size();
    for (std::size_t i = 0; i < n; ++i) {
        auto& car = cars_[i];
        if (car.derail != DerailKind::none && car.tilt_target != 0.0f && car.tilt != car.tilt_target) {
            const float share = std::abs(car.tilt_target - car.tilt) / std::abs(car.tilt_target) + 0.2f;
            car.tilt += car.tilt_rate * dt * share;
            if (std::abs(car.tilt) >= std::abs(car.tilt_target)) {
                car.tilt = car.tilt_target;
                car.tilt_rate = 0.0f;
            }
        }
        if (car.derail == DerailKind::sliding && car.yaw != car.yaw_target) {
            const float rate = (std::max)(0.3f, std::abs(car.slide_speed) * 0.2f) * dt;
            const float diff = car.yaw_target - car.yaw;
            car.yaw = std::abs(diff) <= rate ? car.yaw_target : car.yaw + std::copysign(rate, diff);
        }
        if (car.derail == DerailKind::sliding && car.slide_speed != 0.0f) {
            car.slide += car.slide_speed * dt;
            const float slowed = std::abs(car.slide_speed) - 5.0f * dt;
            car.slide_speed = slowed > 0.0f ? std::copysign(slowed, car.slide_speed) : 0.0f;
        }
        if (car.derail != DerailKind::none && !car.fallen && std::abs(car.tilt) > 0.8f) {
            car.fallen = true;
            car.pipe_ends |= pipe_hose_front | pipe_hose_rear;
            car.pipe_ends &= static_cast<std::uint8_t>(~(pipe_cock_front | pipe_cock_rear));
            if (i > 0) cars_[i - 1].pipe_ends |= pipe_hose_rear;
            if (i + 1 < n) cars_[i + 1].pipe_ends |= pipe_hose_front;
        }
    }
    if (!settings_.derailment) return;
    for (std::size_t i = 0; i < n; ++i) {
        const auto& car = cars_[i];
        if (car.derail != DerailKind::tipping) continue;
        const float pull = 0.637f * std::abs(car.tilt) * mass_of(car);
        for (const std::size_t k : {i - 1, i + 1}) {
            if (k >= n || cars_[k].derailed) continue;
            if (pull > cars_[k].definition->derail_buffer_force) {
                start_tipping(k, &cars_[i]);
            } else if (std::abs(car.tilt) > 0.9f) {
                impl_->events.push_back({.kind = TrainEventKind::coupler_break, .car = static_cast<std::uint32_t>((std::min)(i, k))});
            }
        }
    }
}

// The body on its springs, per axis a mass on a spring and a damper
// (RTS 0x4F5B34, one 0.05 s sub-step as RTS takes per 0.05 s):
// - lateral: the centrifugal force FrameOscCorrection[0] * m * v^2 / R and
//   the hunting sway sin(phase) * v * 0.008 * mass * [0], the phase moving by
//   v * 0.3 * m * dt * [1] / damping;
// - vertical: a knock at node ends (0.001) or rail joints (0.0005), times
//   the impact, |v| / 10.5 and m / dt^2, times [2]; RTS also adds a knock
//   from the grade change under the bogies, not taken here;
// - longitudinal: -acceleration * m.
// Stiffness FrameSpringStiffness (RTS adds 45 * a car field, likely the
// load: UNKNOWN, not taken), damping FrameSpringDamping. RTS feeds the
// result only to the cab camera of the driven engine; openRTS also shows it
// on the car bodies.
void Train::sway() {
    const auto dt = static_cast<float>(train_step);
    for (std::size_t i = 0; i < cars_.size(); ++i) {
        auto& car = cars_[i];
        const auto& wagon = *car.definition;
        const bool resting = car.velocity == 0.0f && car.body[0] == 0.0f && car.body[1] == 0.0f && car.body[2] == 0.0f
            && car.body_rate[0] == 0.0f && car.body_rate[1] == 0.0f && car.body_rate[2] == 0.0f;
        if (resting) continue;
        const float m = mass_of(car);
        const auto stiffness = static_cast<float>(wagon.frame_spring_stiffness > 0.0 ? wagon.frame_spring_stiffness : m * 30.0);
        const auto damping = static_cast<float>(wagon.frame_spring_damping > 0.0 ? wagon.frame_spring_damping : m * 1.25);
        const float v = car.velocity;
        const auto* q = wagon.frame_osc_correction;
        // Lateral: outwards of the curve, the body moves against the turn.
        const float centrifugal = -q[0] * m * v * v * car.curvature;
        const float hunting = std::sin(car.hunting_phase) * (v * 0.008f * (std::max)(wagon.mass, 1.0f) * q[0]);
        car.hunting_phase += v * 0.3f * m * dt * q[1] / damping;
        if (car.hunting_phase > 3.14159265f) car.hunting_phase -= 6.28318531f;
        if (car.hunting_phase < -3.14159265f) car.hunting_phase += 6.28318531f;
        const auto& impact = impl_->impacts[i];
        float knock = 0.0f;
        const float speed_share = std::abs(v) * (1.0f / 10.5f);
        if (impact[0] != 0.0f) {
            knock = -0.001f * impact[0] * speed_share * m / (dt * dt);
        } else if (impact[1] != 0.0f) {
            knock = -0.0005f * impact[1] * speed_share * m / (dt * dt);
        }
        knock *= q[2];
        const float force[3] = {
            hunting + centrifugal - car.body[0] * stiffness - car.body_rate[0] * damping,
            knock - car.body[1] * stiffness - car.body_rate[1] * damping,
            -car.acceleration * m - car.body[2] * stiffness - car.body_rate[2] * damping,
        };
        for (int axis = 0; axis < 3; ++axis) {
            car.body_rate[axis] += force[axis] / m * dt;
            car.body[axis] += car.body_rate[axis] * dt;
            if (std::abs(car.body[axis]) < 0.001f) car.body[axis] = 0.0f;
            if (std::abs(car.body_rate[axis]) < 0.001f) car.body_rate[axis] = 0.0f;
        }
    }
}

// Moves the end travellers and refreshes per-car geometry and events. The
// leading end moves in occupy mode (it throws trailing switches, the RTS
// run-through), the trailing end in release mode.
// The trailing end keeps a junction it passed facing until it is
// ClearanceDist beyond it (RTS 0x4F9550), then releases it. A junction the
// leading end enters again (the train reversed) is simply held again.
void Train::advance_clearance(const MoveResult& leading, const MoveResult& trailing) {
    auto& clearing = impl_->clearing;
    for (const auto& pass : leading.junctions) {
        std::erase(clearing, pass.junction);
        if (auto* state = network_->junction(pass.junction)) state->clearance.erase(id_);
    }
    const double moved = std::abs(trailing.moved);
    for (auto it = clearing.begin(); it != clearing.end();) {
        auto* state = network_->junction(*it);
        if (!state || !state->clearance.contains(id_)) {
            it = clearing.erase(it);
            continue;
        }
        auto& left = state->clearance[id_];
        left -= moved;
        if (left <= 0.0) {
            network_->release_junction(*it, id_);
            it = clearing.erase(it);
        } else {
            ++it;
        }
    }
    // New facing passes count from the next step.
    for (const auto& pass : trailing.junctions) {
        if (pass.facing && std::find(clearing.begin(), clearing.end(), pass.junction) == clearing.end()) {
            clearing.push_back(pass.junction);
        }
    }
}

void Train::move_and_update(const double front_moved, const double rear_moved) {
    auto& impl = *impl_;
    if (front_moved != 0.0 || rear_moved != 0.0) {
        const bool forward = front_moved > 0.0 || (front_moved == 0.0 && rear_moved > 0.0);
        impl.moving_backward = !forward;
        MoveResult front_result;
        MoveResult rear_result;
        // The leading end moves first; when it stops short (end of track), the
        // trailing end loses the same distance, so the ends stay a train
        // length apart.
        const auto shortfall = [](const double wanted, const MoveResult& result) {
            return wanted - std::copysign(result.moved, wanted); // `moved` is unsigned
        };
        if (forward) {
            front_result = impl.head->move(front_moved, JunctionMode::occupy, id_);
            rear_result = impl.tail->move(rear_moved - shortfall(front_moved, front_result), JunctionMode::release, id_);
        } else {
            rear_result = impl.tail->move(rear_moved, JunctionMode::occupy, id_);
            front_result = impl.head->move(front_moved - shortfall(rear_moved, rear_result), JunctionMode::release, id_);
        }
        advance_clearance(forward ? front_result : rear_result, forward ? rear_result : front_result);
        if (!front_result.ok() || !rear_result.ok()) {
            // RTS: a failed advance derails the car there (0x5059A8, its
            // sliding branch); the rest stops at the end as before.
            if (settings_.derailment && !impl.stopped_at_end) {
                auto& lead = forward ? cars_.front() : cars_.back();
                if (!lead.derailed && std::abs(lead.velocity) > 0.5f) {
                    lead.derailed = true;
                    lead.derail = DerailKind::sliding;
                    lead.slide_speed = lead.velocity;
                    lead.slide = 0.0f;
                    lead.slide_origin_set = false;
                    impl.events.push_back({.kind = TrainEventKind::derailment,
                                           .car = static_cast<std::uint32_t>(forward ? 0 : cars_.size() - 1)});
                    // The rest runs into it (the role of RTS's solver for
                    // derailed trains, 0x586E6C): the stopping force of the
                    // cars behind, m * v over 0.5 s, derails each next car
                    // while it is above its DerailBufferForce; they fold to
                    // alternate sides, the more the faster.
                    const float v = std::abs(lead.velocity);
                    double rest = mass() - mass_of(lead);
                    float side = 1.0f;
                    const auto n = cars_.size();
                    for (std::size_t k = 1; k < n; ++k) {
                        auto& next = cars_[forward ? k : n - 1 - k];
                        if (next.derailed || rest * v / 0.5 <= next.definition->derail_buffer_force) break;
                        next.derailed = true;
                        next.derail = DerailKind::sliding;
                        next.slide_speed = lead.velocity * 0.5f;
                        next.slide = 0.0f;
                        next.slide_origin_set = false;
                        next.yaw_target = side * (std::min)(0.1f + 0.02f * v, 0.6f);
                        side = -side;
                        rest -= mass_of(next);
                        impl.events.push_back({.kind = TrainEventKind::derailment,
                                               .car = static_cast<std::uint32_t>(forward ? k : n - 1 - k)});
                    }
                }
            }
            for (auto& car : cars_) {
                car.velocity = 0.0f;
                car.acceleration = 0.0f;
            }
            if (!impl.stopped_at_end) impl.events.push_back({.kind = TrainEventKind::end_of_track});
            impl.stopped_at_end = true;
        } else {
            impl.stopped_at_end = false;
        }
    }
    const auto& last = cars_.back();
    for (std::size_t i = 0; i < cars_.size(); ++i) {
        auto& car = cars_[i];
        const double ahead = car.bogie_ahead > 0.0f ? car.bogie_ahead : length_of(car) * 0.35;
        const double behind = car.bogie_behind > 0.0f ? car.bogie_behind : length_of(car) * 0.35;
        const double centre = length_of(last) * 0.5 + car.offset;
        TrackTraveller rear = *impl.tail;
        rear.move(centre - behind);
        TrackTraveller front = rear;
        front.move(ahead + behind);
        const auto front_pose = front.pose();
        const auto rear_pose = rear.pose();
        if (car.derail == DerailKind::sliding && !car.slide_origin_set) {
            car.slide_from = {front_pose, rear_pose};
            car.slide_origin_set = true;
        }
        const double dx = front_pose.x - rear_pose.x + (front_pose.tile_x - rear_pose.tile_x) * tile_size;
        const double dz = front_pose.z - rear_pose.z + (front_pose.tile_z - rear_pose.tile_z) * tile_size;
        const double dy = front_pose.y - rear_pose.y;
        const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        // Positive pitch: downhill towards the train front (the RTS sign).
        car.pitch = distance > 0.01 ? static_cast<float>(std::asin(std::clamp(-dy / distance, -1.0, 1.0))) : 0.0f;
        car.radius = 0.0f;
        if (const auto* node = network_->node(front.node()); node && front.section() < node->sections.size()) {
            if (const auto* definition = node->sections[front.section()].definition; definition && definition->curved) {
                car.radius = static_cast<float>(definition->radius);
            }
        }
        // Curvature under the car from the bogie headings (towards the
        // side the car turns to); headings may run along the node's other
        // direction, hence the half-turn fold.
        {
            constexpr double pi = 3.14159265358979323846;
            double turn = front_pose.heading - rear_pose.heading;
            while (turn > pi) turn -= 2.0 * pi;
            while (turn < -pi) turn += 2.0 * pi;
            if (turn > pi * 0.5) turn -= pi;
            if (turn < -pi * 0.5) turn += pi;
            car.curvature = distance > 0.5 ? static_cast<float>(turn / distance) : 0.0f;
        }
        // Rail joints as RTS places them (0x4F5B34): a junction event when a
        // bogie changes node; a joint event when it passes a rail joint along
        // the node: every 50 m, or on nodes over 2000 m (welded) a group at
        // 0, 12.5, 25 and 37.5 m every 600 m. Each also gives an impact for
        // the body springs that fades over WheelRadius * 1.25 * NumWheels.
        const auto& wagon = *car.definition;
        const double window = static_cast<double>(wagon.wheel_radius) * 1.25 * wagon.wheels;
        const auto since_joint = [](const double offset, const double node_length) {
            if (node_length > 2000.0) {
                double in = offset - std::floor(offset / 600.0) * 600.0;
                if (in < 37.5) {
                    if (in >= 25.0) in -= 25.0;
                    else if (in >= 12.5) in -= 12.5;
                } else {
                    in -= 37.5;
                }
                return in;
            }
            return offset - std::floor(offset / 50.0) * 50.0;
        };
        const auto joint_index = [](const double offset, const double node_length) {
            if (node_length > 2000.0) {
                const double group = std::floor(offset / 600.0);
                const double in = offset - group * 600.0;
                return group * 4.0 + (in < 12.5 ? 0.0 : in < 25.0 ? 1.0 : in < 37.5 ? 2.0 : 3.0);
            }
            return std::floor(offset / 50.0);
        };
        auto& impact = impl.impacts[i];
        impact = {0.0f, 0.0f};
        const std::array<const TrackTraveller*, 2> bogies{&front, &rear};
        for (std::uint8_t b = 0; b < 2; ++b) {
            const auto* traveller = bogies[b];
            const auto* node = network_->node(traveller->node());
            const double length = node ? node->length : 0.0;
            const double offset = traveller->offset();
            auto& before = impl.last_bogie[i][b];
            if (before.node != ~0u && car.velocity != 0.0f) {
                if (before.node != traveller->node()) {
                    impl.events.push_back(
                        {.kind = TrainEventKind::junction, .car = static_cast<std::uint32_t>(i), .bogie = b});
                } else if (joint_index(before.offset, length) != joint_index(offset, length)) {
                    impl.events.push_back({.kind = TrainEventKind::joint, .car = static_cast<std::uint32_t>(i), .bogie = b});
                }
                if (window > 0.0) {
                    // Distance since the node end the bogie entered by.
                    const bool along = traveller->forward() == (car.velocity > 0.0f);
                    const double entered = along ? offset : length - offset;
                    const double node_impact = entered / window <= 1.0 ? 1.0 - entered / window : 0.0;
                    const double joint = since_joint(offset, length) / window;
                    const double joint_impact = joint <= 1.0 ? 1.0 - joint : 0.0;
                    impact[0] = (std::max)(impact[0], static_cast<float>(node_impact));
                    impact[1] = (std::max)(impact[1], static_cast<float>(joint_impact));
                }
            }
            before = {traveller->node(), offset};
        }
    }
}



// ---------------------------------------------------------------------------
// Trains meeting (RTS 0x424488, 0x469ED8 coupling, 0x4FB8CC pushing apart)

namespace {

struct Meeting {
    Train* other = nullptr;
    bool other_front = false;
    double gap{};
};

// The nearest end of another train along the set route from `start` (a
// traveller facing away from the train), within `reach` metres; a slight
// overlap (up to 0.5 m) still counts as a meeting.
std::optional<Meeting> nearest_end(const TrackNetwork& network, const TrackTraveller& start, const Train& self,
                                   std::span<Train* const> trains, const double reach) {
    std::uint32_t node = start.node();
    bool forward = start.forward();
    double offset = start.offset();
    double walked = 0.0;
    for (int guard = 0; guard < 256; ++guard) {
        const auto* info = network.node(node);
        if (!info) return std::nullopt;
        std::optional<Meeting> best;
        for (auto* other : trains) {
            if (!other || other == &self || !other->placed()) continue;
            for (const bool front : {true, false}) {
                const auto end = front ? other->front_end() : other->rear_end();
                if (!end || end->node() != node) continue;
                const double along = forward ? end->offset() - offset : offset - end->offset();
                if (along < (guard == 0 ? -0.5 : 0.0)) continue;
                const double gap = walked + along;
                if (gap > reach) continue;
                if (!best || gap < best->gap) best = Meeting{other, front, gap};
            }
        }
        if (best) return best;
        walked += forward ? info->length - offset : offset;
        if (walked > reach) return std::nullopt;
        MoveStop stop = MoveStop::completed;
        const auto step = next_vector_node(network, node, forward, stop);
        if (!step) return std::nullopt;
        node = step->node;
        forward = step->forward;
        const auto* next = network.node(node);
        offset = forward || !next ? 0.0 : next->length;
    }
    return std::nullopt;
}

} // namespace

std::optional<EndContact> end_contact(const Train& train, const bool front, const std::span<Train* const> trains, const double reach) {
    if (!train.placed() || !train.network()) return std::nullopt;
    auto start = front ? train.front_end() : train.rear_end();
    if (!start) return std::nullopt;
    if (!front) start->reverse();
    const auto meeting = nearest_end(*train.network(), *start, train, trains, reach);
    if (!meeting) return std::nullopt;
    return EndContact{.other = meeting->other, .other_front = meeting->other_front, .gap = meeting->gap};
}

namespace {

// Mass-weighted mean velocity of a train towards its front.
double mean_velocity(const Train& train) {
    double momentum = 0.0;
    for (const auto& car : train.cars()) momentum += static_cast<double>(car.velocity) * (std::max)(car.definition->mass, 1.0f);
    return momentum / (std::max)(train.mass(), 1.0);
}

} // namespace

std::vector<ContactResult> resolve_contacts(const std::span<Train* const> trains, const std::span<const std::uint8_t> passive) {
    std::vector<ContactResult> results;
    std::vector<bool> gone(trains.size(), false);
    const auto index_of = [&](const Train* train) {
        return static_cast<std::size_t>(std::find(trains.begin(), trains.end(), train) - trains.begin());
    };
    for (std::size_t a = 0; a < trains.size(); ++a) {
        auto* train = trains[a];
        if (gone[a] || !train || !train->placed() || !train->network()) continue;
        for (const bool front : {true, false}) {
            if (gone[a]) break;
            auto start = front ? train->front_end() : train->rear_end();
            if (!start) continue;
            if (!front) start->reverse();
            const auto meeting = nearest_end(*train->network(), *start, *train, trains, 60.0);
            if (!meeting) continue;
            const auto b = index_of(meeting->other);
            if (b >= trains.size() || gone[b]) continue;
            if (a < passive.size() && b < passive.size() && passive[a] != 0 && passive[b] != 0) continue;
            auto* other = meeting->other;
            // Speeds along the line from this train towards the other.
            const double side_a = front ? 1.0 : -1.0;
            const double side_b = meeting->other_front ? 1.0 : -1.0;
            const auto& end_a = front ? train->cars().front() : train->cars().back();
            const auto& end_b = meeting->other_front ? other->cars().front() : other->cars().back();
            const double towards_a = side_a * end_a.velocity;
            const double towards_b = -side_b * end_b.velocity;
            const double closing = towards_a - towards_b;
            const double overlap = closing * train_step - meeting->gap;
            if (overlap <= 0.0) continue;
            // The impact RTS compares with DerailBufferForce.
            const double force = overlap / train_step * ((std::max)(end_a.definition->mass, 1.0f) + (std::max)(end_b.definition->mass, 1.0f));
            const auto& coupler_a = front ? (end_a.flipped ? end_a.definition->couplings[0] : end_a.definition->couplings[1])
                                          : (end_a.flipped ? end_a.definition->couplings[1] : end_a.definition->couplings[0]);
            const auto& coupler_b = meeting->other_front
                ? (end_b.flipped ? end_b.definition->couplings[0] : end_b.definition->couplings[1])
                : (end_b.flipped ? end_b.definition->couplings[1] : end_b.definition->couplings[0]);
            // Held open or broken: it only pushes (RTS +14 0x11 / 0x22).
            constexpr std::uint8_t front_end = coupler_open_front | coupler_broken_front;
            constexpr std::uint8_t rear_end = coupler_open_rear | coupler_broken_rear;
            const bool open_a = (end_a.coupler_open & (front ? front_end : rear_end)) != 0;
            const bool open_b = (end_b.coupler_open & (meeting->other_front ? front_end : rear_end)) != 0;
            const bool too_hard = force > end_a.definition->derail_buffer_force || force > end_b.definition->derail_buffer_force;
            const bool can_couple = coupler_a.type == 0 && coupler_b.type == 0 && !open_a && !open_b && !too_hard
                && closing >= (std::max)(coupler_a.velocity, coupler_b.velocity) && !train->derailed() && !other->derailed();
            if (can_couple) {
                // The earlier train in the list survives.
                const bool keep_a = a < b;
                auto* survivor = keep_a ? train : other;
                auto* absorbed = keep_a ? other : train;
                const bool survivor_front = keep_a ? front : meeting->other_front;
                const bool absorbed_front = keep_a ? meeting->other_front : front;
                if (survivor->couple(*absorbed, survivor_front, absorbed_front, (std::max)(meeting->gap, 0.0))) {
                    results.push_back({.outcome = ContactOutcome::coupled,
                                       .survivor = survivor,
                                       .absorbed = absorbed,
                                       .absorbed_ahead = survivor_front,
                                       .absorbed_turned = absorbed_front == survivor_front,
                                       .force = force});
                    gone[keep_a ? b : a] = true;
                    continue;
                }
            }
            // Inelastic: both trains continue with the common speed along
            // the line (masses of the whole trains); an overlap is pushed
            // apart in proportion to the masses.
            const double mass_a = train->mass();
            const double mass_b = other->mass();
            const double common = (mass_a * side_a * mean_velocity(*train) + mass_b * -side_b * mean_velocity(*other)) / (mass_a + mass_b);
            const double change_a = side_a * common - mean_velocity(*train);
            const double change_b = -side_b * common - mean_velocity(*other);
            results.push_back({.outcome = too_hard ? ContactOutcome::collision : ContactOutcome::buffers,
                               .survivor = train,
                               .absorbed = other,
                               .force = force});
            train->add_velocity(static_cast<float>(change_a));
            other->add_velocity(static_cast<float>(change_b));
            // A collision breaks an end coupler whose share of the impact
            // passes its Break force (RTS 0x58A07C: the share is the other
            // train's mass over both).
            if (too_hard) {
                const double share_a = force * mass_b / (mass_a + mass_b);
                const double share_b = force * mass_a / (mass_a + mass_b);
                if (share_a > coupler_a.break_tension)
                    train->set_car_flags(front ? 0 : train->cars().size() - 1, Train::CarFlags::coupler_open,
                                         front ? coupler_broken_front : coupler_broken_rear, true);
                if (share_b > coupler_b.break_tension)
                    other->set_car_flags(meeting->other_front ? 0 : other->cars().size() - 1, Train::CarFlags::coupler_open,
                                         meeting->other_front ? coupler_broken_front : coupler_broken_rear, true);
            }
            // Each end car takes the hit (RTS 0x58A07C -> 0x589A14): pushed
            // away from the other train, over one step; above its
            // DerailBufferForce it comes off.
            const float push_a[3] = {0.0f, 0.0f, front ? -1.0f : 1.0f};
            const float push_b[3] = {0.0f, 0.0f, meeting->other_front ? -1.0f : 1.0f};
            train->hit(front ? 0 : train->cars().size() - 1, static_cast<float>(force), push_a, static_cast<float>(train_step));
            other->hit(meeting->other_front ? 0 : other->cars().size() - 1, static_cast<float>(force), push_b,
                       static_cast<float>(train_step));
            if (meeting->gap < 0.0) {
                const double push = -meeting->gap;
                train->shift(-side_a * push * mass_b / (mass_a + mass_b));
                other->shift(-side_b * push * mass_a / (mass_a + mass_b));
            }
        }
    }
    return results;
}

// ---------------------------------------------------------------------------
// Test track and test run

std::unique_ptr<TrackNetwork> straight_test_track(const double length) {
    constexpr double section_length = 500.0;
    const auto count = static_cast<int>(std::ceil(length / section_length));
    const std::string sections = "SIMISA@@@@@@@@@@JINX0T0t______\nTrackSections ( 1\n"
                                 "    TrackSection ( 1 SectionSize ( 1.5 500 ) )\n)\nTrackShapes ( 0 )\n";
    std::string placements;
    for (int i = 0; i < count; ++i) {
        placements += "1 0 0 0 0 1 1 00 0 0 0 0 " + std::to_string(i * section_length) + " 0 0 0 ";
    }
    const std::string database = "SIMISA@@@@@@@@@@JINX0T0t______\nTrackDB ( Serial ( 1 ) TrackNodes ( 3\n"
                                 "TrackNode ( 1 TrVectorNode ( TrVectorSections ( "
        + std::to_string(count) + " " + placements
        + ") ) TrPins ( 1 1 TrPin ( 2 1 ) TrPin ( 3 1 ) ) )\n"
          "TrackNode ( 2 TrEndNode ( 0 ) UiD ( 0 0 0 0 0 0 0 0 0 0 0 0 ) TrPins ( 1 0 TrPin ( 1 1 ) ) )\n"
          "TrackNode ( 3 TrEndNode ( 0 ) UiD ( 0 0 0 0 0 0 0 0 0 0 0 0 ) TrPins ( 1 0 TrPin ( 1 0 ) ) )\n"
          ") TrItemTable ( 0\n) )\n";
    auto library = formats::parse_track_sections(sections);
    auto db = formats::parse_track_database(database);
    if (!library.ok() || !db.ok()) return nullptr;
    return std::make_unique<TrackNetwork>(std::move(db), std::move(library));
}

TestRunResult test_run(const std::vector<CarSetup>& cars, const TrainSettings settings) {
    TestRunResult result;
    if (cars.empty()) {
        result.message = "no cars";
        return result;
    }
    double length = 0.0;
    for (const auto& car : cars) length += car.definition ? car.definition->length + 2.0 : 0.0;
    auto network = straight_test_track(length + 60000.0);
    if (!network) {
        result.message = "test track did not build";
        return result;
    }
    Train train(cars, settings);
    std::string error;
    if (!train.place(*network, 1, length + 100.0, true, &error)) {
        result.message = error;
        return result;
    }
    const auto driver = attach_keyboard_driver(train);
    const auto finite = [&] {
        for (const auto& car : train.cars()) {
            if (!std::isfinite(car.velocity) || !std::isfinite(car.offset) || !std::isfinite(car.pipe)) return false;
        }
        return true;
    };
    if (driver) {
        driver->reverser = 1;
        driver->throttle = driver->notches;
    }
    for (int i = 0; i < static_cast<int>(120.0 / train_step); ++i) {
        train.step();
        if (!finite()) {
            result.message = "non-finite state while accelerating";
            return result;
        }
        result.top_speed = (std::max)(result.top_speed, std::abs(train.speed()));
    }
    if (driver) {
        driver->throttle = 0;
        driver->brake = KeyboardController::BrakeHandle::emergency;
        driver->independent = 4.0f;
    }
    const auto fastest = [&] {
        double speed = 0.0;
        for (const auto& car : train.cars()) speed = (std::max)(speed, std::abs(static_cast<double>(car.velocity)));
        return speed;
    };
    double time = 0.0;
    while (fastest() > 0.01 && time < 600.0) {
        train.step();
        time += train_step;
        if (!finite()) {
            result.message = "non-finite state while braking";
            return result;
        }
    }
    result.stopping_time = time;
    if (fastest() > 0.01) {
        result.message = "did not stop within 600 s";
        return result;
    }
    result.ok = true;
    return result;
}

} // namespace openrts::simulation
