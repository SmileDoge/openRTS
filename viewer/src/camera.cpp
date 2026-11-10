#include "camera.hpp"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include "key_layout.hpp"

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
    auto to = fly ? from + forward() : target;
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    if (fly && (head_nod != 0.0f || head_tilt != 0.0f)) {
        // The head's nod on the look's pitch, its tilt about the look: the
        // up leaning towards the screen's right (move()'s right).
        const float nodded = std::clamp(pitch + head_nod, -max_pitch - 0.1f, max_pitch + 0.1f);
        const auto cos_pitch = std::cos(nodded);
        to = from + glm::vec3{-cos_pitch * std::sin(yaw), -std::sin(nodded), -cos_pitch * std::cos(yaw)};
        const glm::vec3 right{-std::cos(yaw), 0.0f, std::sin(yaw)};
        up = up * std::cos(head_tilt) + right * std::sin(head_tilt);
    }
    // MSTS content is authored for a mirrored X axis.
    const auto mirror_x = glm::scale(glm::mat4{1.0f}, glm::vec3{-1.0f, 1.0f, 1.0f});
    return mirror_x * glm::lookAtRH(from, to, up);
}

void ViewerCamera::rotate(const float pixels_x, const float pixels_y) {
    // The player's sensitivity, and narrower with the zoom: a pixel turned
    // the same angle in a view zoomed ten times, a tenth of its width, and
    // the view jumped.
    const float step = rotate_radians_per_pixel * mouse_sensitivity() / (std::max)(view_zoom, 1.0f);
    yaw += pixels_x * step;
    pitch = std::clamp(pitch + pixels_y * step, -max_pitch, max_pitch);
}

float ViewerCamera::far_plane() const {
    return (std::max)(distance * 20.0f, min_far);
}

float ViewerCamera::vertical_fov(const float set) const {
    return 2.0f * std::atan(std::tan(set * 0.5f) / (std::max)(view_zoom, 1.0f));
}

void ViewerCamera::zoom_view(const float scroll_steps) {
    view_zoom = std::clamp(view_zoom * std::exp(scroll_steps * 0.15f), 1.0f, 10.0f);
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
