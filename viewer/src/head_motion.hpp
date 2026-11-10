#pragma once

#include <glm/vec3.hpp>

namespace openrts::viewer {

// The driver's head on its neck in the cab (openRTS's head motion; RTS
// moves the cab camera by the body's offsets alone, docs/reverse/
// rolling-stock.md). Per car axis (lateral towards the curve's turning
// side, vertical, along the train front) a mass on a spring and a damper:
// carried by the body on its springs, pushed by the train's own
// acceleration. Its lag behind the body nods and tilts the camera.
struct HeadMotion {
    glm::vec3 position{}; // m, car axes: where the head is (0: at rest on the body at rest)
    glm::vec3 velocity{}; // m/s
    glm::vec3 body{};     // the body's offset it was carried by at the last step
    bool started = false;

    // At rest on the body at `at`.
    void reset(const glm::vec3& at);
    // One frame of `elapsed` s: the body's offset on its springs now
    // (car axes, m; moving evenly from the last one over the frame) and the
    // train's own acceleration (car axes, m/s^2: towards the curve's
    // inside v^2 / R, along the front its acceleration).
    void step(const glm::vec3& at, const glm::vec3& carried, float elapsed);
    // The head's lag behind the body (car axes, m).
    [[nodiscard]] glm::vec3 lag() const { return position - body; }
};

// The camera's share of a lag: nod (radians, + looks down), tilt (radians,
// + the head's top towards the screen's right), shift (m, world).
struct HeadPose {
    float nod = 0.0f;
    float tilt = 0.0f;
    glm::vec3 shift{};
};

// The pose for a lag `lag_world` (world axes) of which `lag_up` is vertical,
// the camera looking along `forward` (horizontal, unit), times `amount`
// (the setting: 0 none, 1 as tuned).
[[nodiscard]] HeadPose head_pose(const glm::vec3& lag_world, float lag_up, const glm::vec3& forward, float amount);

} // namespace openrts::viewer
