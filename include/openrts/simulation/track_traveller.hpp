#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include <openrts/simulation/track_network.hpp>

namespace openrts::simulation {

// How a move treats junctions (RTS 0x47ECD0 modes).
enum class JunctionMode : std::uint8_t {
    plain,   // follow the selected branch, change nothing
    occupy,  // a train head: mark occupied, a trailing move throws the switch
    release, // a train tail: start clearance (facing) or release (trailing)
    probe,   // look-ahead: stop at a trailing junction set against the route
};

enum class MoveStop : std::uint8_t {
    completed,
    end_of_track,         // an end node or a missing link
    junction_obstructed,  // probe: trailing junction set for another branch
};

struct JunctionPass {
    std::uint32_t junction{};
    bool facing = false;
    std::uint32_t branch{};       // branch taken (facing) or arrived from (trailing)
    bool forced = false;          // held by another owner (RTS "forced a junction")
};

struct MoveResult {
    MoveStop stop = MoveStop::completed;
    double moved{}; // metres actually travelled
    std::vector<JunctionPass> junctions;
    [[nodiscard]] bool ok() const noexcept { return stop == MoveStop::completed; }
};

// Topological position on the network: a vector node, an offset along it and
// the travel direction. Geometry is only consulted for pose().
class TrackTraveller {
public:
    TrackTraveller(TrackNetwork& network, std::uint32_t node, double offset, bool forward);

    // Moves `distance` metres in the travel direction (negative: backwards,
    // keeping the direction). Stops at an end node; the traveller is then
    // left at the end of the last node.
    MoveResult move(double distance, JunctionMode mode = JunctionMode::plain, std::uint64_t owner = 0);
    void reverse() noexcept { forward_ = !forward_; }

    [[nodiscard]] std::uint32_t node() const noexcept { return node_; }
    [[nodiscard]] double offset() const noexcept { return offset_; }
    [[nodiscard]] bool forward() const noexcept { return forward_; }
    [[nodiscard]] std::size_t section() const;
    // World pose; the heading points along the travel direction.
    [[nodiscard]] TrackPoint pose() const;

private:
    // Leaves the current node at its end in `forward` direction; returns false
    // at an end node or an obstruction.
    [[nodiscard]] bool leave(bool forward, JunctionMode mode, std::uint64_t owner, MoveResult& result);

    TrackNetwork* network_;
    std::uint32_t node_;
    double offset_;
    bool forward_;
};

struct ItemAhead {
    std::uint32_t item{};
    std::uint32_t node{};
    double distance{}; // from the start position, along the path
    bool facing_forward = true; // travel direction on that node
};

// The next vector node from the end of `node` in the `forward` direction,
// through junctions as they are set. Junctions passed are listed; a trailing
// junction set for another branch stops with junction_obstructed;
// `visited` (optional) receives every junction looked at, the obstructing
// one included.
struct NodeStep {
    std::uint32_t node{};
    bool forward = true; // travel direction on the new node
    std::vector<JunctionPass> junctions;
};
[[nodiscard]] std::optional<NodeStep> next_vector_node(const TrackNetwork& network, std::uint32_t node, bool forward,
                                                       MoveStop& stop, std::vector<std::uint32_t>* visited = nullptr);

enum class LookAheadStop : std::uint8_t { distance, end_of_track, junction_obstructed, loop };

struct LookAhead {
    std::vector<ItemAhead> items;
    LookAheadStop stop = LookAheadStop::distance;
    double searched{};
};

// Walks the path from `start` through the selected branches without changing
// junctions, collecting items accepted by `filter`, until `max_distance`, an
// end node, a trailing junction set against the path, or `max_items`. Unlike
// RTS (0x47F214, 0x48106C), the distance is checked on every node and visited
// (node, direction) pairs end the walk, so closed loops terminate.
[[nodiscard]] LookAhead look_ahead(const TrackNetwork& network, std::uint32_t node, double offset, bool forward,
                                   double max_distance,
                                   const std::function<bool(const formats::TrackItem&)>& filter = {},
                                   std::size_t max_items = 64);

} // namespace openrts::simulation
