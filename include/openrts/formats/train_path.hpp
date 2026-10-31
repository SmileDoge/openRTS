#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <openrts/core/vfs.hpp>
#include <openrts/formats/route_world.hpp>

namespace openrts::formats {

// One TrackPDP of a .pat file: a point on the track.
struct PathPoint {
    RoutePosition position;
    std::uint32_t kind{};   // 1 junction, 2 plain (MSTS)
    std::uint32_t flags{};
};

// One TrPathNode: the point it stands on and the next nodes.
struct PathNode {
    std::uint32_t flags{};
    std::uint32_t next = invalid;      // main continuation
    std::uint32_t next_siding = invalid;
    std::uint32_t point{};             // index into points
    static constexpr std::uint32_t invalid = 0xFFFFFFFFu;
};

// A route path (ROUTES/<route>/PATHS/*.pat): explore mode puts the player
// train on its start.
struct TrainPath {
    std::string file;  // file stem
    std::string id;    // TrPathName
    std::string name;  // Name
    std::string start; // TrPathStart
    std::string end;   // TrPathEnd
    std::vector<PathPoint> points;
    std::vector<PathNode> nodes;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty() && !nodes.empty(); }
};

[[nodiscard]] TrainPath parse_train_path(std::string_view bytes);
// Every .pat of the route's PATHS folder, sorted by file name.
[[nodiscard]] std::vector<TrainPath> load_train_paths(const core::Vfs& vfs, const core::VirtualPath& route);

} // namespace openrts::formats
