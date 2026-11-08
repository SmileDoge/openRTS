// openrts-dllhost32 end to end with the fake locomotive DLL
// (dllhost/test/fake_loco.cpp): setup, entering a cab with a multiple-unit
// group, steps, CanSwitch / Switched, sounds and DLL messages.
// Usage: openrts_dllhost_test <openrts-dllhost32.exe> <openrts_fake_loco.dll>

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <string>

#include <openrts/trainplugin/legacy_consist.hpp>
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

// `base`: the value of its parameter "Alpha" (and "ALPHA", a later one of
// the same name in other case, base + 2: the first match is "Alpha").
CarSpec loco(const std::u16string& module, float base = 10.0f) {
    CarSpec c;
    c.parameters = {{u"Alpha", base, {}}, {u"Beta", base + 1.0f, {}}, {u"ALPHA", base + 2.0f, {}}};
    c.kind = car_electric;
    c.mass = 92000.0f;
    c.series = u"fake";
    c.module = module;
    c.stack_size = 4;
    c.num_engines = 4;
    c.num_axles = 4;
    c.lights = 2;
    c.cab.present = true;
    c.cab.signal_scripted = true; // ALSN is called
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

// AssignKey as RTS keeps it in ActSwitch::Key (0x45AE90).
void key_bytes_checks() {
    const auto bytes = [](std::vector<std::string> keys) {
        std::uint8_t out[4];
        trainplugin::switch_key_bytes(keys, out);
        return std::array<int, 4>{out[0], out[1], out[2], out[3]};
    };
    check(bytes({"P"}) == std::array<int, 4>{'P', 0, 0, 0}, "AssignKey ( P )");
    check(bytes({"D", "A"}) == std::array<int, 4>{'D', 0, 'A', 0}, "AssignKey ( D A ): two keys");
    check(bytes({"Shift+8"}) == std::array<int, 4>{'8', 1, 0, 0}, "Shift+8");
    check(bytes({"ctrl+alt+z"}) == std::array<int, 4>{'Z', 6, 0, 0}, "Ctrl+Alt, upper case");
    check(bytes({"Space"}) == std::array<int, 4>{0x20, 0, 0, 0}, "Space");
}

int main(int argc, char** argv) {
    key_bytes_checks();
    if (argc < 3) {
        std::printf("usage: openrts_dllhost_test <openrts-dllhost32.exe> <fake_loco.dll>\n");
        return 2;
    }
    // Every by-name answer the host gives from its cache is scanned again;
    // a difference is logged ("the scan"), checked at the end.
    _putenv_s("OPENRTS_DLLHOST_NAME_CHECK", "1");
    std::string error;
    auto host = trainplugin::LegacyHost::start(&error, argv[1]);
    if (!host) {
        std::printf("host: %s\n", error.c_str());
        return 1;
    }
    const auto module = std::filesystem::path(argv[2]).u16string();

    // Two engines joined by control cables, then a wagon.
    SetupRequest setup;
    setup.cars = {loco(module, 10.0f), loco(module, 20.0f), CarSpec{}};
    for (std::uint32_t i = 0; i < 3; ++i) setup.cars[i].key = i + 1; // the game's keys of the cars
    // The first engine's cab script: a NamedTriggersTable and one trigger
    // pair (vl80k_aux.sms has these two names).
    SoundTableSpec table;
    table.names = {u"aux on", u"aux off"};
    table.codes = {1, 2};
    table.pair_on = {{1}};
    setup.sound_tables = {table};
    setup.cars[0].sound_cab = 0;
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
    // Parameters by name (the host keeps the answers): each car its own
    // object for the same names; case aside, the first match; none missing.
    for (int again = 0; again < 3; ++again) {
        if (again) check(host->step(step, reply), "step again for the names");
        check(reply.cars.size() == 3 && reply.cars[0].engine.train_pipe_rate == 10.75f && reply.cars[1].engine.train_pipe_rate == 20.75f,
              again ? "by name again (kept): the same answers" : "by name: each car's own \"Alpha\", any case, \"missing\" none");
    }

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

    // ALSN (RTS 0x508520): the three arrays and the next speed post.
    {
        StepRequest s = step;
        SignalInput ahead;
        ahead.distance = 500.0f;
        ahead.aspect[0] = 6;
        ahead.flags = 1;
        s.alsn_ahead = {ahead, ahead};
        s.alsn_behind = {ahead};
        s.next_limit = 16.0f;
        s.next_limit_distance = 300.0f;
        check(host->step(s, reply), "step with signals");
        if (reply.cars.size() == 3) {
            check(reply.cars[0].engine.charging_rate == 12.0f, "ALSN got two ahead, one behind, none passing");
            check(reply.cars[0].engine.handbrake_percent == 6.0f, "the aspect ahead");
            check(reply.cars[0].engine.dynamic_brake_percent == 16300.0f, "the next speed post in the integer slots");
        }
    }

    // Named SMS triggers and trigger pairs (RTS 0x4394F4, 0x439548): before
    // any pair code there are no PairedFlags (-2); then the pair's state.
    check(host->step(step, reply), "step for the sound tables");
    if (reply.cars.size() == 3) {
        check(reply.cars[0].engine.main_reservoir_rate == 108.0f, "\"Aux On\" is 1 (case aside), its pair not known yet (-2)");
        check(reply.cars[1].engine.main_reservoir_rate == 7.0f, "no script: 0 and -3");
    }
    {
        StepRequest s = step;
        SoundPairState pairs{.car = 0, .object = sound_cabin};
        pairs.on[0] = pairs.known[0] = 1;
        s.sound_pairs = {pairs};
        check(host->step(s, reply), "step with the pair on");
        check(reply.cars.size() == 3 && reply.cars[0].engine.main_reservoir_rate == 111.0f, "the pair is on");
        pairs.on[0] = 0;
        s.sound_pairs = {pairs};
        check(host->step(s, reply), "step with the pair off");
        check(reply.cars.size() == 3 && reply.cars[0].engine.main_reservoir_rate == 110.0f, "the pair is off");
    }

    // Track items (RTS 0x4805D8): ahead within the asked distance, under
    // the train always, signals never; the platform's station name.
    {
        StepRequest s = step;
        TrackItemSpec post{.id = 10, .type = 2};
        TrackItemSpec platform{.id = 11, .type = 3};
        platform.name = u"P1";
        platform.station = u"Станция";
        TrackItemSpec siding{.id = 12, .type = 4};
        TrackItemSpec signal{.id = 13, .type = 1};
        TrackItemSpec crossover{.id = 14, .type = 6};
        s.items = {post, platform, siding, signal, crossover};
        s.track_items = {{.distance = 300.0f, .z = 330.0f, .pos = item_ahead, .item = 10},
                         {.distance = 2000.0f, .z = 2030.0f, .pos = item_ahead, .item = 11},
                         {.distance = -10.0f, .z = 20.0f, .pos = item_under, .item = 12},
                         {.distance = 100.0f, .z = 130.0f, .pos = item_ahead, .item = 13},
                         {.distance = 50.0f, .z = -50.0f, .pos = item_behind, .item = 14}};
        s.items_range = 5000.0f;
        check(host->step(s, reply), "step with track items");
        if (reply.cars.size() == 3) {
            check(reply.cars[0].engine.ep_value == 224.0f, "within 1 km ahead and under: the speed post and the siding");
            check(reply.cars[0].engine.power_consuming == 7.0f, "the platform's station name (5 km)");
        }
        check(reply.items_wanted == 5000.0f, "the farthest distance asked comes back");
    }

    // ALSN's RTSSignalData (RTS 0x4903DC): the route database's entry.
    {
        StepRequest s = step;
        SignalUnitSpec unit{.id = 7};
        unit.tile[0] = -5;
        unit.tile[1] = 3;
        unit.name = u"M14";
        s.signal_units = {unit};
        SignalInput ahead;
        ahead.distance = 500.0f;
        ahead.unit = 7;
        s.alsn_ahead = {ahead};
        check(host->step(s, reply), "step with a named signal");
        check(reply.cars.size() == 3 && reply.cars[0].engine.independent_brake == 3003.0f, "ALSN sees the signal's name and tile");
        s.signal_units.clear();
        s.alsn_ahead[0].unit = -1;
        check(host->step(s, reply), "step with an unnamed signal");
        check(reply.cars.size() == 3 && reply.cars[0].engine.independent_brake == -1.0f, "no entry: no RTSSignalData");
    }

    // The control cable parts (the car menu): the slave gets LostMaster.
    {
        StepRequest s = step;
        s.cars[0].flags &= ~0x20000u;
        s.cars[1].flags &= ~0x10000u;
        check(host->step(s, reply), "step with the cable parted");
        check(reply.cars.size() == 3 && reply.cars[1].engine.auxiliary_rate == 7.0f, "the slave got LostMaster");
    }

    // A flipped car sees its velocity in its own direction.
    step.cars[1].flags |= 1;
    check(host->step(step, reply), "step with a flipped slave");
    check(std::abs(reply.cars[1].engine.brake_force + 2.0f) < 1e-6f, "the flipped car saw -velocity");
    check(std::abs(reply.cars[1].engine.power + 1.0f) < 1e-6f, "the flipped car's TRAINDIR is -1");

    // Coupling and cutting (RTS 0x469ED8 / 0x469858 -> 0x4F995C): the DLLs
    // keep their state, an engine coupled on joins the group as a slave
    // (bound and initialised), one cut off goes on running in its own train
    // (RTS: the cut train keeps flag 4, 0x4EEC50) and comes back as it is.
    {
        check(host->step(step, reply) && reply.cars.size() == 3, "step before the coupling");
        const auto switch0 = reply.cars.empty() ? 0u : reply.cars[0].switches[0].state;
        const auto throttle = reply.cars.empty() ? 0u : reply.cars[0].engine.throttle;
        std::printf("before the coupling: switch 0 at %u, throttle %u\n", switch0, throttle);
        auto added = loco(module, 30.0f);
        added.key = 4;
        ReconsistRequest r;
        r.trains = {{1, {{reconsist_new, added}, {0, {}}, {1, {}}, {2, {}}}}};
        r.inputs = {input(0x20000), input(0x30000), input(0x10000 | 1), input(0)};
        check(host->reconsist(r, reply), "reconsist: an engine coupled on ahead");
        check(reply.cars.size() == 4, "four cars after the coupling");
        if (reply.cars.size() == 4) {
            check(reply.cars[1].switches.size() == 3 && reply.cars[1].switches[0].state == switch0 && reply.cars[1].engine.throttle == throttle
                      && switch0 != 1,
                  "the player's engine kept its state (no second Init: it sets switch 0 to 1)");
            check((reply.cars[1].loco_flags & 0x21) == 0x21, "the player's engine is still master");
            check(reply.cars[0].has_engine && (reply.cars[0].loco_flags & 0x21) == 0x20, "the new engine is bound as a slave");
            check(reply.cars[0].engine.panto == 0, "its Init had the standing train's state (1: no pantographs)");
            check(reply.cars[2].engine.auxiliary_rate == 7.0f, "the old slave kept its state");
        }
        StepRequest s = step;
        s.cars = r.inputs;
        check(host->step(s, reply), "step after the coupling");
        check(reply.cars.size() == 4 && reply.cars[0].engine.throttle == throttle, "the master drives the new slave");
        check(reply.cars.size() == 4 && reply.cars[0].engine.train_pipe_rate == 30.75f && reply.cars[1].engine.train_pipe_rate == 10.75f
                  && reply.cars[2].engine.train_pipe_rate == 20.75f,
              "by name after the coupling: the new engine's own, the others' as they were");

        // The old slave (key 2) is cut off: LostMaster, and it goes on in a
        // train of its own (the player's train first, then the cut part).
        ReconsistRequest cut;
        cut.trains = {{1, {{0, {}}, {1, {}}, {3, {}}}}, {3, {{2, {}}}}};
        cut.inputs = {input(0x20000), input(0x10000), input(0), input(0)};
        cut.inputs[3].velocity = 3.0f;
        check(host->reconsist(cut, reply), "reconsist: a slave cut off into its own train");
        check(reply.cars.size() == 4 && reply.cars[1].engine.throttle == throttle, "the player's engine unchanged");
        if (reply.cars.size() == 4) {
            check(reply.cars[3].engine.auxiliary_rate == 7.0f, "the cut engine got LostMaster");
            check((reply.cars[3].loco_flags & 4) == 0, "it is no longer in the player's join");
        }
        s.cars = cut.inputs;
        check(host->step(s, reply), "step after the cut");
        check(reply.cars.size() == 4 && std::abs(reply.cars[3].engine.brake_force - 3.0f) < 1e-6f,
              "the cut engine's DLL still runs (Run saw its own train's velocity)");
        check(reply.cars.size() == 4 && std::abs(reply.cars[0].engine.brake_force - 2.0f) > 0.5f,
              "the player's train saw its own inputs");

        // And coupled back behind the player's engine: from the cut train.
        ReconsistRequest back;
        back.trains = {{1, {{0, {}}, {1, {}}, {3, {}}, {2, {}}}}};
        back.inputs = {input(0x20000), input(0x30000), input(0x10000), input(0)};
        check(host->reconsist(back, reply), "reconsist: the cut engine coupled again");
        if (reply.cars.size() == 4) {
            check(reply.cars[2].has_engine && (reply.cars[2].loco_flags & 0x21) == 0x20, "it is a slave again");
            check(reply.cars[2].engine.auxiliary_rate == 7.0f && std::abs(reply.cars[2].engine.brake_force - 3.0f) < 1e-6f,
                  "with the state its own train left it in");
        } else {
            check(false, "four cars after coupling back");
        }

        // A car in no train is kept aside and comes back as it was.
        ReconsistRequest gone;
        gone.trains = {{1, {{0, {}}, {1, {}}, {3, {}}}}};
        gone.inputs = {input(0x20000), input(0x10000), input(0)};
        check(host->reconsist(gone, reply) && reply.cars.size() == 3, "reconsist: the engine leaves every train");
        ReconsistRequest again;
        CarSpec parked;
        parked.key = 2;
        again.trains = {{1, {{0, {}}, {1, {}}, {reconsist_parked, parked}, {2, {}}}}};
        again.inputs = back.inputs;
        check(host->reconsist(again, reply) && reply.cars.size() == 4 && reply.cars[2].engine.auxiliary_rate == 7.0f,
              "and comes back with its state");
        step.cars = again.inputs;
    }

    const auto messages = host->take_messages();
    if (!messages.empty()) std::printf("host said:\n%s", messages.c_str());
    check(messages.find("the scan") == std::string::npos, "every by-name answer kept was the scan's");
    host.reset(); // shutdown
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("dllhost: all checks passed\n");
    return 0;
}
