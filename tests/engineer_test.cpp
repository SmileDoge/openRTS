// The engineer on foot in a real cab (RTS content): the VL10k's cab shape
// and .sd. He sits at the driver's seat, stands up, walks to the cab door,
// opens it, goes through to the vestibule, opens the side door, gets off,
// and climbs in again.
//
//   openrts_engineer_test <RTS root>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <openrts/core/installation.hpp>
#include <openrts/core/vfs.hpp>
#include <openrts/formats/cab_description.hpp>
#include <openrts/formats/rolling_stock.hpp>
#include <openrts/formats/shape_description.hpp>

#include "../viewer/src/cab_view.hpp"
#include "../viewer/src/engineer.hpp"
#include "../viewer/src/ui_text.hpp"

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

    // Getting on, the same at every door (RTS 0x4121BC): Enter works the
    // door, Ctrl+Enter goes in. A cab only stood at, its shape not read yet,
    // has its doors as the .sd leaves them (it read them all open before).
    {
        const auto& doors = cab->description().doors;
        std::size_t side = doors.size();
        for (std::size_t i = 0; i < doors.size(); ++i)
            if (doors[i].can_be_opened && !doors[i].opened && (doors[i].from == "EXTERIOR" || doors[i].to == "EXTERIOR")) side = i;
        check(side < doors.size(), "the VL10k has a side door to get on by");
        const auto fresh = viewer::CabView::create(vfs, definition, formats::wagon_path("ev_VL10k-1654", "vl10k-1654a", true));
        check(fresh && side < doors.size() && !fresh->door_open(side), "a cab not read yet: its shut door is shut");
        check(fresh && side < doors.size()
                  && Engineer::get_on_prompt(*fresh, side) == std::string(viewer::tr("Enter: open the door, Ctrl+Enter: climb in")),
              "a cab not read yet: the same prompt as one's own");
        check(side < doors.size() && Engineer::get_on_prompt(*cab, side) == std::string(viewer::tr("Enter: open the door, Ctrl+Enter: climb in")),
              "one's own cab: Enter opens, Ctrl+Enter climbs in");
    }
    // The ED9M's head car (its cab not read yet, as at its door): in by the
    // crew's side doors (DOOR_1 / DOOR_2); its passenger doors (the train's,
    // shut) let nobody in. Before 2026-11-06 a cab not read had every door
    // open, and Enter by a passenger door put him in its closed vestibule.
    if (const auto ed9m = formats::load_wagon(vfs, "mdd_ED9M-0032", "mdd_ED9M-0032_G", true); ed9m.ok()) {
        if (const auto head = viewer::CabView::create(vfs, ed9m, formats::wagon_path("mdd_ED9M-0032", "mdd_ED9M-0032_G", true))) {
            std::vector<std::string> names;
            std::size_t passenger = SIZE_MAX;
            for (const auto& [door, spot] : Engineer::get_on_points(*head)) {
                names.push_back(head->description().doors[door].name);
                if (names.back() == "DOOR_D") passenger = door;
            }
            const auto has = [&](const char* name) { return std::find(names.begin(), names.end(), name) != names.end(); };
            check(has("DOOR_1") && has("DOOR_2"), "ED9M: in by the crew's side doors");
            check(passenger != SIZE_MAX && !head->door_open(passenger)
                      && Engineer::get_on_prompt(*head, passenger) == std::string(viewer::tr("The passenger doors are opened from the driver's cab.")),
                  "ED9M: a shut passenger door lets nobody in");
        } else {
            check(false, "ED9M: the head car's cab");
        }
    } else {
        std::printf("no ED9M-0032 here: its doors not checked\n");
    }
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
    const float eye = 1.81f; // outside: Height above his feet (RTS)

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

    // RTS 0x4121BC: Enter in the chair rises at the desk (Pos2) and sits back
    // down, at the chair all the time; Ctrl+Enter gets him up (Pos3).
    run({.action = true}, 1.0f);
    where("risen at the desk");
    check(engineer.risen() && engineer.mode() == Engineer::Mode::seated, "Enter rises at the desk, still at the chair");
    check(std::abs(camera.position.y - (1.65f + 1.81f)) < 0.01f, "risen: the eye Height above Pos2 (1.65, RTS 0x43778C)");
    const auto risen_at = camera.position;
    run({.forward = true}, 0.5f);
    check(engineer.risen() && glm::distance(camera.position, risen_at) < 0.01f, "risen, the arrows do not walk him off");
    run({.action = true}, 1.0f);
    check(!engineer.risen() && std::abs(camera.position.y - (2.5f + 0.81f)) < 0.01f, "Enter again: back in the seat");
    run({.action = true, .action_ctrl = true}, 1.0f);
    where("stood up");
    check(engineer.mode() == Engineer::Mode::standing, "Ctrl+Enter gets him up");
    const float standing_eye = 1.75f + 1.81f; // the CABIN plane, Height above it
    check(std::abs(camera.position.y - standing_eye) < 0.01f, "standing: the eye Height above the cab's plane (1.75)");

    // RTS walks him on the .sd InteriorPlanes only (0x434C58): the VL10k's
    // CABIN is the aisle between the chairs (X -0.251..0.336). Before
    // 2026-11-11 openRTS walked the cab shape's surfaces: from the floor up
    // onto the driver's seat, from it onto the desk, the head through the roof.
    {
        float highest = camera.position.y, farthest = camera.position.x;
        for (int i = 0; i < 180; ++i) { // at the driver's chair (+X), then on to the desk (+Z)
            run({.forward = i >= 60, .right = true}, dt);
            highest = (std::max)(highest, camera.position.y);
            farthest = (std::max)(farthest, camera.position.x);
        }
        where("at the chair and the desk");
        check(highest < standing_eye + 0.01f, "he does not climb the driver's seat or the desk");
        check(farthest < 0.371f, "the driver's chair is off the planes: he stays in the aisle");
        check(camera.position.z < 6.695f, "the desk end of the aisle stops him");
    }

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
    // Walking out at an angle: sliding along the wall used to keep him in
    // until he found the one spot that stopped him dead. Now the door's
    // face lets him out where he reaches it.
    camera.yaw = 3.14159265f * 0.5f + 0.6f;
    run({}, 0.05f);
    run({.forward = true}, 0.5f);
    where("out at an angle");
    check(engineer.mode() == Engineer::Mode::outside, "walking into the open side door at an angle gets him off");
    camera.yaw = 3.14159265f * 0.5f;
    run({}, 0.05f);
    run({.forward = true}, 1.9f);
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

    // The EP2K (mdd_EP2K-Alias cab.sd): its cab doors DOOR_C / DOOR_C1 have
    // pieces that follow them (DOOR_CR1 / CR2, ParentObject 4). From the
    // machine room a piece's point is nearer than the door's own: Enter
    // opened the piece and the door stayed shut (2026-11-10).
    if (const auto ep2k = formats::load_wagon(vfs, "mdd_ep2k-117", "mdd_EP2k-117", true); ep2k.ok()) {
        auto cab2 = viewer::CabView::create(vfs, ep2k, formats::wagon_path("mdd_ep2k-117", "mdd_EP2k-117", true));
        for (int i = 0; i < 600 && cab2 && !cab2->collision(); ++i) {
            cab2->poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::size_t door_c = SIZE_MAX, piece = SIZE_MAX;
        if (cab2) {
            const auto& doors = cab2->description().doors;
            for (std::size_t i = 0; i < doors.size(); ++i) {
                if (doors[i].name == "DOOR_C") door_c = i;
                if (doors[i].name == "DOOR_CR1") piece = i;
            }
        }
        check(cab2 && cab2->collision() && door_c != SIZE_MAX && piece != SIZE_MAX
                  && cab2->description().doors[piece].parent == static_cast<std::int32_t>(door_c),
              "EP2K: DOOR_CR1 follows DOOR_C");
        if (cab2 && cab2->collision() && door_c != SIZE_MAX && piece != SIZE_MAX) {
            Engineer driver;
            driver.configure("engineer.Height = 1.81\r\nengineer.Height1 = 0.81\r\n");
            driver.sit(0);
            viewer::ViewerCamera view;
            view.fly = true;
            auto go = [&](EngineerControls controls, const float seconds) {
                for (float t = 0.0f; t < seconds; t += dt) {
                    driver.update(*cab2, controls, car, ground, dt, view);
                    controls.action = controls.window = controls.head_out = false;
                }
            };
            go({}, 0.1f);
            go({.action = true, .action_ctrl = true}, 1.0f);
            view.yaw += 3.14159265f; // to the back of the cab, the door
            go({}, 0.05f);
            go({.forward = true}, 2.0f);
            std::printf("  EP2K at the cab door        eye z %.2f\n", view.position.z);
            check(!cab2->door_open(door_c) && view.position.z > 8.6f, "EP2K: the shut cab door stops him");
            go({.action = true}, 1.3f);
            check(cab2->door_open(door_c) && cab2->door_open(piece), "EP2K: Enter opens DOOR_C, its piece with it");
            for (int i = 0; i < 300 && view.position.z > 8.0f; ++i) go({.forward = true}, dt);
            std::printf("  EP2K in the machine room    eye z %.2f\n", view.position.z);
            check(view.position.z <= 8.0f, "EP2K: through the open cab door to the machine room");
            // From the machine room: shut it and open it again, back in.
            view.yaw -= 3.14159265f;
            go({}, 0.05f);
            go({.action = true}, 1.3f);
            check(!cab2->door_open(door_c) && !cab2->door_open(piece), "EP2K: from the machine room Enter shuts the door itself");
            go({.action = true}, 1.3f);
            check(cab2->door_open(door_c), "EP2K: and opens it");
            go({.forward = true}, 1.0f);
            std::printf("  EP2K back in the cab        eye z %.2f\n", view.position.z);
            check(view.position.z > 8.8f, "EP2K: back into the cab");
        }
    } else {
        std::printf("no EP2K-117 here: its cab doors not checked\n");
    }

    // The ER9PK-328 (Pavlovsk): its cab a .repaint of mdd_ER9P-Alias/Cab/
    // cabklub.s. Its outer doors' InteriorPlanes (DOOR1 / DOOR2) go from 1.0
    // down to 0.0, the vestibule's (TAMBUR_M) is at 1.0, under the visible
    // floor (1.39). Climbing in there he stood under the floor and could not
    // walk (before 2026-11-11, on the cab shape's surfaces).
    if (const auto er9 = formats::load_wagon(vfs, "mdd_ER9PK-328", "mdd_ER9P_01", true); er9.ok()) {
        auto cab3 = viewer::CabView::create(vfs, er9, formats::wagon_path("mdd_ER9PK-328", "mdd_ER9P_01", true));
        for (int i = 0; i < 600 && cab3 && !cab3->collision(); ++i) {
            cab3->poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        check(cab3 && cab3->collision() && !cab3->collision()->empty(), "ER9PK: the repainted cab's surfaces are read");
        if (cab3 && cab3->collision()) {
            const auto& sd = cab3->description();
            // The height of the plane holding (x, z) nearest `y` (RTS's corner order).
            const auto plane_at = [&](const glm::vec3& feet) -> std::optional<float> {
                std::optional<float> best;
                auto take = [&](const formats::CabPlane& p) {
                    if (!(p.a[0] <= feet.x && feet.x <= p.b[0] && p.b[2] <= feet.z && feet.z <= p.a[2])) return;
                    const float y = p.a[1] + (p.b[1] - p.a[1]) * (p.a[2] - feet.z) / (p.a[2] - p.b[2]);
                    if (!best || std::abs(y - feet.y) < std::abs(*best - feet.y)) best = y;
                };
                for (const auto& p : sd.planes) take(p);
                for (const auto& d : sd.doors) take(d.plane);
                return best;
            };
            const auto floor_at = [&](const glm::vec3& eye) { return plane_at(eye - glm::vec3{0.0f, 1.81f, 0.0f}); };
            // Up from the driver's chair: on the cab's plane (1.2), walking.
            Engineer driver;
            driver.configure("engineer.Height = 1.81\r\nengineer.Height1 = 0.81\r\n");
            driver.sit(0);
            viewer::ViewerCamera view;
            view.fly = true;
            auto go = [&](Engineer& who, viewer::ViewerCamera& at, EngineerControls controls, const float seconds) {
                for (float t = 0.0f; t < seconds; t += dt) {
                    who.update(*cab3, controls, car, ground, dt, at);
                    controls.action = controls.window = controls.head_out = false;
                }
            };
            go(driver, view, {}, 0.1f);
            go(driver, view, {.action = true, .action_ctrl = true}, 1.0f);
            const auto standing = view.position;
            const auto cab_floor = floor_at(standing);
            check(driver.mode() == Engineer::Mode::standing && cab_floor && std::abs(*cab_floor - 1.2f) < 0.01f
                      && std::abs(standing.y - 1.2f - 1.81f) < 0.01f,
                  "ER9PK: up from the chair, on the cab's plane (1.2)");
            go(driver, view, {.forward = true}, 1.0f);
            check(glm::distance(standing, view.position) > 0.3f, "ER9PK: he walks in the cab");
            // In from outside by every door one gets on by: on the plane there, walking.
            for (const auto& [door, spot] : Engineer::get_on_points(*cab3)) {
                const auto& d = sd.doors[door];
                Engineer walker;
                walker.configure("engineer.Height = 1.81\r\nengineer.Height1 = 0.81\r\n");
                walker.arrive(*cab3, door);
                viewer::ViewerCamera in;
                in.fly = true;
                go(walker, in, {}, 0.05f);
                const auto at = in.position;
                const auto under = floor_at(at);
                in.yaw = at.x > 0.3f ? 3.14159265f * 0.5f : at.x < -0.3f ? -3.14159265f * 0.5f : 0.0f;
                go(walker, in, {}, 0.05f);
                go(walker, in, {.forward = true}, 1.0f);
                const float walked = glm::distance(at, in.position);
                std::printf("  ER9PK in by %-7s feet %.2f, the plane there %.2f, walked %.2f m\n", d.name.c_str(), at.y - 1.81f,
                            under ? *under : -9.0f, walked);
                check(under && std::abs(at.y - 1.81f - *under) < 0.01f, ("ER9PK: in by " + d.name + ", on the plane").c_str());
                check(walked > 0.3f, ("ER9PK: in by " + d.name + ", he walks").c_str());
            }
        }
    } else {
        std::printf("no ER9PK-328 here: its cab doors not checked\n");
    }

    // The ChME3 (mdd_CHME3-1277): RTS's car outside (mode 6). Up its steps
    // from the ground (ExteriorLadders EXTERIOR -> EXT_WAG), along its
    // walkway (ExteriorPlanes at 1.4), into the cab by the door DOOR_D (the
    // way through it, Connects EXT_WAG INTERIOR), out again onto the walkway
    // (the cab's DOOR_D, CABIN -> EXT_WAG), up the roof ladder and down the
    // steps to the ground. Before 2026-11-11 its cab could not be got into
    // or out of: its doors lead to the walkways, which openRTS did not have.
    if (const auto chme = formats::load_wagon(vfs, "mdd_CHME3-1277", "mdd_CHME3-1277", true); chme.ok()) {
        const auto shape = core::VirtualPath::parse("TRAINS/TRAINSET/mdd_CHME3-1277/" + chme.shape);
        const auto sd = shape ? vfs.read(formats::shape_description(vfs, *shape)) : std::nullopt;
        const auto details = sd ? formats::parse_exterior_details(*sd) : formats::ExteriorDetails{};
        check(details.ladders.size() == 6 && details.planes.size() == 6 && details.ledges.size() == 2 && details.door_details.size() == 8,
              "ChME3: 6 ladders, 6 walkway planes, 2 ledges, 8 outer doors");
        auto cab4 = viewer::CabView::create(vfs, chme, formats::wagon_path("mdd_CHME3-1277", "mdd_CHME3-1277", true));
        for (int i = 0; i < 600 && cab4 && !cab4->collision(); ++i) {
            cab4->poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::size_t outer_d = SIZE_MAX, cab_d = SIZE_MAX;
        for (std::size_t k = 0; k < details.door_details.size(); ++k)
            if (details.door_details[k].name == "DOOR_D") outer_d = k;
        for (std::size_t k = 0; cab4 && k < cab4->description().doors.size(); ++k)
            if (cab4->description().doors[k].name == "DOOR_D") cab_d = k;
        check(outer_d != SIZE_MAX && cab_d != SIZE_MAX && details.door_details[outer_d].to == "interior"
                  && details.door_details[outer_d].trajectory.size() == 2,
              "ChME3: DOOR_D leads from the walkway into the cab along its way");
        if (cab4 && outer_d != SIZE_MAX && cab_d != SIZE_MAX) {
            // The outer door follows the cab's door of its name (TrainView).
            viewer::CarOutside car_out;
            car_out.details = &details;
            car_out.width = chme.width > 1.0f ? chme.width : 3.1f;
            car_out.door_open = [&](std::size_t k) {
                while (k < details.door_details.size() && details.door_details[k].parent >= 0)
                    k = static_cast<std::size_t>(details.door_details[k].parent);
                return k == outer_d ? cab4->door_open(cab_d) : false;
            };
            car_out.cab_holds = [&](const glm::vec3& at) {
                return Engineer::cab_holds(*cab4, glm::vec3{glm::inverse(cab4->placement()) * glm::vec4{at, 1.0f}}, 1.81f);
            };
            const viewer::Outdoors flat{.floor = [](const glm::vec3&) -> std::optional<float> { return 0.0f; }};
            Engineer man;
            man.configure("engineer.Height = 1.81\r\nengineer.Height1 = 0.81\r\n");
            viewer::ViewerCamera eye_cam;
            eye_cam.fly = true;
            float doors_time = 0.0f;
            // One frame of what TrainView does with him.
            auto frame = [&](EngineerControls controls) {
                cab4->poll();
                cab4->step_doors(dt);
                doors_time += dt;
                if (man.mode() == Engineer::Mode::on_car) {
                    man.update_on_car(car_out, controls, flat, dt, eye_cam);
                } else if (man.mode() == Engineer::Mode::outside) {
                    man.update_on_foot(controls, flat, dt, eye_cam);
                } else {
                    man.update(*cab4, controls, cab4->placement(), flat, dt, eye_cam);
                }
                const auto e = man.take_event();
                using Kind = Engineer::Event::Kind;
                if (e.kind == Kind::outer_door) {
                    cab4->toggle_door(cab_d);
                } else if (e.kind == Kind::into_cab) {
                    man.enter_cab(*cab4, cab_d, glm::vec3{glm::inverse(cab4->placement()) * glm::vec4{e.at, 1.0f}});
                } else if (e.kind == Kind::onto_car) {
                    const glm::vec3 at{cab4->placement() * glm::vec4{e.at, 1.0f}};
                    std::optional<std::size_t> path;
                    if (e.by_path) path = outer_d;
                    if (e.by_path || viewer::outside_footing(details, car_out.door_open, at).kind != viewer::OutsideFooting::Kind::none)
                        man.step_onto_car(car_out, at, path);
                }
            };
            auto go = [&](EngineerControls controls, const float seconds) {
                for (float t = 0.0f; t < seconds; t += dt) {
                    frame(controls);
                    controls.action = false;
                }
            };
            auto feet_y = [&] { return eye_cam.position.y - 1.81f; };
            // On the ground by the front left steps, facing the car (+X).
            man.stand_outside({-2.3f, 0.0f, 7.736f});
            eye_cam.yaw = -3.14159265f * 0.5f;
            eye_cam.pitch = 0.3f; // + looks down
            go({}, 0.05f);
            const auto steps = Engineer::ladder_in_reach(car_out, eye_cam.position, eye_cam.forward());
            check(steps && steps->first == 1, "ChME3: the front left steps are in reach");
            check(steps && Engineer::ladder_prompt(car_out, steps->first, man.feet()) == std::string(viewer::tr("Ctrl+Enter: climb up")),
                  "ChME3: Ctrl+Enter climbs up");
            if (steps) man.climb(car_out, steps->first);
            for (int i = 0; i < 600 && man.passing(); ++i) frame({});
            std::printf("  ChME3 up the steps          %s eye (%.2f %.2f %.2f)\n", man.mode() == Engineer::Mode::on_car ? "on car" : "?",
                        eye_cam.position.x, eye_cam.position.y, eye_cam.position.z);
            check(man.mode() == Engineer::Mode::on_car && std::abs(feet_y() - 1.4f) < 0.02f && std::abs(eye_cam.position.x + 1.124f) < 0.05f,
                  "ChME3: up the steps onto the left walkway (1.4)");
            // Along the walkway to the cab door; it stops him (shut).
            eye_cam.yaw = 0.0f; // -Z
            eye_cam.pitch = 0.0f;
            go({.forward = true}, 8.0f);
            std::printf("  ChME3 at the cab door       eye (%.2f %.2f %.2f) prompt \"%s\"\n", eye_cam.position.x, eye_cam.position.y,
                        eye_cam.position.z, man.prompt().c_str());
            check(eye_cam.position.z > -3.36f && eye_cam.position.z < -3.0f && std::abs(feet_y() - 1.4f) < 0.02f,
                  "ChME3: the walkway ends at the shut cab door");
            // Look at the door (left, down) and open it.
            eye_cam.yaw = 3.14159265f * 0.5f;
            eye_cam.pitch = 0.9f;
            go({}, 0.05f);
            check(man.prompt() == std::string(viewer::tr("Enter: open the door, Ctrl+Enter: go through")), "ChME3: Enter opens the cab door");
            go({.action = true}, 1.3f);
            check(cab4->door_open(cab_d), "ChME3: the cab door is open");
            eye_cam.yaw = 0.0f;
            eye_cam.pitch = 0.0f;
            for (int i = 0; i < 600 && man.mode() == Engineer::Mode::on_car; ++i) frame({.forward = true});
            go({}, 0.5f);
            std::printf("  ChME3 in the cab            %s eye (%.2f %.2f %.2f)\n", man.mode() == Engineer::Mode::standing ? "standing" : "?",
                        eye_cam.position.x, eye_cam.position.y, eye_cam.position.z);
            check(man.mode() == Engineer::Mode::standing && std::abs(feet_y() - 1.4f) < 0.02f, "ChME3: through the door into the cab");
            go({.forward = true}, 1.0f);
            check(man.mode() == Engineer::Mode::standing && eye_cam.position.z < -3.65f, "ChME3: he walks in the cab");
            // Back out onto the walkway (+Z).
            eye_cam.yaw = 3.14159265f;
            for (int i = 0; i < 600 && man.mode() == Engineer::Mode::standing; ++i) frame({.forward = true});
            go({.forward = true}, 0.5f);
            std::printf("  ChME3 out of the cab        %s eye (%.2f %.2f %.2f)\n", man.mode() == Engineer::Mode::on_car ? "on car" : "?",
                        eye_cam.position.x, eye_cam.position.y, eye_cam.position.z);
            check(man.mode() == Engineer::Mode::on_car && eye_cam.position.z > -3.24f && std::abs(feet_y() - 1.4f) < 0.02f,
                  "ChME3: out of the cab door onto the walkway");
            // Along it to the front, across the front to the right side.
            for (int i = 0; i < 900 && eye_cam.position.z < 7.7f; ++i) frame({.forward = true});
            eye_cam.yaw = -3.14159265f * 0.5f; // +X
            for (int i = 0; i < 900 && eye_cam.position.x < 1.1f; ++i) frame({.forward = true});
            eye_cam.yaw = 0.0f; // -Z, to the roof ladder (z 7.3)
            for (int i = 0; i < 900 && eye_cam.position.z > 7.32f; ++i) frame({.forward = true});
            std::printf("  ChME3 at the roof ladder    eye (%.2f %.2f %.2f) prompt \"%s\"\n", eye_cam.position.x, eye_cam.position.y,
                        eye_cam.position.z, man.prompt().c_str());
            check(man.mode() == Engineer::Mode::on_car && eye_cam.position.x > 1.0f && std::abs(feet_y() - 1.4f) < 0.02f,
                  "ChME3: along the walkways round the front to the right side");
            eye_cam.pitch = -0.5f; // up at it
            go({}, 0.05f);
            check(man.prompt() == std::string(viewer::tr("Ctrl+Enter: climb up")), "ChME3: the roof ladder is in reach");
            go({.action = true, .action_ctrl = true}, 0.05f);
            for (int i = 0; i < 900 && man.passing(); ++i) frame({});
            std::printf("  ChME3 up the roof ladder    eye (%.2f %.2f %.2f)\n", eye_cam.position.x, eye_cam.position.y, eye_cam.position.z);
            check(man.mode() == Engineer::Mode::on_car && std::abs(feet_y() - 4.0f) < 0.02f, "ChME3: up the roof ladder (4.0)");
            go({.action = true, .action_ctrl = true}, 0.05f);
            for (int i = 0; i < 900 && man.passing(); ++i) frame({});
            go({.forward = true}, 0.1f);
            go({}, 1.0f);
            check(man.mode() == Engineer::Mode::on_car && std::abs(feet_y() - 1.4f) < 0.02f, "ChME3: and down it to the walkway");
            // To the front right steps and down them.
            eye_cam.yaw = 3.14159265f; // +Z
            eye_cam.pitch = 0.3f;
            for (int i = 0; i < 900 && eye_cam.position.z < 7.7f; ++i) frame({.forward = true});
            eye_cam.yaw = -3.14159265f * 0.5f; // +X, the steps, looking down at them
            eye_cam.pitch = 0.8f;
            go({}, 0.05f);
            check(man.prompt() == std::string(viewer::tr("Ctrl+Enter: climb down")), "ChME3: the steps down are in reach");
            go({.action = true, .action_ctrl = true}, 0.05f);
            for (int i = 0; i < 900 && man.passing(); ++i) frame({});
            go({}, 1.0f);
            std::printf("  ChME3 down the steps        eye (%.2f %.2f %.2f)\n", eye_cam.position.x, eye_cam.position.y, eye_cam.position.z);
            check(man.mode() == Engineer::Mode::outside && std::abs(feet_y()) < 0.02f && eye_cam.position.x > 1.5f,
                  "ChME3: down the steps to the ground");
        }
    } else {
        std::printf("no ChME3-1277 here: its outside not checked\n");
    }

    // The ED9M's salon (mdd_ED4M-Alias motor9.sd): 700 InteriorSeats. Looking
    // at a seat Enter sits him down at its Pos2 (the eye Height1 above, RTS
    // 0x4372B8), Num0 opens its window (HeadOutWindow WIN1), Num1 puts his
    // head out, Enter gets him up at its Pos1 in the aisle.
    if (const auto motor = formats::load_wagon(vfs, "mdd_ED9M-0032", "mdd_ED9M-0032_M", true); motor.ok()) {
        auto salon = viewer::CabView::create(vfs, motor, formats::wagon_path("mdd_ED9M-0032", "mdd_ED9M-0032_M", true));
        for (int i = 0; i < 600 && salon && !salon->collision(); ++i) {
            salon->poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::printf("  ED9M salon seats %zu, windows %zu\n", salon ? salon->description().seats.size() : 0,
                    salon ? salon->description().windows.size() : 0);
        check(salon && salon->description().seats.size() >= 30, "ED9M salon: its InteriorSeats read");
        if (salon && !salon->description().seats.empty()) {
            const auto& seat = salon->description().seats[0];
            const auto window = salon->find_window(seat.head_out_window);
            Engineer rider;
            rider.configure("engineer.Height = 1.81\r\nengineer.Height1 = 0.81\r\n");
            viewer::ViewerCamera view;
            view.fly = true;
            const viewer::Outdoors none{};
            auto ride = [&](EngineerControls controls, const float seconds) {
                for (float t = 0.0f; t < seconds; t += dt) {
                    rider.update(*salon, controls, salon->placement(), none, dt, view);
                    controls.action = controls.window = controls.head_out = false;
                }
            };
            rider.enter_cab(*salon, 0, {0.0f, 1.2f, 7.0f});
            ride({}, 0.05f);
            const glm::vec3 to = glm::normalize(glm::vec3{seat.position[0], seat.position[1], seat.position[2]} - view.position);
            view.yaw = std::atan2(-to.x, -to.z);
            view.pitch = std::asin(-to.y);
            ride({}, 0.05f);
            std::printf("  ED9M at seat 0               mode %s eye (%.2f %.2f %.2f) yaw %.2f pitch %.2f prompt \"%s\"\n",
                        mode_name(rider.mode()), view.position.x, view.position.y, view.position.z, view.yaw, view.pitch,
                        rider.prompt().c_str());
            check(rider.prompt() == std::string(viewer::tr("Enter: sit down")), "ED9M salon: looking at a seat, Enter sits down");
            ride({.action = true}, 1.0f);
            std::printf("  ED9M seated                 eye (%.2f %.2f %.2f)\n", view.position.x, view.position.y, view.position.z);
            check(rider.mode() == Engineer::Mode::seated && std::abs(view.position.y - (seat.seated[1] + 0.81f)) < 0.01f
                      && std::abs(view.position.x - seat.seated[0]) < 0.01f,
                  "ED9M salon: seated at Pos2, the eye Height1 above it");
            check(window && !salon->window_open(*window), "ED9M salon: the seat's window is shut");
            ride({.window = true}, 1.3f);
            check(window && salon->window_open(*window), "ED9M salon: Num0 opens the seat's window");
            ride({.head_out = true}, 1.0f);
            check(rider.heading_out() && window
                      && glm::distance(view.position, glm::vec3{salon->description().windows[*window].head_out[0],
                                                                salon->description().windows[*window].head_out[1],
                                                                salon->description().windows[*window].head_out[2]}) < 0.01f,
                  "ED9M salon: Num1 puts the head out of it");
            ride({.head_out = true}, 1.0f);
            ride({.action = true}, 1.0f);
            std::printf("  ED9M up                     eye (%.2f %.2f %.2f)\n", view.position.x, view.position.y, view.position.z);
            check(rider.mode() == Engineer::Mode::standing && std::abs(view.position.x - seat.aisle[0]) < 0.01f
                      && std::abs(view.position.z - seat.aisle[2]) < 0.01f,
                  "ED9M salon: Enter gets him up in the aisle (Pos1)");
        }
    } else {
        std::printf("no ED9M-0032 motor car here: its seats not checked\n");
    }

    // The lighting mast Osvstozar4 (Kuban): its ladder is 30 boxes of 1 m
    // one on another (ESD_Complex), a platform 4 x 2 m at the top (30.3).
    // RTS climbs a box whose top is 0.4..0.9 Height above his feet
    // (0x4322F0): walking at the mast he climbs it box by box to the top.
    if (const auto mast = formats::load_shape_description(vfs, *core::VirtualPath::parse("ROUTES/Kuban_RTS/SHAPES/Osvstozar4.sd")); mast.ok()) {
        check(mast.value->boxes.size() == 31, "Osvstozar4: 31 boxes");
        const std::vector<viewer::PlacedBoxes> objects{{.boxes = &mast.value->boxes}};
        const viewer::Outdoors ground_only{
            .floor = [](const glm::vec3&) -> std::optional<float> { return 0.0f; },
            .boxes = [&](const glm::vec3& from, const glm::vec3& to, const float h) { return viewer::step_among_boxes(objects, from, to, h); },
        };
        Engineer climber;
        climber.configure("engineer.Height = 1.81\r\nengineer.Height1 = 0.81\r\n");
        climber.stand_outside({0.0f, 0.0f, -2.0f});
        viewer::ViewerCamera view;
        view.fly = true;
        view.yaw = 3.14159265f; // +Z, at the mast
        float highest = 0.0f;
        for (int i = 0; i < 60 * 120 && climber.feet().y < 30.25f; ++i) {
            climber.update_on_foot({.forward = true}, ground_only, dt, view);
            highest = (std::max)(highest, climber.feet().y);
        }
        std::printf("  Osvstozar4 climbed to       feet %.2f at (%.2f %.2f)\n", climber.feet().y, climber.feet().x, climber.feet().z);
        check(std::abs(climber.feet().y - 30.3f) < 0.01f, "Osvstozar4: up the mast's boxes onto the platform (30.3)");
        // Along the platform, off its end: down to the ground, falling.
        view.yaw = -3.14159265f * 0.5f; // +X
        for (int i = 0; i < 60 && climber.feet().x < 1.5f; ++i) climber.update_on_foot({.forward = true}, ground_only, dt, view);
        check(std::abs(climber.feet().y - 30.3f) < 0.01f, "Osvstozar4: he walks on the platform");
        int frames = 0;
        for (; frames < 600 && (climber.feet().y > 0.0f || climber.feet().x < 2.4f); ++frames)
            climber.update_on_foot({.forward = climber.feet().x < 2.4f}, ground_only, dt, view);
        std::printf("  Osvstozar4 off the platform feet %.2f after %.1f s\n", climber.feet().y, frames * dt);
        check(climber.feet().y == 0.0f && frames * dt > 2.0f, "Osvstozar4: off the platform he falls to the ground (9.8 m/s^2)");
    } else {
        std::printf("no Osvstozar4 here: the mast's boxes not checked\n");
    }

    std::printf("%s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
