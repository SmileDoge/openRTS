#pragma once

// A train on the track network, simulated as rts1.exe does (0x424488 train
// update, 0x505414 / 0x504648 coupler solver, brakes, 0x4F5B34 movement),
// with a fixed step instead of the frame time. See docs/train.md,
// docs/reverse/runtime-overview.md and docs/reverse/brakes.md.

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <optional>
#include <string>
#include <vector>

#include <openrts/formats/rolling_stock.hpp>
#include <openrts/simulation/track_traveller.hpp>

namespace openrts::simulation {

// One simulation step, s: the RTS coupler sub-step and the network tick.
inline constexpr double train_step = 0.05;

// The air brake model: RTS's (the default, and the only one with Legacy on)
// or openRTS's realistic pneumatics (docs/train.md). A train with a legacy
// DLL keeps RTS's: its driver's valve is tuned to the RTS triple valve.
enum class BrakeModel : std::uint8_t { rts, realistic };

struct TrainSettings {
    // Coupler solver passes per step: 1 is RTS (0.05 s); 2..4 are the
    // experimental finer integration of the stiff part only.
    std::uint32_t coupler_substeps = 1;
    // Report coupler breaks and derailments (as RTS with its default config).
    bool coupler_break = true;
    bool derailment = true;
    BrakeModel brake_model = BrakeModel::rts;
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
    // Realistic air only: the equalising reservoir the driver's valve holds
    // (the pipe follows it through the relay; < 0: this engine's valve is
    // not working the pipe), and position VI.
    float equalising = -1.0f;
    bool emergency = false;
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
    bool realistic_air = false; // the train runs BrakeModel::realistic
    // The line at the pantographs (electric engines; Locomotive::LineVoltage,
    // LineFreq): V and Hz. Negative voltage: the route's electrification is
    // not modelled, the engine has power anywhere.
    float line_voltage = -1.0f;
    float line_frequency{};
};

// Source of a locomotive's demands: the built-in keyboard controller now;
// legacy DLLs and openRTS plugins later (docs/train-dll.md).
class LocomotiveController {
public:
    virtual ~LocomotiveController() = default;
    virtual void step(double dt, const EngineInputs& inputs, EngineDemands& demands) = 0;
    // The controller's valve is made for RTS's triple valve (legacy DLLs):
    // the train keeps BrakeModel::rts.
    [[nodiscard]] virtual bool rts_brakes() const { return false; }
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
        ep_service, // VЭ: electro-pneumatic application only (as IV without ЭПТ)
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
    bool ep_brake = false;        // ЭПТ switched on
    float ep_target = 0.0f;       // ЭПТ cylinder demand (Engine::EPTvalue), kept by the master's valve, atm
};

// Built-in controller for driving without a cab. Its traction model is
// openRTS's own (from MaxForce / MaxPower), not RTS: in RTS a DLL does this.
// The brake valve is the 395 of the ВЛ10к DLL (tsrLoco_VL10k-1461,
// vl10k_main.cpp): written for the RTS triple valve, which it drives
// through Engine::UR and TrainPipeRate (docs/reverse/air-brakes-real.md §8).
// The ЭПТ layer is the ЭР9Т DLL's (tedStepanki_emu, er9t_main.cpp):
// EPTvalue ramps up in VЭ and V, holds in III/IV, ramps down in I/II.
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
    coupler_break, // the coupler behind `car` broke (the owner uncouples there)
    derailment,
};

// Brake pipe ends of a car (ts.h Wagon flags byte 2).
inline constexpr std::uint8_t pipe_hose_front = 0x01; // hose disconnected
inline constexpr std::uint8_t pipe_hose_rear = 0x02;
inline constexpr std::uint8_t charging_hose_front = 0x04; // second (charging) pipe hose disconnected
inline constexpr std::uint8_t charging_hose_rear = 0x08;
inline constexpr std::uint8_t ep_cable_front = 0x10; // EP line disconnected
inline constexpr std::uint8_t ep_cable_rear = 0x20;
inline constexpr std::uint8_t pipe_cock_front = 0x40; // end cock closed
inline constexpr std::uint8_t pipe_cock_rear = 0x80;
// Valves on a car (ts.h Wagon Flags1, RTS +32): the emergency valve vents
// the pipe at 18 atm/s; the pulled release (bleed) valve empties the brake
// cylinder at 0.5 atm/s (RTS 0x424488).
inline constexpr std::uint8_t valve_emergency = 0x10;
inline constexpr std::uint8_t valve_release = 0x80;
// Brake shoes (drags) under a car's bogie towards the train front / rear
// (RTS +31 bits 1, 2): each adds 10 kN of static brake force.
inline constexpr std::uint8_t chock_front = 0x01;
inline constexpr std::uint8_t chock_rear = 0x02;
inline constexpr float chock_force = 10000.0f;
// Coupler ends kept open after uncoupling, so the cars do not couple again
// on contact (RTS car +14 bits).
inline constexpr std::uint8_t coupler_open_front = 0x01;
inline constexpr std::uint8_t coupler_open_rear = 0x02;

struct TrainEvent {
    TrainEventKind kind{};
    std::uint32_t car{};
    std::uint8_t bogie{}; // 0 front, 1 rear
};

// Front and rear bogie (or car end) positions of a car on the track.
struct CarPlacement {
    TrackPoint front;
    TrackPoint rear;
};

// How a car has come off the rails (RTS 0x5059A8 / 0x580B30).
enum class DerailKind : std::uint8_t {
    none,
    tipping, // overturning sideways where it stands (curve overspeed, a tipped neighbour)
    sliding, // off the end of the track, sliding on along its heading
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
    // Brake pipe ends (pipe_hose_*, pipe_cock_*). A train's end cars start
    // with the outer hose disconnected and its end cock closed.
    std::uint8_t pipe_ends{};
    std::uint8_t coupler_open{}; // coupler_open_*
    std::uint8_t valves{};       // valve_*
    std::uint8_t chocks{};       // chock_*
    // The body swaying on its springs (RTS 0x4F5B34): offset from its rest
    // pose (m) along the curve's turning side (lateral), up (vertical) and
    // towards the train front (longitudinal), and the rates.
    float body[3]{};
    float body_rate[3]{};
    float hunting_phase{}; // rad, the lateral hunting oscillation
    float curvature{};     // 1/m, positive: turning towards the lateral axis
    // Derailment (RTS 0x580B30, 0x581BC4, 0x581694).
    DerailKind derail = DerailKind::none;
    float tilt{};          // rad, roll towards the lateral axis (tipping)
    float tilt_target{};   // rad, the roll it settles at
    float tilt_rate{};     // rad/s
    float tilt_friction{}; // N at the full target roll
    bool fallen = false;   // rolled past 0.8 rad: hoses torn off
    float slide{};         // m slid off the track (sliding), signed towards the train front
    float slide_speed{};   // m/s, signed towards the train front
    bool slide_origin_set = false;
    CarPlacement slide_from{}; // bogie points where it left the track
    float yaw{};           // rad, a slid car folding about its end towards the slide
    float yaw_target{};    // rad, the fold it settles at
    double travelled{}; // m, along the train front direction, for wheel animation
    // The overhead line (electric engines; set by Electrification::apply):
    // V, Hz. Negative voltage: not modelled.
    float line_voltage = -1.0f;
    float line_frequency{};
};


class Train {
public:
    explicit Train(std::vector<CarSetup> cars, TrainSettings settings = {});
    ~Train();
    Train(const Train&) = delete;
    Train& operator=(const Train&) = delete;

    // Puts the train with the front of car 0 at (node, offset), facing along
    // the node when `forward`; the cars are laid back along the track.
    // A train already placed first frees the junctions it held (vacate()).
    bool place(TrackNetwork& network, std::uint32_t node, double offset, bool forward, std::string* error = nullptr);
    // Takes the train off the track: frees the junctions under it and those
    // its tail still keeps for ClearanceDist. The train stays where it was
    // for drawing until placed again.
    void vacate();
    // Turns the train end for end while it stands (the driver walks to the
    // other end): the last car becomes car 0 and the train front is the old
    // rear. Every car keeps its place and orientation on the track, so each
    // one's `flipped` toggles; controllers move with their cars. Fails while
    // moving faster than 0.1 m/s.
    bool turn_around();
    // Uncouples behind car `index - 1` (RTS 0x469858): cars index.. become a
    // new train standing where they are; this train keeps the cars ahead.
    // The hoses at the cut are disconnected; `close_cocks` closes both end
    // cocks first (RTS's uncoupling from the menu), otherwise the open pipe
    // ends vent and both parts brake (a coupler break). `keep_open` leaves
    // the couplers open so the parts do not couple again on contact.
    // Controllers move with their cars. nullptr for an index out of range.
    [[nodiscard]] std::unique_ptr<Train> uncouple(std::size_t index, bool close_cocks, bool keep_open);
    // Couples `other` onto this train's front (`at_front`) or rear. `other`
    // touches with its front (`other_front`) or rear and is turned as needed
    // so the cars keep their places; `gap` is the distance between the two
    // ends. The hoses at the joint are connected and the cocks opened.
    // `other` is left empty and off the track. Returns false if either train
    // is not placed.
    bool couple(Train& other, bool at_front, bool other_front, double gap);
    // What a hand at the track changes on car `car` (RTS's car menu,
    // 0x463F84): set or clear `mask` in its pipe ends, coupler ends left
    // open, valves or brake shoes. A sleeping train wakes.
    enum class CarFlags : std::uint8_t { pipe_ends, coupler_open, valves, chocks };
    void set_car_flags(std::size_t car, CarFlags which, std::uint8_t mask, bool on);
    // Car `car`'s handbrake, 0..1.
    void set_handbrake(std::size_t car, float value);
    // A car has derailed (it tips over or slides off; placing the train
    // again puts everything back on the rails).
    [[nodiscard]] bool derailed() const noexcept;
    // Moves the whole train along the track by `metres` towards its front.
    void shift(double metres);
    // Adds `change` (m/s, towards the train front) to every car's velocity.
    void add_velocity(float change);
    // Total mass, kg.
    [[nodiscard]] double mass() const noexcept;
    // A hit on car `index` (RTS 0x589A14): `force` N along `direction`
    // (car axes: x towards the side the car turns to, y up, z towards the
    // train front), over `time` s. On the rails, above its DerailBufferForce
    // the car comes off and slides; below, the body on its springs takes the
    // shove (offset += direction * force / m * time). A derailed car hit from
    // the side starts to roll: target (F / m * t * 0.04 + 0.1), rate
    // |F / m * t / width|.
    void hit(std::size_t index, float force, const float (&direction)[3], float time);
    // A sliding car ran into an obstacle and stops sliding.
    void stop_slide(std::size_t index);
    [[nodiscard]] bool placed() const noexcept { return network_ != nullptr; }

    // Controllers drive engines; cars without one get no demands.
    void set_controller(std::size_t car, std::unique_ptr<LocomotiveController> controller);
    // Bogie distances from the car centre, along the train (front, rear).
    void set_bogies(std::size_t car, float ahead, float behind);
    [[nodiscard]] LocomotiveController* controller(std::size_t car);

    // One step of train_step seconds; nothing while asleep.
    void step();
    // A STATIC consist (RTS train type 0, 0x424488): not simulated at all;
    // it keeps its place, its cars stand and their brakes stay as they
    // were. It still occupies the track, couples and is hit like any train;
    // the owner wakes it when something moves it (RTS: it becomes LOOSE).
    void set_asleep(bool asleep) noexcept;
    [[nodiscard]] bool asleep() const noexcept { return asleep_; }

    [[nodiscard]] const std::vector<CarState>& cars() const noexcept { return cars_; }
    [[nodiscard]] std::vector<CarPlacement> placements() const;
    // A traveller at the centre of each car, facing the train front; empty
    // while the train is not on the track.
    [[nodiscard]] std::vector<TrackTraveller> car_centres() const;
    // The overhead line at car `car`'s pantographs (CarState::line_voltage).
    void set_line_supply(std::size_t car, float voltage, float frequency);
    // The front end of car 0, facing the train front; the rear end of the
    // last car, facing the train front too.
    [[nodiscard]] std::optional<TrackTraveller> front_end() const;
    [[nodiscard]] std::optional<TrackTraveller> rear_end() const;
    // The end that led the last movement (the front when standing still at
    // the start), facing the direction of travel.
    [[nodiscard]] std::optional<TrackTraveller> leading_end() const;
    // The other end, facing away from the train.
    [[nodiscard]] std::optional<TrackTraveller> trailing_end() const;
    [[nodiscard]] TrackNetwork* network() const noexcept { return network_; }
    [[nodiscard]] std::vector<TrainEvent> take_events();
    [[nodiscard]] double speed() const noexcept { return cars_.empty() ? 0.0 : cars_.front().velocity; }
    [[nodiscard]] double length() const noexcept;
    [[nodiscard]] const TrainSettings& settings() const noexcept { return settings_; }
    // Coupler solver sub-steps (1 = RTS); takes effect on the next step.
    void set_coupler_substeps(std::uint32_t substeps) noexcept;
    // The air brake model from the next step on.
    void set_brake_model(BrakeModel model) noexcept;
    // The model this step runs: realistic only when no controller needs RTS's.
    [[nodiscard]] BrakeModel brake_model() const noexcept;
    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }

private:
    void reverse_cars();
    void sway();
    void start_tipping(std::size_t index, const CarState* neighbour);
    void derailment();
    void occupy_under();
    void advance_clearance(const MoveResult& leading, const MoveResult& trailing);
    void move_and_update(double front_moved, double rear_moved);

    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<CarState> cars_;
    TrainSettings settings_;
    TrackNetwork* network_ = nullptr;
    std::uint64_t id_;
    bool asleep_ = false;
};

// What happened where two trains meet (RTS train update 0x424488).
enum class ContactOutcome : std::uint8_t {
    coupled,   // automatic couplers met gently: `absorbed` joined `survivor`
    buffers,   // the couplers could not couple: the trains pushed each other
    collision, // the impact exceeded DerailBufferForce
};

struct ContactResult {
    ContactOutcome outcome{};
    Train* survivor = nullptr;
    Train* absorbed = nullptr;   // coupled: now empty, to be removed
    bool absorbed_ahead = false; // coupled: its cars went in front
    bool absorbed_turned = false; // coupled: its car order was reversed
    double force{};              // N, the impact measure RTS compares
};

// Finds trains whose ends will meet within the next step and resolves each
// meeting once: couples them when both couplers are automatic and not left
// open and the impact is at most both cars' DerailBufferForce (the earlier
// train in `trains` survives), otherwise exchanges momentum inelastically.
std::vector<ContactResult> resolve_contacts(std::span<Train* const> trains);

// The nearest end of another train in `trains` facing `train`'s front (or
// rear) end along the track within `reach` metres: that train, which of its
// ends, and the gap between the two ends (m).
struct EndContact {
    Train* other = nullptr;
    bool other_front = false;
    double gap{};
};
[[nodiscard]] std::optional<EndContact> end_contact(const Train& train, bool front, std::span<Train* const> trains, double reach);

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
