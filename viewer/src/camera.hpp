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

    [[nodiscard]] glm::vec3 orbit_eye() const;
    [[nodiscard]] glm::vec3 forward() const;
    [[nodiscard]] glm::vec3 eye() const;
    [[nodiscard]] glm::mat4 view() const;

    void rotate(float pixels_x, float pixels_y);
    void zoom(float scroll_steps);
    void toggle_fly();
    void move(const FlyMovement& movement, float seconds);
};

} // namespace openrts::viewer
