#pragma once

// openRTS's sky (native_sky.hpp) through the fog: the horizon's colour by
// direction, so the far land fogs into the sky behind it. RTS's fog has one
// colour, made for its dome's lowest layer behind the land; under a sky
// that is orange towards the sunset and blue away from it, one colour (any
// average) turned the far hills a flat orange or yellow-green.

#include <array>
#include <cmath>
#include <cstddef>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace openrts::renderer {

struct FogRing {
    static constexpr std::size_t count = 12;
    bool on = false;
    glm::vec2 sun{0.0f, 1.0f}; // the sun's way along the ground (x, z; unit)
    // The horizon (display encoded, as RTS's fog colour) at fog_ring_angle(i)
    // from the sun's way, either side alike.
    std::array<glm::vec3, count> colours{};
};

// The ring's angles from the sun's way: pi * (i / 11)^2, crowded towards
// the sun, where its glow changes fast.
[[nodiscard]] inline float fog_ring_angle(const std::size_t i) {
    const float s = static_cast<float>(i) / static_cast<float>(FogRing::count - 1);
    return 3.14159265f * s * s;
}

} // namespace openrts::renderer
