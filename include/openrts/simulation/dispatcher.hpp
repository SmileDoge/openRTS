#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <openrts/simulation/train_route.hpp>

// Trains following paths, and the junctions reserved for them (docs/paths.md;
// RTS 0x48A0F8 on every train update, junction queues 0x48C7B8 / 0x48C924,
// release 0x48C85C). Each step the dispatcher finds every routed train on its
// path and walks the junctions ahead: a free junction is reserved for the
// train and set for its way; one held by another train queues it, and its
// reservation (its movement authority) ends there, unless it gets there
// clearly earlier than the holder, who is still far away: then it takes the
// junction over (0x48C924) and the holder's way ends there instead.
namespace openrts::simulation {

class Train;

struct RouteProgress {
    bool on_route = false;
    std::size_t leg{};
    double head{};      // leg distance of the end leading along the leg
    double tail{};
    bool front_leads = true; // car 0's front is the head (drive forward)
    double authority{}; // leg distance up to which the way is reserved
    std::optional<std::uint32_t> blocked_at;  // junction the reservation stopped at
    std::uint64_t blocked_by{};               // the train holding it (0: a section in use)
    std::size_t next_mark{};                  // first mark of the leg not yet reached
    bool finished = false;                    // the last mark of the last leg is reached
};

struct DispatcherSettings {
    // How far ahead junctions are reserved, m (RTS walks the whole path; its
    // priority test looks 5000 m / speed ahead, PROBABLE equivalent).
    double horizon = 3000.0;
    // A train takes a junction over when it gets there this much earlier, s.
    double takeover_margin = 30.0;
};

class Dispatcher {
public:
    explicit Dispatcher(TrackNetwork& network, DispatcherSettings settings = {});
    ~Dispatcher();
    Dispatcher(const Dispatcher&) = delete;
    Dispatcher& operator=(const Dispatcher&) = delete;

    // Gives train `id` this path (replacing any it had: its reservations
    // ahead are cancelled). The train is found on it at the next update.
    void assign(std::uint64_t id, PathPlan plan, TrainRoute route);
    // The train has no path any more; its reservations ahead are cancelled.
    void clear(std::uint64_t id);
    // After the trains moved: progress along the paths, then reservations.
    // Paths of trains missing from `trains` are cleared.
    void update(std::span<const Train* const> trains);

    [[nodiscard]] const PathPlan* plan(std::uint64_t id) const;
    [[nodiscard]] const TrainRoute* route(std::uint64_t id) const;
    [[nodiscard]] const RouteProgress* progress(std::uint64_t id) const;
    // Metres ahead of the train's leading end that are reserved for it; no
    // value for a train without a path (unlimited).
    [[nodiscard]] std::optional<double> authority_ahead(std::uint64_t id) const;
    // A leg ended (the train stands at its reverse point): continue on the
    // next one. False on the last leg.
    bool next_leg(std::uint64_t id);
    // Marks the leg's next mark as reached (a waiting point after the wait).
    void pass_mark(std::uint64_t id);
    // Every routed train.
    [[nodiscard]] std::vector<std::uint64_t> routed() const;
    [[nodiscard]] const DispatcherSettings& settings() const noexcept { return settings_; }

private:
    struct Follower;
    void cancel(Follower& follower, std::size_t from_junction);
    void reserve(Follower& follower, const Train& train, std::span<const Train* const> trains);
    [[nodiscard]] std::optional<double> eta(const Follower& follower, std::uint32_t junction, double speed) const;

    TrackNetwork* network_;
    DispatcherSettings settings_;
    std::map<std::uint64_t, std::unique_ptr<Follower>> followers_;
};

} // namespace openrts::simulation
