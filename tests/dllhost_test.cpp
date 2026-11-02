// openrts-dllhost32 end to end with the fake locomotive DLL
// (dllhost/test/fake_loco.cpp): setup, entering a cab with a multiple-unit
// group, steps, CanSwitch / Switched, sounds and DLL messages.
// Usage: openrts_dllhost_test <openrts-dllhost32.exe> <openrts_fake_loco.dll>

#include <cmath>
#include <cstdio>
#include <string>

#include <openrts/trainplugin/legacy_host.hpp>

using namespace openrts;
using namespace openrts::dllhost;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAILED: %s\n", what);
        ++failures;
    }
}

CarSpec loco(const std::u16string& module) {
    CarSpec c;
    c.kind = car_electric;
    c.mass = 92000.0f;
    c.series = u"fake";
    c.module = module;
    c.stack_size = 4;
    c.num_engines = 4;
    c.num_axles = 4;
    c.lights = 2;
    c.cab.present = true;
    for (std::uint32_t id = 0; id < 3; ++id) {
        SwitchSpec s;
        s.id = id;
        s.num_states = 6;
        s.anim_time = 0.1f;
        s.name = u"sw" + std::u16string(1, static_cast<char16_t>(u'0' + id));
        c.cab.switches.push_back(s);
    }
    return c;
}

CarInput input(std::uint32_t flags) {
    CarInput in;
    in.flags = flags;
    in.mass = 92000.0f;
    in.train_pipe = 5.0f;
    in.main_reservoir = 8.0f;
    in.line_voltage = 3000.0f;
    return in;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: openrts_dllhost_test <openrts-dllhost32.exe> <fake_loco.dll>\n");
        return 2;
    }
    std::string error;
    auto host = trainplugin::LegacyHost::start(&error, argv[1]);
    if (!host) {
        std::printf("host: %s\n", error.c_str());
        return 1;
    }
    const auto module = std::filesystem::path(argv[2]).u16string();

    // Two engines joined by control cables, then a wagon.
    SetupRequest setup;
    setup.cars = {loco(module), loco(module), CarSpec{}};
    std::vector<ModuleReport> modules;
    check(host->setup(setup, modules), "setup");
    check(modules.size() == 1 && modules[0].status == 0, "the module loads");
    check(!modules.empty() && modules[0].version[1] == 104, "DLLVersion(1) = 104");

    EnterRequest enter;
    enter.car = 0;
    enter.cars = {input(0x20000), input(0x10000), input(0)};
    StepReply reply;
    check(host->enter(enter, reply), "enter");
    check(reply.cars.size() == 3, "three cars in the reply");
    if (reply.cars.size() == 3) {
        check(reply.cars[0].has_engine && reply.cars[1].has_engine, "both engines initialised");
        check((reply.cars[0].loco_flags & 0x21) == 0x21, "car 0 is master with its script loaded");
        check((reply.cars[1].loco_flags & 0x21) == 0x20, "car 1 is a slave with its script loaded");
        check(reply.cars[0].engine.panto == 3 && reply.cars[1].engine.panto == 3, "Init raised the pantographs");
        check(reply.cars[0].switches.size() == 3 && reply.cars[0].switches[0].state == 1, "Init set switch 0 with SetBoth");
    }

    StepRequest step;
    step.dt = 0.05f;
    step.cars = enter.cars;
    step.cars[0].velocity = step.cars[1].velocity = 2.0f;
    check(host->step(step, reply), "step");
    bool message = false, sound = false;
    for (const auto& m : reply.cars[0].messages)
        if (m.action == 14 && trainplugin::utf16_to_utf8(m.text) == "fake: init done") message = true;
    for (const auto t : reply.cars[0].sound_cab)
        if (t == 7) sound = true;
    check(message, "the ShowMessage text came through GlobalMsg");
    check(sound, "the cab sound trigger came through the SMS queue");
    check(std::abs(reply.cars[1].engine.equalising - 4.25f) < 1e-6f, "the master wrote into the slave's Engine");
    check(std::abs(reply.cars[0].engine.brake_force - 2.0f) < 1e-6f, "Run saw the velocity");
    check(std::abs(reply.cars[0].engine.power - 1.0f) < 1e-6f, "RTSGetInteger(TRAINDIR) answered 1");

    bool allowed = true;
    check(host->can_switch({0, 1, 2}, allowed) && !allowed, "CanSwitch refusal is honoured");
    check(host->can_switch({0, 0, 3}, allowed) && allowed, "CanSwitch allows switch 0 to 3");
    check(host->can_switch({0, 2, 1}, allowed) && allowed, "CanSwitch allows the reverser");
    for (int i = 0; i < 20; ++i) host->step(step, reply);
    if (reply.cars.size() == 3) {
        check(reply.cars[0].switches[0].state == 3, "switch 0 reached state 3");
        check(reply.cars[0].switches[1].set_state == 0, "the refused switch did not move");
        check(reply.cars[0].engine.throttle == 3, "Switched(0) set the throttle");
        check(std::abs(reply.cars[0].engine.force - 3000.0f) < 1e-3f, "Run gave 3 kN");
        check(reply.cars[1].engine.throttle == 3, "the slave follows the master's throttle");
        check(std::abs(reply.cars[1].engine.force - 3000.0f) < 1e-3f, "the slave pulls too");
    }
    // Dragging a switch (RTS 0x437E64 / 0x527190): the frame follows in
    // states, crossing a whole state fires Switched, the DLL can refuse,
    // and let go a controller takes the nearest state.
    bool moved = false;
    dllhost::SwitchState dragged;
    check(host->drag_switch({0, 0, 0.5f, 0}, moved, dragged) && moved, "drag switch 0 half a state");
    check(std::abs(dragged.current_frame - 3.5f) < 1e-4f, "the reply has the dragged frame at once");
    for (int i = 0; i < 3; ++i) host->step(step, reply);
    if (reply.cars.size() == 3) {
        check(std::abs(reply.cars[0].switches[0].current_frame - 3.5f) < 1e-4f, "a dragged switch keeps its frame");
        check(reply.cars[0].switches[0].state == 3, "no whole state crossed yet");
    }
    check(host->drag_switch({0, 0, 1.0f, 0}, moved, dragged) && moved, "drag switch 0 one state further");
    host->step(step, reply);
    if (reply.cars.size() == 3) {
        check(reply.cars[0].switches[0].state == 4, "crossing a state sets State");
        check(reply.cars[0].engine.throttle == 4, "Switched came with the crossing");
    }
    check(host->drag_switch({0, 1, 1.2f, 0}, moved, dragged) && !moved, "a drag the DLL refuses does not move");
    check(host->drag_switch({0, 0, 0.0f, 1}, moved, dragged) && moved, "let switch 0 go");
    for (int i = 0; i < 20; ++i) host->step(step, reply);
    if (reply.cars.size() == 3) {
        check(reply.cars[0].switches[0].state == 5 && reply.cars[0].switches[0].current_frame == 5.0f,
              "let go at 4.5, a controller goes to the nearest state");
        check(reply.cars[0].engine.throttle == 5, "and the DLL hears it");
    }

    // A flipped car sees its velocity in its own direction.
    step.cars[1].flags |= 1;
    check(host->step(step, reply), "step with a flipped slave");
    check(std::abs(reply.cars[1].engine.brake_force + 2.0f) < 1e-6f, "the flipped car saw -velocity");
    check(std::abs(reply.cars[1].engine.power + 1.0f) < 1e-6f, "the flipped car's TRAINDIR is -1");

    const auto messages = host->take_messages();
    if (!messages.empty()) std::printf("host said:\n%s", messages.c_str());
    host.reset(); // shutdown
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("dllhost: all checks passed\n");
    return 0;
}
