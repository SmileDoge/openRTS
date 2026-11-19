#include <openrts/simulation/track_geometry.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace openrts::simulation {

TrackPoint section_start(const formats::TrackVectorSection& placement) {
    return {
        .tile_x = placement.tile_x,
        .tile_z = placement.tile_z,
        .x = placement.x,
        .y = placement.y,
        .z = placement.z,
        .heading = placement.heading,
        .pitch = placement.pitch,
    };
}

TrackPoint evaluate_section(const formats::TrackVectorSection& placement,
                            const formats::TrackSectionDefinition& section,
                            const double offset) {
    auto point = section_start(placement);
    const double length = section.length;
    const double s = std::clamp(offset, 0.0, length);
    const double fraction = length > 0.0 ? s / length : 0.0;

    // Displacement in the section's own frame: forward along the start
    // heading, lateral to the right.
    double forward = s;
    double lateral = 0.0;
    double turn = 0.0;
    constexpr double degrees = std::numbers::pi / 180.0;
    if (section.curved && section.radius > 0.0f) {
        turn = section.angle_degrees * degrees * fraction;
        forward = section.radius * std::sin(std::abs(turn));
        lateral = section.radius * (1.0 - std::cos(turn)) * (turn > 0.0 ? 1.0 : -1.0);
        point.curved = true;
    } else if (section.skewed) {
        // RTS scales the skew angle with the fraction as well.
        turn = section.angle_degrees * degrees * fraction;
        forward = std::cos(turn) * s;
        lateral = std::sin(turn) * s;
    }

    const double heading = placement.heading;
    point.x += std::sin(heading) * forward + std::cos(heading) * lateral;
    point.z += std::cos(heading) * forward - std::sin(heading) * lateral;
    point.y -= std::sin(placement.pitch) * forward;
    point.heading = static_cast<float>(heading + turn);
    return point;
}

double distance(const TrackPoint& a, const TrackPoint& b) {
    const double dx = (b.tile_x - a.tile_x) * tile_size + (b.x - a.x);
    const double dz = (b.tile_z - a.tile_z) * tile_size + (b.z - a.z);
    const double dy = b.y - a.y;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

double heading_difference(const float a, const float b) {
    double difference = std::fmod(std::abs(static_cast<double>(a) - b), 2.0 * std::numbers::pi);
    if (difference > std::numbers::pi) difference = 2.0 * std::numbers::pi - difference;
    return difference;
}

} // namespace openrts::simulation
