#include <openrts/simulation/train.hpp>

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

std::shared_ptr<DriverHandles> attach_keyboard_driver(Train& train) {
    std::shared_ptr<DriverHandles> handles;
    for (std::size_t i = 0; i < train.cars().size(); ++i) {
        const auto* definition = train.cars()[i].definition;
        if (!definition || !definition->engine) continue;
        const bool master = !handles;
        if (master) {
            handles = std::make_shared<DriverHandles>();
            if (definition->engine->throttle_notches > 0) handles->notches = definition->engine->throttle_notches;
        }
        train.set_controller(i, std::make_unique<KeyboardController>(handles, master));
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
    // Traction: MaxForce up to the power hyperbola, scaled by the notch.
    if (engine && reverser != 0 && throttle > 0 && brake != BrakeHandle::emergency) {
        const float max_force = engine->max_force > 0.0f ? engine->max_force : 300000.0f;
        const float power = engine->max_power > 0.0f ? engine->max_power : max_force * 10.0f;
        const float speed = std::abs(inputs.speed);
        float force = (std::min)(max_force, power / (std::max)(speed, 0.5f));
        force *= static_cast<float>(throttle) / static_cast<float>(notches);
        if (engine->max_velocity > 0.0f && speed > engine->max_velocity) force = 0.0f;
        // Stay within adhesion: MaxForce of DLL engines is often far above it
        // (RTS ignores it), and slipping would make keyboard driving jumpy.
        const float mu = inputs.definition->adhesion[1];
        if (mu > 0.0f) force = (std::min)(force, inputs.definition->mass * 10.0f * mu * 0.9f);
        demands.force = force * static_cast<float>(reverser);
    }
    // Compressor with hysteresis (every engine).
    if (inputs.main_reservoir < 7.5f) compressor_ = true;
    if (inputs.main_reservoir > 9.0f) compressor_ = false;
    demands.main_reservoir_rate = compressor_ ? 0.05f : 0.0f;
    // The independent brake acts on every engine (multiple-unit line).
    demands.independent_brake = std::clamp(handles.independent, 0.0f, 4.0f);
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
        } else if (inputs.cylinder > 0.0f && ur - p < 0.1f) {
            // The DLL's own overcharge while the locomotive is still braked:
            // it pushes the release through the RTS triple valve.
            ur += 0.15f * time;
        }
        ur = (std::min)(ur, mr);
        if (ur > p) ur -= 0.003f * time; // stabiliser: overcharge removal
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
    std::vector<std::array<std::pair<std::uint32_t, std::size_t>, 2>> last_bogie; // (node, section) per bogie
    std::vector<TrainEvent> events;
    bool stopped_at_end = false;
    // Junctions the tail passed facing, still held for their ClearanceDist.
    std::vector<std::uint32_t> clearing;
    bool moving_backward = false;
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
        if (setup.definition && setup.definition->engine) car.main_reservoir = 8.5f;
        cars_.push_back(car);
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

std::optional<TrackTraveller> Train::leading_end() const {
    if (!impl_->moving_backward) return impl_->head;
    if (!impl_->tail) return std::nullopt;
    auto lead = *impl_->tail;
    lead.reverse();
    return lead;
}

void Train::set_coupler_substeps(const std::uint32_t substeps) noexcept {
    settings_.coupler_substeps = std::clamp<std::uint32_t>(substeps, 1, 8);
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
    }
    std::reverse(cars_.begin(), cars_.end());
    std::reverse(impl_->controllers.begin(), impl_->controllers.end());
    std::reverse(impl_->demands.begin(), impl_->demands.end());
    std::reverse(impl_->last_bogie.begin(), impl_->last_bogie.end());
    for (auto& bogies : impl_->last_bogie) std::swap(bogies[0], bogies[1]);
    impl_->moving_backward = false;
    impl_->stopped_at_end = false;
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
    for (std::size_t i = 0; i < cars_.size(); ++i) {
        auto& last = impl_->last_bogie[i];
        last = {std::pair{~0u, std::size_t{}}, std::pair{~0u, std::size_t{}}};
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

void Train::step() {
    if (!network_ || cars_.empty()) return;
    const auto n = cars_.size();
    const auto dt = static_cast<float>(train_step);
    auto& impl = *impl_;

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
        const float mu = car.definition->adhesion[1];
        const float limit = mass_of(car) * gravity * mu;
        car.wheelslip = mu > 0.0f && std::abs(force) > limit;
        if (car.wheelslip) force *= 0.25f;
        car.force = force * direction;
        car.handbrake = (std::max)(car.handbrake, 0.01f * demands.handbrake);
    }

    // EPT: an engine's EPTvalue spreads to the neighbouring cars with EP
    // brakes, as far as the EP line runs (RTS: every other car gets -1).
    for (std::size_t i = 0; i < n; ++i) {
        const auto& demands = impl.demands[i];
        if (!(demands.systems & 2) || demands.ep_value < 0.0f || !(cars_[i].definition->brakes.system & 2)) continue;
        cars_[i].ep = demands.ep_value;
        for (std::size_t k = i + 1; k < n && (cars_[k].definition->brakes.system & 2); ++k) cars_[k].ep = demands.ep_value;
        for (std::size_t k = i; k-- > 0 && (cars_[k].definition->brakes.system & 2);) cars_[k].ep = demands.ep_value;
    }

    // 2. Brake force from the cylinders (before this step's air update, as RTS).
    for (auto& car : cars_) {
        const auto& brakes = car.definition->brakes;
        const float cylinder = (std::max)(car.cylinder, car.independent);
        car.brake_force = brakes.cylinder_pressure_for_max_force > 0.0f
            ? cylinder / brakes.cylinder_pressure_for_max_force * brakes.max_brake_force
            : 0.0f;
        car.brake_force += car.handbrake * brakes.max_handbrake_force;
    }
    for (std::size_t i = 0; i < n; ++i) cars_[i].brake_force += impl.demands[i].brake_force;

    // 3. Air: pipe from the locomotives, equalisation, triple valves.
    std::vector<float> pipe_start(n);
    for (std::size_t i = 0; i < n; ++i) pipe_start[i] = cars_[i].pipe;
    const auto pipe_volume = [&](const std::size_t i) { return length_of(cars_[i]) * pipe_area; };
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
            for (std::size_t m = j + 1; m < n; ++m) {
                distance += length_of(cars_[m - 1]);
                const float factor = (std::min)(1.0f, 30.0f / distance) * dt;
                cars_[m].pipe += (std::min)((vented - cars_[m].pipe) * factor, 0.0f);
            }
            distance = 0.0f;
            for (std::size_t m = j; m-- > 0;) {
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
        for (std::size_t m = j + 1; m < n; ++m) {
            distance += length_of(cars_[m - 1]);
            feed(cars_[m], distance, pipe_volume(m - 1));
        }
        distance = 0.0f;
        for (std::size_t m = j; m-- > 0;) {
            distance += length_of(cars_[m + 1]);
            feed(cars_[m], distance, pipe_volume(m));
        }
    }
    // Every car equalises with each car ahead of it, slower with distance.
    for (std::size_t i = 1; i < n; ++i) {
        auto& car = cars_[i];
        const float volume = pipe_volume(i);
        for (std::size_t k = i; k-- > 0;) {
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
                car.derailed = true;
                impl.events.push_back({.kind = TrainEventKind::derailment, .car = static_cast<std::uint32_t>(&car - cars_.data())});
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
        // Joint and junction events per bogie.
        const std::array<const TrackTraveller*, 2> bogies{&front, &rear};
        for (std::uint8_t b = 0; b < 2; ++b) {
            const std::pair now{bogies[b]->node(), bogies[b]->section()};
            auto& before = impl.last_bogie[i][b];
            if (before.first != ~0u && now != before) {
                if (now.first != before.first) {
                    impl.events.push_back(
                        {.kind = TrainEventKind::junction, .car = static_cast<std::uint32_t>(i), .bogie = b});
                }
                impl.events.push_back({.kind = TrainEventKind::joint, .car = static_cast<std::uint32_t>(i), .bogie = b});
            }
            before = now;
        }
    }
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
