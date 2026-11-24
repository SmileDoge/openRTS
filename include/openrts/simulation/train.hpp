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
#include <openrts/simulation/adhesion.hpp>
#include <openrts/simulation/body_motion.hpp>
#include <openrts/simulation/car_registry.hpp>
#include <openrts/simulation/cab_brakes.hpp>
#include <openrts/simulation/distributor.hpp>
#include <openrts/simulation/state_text.hpp>
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
    // The bodies on their springs (body_motion.hpp): RTS's model (the
    // default here) or openRTS's (the viewer's setting).
    BodyMotionModel body_motion = BodyMotionModel::rts;
    // openRTS's body motion: the push aside at switches, a factor (0: none).
    float switch_throws = 1.0f;
    // openRTS: the rails' state for the wheels' adhesion (adhesion.hpp): rts
    // keeps RTS's traction limit; any other scales it, and under the
    // realistic air the brakes slide (юз) and the shoes wet and warm.
    RailCondition rails = RailCondition::rts;
    // openRTS, the realistic air: a bogie's full brake force at most what
    // real stock with its shoes gives (shoe_force_limit), for content that
    // brakes far harder than the real thing.
    bool limit_brake_force = false;
    // openRTS, the realistic air: the shoes' force of every car as a factor
    // (тормозное нажатие, 0.5 .. 1.5; 1: as the files give it).
    float brake_force_factor = 1.0f;
    // openRTS: the RTS triple valve gets a distributor's stability (мягкость):
    // a slow pipe fall applies nothing (docs/train.md). Off: as RTS, which
    // applies on any fall below the reservoir, the stabiliser's too.
    bool distributor_stability = false;
};

struct CarSetup {
    const formats::WagonDefinition* definition = nullptr;
    // The car's identity in its world (car_registry.hpp); none: a car only
    // simulated (a test run), not one of the world's.
    CarId id{};
    bool flipped = false;
    float handbrake{}; // 0..1 (consist HandbrakePercent / 100)
    // The front and rear bogie's brake shoes (the consist's ORTSBrakeShoes);
    // unknown: the car's own (simulation::shoe_of).
    std::array<formats::BrakeShoe, 2> shoes{};
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
    EpCommand ep_command = EpCommand::target; // the 395's ЭПТ command (realistic air), driver_valve.hpp
    float handbrake{};           // percent (HandbrakePercent)
    // The controller works the handbrake (a DLL: RTS sets HandbrakeValue =
    // HandbrakePercent / 100 every step, so the cab releases it); otherwise
    // `handbrake` only applies it on top of the car menu's.
    bool sets_handbrake = false;
    std::uint16_t systems = 1;   // BrakeSystemsEngaged: 1 pipe, 2 EP, 4 charging pipe
    bool release_train_brake = false;       // EngineFlags 1
    bool release_independent_brake = false; // EngineFlags 2
    // Realistic air only: the equalising reservoir the driver's valve holds
    // (the pipe follows it through the relay; < 0: this engine's valve is
    // not working the pipe), and position VI.
    float equalising = -1.0f;
    // The УР that valve keeps whether or not it works the pipe (the cab's
    // УР gauge, F5, the tape): cut off the pipe by double traction or the
    // 367 it goes on holding its own УР. < 0: no valve of the model here.
    float equalising_kept = -1.0f;
    bool emergency = false;
    // The relay may feed the pipe (not in III: ValveOutput::feed).
    bool pipe_feed = true;
    // I: the main reservoir feeds the pipe straight, past the relay
    // (ValveOutput::direct).
    bool pipe_direct = false;
    // openRTS's cab brake devices in place of a DLL's (cab_brakes.hpp): what
    // they did this step, for the cab's gauges and lamps.
    std::optional<CabBrakeOutput> cab_brakes;
    // Engine::Sanding: adhesion x the .eng's sanding factor (Adheasion's
    // third value) while the sand lasts (RTS 0x424488).
    bool sanding = false;
    // Engine::EngineForce: each traction motor's share of `force` (the
    // .eng's EngineAxles say whose axles); none: the locomotive as one.
    std::array<float, 8> engine_forces{};
    std::uint8_t engine_force_count = 0;
    // W the engine draws from the line (ElectricEngine::PowerConsuming);
    // < 0: its traction power |F v|.
    float power = -1.0f;
};

// What a controller sees of its locomotive.
struct EngineInputs {
    float speed{};          // m/s along the car's own front
    float train_pipe{};     // atm, at this car
    float main_reservoir{}; // atm
    float cylinder{};       // train brake cylinder, atm
    float independent{};    // independent brake cylinder, atm
    // Realistic air: how fast its driver's valve's relay fed the pipe at
    // this car over the last step, atm/s (CarState::valve_feed).
    float valve_feed{};
    // Its distributor's ТЦ channel and additional discharge (CarState's).
    float distributor_channel{};
    bool distributor_discharge = false;
    bool flipped = false;   // the car faces the train rear
    const formats::WagonDefinition* definition = nullptr;
    bool realistic_air = false; // the train runs BrakeModel::realistic
    // The rails' adhesion as a share of RTS's (TrainSettings::rails, the
    // treads' wetness): the traction limit's factor.
    float adhesion = 1.0f;
    // The line at the pantographs (electric engines; Locomotive::LineVoltage,
    // LineFreq): V and Hz. Negative voltage: the route's electrification is
    // not modelled, the engine has power anywhere.
    float line_voltage = -1.0f;
    float line_frequency{};
};

// One of a car's systems (cars as entities, step 4, docs/car-architecture.md):
// what drives an engine. The keyboard driver, a legacy DLL (its adapter), an
// openRTS plugin; later the replaceable train systems. A step it reads its
// car's signals (EngineInputs) and sets the car's demands (EngineDemands);
// the train's physics, air and couplers do the rest.
class CarModule {
public:
    virtual ~CarModule() = default;
    // Its kind ("keyboard", "dll", "plugin"): a saved block is its name's.
    [[nodiscard]] virtual std::string_view name() const = 0;
    // Its saved block's layout; a block of another version is not read.
    [[nodiscard]] virtual int version() const { return 1; }
    // Whether it reads a block `saved` wrote (the same version by default;
    // a newer module may read its older blocks).
    [[nodiscard]] virtual bool reads(const int saved) const { return saved == version(); }
    virtual void step(double dt, const EngineInputs& inputs, EngineDemands& demands) = 0;
    // The module's valve is made for RTS's triple valve (legacy DLLs):
    // the train keeps BrakeModel::rts.
    [[nodiscard]] virtual bool rts_brakes() const { return false; }
    // A saved game (Train::save_state): what the module keeps between steps
    // (its handles, its valve, its compressor's governor), under keys
    // starting with `prefix`. Nothing by default. A DLL's own state is the
    // DLL host's (LegacyConsist::save_state), not this.
    virtual void save(StateWriter& /*out*/, std::string_view /*prefix*/) const {}
    virtual void restore(const StateReader& /*in*/, std::string_view /*prefix*/) {}
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
    bool sanding = false;         // the sander held (every engine of the unit sands)
    float ep_target = 0.0f;       // ЭПТ cylinder demand (Engine::EPTvalue), kept by the master's valve, atm
};

// Built-in controller for driving without a cab. Its traction model is
// openRTS's own (from MaxForce / MaxPower), not RTS: in RTS a DLL does this.
// The brake valve is the 395 of the ВЛ10к DLL (tsrLoco_VL10k-1461,
// vl10k_main.cpp): written for the RTS triple valve, which it drives
// through Engine::UR and TrainPipeRate (docs/reverse/air-brakes-real.md §8).
// The ЭПТ layer is the ЭР9Т DLL's (tedStepanki_emu, er9t_main.cpp):
// EPTvalue ramps up in VЭ and V, holds in III/IV, ramps down in I/II.
// An engine's compressor as openRTS runs it (the keyboard controller, and
// DLL engines under the realistic air, whose DLLs keep RTS's main reservoir):
// on below 7.5 atm, off above 9; with the realistic air a КТ-6's 0.088
// m³·atm/s into the real reservoir volume. The rate for MainResRate, atm/s.
[[nodiscard]] float compressor_rate(bool& running, const EngineInputs& inputs);
// The same compressor's delivery while it runs, whoever governs it, atm/s.
[[nodiscard]] float compressor_delivery(const EngineInputs& inputs);
// The compressor governor of an engine, atm: {cut-in, cut-out}. The .eng's
// AirBrakesCompressorRestartPressure and AirBrakesMainMaxAirPressure (a
// ЭД4М's real one is 6.5-8.0, a locomotive's 7.5-9.0), else 7.5 / 9.0.
struct CompressorRange {
    float on = 7.5f;
    float off = 9.0f;
};
[[nodiscard]] CompressorRange compressor_range(const formats::EngineDefinition* engine) noexcept;

class KeyboardController final : public CarModule {
public:
    using BrakeHandle = DriverHandles::BrakeHandle;

    KeyboardController(std::shared_ptr<DriverHandles> handles, bool master);
    [[nodiscard]] std::string_view name() const override { return "keyboard"; }
    void step(double dt, const EngineInputs& inputs, EngineDemands& demands) override;
    void save(StateWriter& out, std::string_view prefix) const override;
    void restore(const StateReader& in, std::string_view prefix) override;

private:
    std::shared_ptr<DriverHandles> handles_;
    bool master_;
    bool compressor_ = false;
};

class Train;
// Puts keyboard controllers on every engine of `train`, engine `master` (the
// first one when it is not an engine) master. Returns the shared handles, or
// nullptr when the train has no engine.
std::shared_ptr<DriverHandles> attach_keyboard_driver(Train& train, std::size_t master = SIZE_MAX);

enum class TrainEventKind : std::uint8_t {
    joint,        // a bogie passed a section boundary
    junction,     // a bogie passed a junction
    end_of_track, // the train ran onto an end node and stopped
    coupler_break, // the coupler behind `car` broke (the owner uncouples there)
    derailment,
    wheel_flat,    // a wheel set of `bogie` struck the rail at its flat (`amount`: the flat's depth, mm)
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
// Coupler ends broken in a collision (RTS car +14 bits 0x10 / 0x20, set by
// 0x58A07C when the impact passes the end's Break force): they do not couple
// until fixed (the car menu's "Fix coupling", 120 s).
inline constexpr std::uint8_t coupler_broken_front = 0x10;
inline constexpr std::uint8_t coupler_broken_rear = 0x20;
// Multiple-unit cables between cars (ts.h Wagon Flags byte 3,
// WagonData::Cables; RTS 0x10000 / 0x20000 SMECable, 0x40000 / 0x80000
// HVBus): connected at the front / rear end. The control cable joins engines
// into one multiple unit, the high-voltage bus shares the line.
inline constexpr std::uint8_t cable_sme_front = 0x01;
inline constexpr std::uint8_t cable_sme_rear = 0x02;
inline constexpr std::uint8_t cable_hv_front = 0x04;
inline constexpr std::uint8_t cable_hv_rear = 0x08;

struct TrainEvent {
    TrainEventKind kind{};
    std::uint32_t car{};
    std::uint8_t bogie{}; // 0 front, 1 rear (wheel_flat: the car's own ends)
    std::uint8_t axle{};  // wheel_flat: the bogie's axle, 0 or 1
    float amount{};       // wheel_flat: mm
};

// Wheel flats (ползуны): a wheel set sliding locked (юз) wears a flat on
// its treads, about 1 mm in 15 m at 20 t an axle, deeper the heavier the
// axle, at most flat_deepest. It never wears away by itself: each turn after,
// the bogie's wheel sets strike the rail at it (a knock, a kick to the body)
// until they are changed (Train::replace_wheelsets).
constexpr float flat_per_metre = 0.065f; // mm a metre slid, at 20 t an axle
constexpr float flat_deepest = 12.0f;    // mm
constexpr float flat_struck = 0.2f;      // mm: from here a flat strikes the rail

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

// How worn a car's brakes are under the realistic air (docs/train.md,
// "Brake condition"): drawn for every car from the train's condition 1..10
// (Train::set_brake_condition); as new, nothing changes.
struct BrakeWear {
    float pipe_leak{};      // atm/min its pipe loses by itself at 5 atm (over the file's TrainPipeLeakRate)
    float reservoir_leak{}; // atm/min an engine's main reservoir loses at 9 atm
    float cylinder_leak{};  // atm/min its cylinder loses at 3.8 atm
    float pace = 1.0f;        // its distributor's fill and release, a factor
    float sensitivity = 1.0f; // the pipe's fall that applies it, a factor (> 1: a deeper one)
    float rigging = 1.0f;     // the rigging's share of the shoes' force
    bool operator==(const BrakeWear&) const = default;
};

// State of one car (the ts.h Wagon fields RTS simulates on).
struct CarState {
    const formats::WagonDefinition* definition = nullptr;
    // Its identity (CarSetup::id): moves with the car through couplings,
    // cuts and turns, as the car's state does.
    CarId id{};
    bool flipped = false;
    // The brake shoes of its front and rear bogie (its own ends): their
    // friction under the realistic air (brake_shoes.hpp).
    std::array<formats::BrakeShoe, 2> shoes{};
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
    EpCommand ep_command = EpCommand::target; // with ep >= 0: the line's command (driver_valve.hpp)
    float ep_fill = 1.0f; // atm/s its ЭВР 305 fills at (scatter_ep_fill)
    float handbrake{}; // 0..1
    float main_reservoir{}; // engines
    // Realistic air: how fast the engine's driver's valve (its relay, I's
    // wide channel) fed the pipe at this car over the last air step, atm/s
    // (the cab's air sounds; Brakes.dll's TrainPipeRate > 0).
    float valve_feed{};
    // Realistic air: its distributor's ТЦ channel (atm) and whether an
    // application began with its additional discharge over the last air
    // step: the 418's ДТЦ and ДДР (cab_brakes.hpp).
    float distributor_channel{};
    bool distributor_discharge = false;
    // Bogie positions from the car centre towards the train front / rear, m
    // (RTS takes them from the shape's BOGIE nodes); 0: 35 % of the length.
    float bogie_ahead{};
    float bogie_behind{};
    // Wheel slip under traction (боксование, RTS 0x424488): a bit per
    // slipping axle (15: the whole locomotive), what the DLL reads as
    // Engine::Wheelslip; and each axle's force over its adhesion (signed
    // along the car's own front, as the force), which spins its wheels
    // (0x4F5B34).
    std::uint32_t wheelslip{};
    std::array<float, 8> slip_ratio{};
    // With the rails other than "as RTS" (openRTS's traction limit): every
    // axle's force over its adhesion in slip_ratio, this many of them (0:
    // RTS's limit, slip_ratio for the slipping axles alone).
    std::uint8_t axles{};
    // The RTS air: the brake holds more than the adhesion, the wheels stand
    // (RTS 0x4F5B34; the realistic air slides by bogie, shoe_state).
    bool brake_locked = false;
    // Wheel flats by bogie (its own front, rear), mm; where each of its two
    // axles has its flat, a share of a turn; and `rolled` when they were
    // last looked at.
    std::array<float, 2> flat{};
    std::array<std::array<float, 2>, 2> flat_phase{};
    std::array<double, 2> flat_seen{};
    bool derailed = false;
    // Brake pipe ends (pipe_hose_*, pipe_cock_*). A train's end cars start
    // with the outer hose disconnected and its end cock closed.
    std::uint8_t pipe_ends{};
    std::uint8_t coupler_open{}; // coupler_open_*, coupler_broken_*
    std::uint8_t cables{};       // cable_*
    std::uint8_t valves{};       // valve_*
    std::uint8_t chocks{};       // chock_*
    // The distributor's switches (realistic air only; the car menu at a bogie).
    DistributorModes distributor{};
    // Its brakes' wear (realistic air only; Train::set_brake_condition).
    BrakeWear wear{};
    // The body swaying on its springs (RTS 0x4F5B34): offset from its rest
    // pose (m) along the curve's turning side (lateral), up (vertical) and
    // towards the train front (longitudinal), and the rates.
    float body[3]{};
    float body_rate[3]{};
    float hunting_phase{}; // rad, the lateral hunting oscillation
    // openRTS's body motion (TrainSettings::body_motion): its state, and
    // the body's roll, pitch and yaw (rad, BodyMotion::turn; 0 with RTS's).
    // body[] holds its shifts then.
    BodyMotion motion{};
    float body_turn[3]{};
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
    // The current at its pantographs (Locomotive::LineCurrent), A, and the
    // wire's heat under them (RTS +416: grows by the current over the wire's
    // BurnQ, cools by MaxLineCurrentVelocityIncrease); a burnt wire
    // (LocoFlags 0x200) gives it no line for 60 s, then `line_heat` counts that.
    float line_current{};
    float line_heat{};
    bool line_burnt = false;
    float power{};              // W the engine drew last step (EngineDemands::power)
    // Sanding (RTS 0x424488): the sand left (s, MaxSandingTime; < 0
    // unlimited), and the effect lasting over the engine's length of travel
    // after the sanding stopped.
    bool sanding = false;
    float sand_left = -1.0f;
    float sand_travel = -1.0f;  // m since the sanding stopped, < 0: no effect left
    // Under the realistic air: each bogie's shoes and treads (wet, warm,
    // sliding: adhesion.hpp) and the distance the car slid (юз), m.
    std::array<ShoeState, 2> shoe_state{};
    float slid{};
    [[nodiscard]] bool sliding() const noexcept { return shoe_state[0].sliding || shoe_state[1].sliding; }
    // How far each bogie's wheels (its own front, rear) have turned, m along
    // the train front as `travelled`: a sliding bogie's wheels stand (юз).
    std::array<double, 2> rolled{};
    // Adds a run of `metres` to `travelled` and to the wheels that roll.
    void run(const double metres) noexcept {
        travelled += metres;
        for (std::size_t b = 0; b < 2; ++b)
            if (!shoe_state[b].sliding && !brake_locked) rolled[b] += metres;
    }
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
    // ends. Only the couplers join: the hoses, the EP cable and the end
    // cocks at the joint stay as they were (hanging, closed) for the driver
    // to join (set_car_flags). `other` is left empty and off the track.
    // Returns false if either train is not placed.
    bool couple(Train& other, bool at_front, bool other_front, double gap);
    // What a hand at the track changes on car `car` (RTS's car menu,
    // 0x463F84): set or clear `mask` in its pipe ends, coupler ends left
    // open, valves or brake shoes. A sleeping train wakes.
    enum class CarFlags : std::uint8_t { pipe_ends, coupler_open, valves, chocks, cables };
    void set_car_flags(std::size_t car, CarFlags which, std::uint8_t mask, bool on);
    // A car of a train another computer runs, derailed as it says (0x4636):
    // tipping towards `tilt_target`, `tilt` the roll now; not derailed puts
    // it back upright.
    void show_car_derailed(std::size_t car, bool derailed, float tilt, float tilt_target);
    // The roll of such a car eased towards its target (rad/s).
    void ease_shown_derail(float elapsed, float rate);
    // Car `car`'s handbrake, 0..1.
    void set_handbrake(std::size_t car, float value);
    // Car `car`'s distributor switches (the realistic air model reads them).
    void set_distributor_modes(std::size_t car, const DistributorModes& modes);
    // Every train fills its cylinders by the ЭПТ at a pace of its own, so a
    // driver gets used to each train's steps (testers, 2026-11-17): the
    // train 0.8-1.2 atm/s, each car within 5 % of it, 0.75-1.25 in all.
    // Drawn from `seed` (a new train: a random one); 1 atm/s until called.
    void scatter_ep_fill(std::uint32_t seed);
    // The brakes' condition (docs/train.md, "Brake condition"): 10 as from
    // the works, 1 due for an overhaul, 0 none (every car as its files give
    // it). Each car's wear (CarState::wear) is drawn from `seed` and the
    // condition: the worse it is, the more the cars leak (pipe, cylinder, an
    // engine's main reservoir), the slower and less alike their
    // distributors, the weaker their rigging; even at 10 they differ a
    // little. The realistic air only.
    void set_brake_condition(int condition, std::uint32_t seed);
    // Another condition, the train's own seed kept.
    void set_brake_condition(int condition) { set_brake_condition(condition, brake_seed_); }
    [[nodiscard]] int brake_condition() const noexcept { return brake_condition_; }
    [[nodiscard]] std::uint32_t brake_seed() const noexcept { return brake_seed_; }
    // TrainSettings::brake_force_factor.
    void set_brake_force_factor(float factor) noexcept { settings_.brake_force_factor = factor; }
    // The train's brake pressing (тормозное нажатие) under the realistic air:
    // its cars' full shoe force at 40 km/h as cast-iron shoes would need to
    // press for it (their friction then 0.126), in tonnes-force per 100
    // tonnes of the train's weight: as the brake certificate counts it.
    [[nodiscard]] double brake_pressing() const;
    // The brake shoes of car `car`'s front and rear bogie (CarState::shoes);
    // a bogie whose shoes change gets new ones (shoe_state as new).
    void set_brake_shoes(std::size_t car, const std::array<formats::BrakeShoe, 2>& shoes);
    // New shoes of the same kind on car `car`'s bogie `bogie` (0 front, 1
    // rear): not worn, cold, dry, not alight.
    void replace_brake_shoes(std::size_t car, std::size_t bogie);
    // New wheel sets under bogie `bogie` (its own end): no flats.
    void replace_wheelsets(std::size_t car, std::size_t bogie) { set_wheel_flat(car, bogie, 0.0f); }
    // A flat of `mm` there (a saved situation, a test); its axles' at two
    // places of the turn.
    void set_wheel_flat(std::size_t car, std::size_t bogie, float mm);
    // Car `car`'s air, atm (a DLL's Init sets it; RTS's cars start empty).
    struct Air {
        float pipe{}, charging_pipe{}, cylinder{}, auxiliary{}, control{}, main_reservoir{};
    };
    void set_air(std::size_t car, const Air& air);
    // A car has derailed (it tips over or slides off; placing the train
    // again puts everything back on the rails).
    [[nodiscard]] bool derailed() const noexcept;
    // Moves the whole train along the track by `metres` towards its front;
    // the cars roll that far (CarState::travelled, the wheels turn).
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

    // Modules drive engines; cars without one get no demands. They move
    // with their cars (a cut, a coupling, a train turned).
    void set_module(std::size_t car, std::unique_ptr<CarModule> module);
    // Bogie distances from the car centre, along the train (front, rear).
    void set_bogies(std::size_t car, float ahead, float behind);
    [[nodiscard]] CarModule* module(std::size_t car);

    // One step of train_step seconds; nothing while asleep.
    void step();
    // A STATIC consist (RTS train type 0, 0x424488): not simulated at all;
    // it keeps its place, its cars stand and their brakes stay as they
    // were. It still occupies the track, couples and is hit like any train;
    // the owner wakes it when something moves it (RTS: it becomes LOOSE).
    void set_asleep(bool asleep) noexcept;
    // A saved game: what changes as the train runs - where its ends stand,
    // each car's motion, air, flats, hoses, cocks, chocks and shoes, the
    // realistic air's distributors and the controllers' handles and valves
    // (state_text.hpp; keys "head", "c3.pipe", "a3.braking", "k0.throttle").
    // restore_state() puts it back into a train placed with the same cars;
    // a key missing keeps what is there. The bodies' sway, a derailment and
    // the wire's heat are not kept: they start at rest.
    void save_state(StateWriter& out) const;
    void restore_state(const StateReader& in);
    [[nodiscard]] bool asleep() const noexcept { return asleep_; }
    // A traffic train (RTS train type TRAFFIC, 0x424488, docs/traffic.md):
    // one rigid body on the track. Each step its speed changes by the
    // acceleration its driver set (set_drive_acceleration), stopping at 0
    // rather than turning back, and every car moves with it; no traction,
    // brakes, air, couplers, resistance or derailment. A derailed train runs
    // the full physics again.
    void set_kinematic(bool kinematic) noexcept;
    [[nodiscard]] bool kinematic() const noexcept { return kinematic_; }
    // m/s^2 towards the train front, for the next steps (kinematic only).
    void set_drive_acceleration(float acceleration) noexcept { drive_acceleration_ = acceleration; }

    [[nodiscard]] const std::vector<CarState>& cars() const noexcept { return cars_; }
    // The train's cars by identity, front to rear: the ordered list the
    // train refers to its cars by (CarState::id, as cars()).
    [[nodiscard]] std::vector<CarId> car_ids() const;
    // Where the car of `id` is in this train; none: not in it.
    [[nodiscard]] std::optional<std::size_t> index_of(CarId id) const noexcept;
    // The force each joint passes after the last step (tension +; joint j is
    // behind car j). On the move it is what the cars behind it need (Newton:
    // each one's m·a less its own forces, its brake and resistance against
    // its motion). RTS's CF (CarState::coupler_force, the stiffness sum
    // times the gap, kept for the break check) stops at the springs' full
    // stretch and the rest goes through the solver's rigid stop. Standing:
    // half of RTS's CF, the force each car gets.
    [[nodiscard]] std::vector<float> joint_forces() const;
    [[nodiscard]] std::vector<CarPlacement> placements() const;
    // Each car's ends on the track, coupler to coupler (`front` towards the
    // train front): the whole car, where placements() gives its bogies.
    [[nodiscard]] std::vector<CarPlacement> car_ends() const;
    // A traveller at the centre of each car, facing the train front; empty
    // while the train is not on the track.
    [[nodiscard]] std::vector<TrackTraveller> car_centres() const;
    // The overhead line at car `car`'s pantographs (CarState::line_voltage).
    void set_line_supply(std::size_t car, float voltage, float frequency);
    // Its line current and wire heat (Electrification::apply).
    void set_line_load(std::size_t car, float current, float heat, bool burnt);
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
    // What car `car`'s controller asked at the last step (its УР, ЭПТ ...);
    // null past the end.
    [[nodiscard]] const EngineDemands* demands_of(std::size_t car) const noexcept;
    // Cars in emergency under the realistic air (their accelerators fired,
    // the pipe not charged again yet); 0 with RTS's air.
    [[nodiscard]] std::size_t emergency_cars() const noexcept;
    void set_distributor_stability(bool on) noexcept { settings_.distributor_stability = on; }
    // The rails' state (TrainSettings::rails).
    void set_rails(RailCondition rails) noexcept { settings_.rails = rails; }
    // TrainSettings::limit_brake_force.
    void set_limit_brake_force(bool on) noexcept { settings_.limit_brake_force = on; }
    // The body motion model from the next step on (the bodies start at rest).
    void set_body_motion(BodyMotionModel model) noexcept;
    // TrainSettings::switch_throws.
    void set_switch_throws(float factor) noexcept { settings_.switch_throws = factor; }
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
    void step_kinematic();

    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<CarState> cars_;
    TrainSettings settings_;
    TrackNetwork* network_ = nullptr;
    std::uint64_t id_;
    int brake_condition_ = 0;
    std::uint32_t brake_seed_ = 0;
    bool asleep_ = false;
    bool kinematic_ = false;
    float drive_acceleration_ = 0.0f;
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
// `passive` (by train, optional): trains another program moves (the
// network's remote trains); two of them never meet here.
std::vector<ContactResult> resolve_contacts(std::span<Train* const> trains, std::span<const std::uint8_t> passive = {});

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
