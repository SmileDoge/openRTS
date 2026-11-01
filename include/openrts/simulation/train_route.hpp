#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <openrts/formats/train_path.hpp>
#include <openrts/simulation/track_network.hpp>

// A train's path on the network (docs/paths.md). RTS keeps a path as the
// list of its .pat points: the start, every junction it passes with the
// branch it takes, and waiting / reverse / end points (0x4886EC loads it,
// 0x48BBF0 re-routes between points when Track control edits it). openRTS
// keeps the editable points (PathPlan) and resolves them to legs of track
// (TrainRoute) that the dispatcher reserves and drivers follow.
namespace openrts::simulation {

// A place on a vector node and a travel direction along it.
struct TrackLocation {
    std::uint32_t node{};
    double offset{};
    bool forward = true;
};

enum class PlanPointKind : std::uint8_t {
    via,     // a point the path runs through (a .pat point between junctions)
    wait,    // stop and wait
    reverse, // stop and go back
    end,     // the path ends here
};

// One point of a path after its start (RTS path points: Track control's
// "Place waiting / reverse / end point").
struct PlanPoint {
    PlanPointKind kind = PlanPointKind::via;
    std::uint32_t node{};
    double offset{};
    float wait{}; // s, waiting points
    // How the way from the previous point gets here. `through`: junctions to
    // pass in this order (a .pat lists every one); empty: the shortest way.
    // `branches`: the branch to take at a facing junction (Track control's
    // "Switch junction"), overriding the shortest way.
    std::vector<std::uint32_t> through;
    std::map<std::uint32_t, std::uint32_t> branches;
};

// An editable train path.
struct PathPlan {
    std::string name;
    TrackLocation start;
    std::vector<PlanPoint> points;
};

// A junction the route passes.
struct RouteJunction {
    std::uint32_t junction{};
    std::uint32_t branch{}; // taken (facing) or arrived from (trailing)
    bool facing = true;
    double distance{};      // along the leg
    std::size_t point{};    // the plan point the way runs to (its `branches`)
};

// A stretch of one vector node in the travel direction.
struct RouteSpan {
    std::uint32_t node{};
    bool forward = true;
    double from{}, to{}; // node offsets, in travel order
    double start{};      // leg distance at `from`
    [[nodiscard]] double length() const noexcept { return forward ? to - from : from - to; }
};

enum class RouteMarkKind : std::uint8_t { via, wait, reverse, end };

struct RouteMark {
    RouteMarkKind kind = RouteMarkKind::via;
    double distance{};  // along the leg
    float wait{};
    std::size_t point{}; // index into PathPlan::points
};

// One run in one direction; a reverse point ends a leg.
struct RouteLeg {
    std::vector<RouteSpan> spans;
    std::vector<RouteJunction> junctions; // by distance
    std::vector<RouteMark> marks;         // by distance; the last ends the leg
    [[nodiscard]] double length() const noexcept;
    // Leg distance of (node, offset), searching from span `hint`; nothing
    // when the place is not on the leg. `span` receives the span index.
    [[nodiscard]] std::optional<double> locate(std::uint32_t node, double offset, std::size_t hint = 0,
                                               std::size_t* span = nullptr) const;
    // The place at a leg distance (clamped to the leg).
    [[nodiscard]] std::optional<TrackLocation> at(double distance) const;
};

struct TrainRoute {
    std::string name;
    std::vector<RouteLeg> legs;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty() && !legs.empty(); }
};

// Resolves the plan on the network: each way between points is the
// shortest (or the listed junctions, or the chosen branches).
[[nodiscard]] TrainRoute build_route(const TrackNetwork& network, const PathPlan& plan);

// A .pat as a plan (RTS 0x4886EC): TrackPDPs of kind 2 are junctions (the
// junction node at that position), others are points on the track; the
// first point is the start, the last the end; node flags give waiting and
// reverse points. The start direction is the one that reaches the path's
// second point.
[[nodiscard]] std::optional<PathPlan> plan_from_path(const TrackNetwork& network, const formats::TrainPath& path,
                                                     std::string* error = nullptr);

// The route as a .pat (Track control's "Save path as"): the start, every
// junction passed and every plan point, in order.
[[nodiscard]] formats::TrainPath path_from_route(const TrackNetwork& network, const PathPlan& plan,
                                                 const TrainRoute& route);

// The junction node whose world position is within `tolerance` metres of
// `point` (RTS 0x47E700 matches .pat junction points this way).
[[nodiscard]] std::optional<std::uint32_t> junction_near(const TrackNetwork& network, const TrackPoint& point,
                                                         double tolerance = 3.0);

} // namespace openrts::simulation
