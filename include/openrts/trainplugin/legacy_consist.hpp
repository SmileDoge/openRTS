#pragma once

// A consist whose locomotives run legacy RTS DLLs (docs/train-dll.md): owns
// the x86 host for the consist, builds its mirrors from the .eng / .wag /
// cab .sd files, and per simulation step sends the cars' state and collects
// what the DLLs did. Locomotive controllers made by controller() turn those
// outputs into the simulation's EngineDemands.
//
// Use: create() -> enter(player car) -> every step: step(train) before
// train.step(). The consist must outlive the train's controllers' use of it
// (they turn inert when it is gone).

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <openrts/core/vfs.hpp>
#include <openrts/dllhost/protocol.hpp>
#include <openrts/formats/cab_description.hpp>
#include <openrts/formats/rolling_stock.hpp>
#include <openrts/formats/route_database.hpp>
#include <openrts/formats/track_database.hpp>
#include <openrts/simulation/cab_brakes.hpp>
#include <openrts/simulation/klub.hpp>
#include <openrts/simulation/train.hpp>
#include <openrts/trainplugin/legacy_host.hpp>

namespace openrts::trainplugin {

// The air of trains with legacy DLLs under openRTS's realistic air model
// (docs/train.md, "Realistic air with DLL locomotives"; never with Legacy).
enum class DllAir : std::uint8_t {
    rts,       // as RTS: the DLLs' valves work the train's air themselves
    dll_valve, // the DLL's valve: its УР (Engine::UR) into the model's relay
    own_valve, // openRTS's 395 on the cab's handle (Brakes.dll's BR_395ID1), the DLL's direct vents (the ЭПК) kept
};
struct DllAirSetting {
    DllAir mode = DllAir::rts;
    // openRTS's cab brake devices in place of Brakes.dll's on the realistic
    // air (simulation/cab_brakes.hpp, docs/brakes-dll-replacement.md step
    // 2), for every car whose Brakes.dll parameters openRTS knows
    // (BrakesParameters); their numbers the books' (CabBrakeNumbers::openrts)
    // or Brakes.dll's.
    bool own_brakes = false;
    bool book_numbers = false;
};

// What a cab's Brakes.dll controls tell openRTS's valve (own_valve).
struct CabBrakes {
    int position = -1; // the 395's (simulation::ValvePosition), -1 not known
    int raw = -1;      // its control's state
    int controller_raw = -1; // the КПТ's (BR_KPTID1): the handle moved last sets the position
    float charging{};  // the reducer's charging pressure (BR_REDID1), atm; 0: none
    int combined = 0;  // the combined valve (BR_KKID1): 0 normal, 1 double traction (cut out), 2 emergency
    // The blocking device усл. 367 (BR_367ID1) out of its operating state
    // (BR_367OPPOL): the 395 is cut off the pipe.
    bool blocked = false;
    // The separating cock (BR_RAZID1 with BR_RAZOBSHK) shut: the 395 off the
    // brake pipe (Brakes.dll charges the pipe only while it is not 0).
    bool separated = false;
};
// The cab's controls read once (brake_cab_controls): per state, the 395's
// position (valve_states; empty: I..VI), the reducer's pressure ("[4.0]" ..
// "[5.9]", 0 not known), the combined valve's kind ("Двойная тяга",
// "Нормальное", "Экстренное").
struct BrakeCabControls {
    std::vector<std::int8_t> valve;
    std::vector<std::int8_t> controller; // the КПТ's, as the valve's
    std::vector<float> reducer;
    std::vector<std::int8_t> combined;
    // The tables of the locomotive's other cabs (BrakesLibrary::cabs from
    // the second on): the same cab file, their own switches.
    std::vector<BrakeCabControls> more;
    [[nodiscard]] const BrakeCabControls& of_cab(const std::size_t k) const { return k == 0 || k > more.size() ? *this : more[k - 1]; }
    // From the switches' states now; `last` the reading before (an unknown
    // state keeps its value).
    [[nodiscard]] CabBrakes read(const formats::EngineDefinition::BrakesLibrary& ids,
                                 const std::vector<dllhost::SwitchState>& switches, const CabBrakes& last = {}) const;
};
[[nodiscard]] BrakeCabControls brake_cab_controls(const formats::EngineDefinition::BrakesLibrary& ids,
                                                  const formats::CabDescription& cab);
// Brakes.dll's working cab of a locomotive with several in one car
// (BR_NUMCABS, the ЭП20's): the first whose blocking device 367
// (BR_367ID<k>) stands in BR_367OPPOL, as Brakes.dll's Run looks for it
// (decompiled, 2026-11-18); none: the first (k from 0).
[[nodiscard]] std::size_t working_cab(const formats::EngineDefinition::BrakesLibrary& ids,
                                      const std::vector<dllhost::SwitchState>& switches);

// A car's Brakes.dll parameters (its AuxLibrary's BR_*) as openRTS's cab
// brake devices take them (simulation/cab_brakes.hpp,
// docs/brakes-dll-replacement.md, step 2): each cab's switches and
// displays, and whether openRTS knows every one. A car with a parameter it
// does not know (a 334, a custom 292, the ЭП2Д's ППТ, BR_KID) keeps
// Brakes.dll; an id of -1 is no device.
struct BrakesParameters {
    bool present = false;
    std::string unknown;        // the first parameter openRTS does not know
    bool flipped = false;       // BR_FLIPCABS 1: the handles' states from I
    bool multiple_unit = false; // BR_MODE 1
    bool buffer_kvt = false;    // BR_BUFERKVT
    float charging{};           // BR_ZARPRESS, 0 not given
    std::int32_t block_operating = -1;     // BR_367OPPOL
    bool separating_fitted = false;        // BR_RAZOBSHK
    std::vector<std::int32_t> stop_valves; // BR_SK1 .. BR_SK<BR_SKNUM>
    struct Cab {
        std::int32_t valve = -1, controller = -1, reducer = -1, combined = -1, block = -1, separating = -1;
        std::int32_t independent = -1, independent_cock = -1, independent2 = -1, independent2_cock = -1;
        std::int32_t vk4 = -1, vk4_cock = -1;
        std::int32_t release = -1, emergency = -1; // ОТ, ЭТ
        std::int32_t ep_apply = -1, ep_release = -1, sensor418 = -1;
        std::int32_t gauge_cylinder = -1, gauge_pipe = -1, gauge_main_reservoir = -1, gauge_equalising = -1;
        std::int32_t lamp_ep_on = -1, lamp_ep_hold = -1, lamp_ep_brake = -1, lamp_pipe = -1;
        std::int32_t ep_volts = -1, ep_amps = -1;
    };
    std::vector<Cab> cabs; // BR_NUMCABS of them, at least one
    [[nodiscard]] bool understood() const noexcept { return present && unknown.empty(); }
};
[[nodiscard]] BrakesParameters brakes_parameters(const formats::EngineDefinition& engine);

// Brakes.dll's КЛУБ as openRTS's КЛУБ takes it (simulation/klub.hpp;
// docs/reverse/brakes-dll.md §11): its settings and the first cab's
// switches (KLUB_*1, PB_*1); an id of -1 is none.
struct KlubParameters {
    bool present = false;
    simulation::KlubSettings settings;
    std::vector<std::pair<std::int32_t, simulation::KlubPress>> keys; // KLUB_B*ID1
    std::vector<std::int32_t> power;                                  // KLUB_PIT1 .. KLUB_PIT<KLUB_PCOUNT>
    std::int32_t rb = -1, rbs = -1, epk_key = -1, cassette = -1, tskbm = -1, saut = -1;
    std::int32_t flip = -1;                       // PB_FLIP: the cab whose forward is the car's back (from 0)
    std::int32_t setup_index = -1;                // PB_SETUP's index in the library's parameters
    // Its displays: the screen (KLUB_DBIL1), the ALSN's lights (KLUB_DALS1,
    // KLUB_DPOM1 screens; KLUB_B1POM1 .. B8POM1 lamps), its lamp
    // (KLUB_PLAMP) and the cab's texture change for the backlight (KLUB_TCHNUM).
    std::int32_t screen = -1, dals = -1, dpom = -1, lamp = -1, backlight = -1;
    std::array<std::int32_t, 8> lamps{-1, -1, -1, -1, -1, -1, -1, -1};
    [[nodiscard]] const simulation::KlubPress* key(std::int32_t id) const noexcept;
};
[[nodiscard]] KlubParameters klub_parameters(const formats::EngineDefinition& engine);
// mdd_Libraries/Brakes/ChangeStanc.bto: the КЛУБ's station names (upper case).
[[nodiscard]] std::vector<std::pair<std::u16string, std::u16string>> load_klub_station_names(const core::Vfs& vfs);
// The working cab's devices for step_cab_brakes, as its switches stand.
// `valve`: its driver's valve's position as CabBrakes reads it (-1 not
// known: the state by BR_FLIPCABS); `exchange`: Brakes.dll's var + 32
// (protocol AuxExchange); `systems`: the engine's BrakeSystemsEngaged.
[[nodiscard]] simulation::CabBrakeControls cab_brake_controls(const BrakesParameters& parameters, std::size_t cab,
                                                              const std::vector<dllhost::SwitchState>& switches,
                                                              const CabBrakes& valve, dllhost::AuxExchange exchange,
                                                              std::uint16_t systems);
// What openRTS's devices show on a cab's gauges and lamps (displays by
// BrakesParameters' ids; the УР only in the working cab).
void show_cab_brakes(const BrakesParameters& parameters, std::size_t working, const simulation::CabBrakeOutput& shown,
                     std::vector<dllhost::DisplayState>& displays);

// A switch's AssignKey keys as RTS keeps them in ActSwitch::Key (+72, the
// loader 0x45AE90): per key, two at most, its virtual-key code and its
// modifiers (1 Shift, 2 Ctrl or Control, 4 Alt), the parts joined by '+'.
// RTS takes the code as VkKeyScanW's low byte (0x45B008, 0x45B19C; Space
// 0x20): a letter's upper case, a digit itself, ; ' [ ] , . / - = ` \ their
// VK_OEM codes (0xBA...). openRTS gives the US layout's codes, the physical
// keys its own key handling binds (RTS took the layout active at its start).
// DLLs read them: the MaDaDev Brakes.dll's Run springs the 254 handle and
// the driver's valve back from their unfixed positions unless
// GetAsyncKeyState of its key, or of the left mouse button, is down. Before
// 2026-11-16 openRTS gave the character itself, ';' 0x3B, ''' 0x27 (the
// right arrow's code), '[' 0x5B (the Windows key's): the 2ES6's valve in I
// and the 254 in its last position sprang back while held, the key's
// repeat put them there again (testers: they jerked and crackled).
void switch_key_bytes(const std::vector<std::string>& keys, std::uint8_t (&out)[4]);

struct LegacyCar {
    const formats::WagonDefinition* definition = nullptr;
    core::VirtualPath file; // its .eng / .wag (relative DLL and cab paths start here)
    std::uint32_t uid{};    // the .con UiD the DLLs see (not unique between consists)
    std::uint32_t key{};    // unique in the game (0: none): reconsist() follows the car by it
};

// What the world tells the DLLs each step.
struct LegacyWorld {
    double day_time = 12.0 * 3600.0; // s since midnight
    bool night = false;
    float air_temperature = 20.0f;   // °C
    // V and Hz at the pantographs for cars whose CarState carries no line
    // (the route's electrification not modelled).
    float line_voltage = 3000.0f;
    float line_frequency = 0.0f;
    dllhost::SpeedLimitInput route, signal, event; // m/s, m
    std::vector<dllhost::SignalInput> signals_ahead; // the next obeyed signal (Cabin::Signal)
    // ALSN's arrays (dllhost::StepRequest) and the next speed post.
    std::vector<dllhost::SignalInput> alsn_ahead, alsn_behind, alsn_passing;
    float next_limit = -1.0f;
    float next_limit_distance = -1.0f;
    // The route database the SignalInputs' units index (RTSSignalData), the
    // TDB the track item list names, and the route's SpeedLimit, m/s (a
    // resuming speed post's Value, RTS 0x482680).
    const formats::RouteDatabase* route_database = nullptr;
    const formats::TrackDatabase* tdb = nullptr;
    float route_speed_limit = 0.0f;
    // The speed posts' limits without the routes' +3 km/h
    // (simulation::without_tolerance) in RTSGetTrackItems too.
    bool limits_without_tolerance = false;
    // Every car's auxiliary libraries' variable stacks in its output
    // (CarOutput::aux_vars): the golden runs (openrts-train-check --record).
    bool record_aux_vars = false;
    // RTSGetTrackItems: the player's train's items (RTS 0x4808DC) and how
    // far ahead and behind the list reaches.
    std::vector<dllhost::TrackItemEntry> track_items;
    float items_range = 0.0f;
    // The SMS objects' trigger pair states (RTSIsSMSTriggerOn).
    std::vector<dllhost::SoundPairState> sound_pairs;
    // The player's train's car whose tape rolls (simulation/tape.hpp), -1 none.
    std::int32_t tape_car = -1;
};

class LegacyConsist {
public:
    struct Outputs {
        std::vector<dllhost::CarOutput> cars;
        bool valid = false;
        // Started cold (TrainState 0): the main reservoirs empty, and
        // openRTS's compressors (the realistic air) off until a DLL reports
        // a pantograph up, a diesel on, its own compressor or traction.
        bool cold = false;
        // openRTS's КЛУБ's ЭПК or КОН venting the pipe at a car (by the
        // index of `cars`; set_own_klub).
        std::vector<std::uint8_t> klub_vent;
    };

    // nullptr (with `error`) when no car has a DLL or the host cannot start.
    [[nodiscard]] static std::unique_ptr<LegacyConsist> create(const core::Vfs& vfs,
                                                               const std::filesystem::path& cache_root,
                                                               std::vector<LegacyCar> cars,
                                                               const std::filesystem::path& rts_root,
                                                               std::string* error,
                                                               const std::filesystem::path& host_executable = {},
                                                               const core::VirtualPath* route = nullptr);
    ~LegacyConsist();

    // The player takes locomotive `car` (Init of it and its multiple-unit group).
    // `state`: Init's start state as an activity's TrainState (0 cold, 1 on,
    // 2 / 3 running), -1 from the train (`running`).
    bool enter(std::size_t car, const simulation::Train& train, bool running = true, int state = -1);
    // Every car's air as the last enter() left it (Init writes the
    // pressures itself: RTS keeps them, LegacyCab::attach puts them in).
    [[nodiscard]] const std::vector<dllhost::CarAir>& entered_air() const noexcept { return entered_air_; }
    // A train just made, started cold (enter_new_train): Outputs::cold.
    void set_cold_start(bool cold) noexcept { outputs_->cold = cold; }
    // One round trip: the trains' state in (the player's first, as the
    // last reconsist ordered them), the DLLs' outputs out.
    bool step(double dt, const std::vector<const simulation::Train*>& trains, const LegacyWorld& world);
    bool step(double dt, const simulation::Train& train, const LegacyWorld& world) { return step(dt, {&train}, world); }
    // Cars were coupled on or cut off: `trains` are the host's trains now,
    // the player's first, then the parts cut off from it, whose DLLs keep
    // running as in RTS (cars matched by key); `sims` their trains. The DLLs
    // of the cars that stay keep their state; new engines join the groups
    // (bound and initialised as slaves); a car in no train keeps its state
    // until it comes back. When the player's engine is no longer in the
    // first train, player() is none until enter(). False: the host failed
    // (start again with create()).
    bool reconsist(const core::Vfs& vfs, std::vector<std::vector<LegacyCar>> trains,
                   const std::vector<const simulation::Train*>& sims);
    // The trains' RTS kinds for the next reconsist (default: the first the
    // player's, 1, the rest LOOSE, 3); 2 (another player's) keeps a train's
    // DLLs bound without running them.
    void set_train_types(std::vector<std::uint32_t> types) { train_types_ = std::move(types); }
    // Cars with a DLL that came back from the parked ones (by their keys)
    // since the last call: their DLLs as they left them.
    [[nodiscard]] std::size_t take_restored() { return std::exchange(restored_, std::size_t{0}); }
    // A saved game (RTS 0x57194C, docs/reverse/train-dll.md §20): each
    // locomotive's DLL state by its car's key (LegacyCar::key), opaque bytes
    // to keep; restore_state() puts them back into the cars with those keys
    // (after the trains are set up and the player entered: RTS runs Init and
    // then writes the saved state over it). False: no host, or it failed.
    [[nodiscard]] bool save_state(std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>>& states);
    bool restore_state(const std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>>& states);
    bool reconsist(const core::Vfs& vfs, std::vector<LegacyCar> cars, const simulation::Train& train);
    [[nodiscard]] std::size_t trains() const noexcept { return train_first_.size() - 1; }
    [[nodiscard]] std::size_t train_size(std::size_t train) const noexcept {
        return train + 1 < train_first_.size() ? train_first_[train + 1] - train_first_[train] : 0;
    }
    // Train `train`'s car `car` (train 0: the player's, as output(car)).
    [[nodiscard]] const dllhost::CarOutput* output(std::size_t train, std::size_t car) const;
    [[nodiscard]] bool has_dll(std::size_t train, std::size_t car) const;
    [[nodiscard]] std::unique_ptr<simulation::LocomotiveController> controller(std::size_t train, std::size_t car) const;
    // The player moves a cab switch of `car` to `state`: CanSwitch, then the
    // host moves it (Switched fires when it gets there). False if refused.
    bool operate(std::size_t car, std::uint32_t id, std::uint32_t state);
    // Drags switch `id` (DragSwitchRequest): `delta` in states, or lets it go.
    bool drag(std::size_t car, std::uint32_t id, float delta, bool release);

    [[nodiscard]] const dllhost::CarOutput* output(std::size_t car) const;
    [[nodiscard]] const formats::CabDescription* cab(std::size_t car) const;
    // Train `train`'s car `car`: its cab's Brakes.dll controls as openRTS's
    // valve reads them (the 395's position, the reducer, the combined valve).
    [[nodiscard]] CabBrakes cab_brakes(std::size_t train, std::size_t car) const;
    [[nodiscard]] bool has_dll(std::size_t car) const;
    [[nodiscard]] const std::vector<dllhost::ModuleReport>& modules() const noexcept { return modules_; }
    [[nodiscard]] std::size_t player() const noexcept { return player_; }
    // The farthest RTSGetTrackItems asked for in the last step, m (0: none).
    [[nodiscard]] float items_wanted() const noexcept { return items_wanted_; }
    // What the DLLs asked of the tape in the last step (RTSSetIntegerG 7),
    // once: nothing when `set` is false.
    struct TapeRequest {
        bool set = false;
        bool restart = false; // started (again): a new file
        std::int32_t car = -1; // the player's train's car, -1 stopped
    };
    [[nodiscard]] TapeRequest take_tape_request() { return std::exchange(tape_request_, TapeRequest{}); }
    [[nodiscard]] bool alive() const;
    [[nodiscard]] std::string error() const;
    [[nodiscard]] std::string take_messages();
    // A controller for `car` reading this consist's outputs.
    [[nodiscard]] std::unique_ptr<simulation::LocomotiveController> controller(std::size_t car) const;
    // The air of its trains under the realistic air model (every controller
    // of this consist follows it at once).
    void set_air(DllAir mode) noexcept { air_->mode = mode; }
    [[nodiscard]] DllAir air() const noexcept { return air_->mode; }
    // openRTS's cab brake devices in place of Brakes.dll's, with the books'
    // numbers or Brakes.dll's (DllAirSetting::own_brakes, book_numbers).
    void set_own_brakes(const bool own, const bool books) noexcept {
        air_->own_brakes = own;
        air_->book_numbers = books;
    }
    [[nodiscard]] bool own_brakes() const noexcept { return air_->own_brakes; }
    // openRTS's КЛУБ-У in place of Brakes.dll's (simulation/klub.hpp;
    // docs/brakes-dll-replacement.md, step 3.4), with the air not as RTS's:
    // on the player's train's cars whose Brakes.dll has a КЛУБ (PB_SETUP 2)
    // and one cab, Brakes.dll's is switched off (its PB_SETUP without 2)
    // and openRTS's works the ЭПК (Outputs::klub_vent), draws the screen and
    // the lamps, sounds its beeps and whistle on Brakes.sms and leaves in
    // Brakes.dll's variables what the train's DLL reads of a КЛУБ. Switched
    // on, it starts afresh: the track and the train's data to be entered.
    void set_own_klub(const bool own) noexcept { own_klub_ = own; }
    [[nodiscard]] bool own_klub() const noexcept { return own_klub_; }
    // The cars it works on in the last step.
    [[nodiscard]] std::size_t klub_cars() const noexcept { return klub_working_; }
    // Its state on a car (nullptr: not there).
    [[nodiscard]] const simulation::KlubState* klub_state(std::size_t car) const;

private:
    struct KlubCar;
    void klub_before(dllhost::StepRequest& request);
    void klub_after(const dllhost::StepRequest& request, const LegacyWorld& world);
    void klub_key(std::size_t at, std::uint32_t id);
    KlubCar* klub_car(std::size_t at);
    void klub_save(std::uint32_t key, std::vector<std::uint8_t>& bytes) const;
    void klub_restore(std::size_t at, std::vector<std::uint8_t>& bytes);
    // By the car's key; nullptr: no КЛУБ of Brakes.dll that openRTS's can take.
    std::unordered_map<std::uint32_t, std::unique_ptr<KlubCar>> klub_;
    bool own_klub_ = false;
    std::size_t klub_working_ = 0;
    // mdd_Libraries/Brakes/ChangeStanc.bto (the КЛУБ's station names).
    std::vector<std::pair<std::u16string, std::u16string>> station_names_;

    LegacyConsist() = default;
    std::vector<dllhost::CarInput> inputs(const std::vector<const simulation::Train*>& trains, const LegacyWorld* world) const;
    std::vector<dllhost::CarInput> inputs(const simulation::Train& train, const LegacyWorld* world) const {
        return inputs(std::vector<const simulation::Train*>{&train}, world);
    }
    // Where each train's cars begin in cars_ (and the end): [0] = 0.
    std::vector<std::size_t> train_first_{0, 0};
    [[nodiscard]] std::size_t flat(std::size_t train, std::size_t car) const {
        return train + 1 < train_first_.size() && car < train_first_[train + 1] - train_first_[train] ? train_first_[train] + car : SIZE_MAX;
    }

    std::unique_ptr<LegacyHost> host_;
    std::vector<LegacyCar> cars_;
    std::vector<formats::CabDescription> cabs_;
    std::vector<bool> dll_;
    std::vector<dllhost::ModuleReport> modules_;
    std::shared_ptr<Outputs> outputs_ = std::make_shared<Outputs>();
    std::shared_ptr<DllAirSetting> air_ = std::make_shared<DllAirSetting>();
    std::size_t player_ = SIZE_MAX;
    std::vector<dllhost::CarAir> entered_air_;
    std::vector<std::uint32_t> train_types_;
    std::size_t restored_ = 0;
    std::string messages_;
public:
    // The host's number of car `car`'s auxiliary library `name` (AuxLibraryName,
    // case ignored), -1 when it has none or its module was not found: its
    // index in CarOutput::aux_vars and aux_sound.
    [[nodiscard]] int aux_library(std::size_t car, std::string_view name) const;

private:
    // Per car: each AuxLibrary of the .eng as the host numbers them (only
    // the ones whose module was found), -1 missing.
    std::vector<std::vector<std::int16_t>> aux_index_;
    // The sound scripts the host has tables of (by path), for new cars.
    std::map<std::string, std::int32_t> sound_paths_;
    std::int32_t sound_count_ = 0;
    std::filesystem::path cache_root_;
    std::optional<core::VirtualPath> route_;
    // Cars cut off (by key), as the host keeps them, until coupled again.
    struct Parked {
        LegacyCar car;
        formats::CabDescription cab;
        bool dll = false;
        std::vector<std::int16_t> aux_index;
    };
    std::unordered_map<std::uint32_t, Parked> parked_;
    std::unordered_set<std::uint32_t> items_sent_;
    std::unordered_set<std::int32_t> units_sent_;
    float items_wanted_ = 0.0f;
    TapeRequest tape_request_;
};

// Engine outputs of a legacy DLL as the simulation's demands.
void apply_engine_output(const dllhost::EngineOutput& out, simulation::EngineDemands& demands);

// The player's first entering of a train just made, as RTS does it: the
// cars start with no air (the Wagon constructor 0x4FD65C clears its first
// 0x184 bytes, every pressure), Init writes what its start wants (the
// MaDaDev VL80T and EP1M: cold pipe and main reservoir 0, on 5.0 / 7.0,
// running 5.2 / 8.5), then (0x46955C, once a train) every car takes the
// player's engine's pipe, charging pipe, cylinder, auxiliary reservoir and
// control chamber; main reservoirs stay each engine's. enter()'s result.
bool enter_new_train(LegacyConsist& consist, std::size_t car, simulation::Train& train, bool running = true, int state = -1);

// What each state of a cab's 395 control is (simulation::ValvePosition, -1
// not known), read from its StateHints ("[I]" .. "[VI]", "[Vа]" / "[VЭ]",
// or words: "экстренное", "поездное", "перекрыша" ...): cabs order them
// either way (the ЭД9М's I..VI, the ВЛ80С's VI..I). Without hints a 7-state
// control is taken as I..VI.
[[nodiscard]] std::vector<std::int8_t> valve_states(const formats::CabSwitch& control);

// A controller for car `car` of `outputs` (LegacyConsist::controller makes
// them; tests make them on outputs of their own). `cab`: its cab's brake
// controls (brake_cab_controls); empty: the 395's states I..VI, no reducer.
// `brakes`: its Brakes.dll's parameters, `brakes_library` that library's
// number in the host (aux_library; -1 none), for DllAirSetting::own_brakes.
[[nodiscard]] std::unique_ptr<simulation::LocomotiveController> make_legacy_controller(
    std::weak_ptr<const LegacyConsist::Outputs> outputs, std::size_t car, std::shared_ptr<const DllAirSetting> air,
    BrakeCabControls cab = {}, BrakesParameters brakes = {}, int brakes_library = -1);

} // namespace openrts::trainplugin
