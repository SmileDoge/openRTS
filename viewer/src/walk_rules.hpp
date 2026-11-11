#pragma once

// RTS's rules for the engineer on foot outside a cab (rts1.exe, static
// analysis; docs/reverse/walking.md): a passage along a trajectory
// (0x4352B0 sets it up, 0x435654 runs it each frame), a step on a car's
// outside (0x434444, mode 6) and a step among the world's objects' boxes
// on the ground (0x4322F0, mode 1). Coordinates are as the files write them
// (RTS keeps X negated in its own frame; the rules are the same).

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <openrts/formats/cab_description.hpp>
#include <openrts/formats/shape_description.hpp>

namespace openrts::viewer {

// A passage: from where he stands to the trajectory's point nearest him,
// then point by point to its end (or its start, `reverse`), at the speed of
// the kind of the point he goes to (RTS 0x435654):
//   0 (and any other) the pace; 1 the pace, slowing in the last 0.3 of the
//   stretch; 2 speeding up in its first 0.4 and slowing in its last 0.3;
//   3 speeding up only; 4 "a jerk" (0.4 the pace, a burst at 0.75..0.9);
//   5 at once; 6 by the horizontal distance, at least twice the pace.
class Passage {
public:
    // `feet` and `points` in the frame he moves in.
    void start(std::vector<formats::TrajectoryPoint> points, const glm::vec3& feet, bool reverse);
    // Moves `feet` (his feet, not his eye) on; false once it has ended: then
    // his height is the last point's.
    bool step(glm::vec3& feet, float elapsed, float pace);
    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] bool reverse() const noexcept { return reverse_; }
    void stop() noexcept { active_ = false; }
    // The frame's origin moved (scene passages: the climb onto a box).
    void shift(const glm::vec3& delta) noexcept;

private:
    std::vector<formats::TrajectoryPoint> points_;
    glm::vec3 start_{};
    int index_ = 0;
    bool reverse_ = false;
    bool active_ = false;
    float longest_ = 0.0f; // the stretch's length (RTS +504: the most it has been)
    float speed_ = -1.0f;  // m/s; < 0: not set yet (the pace)
};

// Where the engineer can step on a car's outside (RTS 0x434444): the place
// he steps to tested against the car's ESD_Exterior_Details, in its frame.
struct OutsideFooting {
    enum class Kind : std::uint8_t { none, plane, ledge, door };
    Kind kind = Kind::none;
    std::size_t index{};
    glm::vec3 at{}; // where he stands: at the plane's height, on the ledge, or (a door's plane) as he was
};
// `door_open`: an ExteriorDoor's state. A plane holds a point inside it
// (its corners sorted, 0x45B750) that is not more than 0.05 m under it: the
// first such plane, its height from y2 at the low Z by (y2 - y1) per its
// length along Z (RTS's own formula; every plane in the 2026-11 build is
// flat). Else a ledge's segment within 0.3 m (the last such); else an open
// door's InteriorPlane, as a plane.
[[nodiscard]] OutsideFooting outside_footing(const formats::ExteriorDetails& details,
                                             const std::function<bool(std::size_t)>& door_open, const glm::vec3& at);

// A world object's boxes, placed (its shape's frame -> the scene).
struct PlacedBoxes {
    const std::vector<formats::WalkBox>* boxes = nullptr;
    glm::mat4 transform{1.0f};
    glm::mat4 inverse{1.0f};
};
// What a step on the ground does among the objects' boxes (RTS 0x4322F0,
// mode 1; within 0.3 m of a box):
// - where his head would be inside a box, `to` is pushed out of it across
//   its nearer side (a wall, a roof);
// - feet on its top (within 0.3 m above it): he stands on it;
// - a top less than 0.4 Height above his feet: he steps onto it; from 0.4
//   to 0.9 Height: he climbs it, `climb` the three points of RTS's path
//   (half way up and up where he stands, then over to the place, scene
//   frame); higher, a wall: `to` pushed out across the side nearer his feet.
struct BoxStep {
    glm::vec3 to{};
    bool on_box = false;                          // to.y the top he stands on
    std::optional<std::vector<formats::TrajectoryPoint>> climb;
};
[[nodiscard]] BoxStep step_among_boxes(const std::vector<PlacedBoxes>& objects, const glm::vec3& from, const glm::vec3& to,
                                       float height);

} // namespace openrts::viewer
