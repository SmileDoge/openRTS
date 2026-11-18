// openRTS's cab brake devices (simulation/cab_brakes.hpp, the
// replacement of Brakes.dll's, docs/brakes-dll-replacement.md): each does
// what docs/reverse/brakes-dll.md §10 says Brakes.dll's does, with its
// numbers, and the openRTS numbers differ where the books say.
#include <cmath>
#include <iostream>
#include <string>

#include <openrts/simulation/cab_brakes.hpp>

namespace {

using namespace openrts::simulation;

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

bool near(const float a, const float b, const float within = 0.01f) { return std::abs(a - b) <= within; }

constexpr float dt = 1.0f / 30.0f;

struct Rig {
    CabBrakeState state;
    CabBrakeControls controls;
    CabBrakeAir air{.pipe = 5.0f, .main_reservoir = 8.5f};
    CabBrakeNumbers numbers = CabBrakeNumbers::brakes_dll();
    CabBrakeOutput out;
    explicit Rig(const float ur = 5.0f) { state.valve.equalising = ur; }
    // `seconds` of steps; the pipe follows the УР as the relay holds it.
    void run(const float seconds, const bool pipe_follows = false) {
        for (float t = 0.0f; t < seconds - 1e-4f; t += dt) {
            out = step_cab_brakes(state, controls, air, numbers, dt);
            if (pipe_follows && out.equalising >= 0.0f) air.pipe = out.equalising;
        }
    }
};

void valve() {
    // II charges the УР to the charging pressure: Brakes.dll's 0.1 below it.
    for (const bool books : {false, true}) {
        Rig rig(0.0f);
        if (books) rig.numbers = CabBrakeNumbers::openrts();
        rig.controls.charging = 4.6f;
        rig.run(60.0f);
        const float expected = books ? 4.6f : 4.5f;
        check(near(rig.out.equalising, expected, 0.02f), std::string("II charges the УР to ") + std::to_string(expected) +
                                                              " (" + (books ? "openRTS" : "Brakes.dll") + "): " +
                                                              std::to_string(rig.out.equalising));
    }
    {
        // At most 0.1 atm/s on the way.
        Rig rig(0.0f);
        rig.run(5.0f);
        check(rig.out.equalising <= 0.51f && rig.out.equalising > 0.45f, "II: the УР up 0.1 atm/s: " + std::to_string(rig.out.equalising));
    }
    {
        // The stabiliser takes an overcharge off at 0.002 atm/s.
        Rig rig(6.0f);
        rig.run(100.0f);
        check(near(rig.out.equalising, 5.8f, 0.01f), "II: 6.0 to 5.8 in 100 s: " + std::to_string(rig.out.equalising));
    }
    {
        // I: up to 0.18 atm/s, to the main reservoir at most.
        Rig rig(5.0f);
        rig.controls.valve = ValvePosition::charge;
        rig.run(5.0f);
        check(near(rig.out.equalising, 5.9f, 0.02f), "I: the УР up 0.18 atm/s: " + std::to_string(rig.out.equalising));
        rig.run(60.0f);
        check(near(rig.out.equalising, 8.5f, 0.02f), "I: the УР at most the main reservoir: " + std::to_string(rig.out.equalising));
    }
    {
        // V lowers the УР 0.22 atm/s while the handle rests there, VЭ 0.03.
        Rig rig(5.0f);
        rig.controls.valve = ValvePosition::service;
        rig.run(2.0f);
        check(near(rig.out.equalising, 4.56f), "V: the УР down 0.22 atm/s: " + std::to_string(rig.out.equalising));
        rig.controls.valve_resting = false;
        rig.run(2.0f);
        check(near(rig.out.equalising, 4.56f), "V passed through: the УР stays");
        rig.controls.valve_resting = true;
        rig.controls.valve = ValvePosition::ep_service;
        rig.run(10.0f);
        check(near(rig.out.equalising, 4.26f), "VЭ: the УР down 0.03 atm/s: " + std::to_string(rig.out.equalising));
    }
    {
        // III: the УР after the pipe, down only, at most 0.1 atm/s; no feed.
        Rig rig(5.0f);
        rig.controls.valve = ValvePosition::lap;
        rig.air.pipe = 4.0f;
        rig.run(3.0f);
        check(near(rig.out.equalising, 4.7f), "III: the УР down 0.1 atm/s to the pipe: " + std::to_string(rig.out.equalising));
        check(!rig.out.feed, "III: the relay does not feed the pipe");
        rig.run(20.0f);
        check(near(rig.out.equalising, 4.0f), "III: the УР stops at the pipe");
        rig.air.pipe = 4.5f;
        rig.run(2.0f);
        check(near(rig.out.equalising, 4.0f), "III: the УР does not rise with the pipe");
        rig.controls.valve = ValvePosition::lap_feed;
        rig.run(1.0f);
        check(rig.out.feed && near(rig.out.equalising, 4.0f), "IV: the УР held, the pipe fed");
    }
    {
        // VI: the pipe opened at the engine, the УР out 0.4 atm/s.
        Rig rig(5.0f);
        rig.controls.valve = ValvePosition::emergency;
        rig.run(2.0f);
        check(rig.out.emergency, "VI opens the pipe");
        check(near(rig.out.equalising, 4.2f), "VI: the УР down 0.4 atm/s: " + std::to_string(rig.out.equalising));
    }
    {
        // Cut off (the 367, double traction, the separating cock): the
        // pipe is not held, the УР kept; an emergency cock opens it all the same.
        Rig rig(5.0f);
        rig.controls.cut_off = true;
        rig.run(1.0f);
        check(rig.out.equalising < 0.0f && near(rig.out.equalising_kept, 5.0f, 0.02f), "cut off: the pipe not held, the УР kept");
        rig.controls.emergency = true;
        rig.run(dt);
        check(rig.out.emergency, "the ЭТ / a stop valve / the ЭПК open the pipe with the valve cut off");
    }
    {
        // The train's DLL keeps the УР (var + 32 0x4000).
        Rig rig(5.0f);
        rig.controls.valve = ValvePosition::service;
        rig.controls.keep_equalising = true;
        rig.run(2.0f);
        check(near(rig.out.equalising, 5.0f), "the УР kept by the train's DLL");
    }
}

void ept() {
    Rig rig(5.0f);
    rig.controls.ep_on = true;
    rig.run(dt);
    check(rig.out.systems == 3 && rig.out.ep_command == EpCommand::release && rig.out.ep_value >= 0.0f, "ЭПТ in II: release");
    check(rig.out.lamp_ep_on && !rig.out.lamp_ep_hold && !rig.out.lamp_ep_brake, "ЭПТ in II: lamp О only");
    check(near(rig.out.ep_volts, 50.0f) && near(rig.out.ep_amps, 0.0f), "ЭПТ in II: 50 V");
    rig.controls.valve = ValvePosition::ep_service;
    rig.run(dt);
    check(rig.out.ep_command == EpCommand::apply && rig.out.lamp_ep_brake && !rig.out.lamp_ep_hold, "ЭПТ in VЭ: apply, lamp Т");
    check(near(rig.out.ep_volts, 40.0f) && near(rig.out.ep_amps, 5.0f), "ЭПТ braking: 40 V, 5 A");
    rig.controls.valve = ValvePosition::lap_feed;
    rig.run(dt);
    check(rig.out.ep_command == EpCommand::hold && rig.out.lamp_ep_hold && !rig.out.lamp_ep_brake, "ЭПТ in IV: hold, lamp П");
    rig.controls.ep_release_button = true;
    rig.run(dt);
    check(rig.out.ep_command == EpCommand::hold, "the release button does not undo the handle's hold");
    rig.controls.ep_release_button = false;
    rig.controls.multiple_unit = true;
    rig.controls.valve = ValvePosition::service;
    rig.run(dt);
    check(rig.out.lamp_ep_hold && rig.out.lamp_ep_brake, "BR_MODE 1: lamp П braking too");
    rig.controls.valve = ValvePosition::running;
    rig.controls.ep_apply_asked = true;
    rig.run(dt);
    check(rig.out.ep_command == EpCommand::apply, "the train's DLL applies the ЭПТ (var + 32 0x20)");
    rig.controls.ep_apply_asked = false;
    rig.controls.ep_hold_asked = true;
    rig.run(dt);
    check(rig.out.ep_command == EpCommand::hold, "the train's DLL holds it (0x10)");
    rig.controls.ep_hold_asked = false;
    rig.controls.ep_apply_button = true;
    rig.run(dt);
    check(rig.out.ep_command == EpCommand::apply, "the apply button");
    rig.controls.ep_apply_button = false;
    rig.controls.emergency = true;
    rig.run(dt);
    check(rig.out.ep_value < 0.0f && rig.out.emergency, "an emergency takes the ЭПТ's demand away");
    rig.controls.emergency = false;
    rig.controls.ep_on = false;
    rig.controls.valve = ValvePosition::service;
    rig.run(dt);
    check(rig.out.systems == 1 && rig.out.ep_value < 0.0f && !rig.out.lamp_ep_on, "ЭПТ off: no demand, no lamps");
    // The valve cut off gives no commands of its handle.
    Rig cut(5.0f);
    cut.controls.ep_on = true;
    cut.controls.cut_off = true;
    cut.controls.valve = ValvePosition::service;
    cut.run(dt);
    check(cut.out.ep_command == EpCommand::release, "a valve cut off does not command the ЭПТ");
}

void independent() {
    // The 254 (BR_FLIPCABS 1, I first): III .. VI give 1 .. 4 atm.
    for (int s = 2; s <= 5; ++s) {
        Rig rig;
        rig.controls.independent = independent_position_of(s, true);
        rig.run(dt);
        check(near(rig.out.independent_brake, static_cast<float>(s - 1)), "254 state " + std::to_string(s));
    }
    // The 224 (BR_FLIPCABS 0): 0 .. 3 give 4 .. 1 atm, 5 is I.
    {
        Rig rig;
        rig.controls.independent = independent_position_of(0, false);
        rig.run(dt);
        check(near(rig.out.independent_brake, 4.0f), "224 state 0: 4 atm");
        rig.controls.independent = independent_position_of(3, false);
        rig.run(dt);
        check(near(rig.out.independent_brake, 1.0f), "224 state 3: 1 atm");
        check(independent_position_of(5, false) == IndependentPosition::release, "224 state 5: I");
    }
    // The higher of the 254 and its second; at most the main reservoir.
    {
        Rig rig;
        rig.controls.independent = IndependentPosition::step1;
        rig.controls.independent2 = IndependentPosition::step3;
        rig.air.main_reservoir = 2.5f;
        rig.run(dt);
        check(near(rig.out.independent_brake, 2.5f), "the higher valve, at most the main reservoir");
    }
    // II lets it off: Brakes.dll's 0.1 a step, openRTS's at once (the air
    // lets the cylinder out at its own rate).
    for (const bool books : {false, true}) {
        Rig rig;
        if (books) rig.numbers = CabBrakeNumbers::openrts();
        rig.controls.independent = IndependentPosition::step4;
        rig.run(dt);
        rig.controls.independent = IndependentPosition::running;
        rig.run(dt);
        check(books ? near(rig.out.independent_brake, 0.0f) : near(rig.out.independent_brake, 3.9f),
              std::string("254 II lets the brake off (") + (books ? "openRTS" : "Brakes.dll") + ")");
        check(!rig.out.release_train_brake, "254 II leaves the train brake");
        // I lets the engine's own train brake off: Brakes.dll only with BR_BUFERKVT.
        rig.controls.independent = IndependentPosition::release;
        rig.run(dt);
        check(near(rig.out.independent_brake, 0.0f), "254 I: the independent brake off at once");
        check(rig.out.release_train_brake == books, std::string("254 I and the train brake (") + (books ? "openRTS" : "Brakes.dll") + ")");
        rig.controls.buffer_kvt = true;
        rig.run(dt);
        check(rig.out.release_train_brake, "254 I with BR_BUFERKVT lets the train brake off");
    }
    {
        // ОТ lets the train brake off.
        Rig rig;
        rig.controls.release_train_brake = true;
        rig.run(dt);
        check(rig.out.release_train_brake, "ОТ");
    }
    {
        // The 4ВК: +0.3 atm/s to 4, -0.25 atm/s, holds.
        Rig rig;
        rig.controls.vk4 = vk4_position_of(2);
        rig.run(5.0f);
        check(near(rig.out.independent_brake, 1.5f, 0.02f), "4ВК applies 0.3 atm/s: " + std::to_string(rig.out.independent_brake));
        rig.run(20.0f);
        check(near(rig.out.independent_brake, 4.0f), "4ВК at most 4 atm");
        rig.controls.vk4 = vk4_position_of(1);
        rig.run(3.0f);
        check(near(rig.out.independent_brake, 4.0f), "4ВК holds");
        rig.controls.vk4 = vk4_position_of(0);
        rig.run(4.0f);
        check(near(rig.out.independent_brake, 3.0f, 0.02f), "4ВК lets off 0.25 atm/s");
    }
}

void gauges() {
    for (const bool books : {false, true}) {
        Rig rig;
        if (books) rig.numbers = CabBrakeNumbers::openrts();
        rig.run(dt);
        rig.air.cylinder = 3.5f;
        rig.air.pipe = 3.5f;
        rig.run(1.0f);
        if (books) {
            check(near(rig.out.gauge_cylinder, 3.5f) && near(rig.out.gauge_pipe, 3.5f), "openRTS: the gauges show the air");
        } else {
            check(near(rig.out.gauge_cylinder, 1.0f, 0.04f), "Brakes.dll: the ТЦ needle 1 atm/s: " + std::to_string(rig.out.gauge_cylinder));
            check(near(rig.out.gauge_pipe, 4.5f, 0.04f), "Brakes.dll: the ТМ needle falls 0.5 atm/s: " + std::to_string(rig.out.gauge_pipe));
            // 1 atm/s to 3.0, then 0.2 atm/s within 0.5 of it: 5.5 s.
            rig.run(4.0f);
            check(rig.out.gauge_cylinder < 3.45f, "Brakes.dll: the ТЦ needle slow near the pressure");
            rig.run(0.6f);
            check(near(rig.out.gauge_cylinder, 3.5f, 0.01f), "Brakes.dll: the ТЦ needle gets there");
        }
        check(near(rig.out.gauge_main_reservoir, 8.5f), "the ГР gauge");
    }
    {
        // The 418: the pipe falling without the ЭПТ until the brake answers.
        Rig rig;
        rig.controls.sensor418 = true;
        rig.run(dt);
        for (int i = 0; i < 18; ++i) {
            rig.air.pipe -= 0.5f * dt;
            rig.run(dt);
        }
        check(rig.out.lamp_pipe, "418: lamp ТМ after 0.5 s of the pipe falling");
        rig.air.cylinder = 0.5f;
        rig.run(dt);
        check(!rig.out.lamp_pipe, "418: out once the cylinder is over 0.3");
    }
}

void positions() {
    check(valve_position_of(0, true) == ValvePosition::charge, "395 FLIPCABS 1: 0 is I");
    check(valve_position_of(6, true) == ValvePosition::emergency, "395 FLIPCABS 1: 6 is VI");
    check(valve_position_of(0, false) == ValvePosition::emergency, "130 FLIPCABS 0: 0 is VI");
    check(valve_position_of(6, false) == ValvePosition::charge, "130 FLIPCABS 0: 6 is I");
    check(!valve_position_of(7, true) && !valve_position_of(-1, false), "395 out of range");
    check(independent_position_of(0, true) == IndependentPosition::release, "254 FLIPCABS 1: 0 is I");
    check(independent_position_of(4, false) == IndependentPosition::running, "224 FLIPCABS 0: 4 is II");
    check(!vk4_position_of(3), "4ВК out of range");
}

} // namespace

int main() {
    valve();
    ept();
    independent();
    gauges();
    positions();
    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "cab brakes: all checks passed\n";
    return 0;
}
