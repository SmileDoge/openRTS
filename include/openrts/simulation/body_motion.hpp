#pragma once

#include <cstdint>

namespace openrts::formats {
struct WagonDefinition;
}

// How a car's body moves on its springs. RTS's model (0x4F5B34, Train) is a
// spring along three axes driven by one sine of the speed; RTS only ever
// moved the driver's camera with it, never a body. openRTS's (this file,
// docs/reverse/rolling-stock.md "openRTS's body motion") moves the body
// over its bogies: bounce, pitch, sway, yaw and roll, driven by the track
// under each bogie (its errors, laid along the track, so each car meets the
// same ones after the car ahead), the rail joints and junctions each bogie
// passes, the bogies' own hunting, the curves and the train's pull and
// brake; each car with springs of its own.
namespace openrts::simulation {

enum class BodyMotionModel : std::uint8_t {
    openrts,
    rts,
};

// The track's errors at a point of a node (openRTS): the rails' level
// (m, up), the line's shift (m, towards the right of the node's direction)
// and the cross level (rad, the right rail up, looking along the node).
// From the node's index, the same at every run; nothing at the node's ends
// (its junctions), so nodes join without a step.
struct TrackIrregularity {
    float vertical{};
    float lateral{};
    float cross_level{};
};
[[nodiscard]] TrackIrregularity track_irregularity(std::uint32_t node, double length, double offset);
// The same over a bogie's wheelbase (`wheelbase` m around the point): the
// bogie rides over the shorter waves.
[[nodiscard]] TrackIrregularity bogie_irregularity(std::uint32_t node, double length, double offset, double wheelbase);
// A rail joint's dip (m), by the joint's node and its stretch index.
[[nodiscard]] float joint_depth(std::uint32_t node, double index);

// A body's springs: natural angular frequencies (rad/s) and damping ratios
// of its modes, and its bogies' hunting.
struct BodyModes {
    float bounce{}, pitch{}, sway{}, yaw{}, roll{}, surge{};
    float vertical_damping{}, lateral_damping{}, roll_damping{}, surge_damping{};
    float hunting_wavelength{}; // m
    float hunting_amplitude{};  // m at a bogie, at full speed
    float swell_length{};       // m: the hunting comes and goes over this
};
// From the car's FrameSpringStiffness / FrameSpringDamping when it has
// them (their bounce frequency and damping, held to sensible ranges), else
// by its kind; each car a little different by `seed`.
[[nodiscard]] BodyModes body_modes(const formats::WagonDefinition& wagon, float mass, std::uint64_t seed);

// What moves the body in one step. Bogie 0 is the one towards the train's
// front, 1 towards its rear; lateral values towards the body's lateral axis
// (CarState::body[0]).
struct BodyInput {
    TrackIrregularity bogie[2]; // the track under each bogie, in the car's axes
    float kick_vertical[2]{};   // m/s downwards at the bogie (a rail joint, a junction): the slow springs
    float kick_lateral[2]{};    // m/s towards the lateral axis: the slow springs
    // A junction pushes the bogie's end aside this far (m, towards the
    // lateral axis): a smooth push over the switch rails and the frog,
    // `throw_time` s long, that a stiff spring of its own softens (not the
    // slow sway). Until 2026-11-14 a sharp throw of 1.5-2 cm (testers,
    // 2026-11-12), which then jerked the body like a blow.
    float throw_lateral[2]{};
    float throw_time = 0.2f;
    // A rail joint or a frog knocks the bogie's end down this far (m): a
    // short, crisp knock of its own.
    float knock[2]{};
    float speed{};              // m/s, its size
    float lateral_acceleration{}; // m/s^2 the curve gives the car towards its lateral axis (v^2 * curvature)
    float acceleration{};       // m/s^2 towards the train front
    float base = 15.0f;         // m between the bogies
};

struct BodyMotion {
    // Offsets from the rest pose: lateral (towards CarState::body[0]'s
    // axis), vertical, longitudinal (towards the train front), m; and roll
    // (the top towards the lateral axis), pitch (the front up), yaw (the
    // front towards the lateral axis), rad.
    float shift[3]{};
    float turn[3]{};
    float shift_rate[3]{};
    float turn_rate[3]{};
    float hunting[2]{};          // rad, each bogie's hunting phase
    float travelled{};           // m, for the hunting's swell
    TrackIrregularity last[2]{}; // the track under the bogies a step ago
    bool started = false;        // `last` holds a step
    std::uint64_t seed = 0;      // 0: not given yet
    BodyInput input{};           // this step's, filled as the train moves
    // Each end's throw aside and knock down (m, m/s): stiff, well damped
    // springs of their own, over in a fraction of a second.
    float jolt[2]{}, jolt_rate[2]{};
    // The push under each end now: its size (m), length and clock (s).
    float push[2]{}, push_length[2]{}, push_clock[2]{};
    float knocked[2]{}, knocked_rate[2]{};
    // The pose: the springs above with the throws and knocks on top, in the
    // axes of shift and turn. What the car shows.
    float pose_shift[3]{};
    float pose_turn[3]{};
};

// One step (`dt` s, in sub-steps) of `motion` with its `input`; the input's
// kicks are taken.
void step_body(BodyMotion& motion, const BodyModes& modes, float dt);

// The car turned end for end in its train (Train::reverse): the same body
// motion in the new axes.
void reverse_body(BodyMotion& motion);

} // namespace openrts::simulation
