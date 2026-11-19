#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <openrts/formats/route_world.hpp>
#include <openrts/formats/signal_config.hpp>
#include <openrts/formats/signal_script.hpp>
#include <openrts/simulation/track_network.hpp>

// The signals of a route as RTS runs them (docs/reverse/signals.md, plan in
// docs/signals.md): compiled scripts on a register VM, block_state and
// route_set on the track network, the per-train chain from the farthest to
// the nearest signal, and free-running heads. Runs on the 0.05 s train step.
namespace openrts::simulation {

class Train;

enum class SignalMode : std::uint8_t { normal = 0, shunt = 1, failed = 2, excluded = 3 };

struct SignalHead {
    std::uint32_t object{};     // index into SignalSystem::objects()
    std::uint32_t sub_object{}; // the shape's sub-object (SignalUnit)
    std::uint32_t track_item{}; // TDB SignalItem
    const formats::SignalType* type = nullptr;
    const formats::SignalScript* script = nullptr;
    // Where it stands: the vector node, the offset and the travel direction
    // it protects (trains moving that way approach it).
    bool placed = false;
    std::uint32_t node{};
    double offset{};
    bool forward = true;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> links; // TrSignalDirs: junction, branch
    std::uint32_t features{}; // sig_feature bits (from the world SignalSubObj mask)

    // Script outputs and RTS head fields.
    float state{};
    float draw_state{};
    float enabled{};
    float speed = -1.0f; // m/s, -1 = none
    float mode{};        // SignalMode
    float flash_timer{}; // s

    [[nodiscard]] formats::SignalAspect aspect() const noexcept;
};

// RTS SignalOverride (an activity outcome; later the dispatcher console),
// head +120: replaces what the head's script gives (0x499948). Unset fields
// are left to the script.
struct SignalOverride {
    std::optional<SignalMode> mode;
    std::optional<formats::SignalAspect> aspect;
    std::optional<int> draw_state; // -2: the type's draw state for the aspect
    std::optional<float> speed;    // m/s
};

// RTS train flags read by the signal pass (0x508520). +7 bit 0 / bit 1: the
// mode the train's chain gives its heads, SHUNT or NORMAL (activities'
// SetSignalMode); +6 bits 0 / 1 / 2: distance, shunting and info heads count
// for the aspect the train obeys, when they are the object's lowest function.
struct TrainSignalSettings {
    std::optional<SignalMode> mode; // shunt or normal; none: the heads keep theirs
    bool obey_distance = false;
    bool obey_shunting = false;
    bool obey_info = false;
};

struct SignalObject {
    formats::WorldObjectPlacement placement; // the world Signal (shape, position, sub-objects)
    std::vector<std::uint32_t> heads;
};

// The RTS server's signals (rts_server.exe; docs/reverse/dispatcher.md,
// "Signal modes on the server"). Its signal objects, here signal units: the
// TDB signal items at one place of a vector node facing one way, in the
// order rts_server.exe builds its table (0x402F80: nodes in TDB order, their
// items in TrItemRefs order), which is the order of the route database's
// SignalUnits and the index of 0x4618. The dispatcher gives every unit a
// mode. Manual: its heads hold what they were given and run no script.
// Open: they run every step until a train passes the signal or its route
// ends. Automatic block: they always run. A running head sees `enabled` =
// the mode (RTS head +96) and the units ahead as the junctions stand (each
// unit's next signal, 0x40D1D8, renewed when a junction is thrown).
enum class UnitMode : std::uint8_t { manual = 0, open = 1, automatic = 2 };

// A unit's defaults from the route database (its SignalUnit, 0x41F39C):
// UnitFlags bit 2 open, else bit 4 automatic block, else manual at its
// DefaultAspect.
struct UnitDefaults {
    UnitMode mode = UnitMode::manual;
    formats::SignalAspect aspect = formats::SignalAspect::stop;
};

// The next signal a train obeys.
struct SignalAhead {
    bool valid = false;
    std::uint32_t object{};
    double distance{};
    formats::SignalAspect aspect = formats::SignalAspect::stop;
    float speed = -1.0f;
};

// A signal object along a train's way as RTS hands it to a locomotive DLL's
// ALSN (SignalsInfo, 0x508520): the strongest aspect of its heads of each
// function, which functions it has, and the speed the train obeys there.
struct SignalPosition {
    std::uint32_t object{};
    double distance{};
    // [0] NORMAL, [1] DISTANCE, [2] SHUNTING, [3] INFO (0 without a head of it).
    std::uint32_t aspect[4]{};
    std::uint32_t flags{}; // 1 normal, 2 distance, 4 shunting, 8 info (ts.h SignalsInfo::Flags)
    float speed = -1.0f;   // m/s
};

// A train's leading end passed a signal object.
struct SignalPass {
    std::uint64_t train{};
    std::uint32_t object{};
    formats::SignalAspect aspect = formats::SignalAspect::stop;
    // RTS 0x41B4F4: STOP_AND_PROCEED is "passed red signal" (1), STOP is
    // "passed stop signal" (4).
    [[nodiscard]] bool at_danger() const noexcept { return aspect <= formats::SignalAspect::stop_and_proceed; }
};

struct SignalStats {
    std::size_t heads{};
    std::size_t chain_runs{};      // runs in train chains this step
    std::size_t free_runs{};       // free-running runs this step
    std::size_t always_run_heads{}; // heads that run every step: their last run read a local before writing it
    std::size_t without_script{};
    std::size_t unplaced{};        // heads whose TDB item is not on the network
};

class SignalSystem {
public:
    // `signals`: every world Signal of the route.
    SignalSystem(TrackNetwork& network, std::shared_ptr<const formats::SignalConfig> config,
                 std::shared_ptr<const formats::SignalScriptFile> scripts,
                 std::vector<formats::WorldObjectPlacement> signals);
    ~SignalSystem();
    SignalSystem(const SignalSystem&) = delete;
    SignalSystem& operator=(const SignalSystem&) = delete;

    // One step (after the trains moved): occupancy, train chains, then the
    // free-running heads whose inputs changed.
    void update(std::span<const Train* const> trains, double dt);

    [[nodiscard]] const std::vector<SignalHead>& heads() const noexcept { return heads_; }
    [[nodiscard]] const std::vector<SignalObject>& objects() const noexcept { return objects_; }
    [[nodiscard]] const formats::SignalConfig& config() const noexcept { return *config_; }
    [[nodiscard]] SignalAhead ahead(const Train& train) const;
    // The objects of the train's chain ahead of its front end (`front`) or
    // its rear end, nearest first. The rear one is known only for the
    // player's train (both ends run) or when the train leads with its rear.
    [[nodiscard]] std::vector<SignalPosition> positions(const Train& train, bool front) const;
    // Passes since the last call.
    [[nodiscard]] std::vector<SignalPass> take_passes();
    [[nodiscard]] const SignalStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const std::vector<std::string>& messages() const noexcept { return messages_; }
    // Movement authority: metres ahead of a train's leading end reserved for
    // it (its path's reserved junctions, simulation::Dispatcher). Heads
    // beyond it run with `enabled` 0 (RTS 0x508520: beyond the train's path
    // or reservation end). No value: unlimited (a train without a path).
    void set_authority(std::function<std::optional<double>(const Train&)> authority);
    // The player's train (RTS train types 1 and 4). RTS runs the signals
    // ahead of both its ends every step, whichever way it moves or when it
    // stands (0x508520: the front pass and the rear pass). Other trains: the
    // end they last moved towards. No function: no player train.
    void set_player(std::function<bool(const Train&)> is_player);
    // Overrides by head; the head runs again at the next step.
    void set_override(std::uint32_t head, const SignalOverride& value);
    void clear_override(std::uint32_t head);
    [[nodiscard]] const SignalOverride* override_of(std::uint32_t head) const;
    // A train's signal flags (by train id); default: none set.
    void set_train_settings(std::uint64_t train, const TrainSignalSettings& settings);
    [[nodiscard]] TrainSignalSettings train_settings(std::uint64_t train) const;
    // Debug/test: run every head every step instead of only changed ones.
    void set_full_evaluation(bool on) noexcept { full_evaluation_ = on; }
    // block_state / route_set of a head right now (debug panel).
    [[nodiscard]] int block_state(std::uint32_t head, int mask = 0) const;
    [[nodiscard]] int route_set(std::uint32_t head, int mask = 0) const;
    // Locals of a head's script (shared by every head of that type).
    [[nodiscard]] std::vector<std::pair<std::string, float>> locals(std::uint32_t head) const;

    // Whether the head's last run read a local before writing it.
    [[nodiscard]] bool reads_stale(std::uint32_t head) const;

    // The RTS server's way (UnitMode): on, the signal units are made, every
    // one manual, its heads run once with `enabled` 0, holding what that
    // gives (0x402F80; mostly STOP), and from then on update() runs the units
    // by their modes instead of the trains' chains and the free-running
    // heads (ahead(), positions() and passes stay empty). Defaults are
    // applied by restore_defaults. Off: back to the game's way.
    void set_dispatched(bool on);
    [[nodiscard]] bool dispatched() const noexcept { return dispatched_; }
    [[nodiscard]] std::size_t unit_count() const;
    // A unit's TDB signal items (in TrItemRefs order) and its heads here.
    [[nodiscard]] const std::vector<std::uint32_t>& unit_items(std::uint32_t unit) const;
    [[nodiscard]] const std::vector<std::uint32_t>& unit_heads(std::uint32_t unit) const;
    // Where rts_server.exe keeps a unit (its first head's place, which the
    // route database's SignalUnit Position names, 0x41F418): the first
    // item's position; but an item facing the other way at a place whose
    // other direction began a unit before it on the node is moved 20 m
    // along the track (0x402F80), and that place is written. Then both
    // ways along the node are given (which way it moves is not read).
    [[nodiscard]] std::vector<TrackPoint> unit_places(std::uint32_t unit) const;
    // The unit of TDB signal item `item`.
    [[nodiscard]] std::optional<std::uint32_t> unit_of_item(std::uint32_t item) const;
    [[nodiscard]] UnitMode unit_mode(std::uint32_t unit) const;
    // The unit's aspect (RTS +0): the strongest of its heads of its lowest
    // function.
    [[nodiscard]] formats::SignalAspect unit_aspect(std::uint32_t unit) const;
    // The dispatcher chose its aspect (RTS +17): a route does not open it.
    [[nodiscard]] bool unit_pinned(std::uint32_t unit) const;
    // A mode (the console's "open" / "automatic block", a route, a type's
    // switch): its heads run at once with it (0x40CD40); manual then holds
    // what they give with `enabled` 0. Unpins it.
    void set_unit_mode(std::uint32_t unit, UnitMode mode);
    // Manual at `aspect` (0x40C714): its heads run once with `enabled` 0,
    // then the first head of its lowest function whose type has the aspect
    // shows it (its draw state and speed) and its other heads go dark; when
    // none has it, every head takes the aspect with the type's draw state
    // for it, else for STOP, else for STOP_AND_PROCEED. `pinned`: the
    // dispatcher picked it.
    void hold_aspect(std::uint32_t unit, formats::SignalAspect aspect, bool pinned);
    void set_defaults(std::uint32_t unit, const UnitDefaults& defaults);
    [[nodiscard]] std::optional<UnitDefaults> defaults(std::uint32_t unit) const;
    // Back to its defaults (0x41F39C); a unit without any keeps its state.
    void restore_defaults(std::uint32_t unit);
    // "Задать как умолчания" (0x4499F0): every unit's defaults become its
    // mode and aspect now.
    void set_defaults_from_state();
    // SignalTypeShownAsNormal (the route database, the console's "Отображать
    // как поездной" by type): heads of such a type count for their unit's
    // aspect besides those of its lowest function, at the step's runs
    // (0x40CBB4; type flag 0x100). `type`: its sigcfg name, case ignored.
    void set_shown_as_normal(std::string_view type, bool on);
    [[nodiscard]] bool shown_as_normal(const formats::SignalType& type) const;
    // Their sigcfg names as written there.
    [[nodiscard]] std::vector<std::string> shown_as_normal_types() const;
    // Open units that went back to manual since the last call because a
    // train's leading end reached them (0x45A1DC).
    [[nodiscard]] std::vector<std::uint32_t> take_closed();

    struct Impl; // internal (public only for the implementation's helpers)

private:
    void dispatch_step(std::span<const Train* const> trains, double dt);
    void run_unit(std::uint32_t unit, bool step = false);
    void build_units();
    void build_chains();
    bool dispatched_ = false;
    std::unique_ptr<Impl> impl_;
    std::shared_ptr<const formats::SignalConfig> config_;
    std::shared_ptr<const formats::SignalScriptFile> scripts_;
    std::vector<SignalHead> heads_;
    std::vector<SignalObject> objects_;
    std::vector<std::string> messages_;
    SignalStats stats_;
    bool full_evaluation_ = false;
};

} // namespace openrts::simulation
