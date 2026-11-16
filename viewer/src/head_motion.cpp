#include "head_motion.hpp"

#include <algorithm>
#include <cmath>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

namespace openrts::viewer {
namespace {

constexpr float two_pi = 6.28318531f;
// The neck per car axis (lateral, vertical, along the train front): natural
// frequencies (Hz) and damping ratios of a seated person's head and trunk
// in a swaying vehicle, about 1 Hz fore and aft and sideways, 2 Hz up and
// down; damped so that a stop nods the head back once and it settles
// (fore and aft 0.5: until 2026-11-15 0.3, and with the setting raised a
// stop under the independent brake rocked it back and forth, testers).
constexpr glm::vec3 frequency{0.9f, 2.0f, 0.85f};
constexpr glm::vec3 damping{0.45f, 0.35f, 0.5f};
constexpr float longest_step = 1.0f / 240.0f; // s
constexpr float farthest = 0.12f;             // m: the lag at most (a hit, a derailment)
// Radians per metre of lag: forward and aside the nod and the tilt (service
// braking, 0.7 m/s^2, nods about 2 degrees), up and down a jolt's nod.
constexpr float nod_per_metre = 1.5f;
constexpr float tilt_per_metre = 1.5f;
constexpr float jolt_per_metre = 2.5f;
constexpr float widest = 0.2f; // radians: the nod and the tilt at most
// The seat lean (seat_lean): smoothing, and per m/s^2 at the default setting.
constexpr float lean_time = 0.6f;          // s
constexpr float lean_metres = 0.014f;      // m
constexpr float lean_radians = 0.022f;     // about 1.3 degrees
constexpr float lean_most = 1.5f;          // m/s^2 counted at most
constexpr float lean_default = 0.25f;      // key_layout.hpp default_head_motion

} // namespace

void HeadMotion::reset(const glm::vec3& at) {
    position = body = at;
    velocity = {};
    started = true;
}

void HeadMotion::step(const glm::vec3& at, const glm::vec3& carried, const float elapsed) {
    if (!started) reset(at);
    if (!(elapsed > 0.0f)) return;
    const glm::vec3 omega = frequency * two_pi;
    const glm::vec3 stiffness = omega * omega;
    const glm::vec3 resistance = 2.0f * damping * omega;
    const int steps = std::clamp(static_cast<int>(std::ceil(elapsed / longest_step)), 1, 240);
    const float dt = elapsed / static_cast<float>(steps);
    const glm::vec3 from = body;
    const glm::vec3 body_rate = (at - from) / elapsed;
    // Only the neck's spring and damper hold the head to the body; the
    // train's own acceleration acts on it as on everything in the car.
    for (int s = 1; s <= steps; ++s) {
        const glm::vec3 base = from + (at - from) * (static_cast<float>(s) / static_cast<float>(steps));
        const glm::vec3 acceleration = -stiffness * (position - base) - resistance * (velocity - body_rate) - carried;
        velocity += acceleration * dt;
        position += velocity * dt;
    }
    body = at;
    lean += (carried.z - lean) * (1.0f - std::exp(-elapsed / lean_time));
    // A hit or a derailment: held at the neck's reach.
    const glm::vec3 lagging = position - body;
    const glm::vec3 held = glm::clamp(lagging, glm::vec3{-farthest}, glm::vec3{farthest});
    if (held != lagging) {
        position = body + held;
        velocity = body_rate;
    }
}

HeadPose head_pose(const glm::vec3& lag_world, const float lag_up, const glm::vec3& forward, const float amount) {
    if (!(amount > 0.0f)) return {};
    // The screen's right of a camera looking along `forward` (as
    // ViewerCamera::move: MSTS's mirrored X).
    const glm::vec3 right{forward.z, 0.0f, -forward.x};
    HeadPose pose;
    // Thrown forward the head looks down; jolted up, the body leaves it
    // behind and it nods down too.
    pose.nod = std::clamp(amount * (glm::dot(lag_world, forward) * nod_per_metre - lag_up * jolt_per_metre), -widest, widest);
    // Thrown aside, it leans that way.
    pose.tilt = std::clamp(amount * glm::dot(lag_world, right) * tilt_per_metre, -widest, widest);
    pose.shift = lag_world * amount;
    return pose;
}

HeadPose seat_lean(const float lean, const glm::vec3& front, const glm::vec3& forward, const float amount) {
    if (!(amount > 0.0f) || !std::isfinite(lean)) return {};
    const float share = (std::min)(amount / lean_default, 2.0f);
    const float a = std::clamp(lean, -lean_most, lean_most) * share;
    HeadPose pose;
    // Pulling away the head goes back against the train's way (towards the
    // seat back), braking forward; looking ahead it tips back (looks up)
    // pulling away and down braking.
    const glm::vec3 lag = -front * (a * lean_metres);
    pose.shift = lag;
    pose.nod = std::clamp(glm::dot(-front, forward) * a * lean_radians, -widest, widest);
    return pose;
}

} // namespace openrts::viewer
