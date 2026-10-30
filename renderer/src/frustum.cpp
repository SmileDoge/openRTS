#include <openrts/renderer/frustum.hpp>

#include <glm/geometric.hpp>

namespace openrts::renderer {

Frustum Frustum::from_view_projection(const glm::mat4& view_projection) {
    // Rows of the (column-major) matrix.
    const auto row = [&](const int index) {
        return glm::vec4{view_projection[0][index], view_projection[1][index], view_projection[2][index],
                         view_projection[3][index]};
    };
    const auto x = row(0);
    const auto y = row(1);
    const auto z = row(2);
    const auto w = row(3);

    Frustum frustum;
    frustum.planes_ = {
        w + x, // left:   -w <= x
        w - x, // right:   x <= w
        w + y, // bottom: -w <= y
        w - y, // top:     y <= w
        z,     // near:    0 <= z
        w - z, // far:     z <= w
    };
    for (auto& plane : frustum.planes_) {
        const auto length = glm::length(glm::vec3{plane});
        if (length > 0.0f) plane /= length;
    }
    return frustum;
}

bool Frustum::intersects_sphere(const glm::vec3& center, const float radius) const noexcept {
    for (const auto& plane : planes_) {
        if (glm::dot(glm::vec3{plane}, center) + plane.w < -radius) return false;
    }
    return true;
}

} // namespace openrts::renderer
