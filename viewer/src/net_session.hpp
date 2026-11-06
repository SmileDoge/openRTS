#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <openrts/formats/route_world.hpp>
#include <openrts/net/connection.hpp>
#include <openrts/net/play.hpp>
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
// one package interval behind and interpolated along the track, with the
// lights and pantographs their owners send. It sends the camera's place
// (player_pos, so the server relays the trains' looks to it) and asks each
// train's owner for its look (state_request 2); nothing else yet.
class NetSession {
public:
    // `early`: packages that came with the snapshot, after its end.
    NetSession(std::shared_ptr<net::RtsConnection> connection, net::Snapshot snapshot,
               std::vector<net::ReceivedPackage> early);

    // The first call puts the snapshot's trains on the track; every call
    // applies what the server sent and moves the trains.
    // `camera`: where the player looks from, for player_pos.
    void update(simulation::TrackNetwork& network, simulation::SignalSystem* signals, TrainView& trains,
                const std::optional<formats::RoutePosition>& camera);
    // The server's time of day, once after it came.
    [[nodiscard]] std::optional<double> take_time_of_day() { return std::exchange(time_of_day_, std::nullopt); }
    [[nodiscard]] bool down() const noexcept { return down_; }

private:
    struct Remote {
        net::SnapshotTrain record;
        std::uint64_t train = 0; // in TrainView; 0 while not on the track
        net::RemoteMotion motion;
        bool warned = false;
        std::vector<std::uint8_t> pantographs; // the masks last logged, by car
    };

    void apply(const net::ReceivedPackage& package, simulation::TrackNetwork& network, simulation::SignalSystem* signals,
               TrainView& trains, double now);
    void position(Remote& remote, const net::TrainPos& pos, simulation::TrackNetwork& network, TrainView& trains,
                  double now);
    void move_trains(simulation::TrackNetwork& network, TrainView& trains, double now);
    // The trains the server gave us (docs/net.md, "Own trains"): simulated
    // here, one of them driven, all sent out.
    struct Owned {
        net::SnapshotTrain record;
        std::uint64_t train = 0;              // in TrainView; 0 while off the track
        std::optional<net::TrainPos> back_at; // the server's place for it while off the track
        bool moving = false;
        double next_position = 0.0; // train_pos due while it moves
        double next_look = 0.0;     // car_state at most every 250 ms
        double next_sound = 0.0;    // sound_values at most every 200 ms
        std::vector<std::array<float, 9>> sound_sent;          // by car: Var1..8, travelled
        std::vector<std::vector<std::uint8_t>> lights_sent;    // by car, by light
        std::vector<std::pair<std::uint8_t, bool>> looks_sent; // by car: pantographs, running
    };
    // Puts an own train on the track at record.pos; `drive`: the player
    // takes it (its DLLs start in the server's state).
    bool place_owned(std::uint32_t id, Owned& own, simulation::TrackNetwork& network, TrainView& trains, bool drive);
    // Off the track here (`keep`: it stays ours, to be put back).
    void take_off(std::uint32_t id, Owned& own, TrainView& trains);
    void assigned(const net::TrainAssignUpdate& a, simulation::TrackNetwork& network, TrainView& trains, double now);
    [[nodiscard]] std::uint32_t driven_id(const TrainView& trains) const;
    void send_own(TrainView& trains, double now);
    void send_look(std::uint32_t id, Owned& own, TrainView& trains, bool all);
    void send_sound(std::uint32_t id, Owned& own, TrainView& trains, double now);
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
    std::map<std::uint16_t, std::size_t> received_;    // ids since the last report, with counts
    std::map<std::uint16_t, std::size_t> not_handled_; // of them, the ones the observer leaves
    double report_at_ = 10.0;
    std::map<std::uint32_t, Owned> owned_;
    std::size_t triggers_sent_ = 0, values_sent_ = 0; // for the log
    double position_at_ = 0.0;                           // the next player_pos due
    std::optional<formats::RoutePosition> sent_camera_; // the last one sent
};

} // namespace openrts::viewer
