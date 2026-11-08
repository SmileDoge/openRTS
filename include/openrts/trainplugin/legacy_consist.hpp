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

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <openrts/core/vfs.hpp>
#include <openrts/dllhost/protocol.hpp>
#include <openrts/formats/cab_description.hpp>
#include <openrts/formats/rolling_stock.hpp>
#include <openrts/formats/route_database.hpp>
#include <openrts/formats/track_database.hpp>
#include <openrts/simulation/train.hpp>
#include <openrts/trainplugin/legacy_host.hpp>

namespace openrts::trainplugin {

// A switch's AssignKey keys as RTS keeps them in ActSwitch::Key (+72, the
// loader 0x45AE90): per key, two at most, its character in upper case
// (Space 0x20) and its modifiers (1 Shift, 2 Ctrl or Control, 4 Alt), the
// parts joined by '+'. DLLs read them: the MaDaDev Brakes.dll's Run springs
// the 254 handle back from its unfixed positions unless GetAsyncKeyState of
// its key, or of the left mouse button, is down.
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
    // RTSGetTrackItems: the player's train's items (RTS 0x4808DC) and how
    // far ahead and behind the list reaches.
    std::vector<dllhost::TrackItemEntry> track_items;
    float items_range = 0.0f;
    // The SMS objects' trigger pair states (RTSIsSMSTriggerOn).
    std::vector<dllhost::SoundPairState> sound_pairs;
};

class LegacyConsist {
public:
    struct Outputs {
        std::vector<dllhost::CarOutput> cars;
        bool valid = false;
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
    [[nodiscard]] bool has_dll(std::size_t car) const;
    [[nodiscard]] const std::vector<dllhost::ModuleReport>& modules() const noexcept { return modules_; }
    [[nodiscard]] std::size_t player() const noexcept { return player_; }
    // The farthest RTSGetTrackItems asked for in the last step, m (0: none).
    [[nodiscard]] float items_wanted() const noexcept { return items_wanted_; }
    [[nodiscard]] bool alive() const;
    [[nodiscard]] std::string error() const;
    [[nodiscard]] std::string take_messages();
    // A controller for `car` reading this consist's outputs.
    [[nodiscard]] std::unique_ptr<simulation::LocomotiveController> controller(std::size_t car) const;

private:
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
    std::size_t player_ = SIZE_MAX;
    std::vector<std::uint32_t> train_types_;
    std::size_t restored_ = 0;
    std::string messages_;
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
};

// Engine outputs of a legacy DLL as the simulation's demands.
void apply_engine_output(const dllhost::EngineOutput& out, simulation::EngineDemands& demands);

} // namespace openrts::trainplugin
