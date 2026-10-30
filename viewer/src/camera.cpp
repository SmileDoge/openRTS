#include "camera.hpp"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

namespace openrts::viewer {
namespace {

constexpr float rotate_radians_per_pixel = 0.008f;
constexpr float max_pitch = 1.45f;
constexpr float zoom_per_scroll_step = 0.12f;
constexpr float min_distance = 0.1f;
constexpr float max_distance = 100000.0f;
constexpr float fly_speed = 100.0f;
constexpr float fly_boost_speed = 500.0f;

} // namespace

glm::vec3 ViewerCamera::orbit_eye() const {
    const auto cos_pitch = std::cos(pitch);
    return {
        target.x + distance * cos_pitch * std::sin(yaw),
        target.y + distance * std::sin(pitch),
        target.z + distance * cos_pitch * std::cos(yaw),
    };
}

glm::vec3 ViewerCamera::forward() const {
    const auto cos_pitch = std::cos(pitch);
    return {-cos_pitch * std::sin(yaw), -std::sin(pitch), -cos_pitch * std::cos(yaw)};
}

glm::vec3 ViewerCamera::eye() const {
    return fly ? position : orbit_eye();
}

glm::mat4 ViewerCamera::view() const {
    const auto from = eye();
    const auto to = fly ? from + forward() : target;
    // MSTS content is authored for a mirrored X axis.
    const auto mirror_x = glm::scale(glm::mat4{1.0f}, glm::vec3{-1.0f, 1.0f, 1.0f});
    return mirror_x * glm::lookAtRH(from, to, glm::vec3{0.0f, 1.0f, 0.0f});
}

void ViewerCamera::rotate(const float pixels_x, const float pixels_y) {
    yaw += pixels_x * rotate_radians_per_pixel;
    pitch = std::clamp(pitch + pixels_y * rotate_radians_per_pixel, -max_pitch, max_pitch);
}

void ViewerCamera::zoom(const float scroll_steps) {
    distance = std::clamp(distance * std::exp(-scroll_steps * zoom_per_scroll_step), min_distance, max_distance);
}

void ViewerCamera::toggle_fly() {
    if (fly) {
        target = position + forward() * distance;
        fly = false;
    } else {
        position = orbit_eye();
        fly = true;
    }
}

void ViewerCamera::move(const FlyMovement& movement, const float seconds) {
    if (!fly) return;

    const float step = (movement.boost ? fly_boost_speed : fly_speed) * seconds;
    const auto ahead = forward();
    const glm::vec3 right{ahead.z, 0.0f, -ahead.x};

    if (movement.forward) position += ahead * step;
    if (movement.backward) position -= ahead * step;
    if (movement.right) position += right * step;
    if (movement.left) position -= right * step;
    if (movement.up) position.y += step;
    if (movement.down) position.y -= step;
}

} // namespace openrts::viewer
