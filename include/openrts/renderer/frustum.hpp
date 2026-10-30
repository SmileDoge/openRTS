#pragma once

#include <array>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace openrts::renderer {

// View frustum for visibility tests, extracted from a view-projection matrix
// with WebGPU clip conventions (depth 0..1). Planes point inwards and are
// normalised, so plane distances are in world units.
class Frustum {
public:
    [[nodiscard]] static Frustum from_view_projection(const glm::mat4& view_projection);

    // True unless the sphere lies entirely outside one plane. Spheres that
    // contain the camera or straddle a plane are always visible.
    [[nodiscard]] bool intersects_sphere(const glm::vec3& center, float radius) const noexcept;

private:
    std::array<glm::vec4, 6> planes_{};
};

} // namespace openrts::renderer
