#include <openrts/renderer/model_renderer.hpp>

#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

// Backend-independent part of the model renderer API.
namespace openrts::renderer {

glm::vec3 rts_sun_direction(const float azimuth_degrees, const float elevation_degrees) {
    const auto azimuth = glm::radians(azimuth_degrees);
    const auto elevation = glm::radians(elevation_degrees);
    return {
        std::sin(azimuth) * std::cos(elevation),
        -std::sin(elevation),
        -std::cos(azimuth) * std::cos(elevation),
    };
}

void LightRings::reset(const glm::vec3& camera, const float max_light_distance, const float view_distance) {
    for (auto& ring : lights) ring.clear();
    for (auto& ring : distances) ring.clear();
    all.clear();
    eye = camera;
    // Without MaxLightDistance RTS rings the lights by ViewDistance (0x408574).
    light_step = (max_light_distance > 0.0f ? max_light_distance : view_distance) / static_cast<float>(count);
    object_step = view_distance / static_cast<float>(count);
    placed_ = 0;
}

void LightRings::add(const ModelExtraLight& light, const glm::vec3& centre, const float reach) {
    all.push_back({light, centre, reach});
    const float distance = glm::distance(eye, centre);
    bool placed = false;
    for (std::size_t k = 0; k < count; ++k) {
        const float ring_centre = static_cast<float>(k + 1) * light_step;
        if (reach + light_step < std::abs(ring_centre - distance)) continue;
        auto& ring = lights[k];
        auto& ring_distances = distances[k];
        if (ring.size() < capacity) {
            ring.push_back(light);
            ring_distances.push_back(distance);
            placed = true;
            continue;
        }
        // Full: the farthest light, if it is farther than this one, gives way.
        const auto farthest = std::max_element(ring_distances.begin(), ring_distances.end());
        if (*farthest > distance) {
            const auto at = static_cast<std::size_t>(farthest - ring_distances.begin());
            ring[at] = light;
            ring_distances[at] = distance;
            placed = true;
        }
    }
    if (placed) ++placed_;
}

std::uint32_t LightRings::ring_for(const glm::vec3& centre, const float radius) const noexcept {
    if (lights[0].empty() || object_step <= 0.0f) return 0;
    const float distance = glm::distance(eye, centre) - radius;
    if (distance <= 0.0f) return 0;
    const auto ring = static_cast<std::size_t>(distance / object_step);
    return static_cast<std::uint32_t>((std::min)(ring, count - 1));
}

} // namespace openrts::renderer
