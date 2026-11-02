// The engineer on foot in a real cab (RTS content): the VL10k's cab shape
// and .sd. He sits at the driver's seat, stands up, walks to the cab door,
// opens it, goes through to the vestibule, opens the side door, gets off,
// and climbs in again.
//
//   openrts_engineer_test <RTS root>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <openrts/core/installation.hpp>
#include <openrts/core/vfs.hpp>
#include <openrts/formats/rolling_stock.hpp>

#include "../viewer/src/cab_view.hpp"
#include "../viewer/src/engineer.hpp"

using namespace openrts;
using viewer::Engineer;
using viewer::EngineerControls;

namespace {

int failures = 0;

void check(const bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

const char* mode_name(const Engineer::Mode m) {
    switch (m) {
    case Engineer::Mode::off: return "off";
    case Engineer::Mode::seated: return "seated";
    case Engineer::Mode::standing: return "standing";
    case Engineer::Mode::outside: return "outside";
    }
    return "?";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: openrts_engineer_test <RTS root>\n");
        return 2;
    }
    const std::filesystem::path root = std::filesystem::path(argv[1]);
    core::Vfs vfs;
    core::mount_installation(vfs, root, core::load_installation_config(root));
    const auto definition = formats::load_wagon(vfs, "ev_VL10k-1654", "vl10k-1654a", true);
    if (!definition.ok()) {
        std::fprintf(stderr, "no VL10k-1654a: %s\n", definition.error.c_str());
        return 2;
    }
    auto cab = viewer::CabView::create(vfs, definition, formats::wagon_path("ev_VL10k-1654", "vl10k-1654a", true));
    if (!cab) {
        std::fprintf(stderr, "no cab\n");
        return 2;
    }
    for (int i = 0; i < 600 && !cab->collision(); ++i) {
        cab->poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    check(cab->collision() != nullptr && !cab->collision()->empty(), "the cab shape's surfaces are read");
    // The locomotive signal lights (КЛУБ): Aspect ( n lights.. ) as RTS reads it.
    {
        const auto& sig = cab->description().signalization;
        check(sig.lights.size() == 8 && sig.aspects.size() == 9, "8 signal lights, 9 aspects");
        check(sig.aspects.size() == 9 && sig.aspects[0] == std::vector<std::uint32_t>{7, 15} && sig.aspects[8] == std::vector<std::uint32_t>{6, 14},
              "STOP lights the white, BLOCK_OBSTRUCTED the red (the count is not a light)");
    }
    if (!cab->collision()) return 1;

    Engineer engineer;
    engineer.configure("engineer.Height = 1.81\r\nengineer.Height1 = 0.81\r\n");
    {
        Engineer paces;
        paces.configure("engineer.velocity = 3\r\nengineer.velocity1 = 120\r\n");
        check(paces.walk_speed() == 3.0f && paces.run_speed() == 120.0f, "config.ini engineer.velocity / velocity1 (RTS read_config)");
        const Engineer defaults;
        check(defaults.walk_speed() == 1.6f && defaults.run_speed() == 5.0f, "RTS's default paces 1.6 / 5 m/s");
    }
    engineer.sit(0);
    viewer::ViewerCamera camera;
    camera.fly = true;
    const glm::mat4 car{1.0f}; // the cab shape's coordinates are the scene's
    // Flat ground 0.3 m under the rails; a wall 4.5 m out on the left, along the track.
    const viewer::Outdoors ground{
        .floor = [](const glm::vec3&) -> std::optional<float> { return -0.3f; },
        .blocked = [](const glm::vec3&, const glm::vec3& to) { return to.x < -4.5f; },
    };
    const float dt = 1.0f / 60.0f;
    auto run = [&](EngineerControls controls, const float seconds) {
        for (float t = 0.0f; t < seconds; t += dt) {
            engineer.update(*cab, controls, car, ground, dt, camera);
            controls.action = controls.window = controls.head_out = false; // presses, not holds
        }
    };
    auto where = [&](const char* what) {
        std::printf("  %-28s %-8s eye (%.2f %.2f %.2f)  prompt \"%s\"\n", what, mode_name(engineer.mode()), camera.position.x,
                    camera.position.y, camera.position.z, engineer.prompt().c_str());
    };
    const float eye = 0.85f * 1.81f;

    run({}, 0.1f);
    where("seated");
    check(std::abs(camera.position.y - (2.5f + 0.81f)) < 0.01f, "seated: the eye is Height1 above Pos1");

    // The mouse: the train brake valve from the seat.
    {
        const auto& sd = cab->description();
        const glm::vec3 eye_now = camera.position;
        // At the valve's own surfaces (RTS picks a control with a node by
        // its node under the cursor, not by the .sd Position).
        std::size_t valve_index = 0;
        for (std::size_t k = 0; k < sd.switches.size(); ++k)
            if (sd.switches[k].name == "C_394") valve_index = k;
        const glm::vec3 valve = cab->control_label_point(valve_index).value_or(glm::vec3{1.307f, 2.968f, 6.767f});
        const auto picked = cab->pick(eye_now, glm::normalize(valve - eye_now), 4.0f);
        std::printf("  mouse at the brake valve: %s\n",
                    picked ? (picked->control ? sd.switches[picked->index].name.c_str() : sd.displays[picked->index].name.c_str())
                           : "nothing");
        check(picked && picked->control && sd.switches[picked->index].name == "C_394", "the mouse finds the train brake valve");
    }

    // The window: Num1 does nothing while it is shut; Num0 opens it, Num1 puts the head out.
    run({.head_out = true}, 0.2f);
    check(!engineer.heading_out(), "the head stays in while the window is shut");
    run({.window = true}, 1.2f);
    where("window open");
    run({.head_out = true}, 1.0f);
    where("head out");
    check(engineer.heading_out() && std::abs(camera.position.x - 1.9f) < 0.01f && std::abs(camera.position.y - 3.25f) < 0.01f,
          "Num1: the eye at the window's HeadOutPosition");
    run({.head_out = true}, 1.0f);
    check(!engineer.heading_out() && std::abs(camera.position.y - (2.5f + 0.81f)) < 0.01f, "Num1 again: back in the seat");
    run({.window = true}, 1.2f);

    run({.action = true}, 1.0f);
    where("stood up");
    check(engineer.mode() == Engineer::Mode::standing, "Enter stands him up");
    check(std::abs(camera.position.y - (2.04f + eye)) < 0.05f, "standing on the cab floor (2.04)");

    // Face the back of the cab (-Z), step to the middle, walk to the door.
    camera.yaw += 3.14159265f;
    run({}, 0.05f);
    for (int i = 0; i < 240 && camera.position.x > 0.05f; ++i) run({.right = true}, dt);
    where("in the aisle");
    check(std::abs(camera.position.x) < 0.2f, "walked across to the aisle");
    run({.forward = true}, 3.0f);
    where("at the cab door");
    check(camera.position.z > 5.5f && camera.position.z < 6.2f, "the closed cab door stops him");

    run({.action = true}, 1.3f);
    run({.forward = true}, 1.2f);
    where("through the cab door");
    check(camera.position.z < 5.35f, "the open cab door lets him through to the vestibule");

    // Face the left side door (-X) and walk to it.
    camera.yaw = 3.14159265f * 0.5f;
    run({}, 0.05f);
    run({.forward = true}, 2.0f);
    where("at the side door");
    check(engineer.mode() == Engineer::Mode::standing && camera.position.x > -1.5f, "the closed side door stops him");

    run({.action = true}, 1.3f);
    run({.forward = true}, 2.0f);
    where("got off");
    check(engineer.mode() == Engineer::Mode::outside, "out through the open side door");
    check(std::abs(camera.position.y - (-0.3f + eye)) < 0.01f, "outside: on the ground");
    check(camera.position.x >= -4.5f, "outside: the wall stops him");

    run({.back = true}, 1.4f);
    run({.forward = true}, 0.3f);
    where("next to the door");
    run({.action = true, .action_ctrl = true}, 0.2f); // Enter would only open or close the door
    where("climbed in");
    check(engineer.mode() == Engineer::Mode::standing, "Enter at the door climbs back in");

    // Through the machine room to the back door, into the next section.
    // He stands in the corridor along the left side (the side door behind him is open).
    camera.yaw = 0.0f; // -Z: to the back
    run({}, 0.05f);
    run({.forward = true}, 10.0f);
    where("end of the corridor");
    camera.yaw = -3.14159265f * 0.5f; // +X: to the middle
    run({}, 0.05f);
    for (int i = 0; i < 240 && camera.position.x < -0.05f; ++i) run({.forward = true}, dt);
    camera.yaw = 0.0f;
    run({}, 0.05f);
    run({.forward = true}, 1.5f);
    where("at the back door");
    run({.action = true}, 1.3f);
    std::optional<std::size_t> passed;
    for (int i = 0; i < 180 && !passed; ++i) {
        engineer.update(*cab, {.forward = true}, car, ground, dt, camera);
        passed = engineer.take_pass();
    }
    where("into the next section");
    check(passed && cab->description().doors[*passed].name == "DOOR3", "the open back door leads into the next section");

    std::printf("%s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
