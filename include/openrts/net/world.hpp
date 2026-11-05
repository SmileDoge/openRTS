#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <openrts/net/snapshot.hpp>
#include <openrts/simulation/train_route.hpp>

namespace openrts::simulation {
class SignalSystem;
class TrackNetwork;
} // namespace openrts::simulation

// An RTS snapshot in openRTS terms (docs/net.md): junction positions on the
// track network, the heads' aspects as overrides, and where each train's
// front stands (RTS gives the rear end, openRTS places by the front).
namespace openrts::net {

struct WorldApplied {
    std::size_t applied{};
    std::vector<std::string> problems;
};

// Throws every junction the snapshot names. A junction index that is no
// junction node, or a branch it has not got, is a problem.
WorldApplied apply_junctions(simulation::TrackNetwork& network, const Snapshot& snapshot);

// Gives every head the snapshot names its aspect, drawing state and speed as
// an override (the server decides signals; RTS's client only shows them).
WorldApplied apply_signals(simulation::SignalSystem& signals, const Snapshot& snapshot);

// The front of car 0: from the snapshot's rear end along the track by
// front_distance(), the way the train faces. Junctions are passed as they
// stand, so apply_junctions() first.
[[nodiscard]] std::optional<simulation::TrackLocation> train_front(simulation::TrackNetwork& network,
                                                                  const SnapshotTrain& train,
                                                                  std::string* error = nullptr);

// A car's file as a consist names it: the folder under TRAINS/TRAINSET and
// the file name without its extension.
struct CarFile {
    std::string folder;
    std::string name;
    bool engine = false; // .eng
};
[[nodiscard]] CarFile car_file(const TrainCar& car);

// A train's display name: its name from the console, else "<number>" from
// its assignment, else "train <id>".
[[nodiscard]] std::string train_name(const SnapshotTrain& train);

// Sets a junction the way the server says, whatever holds it here (an RTS
// client takes 0x460A as it comes, 0x4C3DE0). False for no such junction.
bool set_junction(simulation::TrackNetwork& network, const JunctionState& junction);

// The signal heads by TDB item, made once the signals have loaded.
class SignalHeads {
public:
    explicit SignalHeads(const simulation::SignalSystem& signals);
    // Overrides the heads of one 0x460C; false when no head has its item.
    bool apply(simulation::SignalSystem& signals, const SignalHeadState& head) const;

private:
    std::vector<std::pair<std::uint32_t, std::uint32_t>> by_item_; // item, head; sorted
};

// Metres along the track from `from` to `to`, following the junctions as
// they stand: positive when `to` is ahead of `from` (the way `from` faces),
// negative behind. Nothing beyond `limit` either way, across an end of
// track, or when `to` faces the other way.
[[nodiscard]] std::optional<double> track_distance(simulation::TrackNetwork& network, const simulation::TrackLocation& from,
                                                   const simulation::TrackLocation& to, double limit);

// Where a train another client drives stands, drawn from its positions
// (0x4603) one package interval in the past and interpolated along the
// track between them, as docs/reverse/multiplayer.md asks of openRTS
// ("Remote trains"); RTS extrapolates instead. Past the last position it
// keeps on at the last speed for at most one more interval.
class RemoteMotion {
public:
    // A position at `time` (seconds, any steady clock): car 0's front.
    void add(simulation::TrackNetwork& network, double time, const simulation::TrackLocation& front, float speed);
    // The train stands (0x4606): no extrapolation past the last position.
    void stand(double time);
    // Where car 0's front is drawn at `time`.
    [[nodiscard]] std::optional<simulation::TrackLocation> at(simulation::TrackNetwork& network, double time) const;
    // The speed drawn then (m/s, towards the train front).
    [[nodiscard]] float speed_at(double time) const;
    // The delay: one package interval, measured.
    [[nodiscard]] double delay() const noexcept { return interval_; }
    [[nodiscard]] bool empty() const noexcept { return samples_.empty(); }

private:
    struct Sample {
        double time{};
        simulation::TrackLocation front;
        float speed{};
        // Metres from the previous sample's front, when the track joins
        // them; otherwise the train is put straight there.
        std::optional<double> from_previous;
    };
    std::vector<Sample> samples_; // a few, oldest first
    double interval_ = 1.5;       // RTS players send every 1.5 s
    bool standing_ = false;
};

} // namespace openrts::net
