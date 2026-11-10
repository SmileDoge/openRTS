// The driver's head on its neck in the cab (viewer/src/head_motion): still
// at rest, thrown forward by braking and back once at the stop, jolted by
// the body, the same at any frame rate, held at the neck's reach; the
// camera's nod and tilt from its lag.
#include <cmath>
#include <iostream>
#include <string>

#include <glm/geometric.hpp>

#include "../viewer/src/head_motion.hpp"

namespace {

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

using openrts::viewer::HeadMotion;

// `seconds` at `fps` with the body still at `body` and the train's own
// acceleration `carried`.
void run(HeadMotion& head, const glm::vec3& body, const glm::vec3& carried, const float seconds, const float fps) {
    const int frames = static_cast<int>(std::lround(seconds * fps));
    for (int f = 0; f < frames; ++f) head.step(body, carried, 1.0f / fps);
}

} // namespace

int main() {
    using namespace openrts::viewer;
    const glm::vec3 ahead{0.0f, 0.0f, 1.0f}; // looking along the train front

    {
        HeadMotion head;
        run(head, {}, {}, 2.0f, 60.0f);
        check(glm::length(head.lag()) == 0.0f, "at rest the head stays where it is");
    }
    {
        // Service braking (0.7 m/s^2) going forward, then the stop.
        HeadMotion head;
        run(head, {}, {0.0f, 0.0f, -0.7f}, 4.0f, 60.0f);
        const float thrown = head.lag().z;
        std::cerr << "  braking 0.7 m/s^2: lag " << thrown * 1000.0f << " mm, nod "
                  << head_pose(glm::vec3{0.0f, 0.0f, thrown}, 0.0f, ahead, 1.0f).nod * 57.2958f << " deg\n";
        check(thrown > 0.015f && thrown < 0.04f, "braking throws the head forward by centimetres");
        const auto pose = head_pose(glm::vec3{0.0f, 0.0f, thrown}, 0.0f, ahead, 1.0f);
        check(pose.nod > 0.02f && pose.nod < 0.06f, "thrown forward it looks down, a degree or few");
        float back = 0.0f;
        for (int f = 0; f < 120; ++f) {
            head.step({}, {}, 1.0f / 60.0f);
            back = (std::min)(back, head.lag().z);
        }
        std::cerr << "  at the stop it swings back " << -back * 1000.0f << " mm\n";
        check(back < -0.003f, "at the stop it nods back once");
        run(head, {}, {}, 6.0f, 60.0f);
        check(std::abs(head.lag().z) < 0.001f, "and settles");
    }
    {
        // The same braking at 30 and at 240 frames/s.
        HeadMotion slow, fast;
        run(slow, {}, {0.0f, 0.0f, -0.7f}, 1.0f, 30.0f);
        run(fast, {}, {0.0f, 0.0f, -0.7f}, 1.0f, 240.0f);
        check(std::abs(slow.lag().z - fast.lag().z) < 0.1f * std::abs(fast.lag().z), "the frame rate does not change it");
    }
    {
        // A rail joint: the body knocked up 8 mm in a frame.
        HeadMotion head;
        head.reset({});
        head.step({0.0f, 0.008f, 0.0f}, {}, 1.0f / 60.0f);
        head.step({0.0f, 0.008f, 0.0f}, {}, 1.0f / 60.0f);
        check(head.lag().y < -0.001f, "a joint's knock leaves the head behind");
        check(head_pose(glm::vec3{0.0f, head.lag().y, 0.0f}, head.lag().y, ahead, 1.0f).nod > 0.0f, "and nods it down");
    }
    {
        HeadMotion head;
        run(head, {}, {0.0f, 0.0f, -50.0f}, 1.0f, 60.0f);
        check(glm::length(head.lag()) <= 0.12f * 1.001f, "a hit holds it at the neck's reach");
    }
    {
        // The screen's right of a camera looking along +z is +x (ViewerCamera::move).
        const auto right = head_pose(glm::vec3{0.02f, 0.0f, 0.0f}, 0.0f, ahead, 1.0f);
        check(right.tilt > 0.0f && std::abs(right.nod) < 1e-6f, "thrown to the right it leans right");
        const auto off = head_pose(glm::vec3{0.0f, 0.0f, 0.03f}, 0.0f, ahead, 0.0f);
        check(off.nod == 0.0f && off.tilt == 0.0f && glm::length(off.shift) == 0.0f, "0: no head motion (RTS)");
        const auto twice = head_pose(glm::vec3{0.0f, 0.0f, 0.01f}, 0.0f, ahead, 2.0f);
        const auto once = head_pose(glm::vec3{0.0f, 0.0f, 0.01f}, 0.0f, ahead, 1.0f);
        check(std::abs(twice.nod - 2.0f * once.nod) < 1e-5f, "the setting scales it");
    }
    if (failures == 0) std::cout << "head motion: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
