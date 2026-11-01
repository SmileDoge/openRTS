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

struct SignalObject {
    formats::WorldObjectPlacement placement; // the world Signal (shape, position, sub-objects)
    std::vector<std::uint32_t> heads;
};

// The next signal a train obeys.
struct SignalAhead {
    bool valid = false;
    std::uint32_t object{};
    double distance{};
    formats::SignalAspect aspect = formats::SignalAspect::stop;
    float speed = -1.0f;
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
    // Debug/test: run every head every step instead of only changed ones.
    void set_full_evaluation(bool on) noexcept { full_evaluation_ = on; }
    // block_state / route_set of a head right now (debug panel).
    [[nodiscard]] int block_state(std::uint32_t head, int mask = 0) const;
    [[nodiscard]] int route_set(std::uint32_t head, int mask = 0) const;
    // Locals of a head's script (shared by every head of that type).
    [[nodiscard]] std::vector<std::pair<std::string, float>> locals(std::uint32_t head) const;

    // Whether the head's last run read a local before writing it.
    [[nodiscard]] bool reads_stale(std::uint32_t head) const;

    struct Impl; // internal (public only for the implementation's helpers)

private:
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
