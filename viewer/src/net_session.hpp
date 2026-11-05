#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <openrts/net/connection.hpp>
#include <openrts/net/snapshot.hpp>
#include <openrts/net/world.hpp>

namespace openrts::simulation {
class SignalSystem;
class TrackNetwork;
} // namespace openrts::simulation

namespace openrts::viewer {

class TrainView;

// openRTS watching a game on an RTS server (--mp-connect, docs/net.md,
// "Observer"): the server's world (time of day, junctions, signal aspects)
// and its trains, which other clients and the server drive. They are drawn
// one package interval behind and interpolated along the track. Nothing is
// sent back yet.
class NetSession {
public:
    // `early`: packages that came with the snapshot, after its end.
    NetSession(std::shared_ptr<net::RtsConnection> connection, net::Snapshot snapshot,
               std::vector<net::ReceivedPackage> early);

    // The first call puts the snapshot's trains on the track; every call
    // applies what the server sent and moves the trains.
    void update(simulation::TrackNetwork& network, simulation::SignalSystem* signals, TrainView& trains);
    // The server's time of day, once after it came.
    [[nodiscard]] std::optional<double> take_time_of_day() { return std::exchange(time_of_day_, std::nullopt); }
    [[nodiscard]] bool down() const noexcept { return down_; }

private:
    struct Remote {
        net::SnapshotTrain record;
        std::uint64_t train = 0; // in TrainView; 0 while not on the track
        net::RemoteMotion motion;
        bool warned = false;
    };

    void apply(const net::ReceivedPackage& package, simulation::TrackNetwork& network, simulation::SignalSystem* signals,
               TrainView& trains, double now);
    void position(Remote& remote, const net::TrainPos& pos, simulation::TrackNetwork& network, TrainView& trains,
                  double now);
    void move_trains(simulation::TrackNetwork& network, TrainView& trains, double now);
    [[nodiscard]] double now() const;

    std::shared_ptr<net::RtsConnection> connection_;
    net::Snapshot snapshot_;
    std::vector<net::ReceivedPackage> early_;
    bool started_ = false;
    bool down_ = false;
    std::map<std::uint32_t, Remote> remotes_;
    std::optional<net::SignalHeads> heads_;
    std::vector<net::SignalHeadState> waiting_heads_; // until the signals have loaded
    std::optional<double> time_of_day_;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    std::map<std::uint16_t, std::size_t> not_handled_; // ids the observer leaves, with counts
    double report_at_ = 30.0;
};

} // namespace openrts::viewer
