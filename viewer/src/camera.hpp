#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace openrts::viewer {

struct FlyMovement {
    bool forward = false;
    bool backward = false;
    bool left = false;
    bool right = false;
    bool up = false;
    bool down = false;
    bool boost = false;
};

// Orbit camera around `target`, switchable to a free-flying camera at `position`.
// Angles are radians; `distance` is the orbit radius and also scales the clip
// range, so it stays meaningful in fly mode.
struct ViewerCamera {
    float yaw = 0.7f;
    float pitch = 0.25f;
    float distance = 12.0f;
    glm::vec3 target{};
    glm::vec3 position{};
    bool fly = false;
    // The view's zoom: the field of view narrowed this many times (1: as
    // set). Ctrl and the wheel outside the cab.
    float view_zoom = 1.0f;
    // The far plane at least this far, m (the car view's orbit is close but
    // looks along the line); 0: by the distance alone.
    float min_far = 0.0f;

    [[nodiscard]] glm::vec3 orbit_eye() const;
    [[nodiscard]] glm::vec3 forward() const;
    [[nodiscard]] glm::vec3 eye() const;
    [[nodiscard]] glm::mat4 view() const;
    [[nodiscard]] float far_plane() const;
    // The vertical field of view with the zoom, from the set one (radians).
    [[nodiscard]] float vertical_fov(float set) const;

    void rotate(float pixels_x, float pixels_y);
    void zoom(float scroll_steps);
    void zoom_view(float scroll_steps);
    void toggle_fly();
    void move(const FlyMovement& movement, float seconds);
};

} // namespace openrts::viewer
