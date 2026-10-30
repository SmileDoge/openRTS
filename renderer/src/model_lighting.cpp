#include <openrts/renderer/model_renderer.hpp>

#include <cmath>

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

} // namespace openrts::renderer
