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

} // namespace openrts::net
