#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <openrts/core/vfs.hpp>

// Track geometry (tsection.dat): the shapes of track sections and the paths
// through track shapes. Topology (which section follows which) lives in the
// .tdb; see track_database.hpp and docs/track.md.
namespace openrts::formats {

// One section: a straight (`SectionSize`), optionally skewed, or an arc
// (`SectionCurve`; a positive angle turns right).
struct TrackSectionDefinition {
    std::uint32_t index{};
    float gauge{};
    float length{};        // metres; for arcs |angle| * radius, as RTS computes it
    float radius{};        // 0 for a straight
    float angle_degrees{}; // arc angle, or the skew of a straight
    bool curved = false;
    bool skewed = false;
};

// A path through a track shape: start offset and heading relative to the
// shape origin, then the sections in order.
struct TrackShapePath {
    std::array<float, 3> start{};
    float heading_degrees{};
    std::vector<std::uint32_t> sections;
};

struct TrackShapeDefinition {
    std::uint32_t index{};
    std::string file_name;
    std::uint32_t path_count{};
    std::optional<std::uint32_t> main_route;
    float clearance_distance{};
    std::vector<TrackShapePath> paths;
    std::vector<std::array<float, 3>> crossover_points;
    bool tunnel = false;
    bool road = false;
    bool crossover = false;
    bool manual_junction = false;
};

struct TrackSectionLibrary {
    std::unordered_map<std::uint32_t, TrackSectionDefinition> sections;
    std::unordered_map<std::uint32_t, TrackShapeDefinition> shapes;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
    [[nodiscard]] const TrackSectionDefinition* section(std::uint32_t index) const;
    [[nodiscard]] const TrackShapeDefinition* shape(std::uint32_t index) const;
    // Adds `other` (a route's tsection.dat) on top: same indices are replaced.
    void merge(TrackSectionLibrary other);
};

[[nodiscard]] TrackSectionLibrary parse_track_sections(std::string_view bytes);
// GLOBAL/tsection.dat, then `route`/tsection.dat when present.
[[nodiscard]] TrackSectionLibrary load_track_sections(const core::Vfs& vfs, const core::VirtualPath& route);

} // namespace openrts::formats
