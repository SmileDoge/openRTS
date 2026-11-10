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
    std::uint32_t kind{};   // 2 a junction (RTS 0x4886EC finds the junction there), else a point on a vector node
    std::uint32_t flags{};
    [[nodiscard]] bool junction() const noexcept { return kind == 2; }
};

// One TrPathNode: the point it stands on and the next nodes.
struct PathNode {
    std::uint32_t flags{};
    std::uint32_t next = invalid;      // main continuation
    std::uint32_t next_siding = invalid;
    std::uint32_t point{};             // index into points
    static constexpr std::uint32_t invalid = 0xFFFFFFFFu;
    // Node flags as MSTS writes them (Open Rails' reading, PROBABLE; no
    // installed path uses them): low two bits 1 = reverse here, 2 = wait,
    // the wait in seconds in the high 16 bits; 4 marks a point placed
    // between junctions.
    [[nodiscard]] bool reverse() const noexcept { return (flags & 3u) == 1u; }
    [[nodiscard]] bool wait() const noexcept { return (flags & 3u) == 2u; }
    [[nodiscard]] std::uint32_t wait_seconds() const noexcept { return flags >> 16; }
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
// The path as a .pat file (UTF-16 LE with a byte order mark, as MSTS and
// RTS write them); nodes follow each other in order.
[[nodiscard]] std::string write_train_path(const TrainPath& path);
// Every .pat of the route's PATHS folder, sorted by file name.
[[nodiscard]] std::vector<TrainPath> load_train_paths(const core::Vfs& vfs, const core::VirtualPath& route);
// Every .pat of a folder (a scenario's own, RTS_ACT/<name>), sorted by file name.
[[nodiscard]] std::vector<TrainPath> load_train_paths_in(const core::Vfs& vfs, const core::VirtualPath& directory);

} // namespace openrts::formats
