#pragma once

#include <cstdint>

#include <openrts/formats/track_database.hpp>
#include <openrts/formats/track_sections.hpp>

// Track geometry: where a point of a section lies. Pure functions over the
// section shape (tsection.dat) and the section placement (.tdb); no topology.
namespace openrts::simulation {

// A pose on the track in MSTS coordinates (tile + local metres). Heading is
// the direction of increasing offset: forward = (sin heading, 0, cos heading).
// Height changes by -sin(pitch) per metre forward, as in RTS.
struct TrackPoint {
    std::int32_t tile_x{};
    std::int32_t tile_z{};
    double x{};
    double y{};
    double z{};
    float heading{};
    float pitch{};
    bool curved = false;
};

[[nodiscard]] TrackPoint section_start(const formats::TrackVectorSection& placement);

// Point `offset` metres into a section (clamped to its length). Same maths
// as RTS 0x4882A8, expressed in MSTS axes (checked on Kuban_RTS: section
// joints median 0.9 mm): an arc of positive angle turns right; height
// follows the start pitch along the chord.
[[nodiscard]] TrackPoint evaluate_section(const formats::TrackVectorSection& placement,
                                          const formats::TrackSectionDefinition& section,
                                          double offset);

// Straight-line distance between two points on possibly different tiles.
[[nodiscard]] double distance(const TrackPoint& a, const TrackPoint& b);
// Absolute heading difference in radians, wrapped to [0, pi].
[[nodiscard]] double heading_difference(float a, float b);

inline constexpr float track_gauge = 1.52f;

// The cant of the track through a curve, radians, positive the way the curve
// turns: laid for `design_speed` m/s, less 0.05 m of underbalance, at most
// 0.15 m at `track_gauge`; straight track has none.
[[nodiscard]] float cant_angle(float curvature, float radius, float design_speed) noexcept;

inline constexpr double tile_size = 2048.0;

} // namespace openrts::simulation
