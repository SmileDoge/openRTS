#include <cstdio>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <openrts/renderer/frustum.hpp>

namespace {

int failures = 0;

void check(const bool condition, const char* what) {
    if (!condition) {
        std::printf("FAILED: %s\n", what);
        ++failures;
    }
}

} // namespace

int main() {
    using openrts::renderer::Frustum;

    // Same conventions as the viewer: mirrored X, right-handed look-at, depth 0..1.
    const auto mirror = glm::scale(glm::mat4{1.0f}, glm::vec3{-1.0f, 1.0f, 1.0f});
    const auto view = mirror * glm::lookAtRH(glm::vec3{0.0f, 2.0f, 0.0f}, glm::vec3{0.0f, 2.0f, -1.0f},
                                             glm::vec3{0.0f, 1.0f, 0.0f});
    const auto projection = glm::perspectiveRH_ZO(0.85f, 16.0f / 9.0f, 2.0f, 20000.0f);
    const auto frustum = Frustum::from_view_projection(projection * view);

    check(frustum.intersects_sphere({0.0f, 2.0f, -50.0f}, 1.0f), "object straight ahead");
    check(!frustum.intersects_sphere({0.0f, 2.0f, 50.0f}, 1.0f), "object behind the camera");
    check(!frustum.intersects_sphere({500.0f, 2.0f, -50.0f}, 1.0f), "object far to the side");
    check(!frustum.intersects_sphere({0.0f, 2.0f, -30000.0f}, 1.0f), "object beyond the far plane");

    // Regression: the camera is inside a large object whose centre is behind
    // and beside it (a building or a platform the camera stands on).
    check(frustum.intersects_sphere({8.0f, 0.0f, 9.0f}, 15.0f), "camera inside a sphere centred behind it");
    // A big object straddling the side plane near the camera.
    check(frustum.intersects_sphere({40.0f, 0.0f, -5.0f}, 36.0f), "large object straddling the side plane");

    if (failures == 0) std::printf("frustum: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
