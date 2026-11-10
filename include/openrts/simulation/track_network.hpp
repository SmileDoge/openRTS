#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <openrts/core/vfs.hpp>
#include <openrts/formats/track_database.hpp>
#include <openrts/formats/track_sections.hpp>
#include <openrts/simulation/rail_joints.hpp>
#include <openrts/simulation/track_geometry.hpp>

// Track topology as a graph, joined with geometry for positions. See
// docs/track.md: movement uses only the graph and distances; geometry only
// answers "where is this point".
namespace openrts::simulation {

// One section of a vector node with its resolved shape and its offset range
// along the node.
struct NetworkSection {
    const formats::TrackVectorSection* placement = nullptr;
    const formats::TrackSectionDefinition* definition = nullptr; // null: unknown section index
    double start{};  // offset of the section start along the node
    double length{};
};

struct NetworkItem {
    std::uint32_t item{};
    double offset{};
};

struct NetworkNode {
    std::uint32_t index{};
    formats::TrackNodeKind kind = formats::TrackNodeKind::end;
    std::vector<NetworkSection> sections; // vector nodes
    double length{};                      // vector nodes: sum of section lengths
    std::vector<formats::TrackPin> pins;  // vector: [start, end]; junction: [trunk, branches...]; end: [link]
    std::vector<NetworkItem> items;       // by offset
    // Junction nodes.
    const formats::TrackShapeDefinition* shape = nullptr;
    std::optional<formats::TrackNodeUid> uid;

    [[nodiscard]] std::size_t branch_count() const noexcept { return pins.size() > 1 ? pins.size() - 1 : 0; }
};

// Run-time state of a junction (RTS junction record, 0x47ECD0 / 0x48C85C).
struct JunctionState {
    std::uint32_t selected{};     // branch index (out pin)
    std::uint32_t main_route{};   // TrackShape MainRoute; the rest position
    bool occupied = false;
    // Owners holding the junction, oldest first, with the branch each wants.
    std::vector<std::pair<std::uint64_t, std::uint32_t>> reservations;
    // Tails that passed the junction in the facing direction and keep it
    // occupied for `ClearanceDist` more metres: owner -> metres left.
    std::map<std::uint64_t, double> clearance;
};

struct NetworkIssue {
    std::uint32_t node{};
    std::string message;
};

// A gap between two pieces of geometry that topology says are joined.
struct GeometryJoint {
    std::uint32_t node{};
    std::uint32_t other{};    // linked node, or the same node for section joints
    std::size_t section{};    // section index for section joints
    bool between_nodes = false;
    double gap{};             // metres
    double heading{};         // radians, 0 for joints at a junction or end node
};

struct JointStatistics {
    std::size_t count{};
    double median{};
    double p95{};
    double max{};
    std::vector<GeometryJoint> worst; // largest gaps first
};

class TrackNetwork {
public:
    TrackNetwork(formats::TrackDatabase database, formats::TrackSectionLibrary sections);
    // Nodes point into the owned database and section library.
    TrackNetwork(const TrackNetwork&) = delete;
    TrackNetwork& operator=(const TrackNetwork&) = delete;

    [[nodiscard]] static std::unique_ptr<TrackNetwork> load(const core::Vfs& vfs, const core::VirtualPath& route,
                                                            std::string* error = nullptr);

    [[nodiscard]] const formats::TrackDatabase& database() const noexcept { return database_; }
    [[nodiscard]] const formats::TrackSectionLibrary& sections() const noexcept { return sections_; }
    [[nodiscard]] const NetworkNode* node(std::uint32_t index) const;
    [[nodiscard]] std::size_t node_count() const noexcept { return nodes_.size(); }
    [[nodiscard]] const std::vector<NetworkIssue>& issues() const noexcept { return issues_; }

    [[nodiscard]] JunctionState* junction(std::uint32_t node);
    [[nodiscard]] const JunctionState* junction(std::uint32_t node) const;
    // Manual switching; refused while the junction is occupied or reserved
    // for another owner's path (a driver may throw a hand switch only when
    // no dispatcher route holds it, rts_server version notes).
    bool throw_junction(std::uint32_t node, std::uint32_t branch, std::uint64_t owner = 0);
    // Drops `owner`'s reservation without touching occupancy (its path
    // changed or no longer reaches here). A free junction is then set as a
    // release sets it: to the next reservation's branch or MainRoute.
    void cancel_reservation(std::uint32_t node, std::uint64_t owner);
    // A train's tail has left the junction (RTS 0x48C85C): free it, drop the
    // owner's reservation and clearance, and set the switch to the next
    // reservation's branch or to MainRoute.
    void release_junction(std::uint32_t node, std::uint64_t owner);
    // The junction whose switch shape is world object `uid` on tile (x, z).
    [[nodiscard]] std::optional<std::uint32_t> junction_for_object(std::int32_t tile_x, std::int32_t tile_z,
                                                                   std::uint32_t uid) const;

    // Geometry along a vector node at `offset` from its start.
    [[nodiscard]] std::optional<TrackPoint> point(std::uint32_t node, double offset) const;
    // Position of a junction or end node (its world object).
    [[nodiscard]] std::optional<TrackPoint> node_position(std::uint32_t node) const;

    // Nearest point of any vector node to `target` within `max_distance`
    // metres (sampled every `step` metres, then refined). RTS (0x47E364)
    // instead takes the first 5 m sample within tolerance + 2.6 m, in node
    // order.
    struct Location {
        std::uint32_t node{};
        double offset{};
        double distance{};
    };
    [[nodiscard]] std::optional<Location> locate(const TrackPoint& target, double max_distance, double step = 2.0) const;

    // Measured, not enforced: real routes are hand-built and have gaps.
    [[nodiscard]] JointStatistics measure_joints(std::size_t worst = 10) const;

    // The rail joints along the vector nodes (rail_joints.hpp): RTS's layout
    // unless set otherwise.
    [[nodiscard]] const RailJoints& rail_joints() const noexcept { return rail_joints_; }
    void set_rail_joint_layout(RailJointLayout layout);

private:
    formats::TrackDatabase database_;
    formats::TrackSectionLibrary sections_;
    std::vector<NetworkNode> nodes_; // by node index; [0] unused
    std::map<std::uint32_t, JunctionState> junctions_;
    std::map<std::tuple<std::int32_t, std::int32_t, std::uint32_t>, std::uint32_t> junction_objects_;
    std::vector<NetworkIssue> issues_;
    RailJoints rail_joints_;
};

} // namespace openrts::simulation
