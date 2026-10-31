#pragma once

// A train on the track network, simulated as rts1.exe does (0x424488 train
// update, 0x505414 / 0x504648 coupler solver, brakes, 0x4F5B34 movement),
// with a fixed step instead of the frame time. See docs/train.md,
// docs/reverse/runtime-overview.md and docs/reverse/brakes.md.

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <openrts/formats/rolling_stock.hpp>
#include <openrts/simulation/track_traveller.hpp>

namespace openrts::simulation {

// One simulation step, s: the RTS coupler sub-step and the network tick.
inline constexpr double train_step = 0.05;

struct TrainSettings {
    // Coupler solver passes per step: 1 is RTS (0.05 s); 2..4 are the
    // experimental finer integration of the stiff part only.
    std::uint32_t coupler_substeps = 1;
    // Report coupler breaks and derailments (as RTS with its default config).
    bool coupler_break = true;
    bool derailment = true;
};

struct CarSetup {
    const formats::WagonDefinition* definition = nullptr;
    bool flipped = false;
    float handbrake{}; // 0..1 (consist HandbrakePercent / 100)
};

// What a locomotive controller asks of its engine: the ts.h `Engine` outputs
// a locomotive DLL sets (Force, BrakeForce, TrainPipeRate, ...).
struct EngineDemands {
    float force{};             // N along the car's own front (Engine::Force)
    float brake_force{};       // N, e.g. dynamic brake (Engine::BrakeForce)
    float train_pipe_rate{};   // >0: pipe fed from the main reservoir; <0: vented, atm/s
    float independent_brake = -1.0f; // atm target, <0: unchanged (IndependentBrakeValue)
    float main_reservoir_rate{}; // atm/s (MainResRate)
    float charging_rate{};       // second pipe (ChargingRate)
    float ep_value = -1.0f;      // EP brake target, <0: none (EPTvalue)
    float handbrake{};           // percent (HandbrakePercent)
    std::uint16_t systems = 1;   // BrakeSystemsEngaged: 1 pipe, 2 EP, 4 charging pipe
    bool release_train_brake = false;       // EngineFlags 1
    bool release_independent_brake = false; // EngineFlags 2
};

// What a controller sees of its locomotive.
struct EngineInputs {
    float speed{};          // m/s along the car's own front
    float train_pipe{};     // atm, at this car
    float main_reservoir{}; // atm
    float cylinder{};       // train brake cylinder, atm
    float independent{};    // independent brake cylinder, atm
    bool flipped = false;   // the car faces the train rear
    const formats::WagonDefinition* definition = nullptr;
};

// Source of a locomotive's demands: the built-in keyboard controller now;
// legacy DLLs and openRTS plugins later (docs/train-dll.md).
class LocomotiveController {
public:
    virtual ~LocomotiveController() = default;
    virtual void step(double dt, const EngineInputs& inputs, EngineDemands& demands) = 0;
};

// Driver's handles shared by every engine of a train (multiple-unit
// control): the master engine works the brake valve, all engines pull.
struct DriverHandles {
    // Driver's brake valve 395 positions I..VI.
    enum class BrakeHandle : std::uint8_t {
        charge,    // I: charge and release (held: the valve springs back to II)
        running,   // II
        lap,       // III: lap without feed
        lap_feed,  // IV: lap with feed
        service,   // V
        emergency, // VI
    };

    std::uint32_t notches = 10;   // throttle positions above 0 (the master's)
    std::uint32_t throttle = 0;   // 0..notches
    int reverser = 0;             // -1, 0, +1, towards the train front
    BrakeHandle brake = BrakeHandle::running;
    float independent = 0.0f;     // independent brake target, atm (0..4)
    float charging_pressure = 5.0f; // the pressure II charges the equalising reservoir to, atm
    float equalising = 5.0f;      // equalising reservoir (УР, Engine::UR), kept by the master's valve, atm
};

// Built-in controller for driving without a cab. Its traction model is
// openRTS's own (from MaxForce / MaxPower), not RTS: in RTS a DLL does this.
// The brake valve is the 395 of the ВЛ10к DLL (tsrLoco_VL10k-1461,
// vl10k_main.cpp): written for the RTS triple valve, which it drives
// through Engine::UR and TrainPipeRate (docs/reverse/air-brakes-real.md §8).
class KeyboardController final : public LocomotiveController {
public:
    using BrakeHandle = DriverHandles::BrakeHandle;

    KeyboardController(std::shared_ptr<DriverHandles> handles, bool master);
    void step(double dt, const EngineInputs& inputs, EngineDemands& demands) override;

private:
    std::shared_ptr<DriverHandles> handles_;
    bool master_;
    bool compressor_ = false;
};

class Train;
// Puts keyboard controllers on every engine of `train`, the first one master.
// Returns the shared handles, or nullptr when the train has no engine.
std::shared_ptr<DriverHandles> attach_keyboard_driver(Train& train);

enum class TrainEventKind : std::uint8_t {
    joint,        // a bogie passed a section boundary
    junction,     // a bogie passed a junction
    end_of_track, // the train ran onto an end node and stopped
    coupler_break,
    derailment,
};

struct TrainEvent {
    TrainEventKind kind{};
    std::uint32_t car{};
    std::uint8_t bogie{}; // 0 front, 1 rear
};

// State of one car (the ts.h Wagon fields RTS simulates on).
struct CarState {
    const formats::WagonDefinition* definition = nullptr;
    bool flipped = false;
    double offset{};     // centre along the train, m; the last car is 0, ahead is positive
    float velocity{};    // m/s, towards the train front
    float acceleration{};
    // Forces of the last step, N.
    float force{};       // active (traction), Wagon::Force
    float grade{};       // Wagon::Slope
    float resistance{};  // Wagon::Resistance
    float brake_force{}; // Wagon::BrakeForce
    float coupler_force{}; // to the car behind, first sub-step
    float pitch{};       // rad, positive uphill towards the train front
    float radius{};      // curve radius under the car, 0 straight
    // Brakes, atm.
    float pipe{}, auxiliary{}, cylinder{}, independent{}, control{}, charging_pipe{};
    float ep = -1.0f;
    float handbrake{}; // 0..1
    float main_reservoir{}; // engines
    // Bogie positions from the car centre towards the train front / rear, m
    // (RTS takes them from the shape's BOGIE nodes); 0: 35 % of the length.
    float bogie_ahead{};
    float bogie_behind{};
    bool wheelslip = false;
    bool derailed = false;
    double travelled{}; // m, along the train front direction, for wheel animation
};

// Front and rear bogie (or car end) positions of a car on the track.
struct CarPlacement {
    TrackPoint front;
    TrackPoint rear;
};

class Train {
public:
    explicit Train(std::vector<CarSetup> cars, TrainSettings settings = {});
    ~Train();
    Train(const Train&) = delete;
    Train& operator=(const Train&) = delete;

    // Puts the train with the front of car 0 at (node, offset), facing along
    // the node when `forward`; the cars are laid back along the track.
    bool place(TrackNetwork& network, std::uint32_t node, double offset, bool forward, std::string* error = nullptr);
    [[nodiscard]] bool placed() const noexcept { return network_ != nullptr; }

    // Controllers drive engines; cars without one get no demands.
    void set_controller(std::size_t car, std::unique_ptr<LocomotiveController> controller);
    // Bogie distances from the car centre, along the train (front, rear).
    void set_bogies(std::size_t car, float ahead, float behind);
    [[nodiscard]] LocomotiveController* controller(std::size_t car);

    // One step of train_step seconds.
    void step();

    [[nodiscard]] const std::vector<CarState>& cars() const noexcept { return cars_; }
    [[nodiscard]] std::vector<CarPlacement> placements() const;
    // The front end of car 0, facing the train front; the rear end of the
    // last car, facing the train front too.
    [[nodiscard]] std::optional<TrackTraveller> front_end() const;
    [[nodiscard]] std::optional<TrackTraveller> rear_end() const;
    [[nodiscard]] std::vector<TrainEvent> take_events();
    [[nodiscard]] double speed() const noexcept { return cars_.empty() ? 0.0 : cars_.front().velocity; }
    [[nodiscard]] double length() const noexcept;
    [[nodiscard]] const TrainSettings& settings() const noexcept { return settings_; }
    // Coupler solver sub-steps (1 = RTS); takes effect on the next step.
    void set_coupler_substeps(std::uint32_t substeps) noexcept;
    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }

private:
    void advance_clearance(const MoveResult& leading, const MoveResult& trailing);
    void move_and_update(double front_moved, double rear_moved);

    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<CarState> cars_;
    TrainSettings settings_;
    TrackNetwork* network_ = nullptr;
    std::uint64_t id_;
};

// A straight test track of `length` metres (synthetic network), for checks.
[[nodiscard]] std::unique_ptr<TrackNetwork> straight_test_track(double length);

struct TestRunResult {
    bool ok = false;
    std::string message;
    double top_speed{};
    double stopping_time{}; // s from full brake to standstill
};

// Runs `cars` on a straight track: 120 s full throttle on the first engine,
// then emergency brake until standstill (at most 600 s).
[[nodiscard]] TestRunResult test_run(const std::vector<CarSetup>& cars, TrainSettings settings = {});

} // namespace openrts::simulation
