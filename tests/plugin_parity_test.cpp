// Parity: one locomotive logic as a legacy RTS DLL (dllhost/test/fake_loco.cpp,
// run by openrts-dllhost32) and as a native openRTS plugin
// (sdk/samples/fake_native), driven through the same script. Every step the
// force, the controls' states, the sounds and the messages must agree: the
// native API carries what a legacy DLL does through raw ts.h objects.
// Usage: openrts_plugin_parity_test <openrts-dllhost32.exe> <fake_loco.dll> <fake_native.dll>

#include <cmath>
#include <cstdio>
#include <set>
#include <string>

#include <openrts/trainplugin/legacy_host.hpp>
#include <openrts/trainplugin/native_plugin.hpp>

using namespace openrts;
using namespace openrts::dllhost;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAILED: %s\n", what.c_str());
        ++failures;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::printf("usage: openrts_plugin_parity_test <openrts-dllhost32.exe> <fake_loco.dll> <fake_native.dll>\n");
        return 2;
    }
    // One cab for both: three switches of six states, 0.1 s to move.
    formats::CabDescription cab;
    CarSpec spec;
    spec.kind = car_electric;
    spec.mass = 92000.0f;
    spec.series = u"fake";
    spec.module = std::filesystem::path(argv[2]).u16string();
    spec.stack_size = 4;
    spec.num_engines = 4;
    spec.num_axles = 4;
    spec.cab.present = true;
    for (std::uint32_t id = 0; id < 3; ++id) {
        formats::CabSwitch s;
        s.id = id;
        s.name = "sw" + std::to_string(id);
        s.num_states = 6;
        s.anim_time = 0.1f;
        cab.switches.push_back(s);
        SwitchSpec ss;
        ss.id = id;
        ss.num_states = 6;
        ss.anim_time = 0.1f;
        ss.name = trainplugin::utf8_to_utf16(s.name);
        spec.cab.switches.push_back(ss);
    }

    // Legacy: the fake DLL in the host.
    std::string error;
    auto host = trainplugin::LegacyHost::start(&error, argv[1]);
    if (!host) {
        std::printf("host: %s\n", error.c_str());
        return 1;
    }
    SetupRequest setup;
    setup.cars = {spec};
    std::vector<ModuleReport> modules;
    check(host->setup(setup, modules), "legacy setup");
    CarInput input;
    input.mass = 92000.0f;
    input.velocity = 2.0f;
    input.train_pipe = 5.0f;
    input.main_reservoir = 8.0f;
    input.line_voltage = 3000.0f;
    EnterRequest enter;
    enter.cars = {input};
    StepReply legacy;
    check(host->enter(enter, legacy), "legacy enter");

    // Native: the plugin in this process.
    formats::WagonDefinition engine;
    engine.name = "fake";
    engine.mass = 92000.0f;
    engine.engine = formats::EngineDefinition{};
    engine.engine->kind = formats::EngineKind::electric;
    auto native = trainplugin::NativeLocomotive::load(argv[3], engine, &cab, &error);
    if (!native) {
        std::printf("native: %s\n", error.c_str());
        return 1;
    }

    check(legacy.cars[0].switches[0].state == 1, "legacy: Init set switch 0 to 1");
    check(native->cab().controls()[0].state == 1.0f, "native: create set switch 0 to 1");

    StepRequest step;
    step.dt = 0.05f;
    step.cars = {input};
    const simulation::EngineInputs inputs{.speed = 2.0f, .train_pipe = 5.0f, .main_reservoir = 8.0f, .definition = &engine};
    std::multiset<std::uint32_t> legacy_sounds, native_sounds;
    std::set<std::string> legacy_messages, native_messages;
    auto run = [&](int steps) {
        for (int i = 0; i < steps; ++i) {
            check(host->step(step, legacy), "legacy step");
            simulation::EngineDemands demands;
            native->step(step.dt, inputs, demands);
            const auto& l = legacy.cars[0];
            for (const auto t : l.sound_cab) legacy_sounds.insert(t);
            for (const auto t : native->take_sounds(true)) native_sounds.insert(t);
            for (const auto& m : l.messages) legacy_messages.insert(trainplugin::utf16_to_utf8(m.text));
            for (const auto& m : native->take_messages()) native_messages.insert(m);
            if (std::abs(l.engine.force - demands.force) > 1e-3f)
                check(false, "force differs: legacy " + std::to_string(l.engine.force) + ", native " + std::to_string(demands.force));
            for (std::size_t s = 0; s < 3; ++s) {
                if (static_cast<float>(l.switches[s].state) != native->cab().controls()[s].state)
                    check(false, "switch " + std::to_string(s) + " differs");
            }
        }
    };
    run(2);
    bool allowed = true;
    check(host->can_switch({0, 1, 2}, allowed) && !allowed, "legacy refuses switch 1 to 2");
    check(!native->operate(1, 2.0f), "native refuses switch 1 to 2");
    check(host->can_switch({0, 0, 3}, allowed) && allowed, "legacy allows switch 0 to 3");
    check(native->operate(0, 3.0f), "native allows switch 0 to 3");
    check(host->can_switch({0, 2, 1}, allowed) && allowed, "legacy allows the reverser");
    check(native->operate(2, 1.0f), "native allows the reverser");
    run(20);
    check(std::abs(legacy.cars[0].engine.force - 3000.0f) < 1e-3f, "legacy pulls 3 kN");
    check(legacy_sounds == native_sounds, "the same sound triggers");
    check(legacy_messages == native_messages, "the same messages");
    check(legacy_messages.count("fake: init done") == 1, "the greeting came");
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("parity: the legacy DLL and the native plugin agree over %d steps\n", 22);
    return 0;
}
