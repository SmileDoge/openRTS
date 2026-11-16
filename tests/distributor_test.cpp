// Brake distributors (simulation/distributor.hpp, docs/scripting.md): the
// built-in ones on the test stand behave as a ВР should; the Luau copy of
// the 292 (sdk/luau/vr292.luau) gives what the built-in one gives; Luau's
// sandbox, time limit and errors.
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

#include <openrts/script/luau.hpp>
#include <openrts/simulation/distributor.hpp>

#ifndef OPENRTS_SOURCE_DIR
#define OPENRTS_SOURCE_DIR "."
#endif

namespace {

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

std::string read(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

openrts::simulation::DistributorCar car_of(const openrts::simulation::DistributorKind kind) {
    openrts::simulation::DistributorCar car;
    car.kind = kind;
    car.auxiliary_volume = openrts::simulation::distributor_auxiliary_volume(kind);
    car.mass = 70000.0f;
    car.full_cylinder = 3.8f;
    return car;
}

} // namespace

int main() {
    using namespace openrts::simulation;

    // The built-in 292 and 483 on the stand: charged, a step brakes, a
    // release releases, full service is full, a slow fall does not brake,
    // emergency fills.
    for (const auto kind : {DistributorKind::vr292, DistributorKind::vr483}) {
        const std::string name = kind == DistributorKind::vr292 ? "292" : "483";
        const auto r = run_stand(car_of(kind), nullptr);
        check(r.error.empty(), name + ": no error");
        // The ЗР to 4.8 from the pipe held at 5.0: a 292's in 130-180 s
        // (Крылов §31), a 483's 78 l in about 4.5 min (Асадченко).
        float to48 = -1.0f;
        for (const auto& s : r.samples) {
            if (s.auxiliary >= 4.8f) {
                to48 = s.time;
                break;
            }
        }
        const bool in_time = kind == DistributorKind::vr292 ? to48 >= 130.0f && to48 <= 180.0f : to48 >= 240.0f && to48 <= 300.0f;
        check(in_time, name + ": the ЗР charges to 4.8 in its time (" + std::to_string(to48) + " s)");
        check(r.step_cylinder > 0.5f && r.step_cylinder < 2.5f, name + ": a 0.5 step brakes (" + std::to_string(r.step_cylinder) + ")");
        check(r.release > 0.0f && r.release < 60.0f, name + ": released after the step (" + std::to_string(r.release) + " s)");
        check(r.full_cylinder > 3.0f, name + ": full service is full (" + std::to_string(r.full_cylinder) + ")");
        check(r.creep < 0.05f, name + ": an overcharge removed slowly brakes nothing (" + std::to_string(r.creep) + ")");
        check(r.emergency_fill > 0.0f && r.emergency_fill < 15.0f && r.emergency_cylinder > 3.5f,
              name + ": emergency fills (" + std::to_string(r.emergency_fill) + " s)");
    }

    // The 292's plug: Д and УВ release and fill in emergency about twice as
    // slow as К (the research table: release К 9–12 s, Д 19–24 s; emergency
    // to 3.5 К 6–9 s, Д 12–16 s).
    {
        auto k = car_of(DistributorKind::vr292);
        auto d = k;
        d.plug = DistributorPlug::long_train;
        const auto rk = run_stand(k, nullptr);
        const auto rd = run_stand(d, nullptr);
        check(rd.release > rk.release * 1.6f, "292 Д releases slower than К (" + std::to_string(rk.release) + " / " + std::to_string(rd.release) + " s)");
        check(rd.emergency_fill > rk.emergency_fill * 1.6f && rd.emergency_fill <= 16.5f,
              "292 Д fills in emergency slower than К (" + std::to_string(rk.emergency_fill) + " / " + std::to_string(rd.emergency_fill) + " s)");
        check(rd.creep < 0.05f, "292 Д: still stable");
    }

    // The 483's load mode sets the full ТЦ; mountain mode releases in steps,
    // flat mode holds until the pipe is back at РК.
    {
        check(distributor_full_cylinder(DistributorKind::vr483, DistributorLoad::empty) < 2.0f &&
                  distributor_full_cylinder(DistributorKind::vr483, DistributorLoad::loaded) > 3.5f,
              "483: empty and loaded full ТЦ");
        check(effective_load(DistributorLoad::by_mass, 80000.0f) == DistributorLoad::loaded &&
                  effective_load(DistributorLoad::by_mass, 22000.0f) == DistributorLoad::empty &&
                  effective_load(DistributorLoad::empty, 80000.0f) == DistributorLoad::empty,
              "483: the load by mass until a hand sets it");
        // RTS content's force at the full cylinder of the mode the car's
        // mass gives: an empty file's MaxBrakeForce is its empty mode's.
        {
            openrts::formats::WagonDefinition wagon;
            wagon.kind = openrts::formats::WagonKind::freight;
            wagon.brakes.cylinder_pressure_for_max_force = 57.0f / 14.69f;
            const float full = wagon.brakes.cylinder_pressure_for_max_force;
            wagon.mass = 23300.0f;
            const float empty_at = brake_force_pressure(wagon);
            check(std::abs(empty_at - full * 1.5f / 3.8f) < 1e-3f, "483 empty file: its force at the empty mode's full ТЦ (" + std::to_string(empty_at) + ")");
            wagon.mass = 50000.0f;
            check(std::abs(brake_force_pressure(wagon) - full * 3.0f / 3.8f) < 1e-3f, "483 medium by mass: the medium mode's full ТЦ");
            wagon.mass = 93100.0f;
            check(brake_force_pressure(wagon) == full, "483 loaded file: as the content gives it");
            wagon.mass = 23300.0f;
            wagon.brakes.realistic_brake_force = 48000.0f;
            check(brake_force_pressure(wagon) == full, "ORTSBrakeForce: a real car's force at the content's pressure, every mode");
            wagon.brakes.realistic_brake_force = 0.0f;
            wagon.kind = openrts::formats::WagonKind::passenger;
            check(brake_force_pressure(wagon) == full, "a 292 car: as the content gives it");
        }
        auto empty = car_of(DistributorKind::vr483);
        empty.load = DistributorLoad::empty;
        empty.full_cylinder = distributor_full_cylinder(empty.kind, empty.load);
        const auto re = run_stand(empty, nullptr);
        check(re.full_cylinder < 1.6f, "483 empty: full service gives the empty ТЦ (" + std::to_string(re.full_cylinder) + ")");
        // Charge, a 1.0 step, then the pipe back up by 0.5 and held.
        const auto partial_release = [](const bool mountain, const float back_to = 4.5f) {
            auto car = car_of(DistributorKind::vr483);
            car.mountain = mountain;
            DistributorState s;
            const float h = 1.0f / 300.0f;
            const auto hold = [&](const float target, const float seconds) {
                for (float t = 0.0f; t < seconds; t += h) {
                    s.pipe = s.pipe < target ? (std::min)(s.pipe + 0.25f * h, target) : (std::max)(s.pipe - 0.25f * h, target);
                    step_distributor(s, car, 0.0f, -1.0f, h);
                }
            };
            hold(5.0f, 400.0f);
            hold(4.0f, 40.0f);
            const float applied = s.cylinder;
            hold(back_to, 40.0f);
            return std::pair{applied, s.cylinder};
        };
        // Flat: no graduated release; a rise of 0.15-0.3 over the braked
        // pipe releases fully (Крылов, table 9), a smaller one nothing.
        const auto [flat_applied, flat_after] = partial_release(false);
        const auto [small_applied, small_after] = partial_release(false, 4.1f);
        const auto [mountain_applied, mountain_after] = partial_release(true);
        check(flat_applied > 2.0f && flat_after < 0.1f,
              "483 flat: a rise of 0.5 releases fully (" + std::to_string(flat_applied) + " -> " + std::to_string(flat_after) + ")");
        check(std::abs(small_after - small_applied) < 0.05f,
              "483 flat: a rise of 0.1 releases nothing (" + std::to_string(small_applied) + " -> " + std::to_string(small_after) + ")");
        check(mountain_applied > 2.0f && mountain_after < mountain_applied * 0.75f && mountain_after > 0.5f,
              "483 mountain: a partial pipe rise releases part (" + std::to_string(mountain_applied) + " -> " + std::to_string(mountain_after) + ")");
    }

    // The Luau 292 matches the built-in one.
    {
        const std::string path = std::string(OPENRTS_SOURCE_DIR) + "/sdk/luau/vr292.luau";
        std::string error;
        auto script = ScriptDistributor::load(read(path), "vr292.luau", error);
        check(script != nullptr, "the Luau 292 loads: " + error);
        if (script) {
            for (const auto plug : {DistributorPlug::short_train, DistributorPlug::long_train}) {
                auto car = car_of(DistributorKind::vr292);
                car.plug = plug;
                const auto native = run_stand(car, nullptr);
                const auto luau = run_stand(car, script.get());
                const std::string name = plug == DistributorPlug::short_train ? " (К)" : " (Д)";
                check(luau.error.empty(), "the Luau 292 runs: " + luau.error);
                check(native.samples.size() == luau.samples.size(), "as many samples" + name);
                float most = 0.0f;
                for (std::size_t k = 0; k < native.samples.size() && k < luau.samples.size(); ++k) {
                    most = (std::max)({most, std::abs(native.samples[k].cylinder - luau.samples[k].cylinder),
                                       std::abs(native.samples[k].auxiliary - luau.samples[k].auxiliary)});
                }
                check(most < 0.01f, "the Luau 292 gives the built-in one's ТЦ and ЗР" + name + " (most apart " + std::to_string(most) + ")");
            }
        }
    }

    // Luau: a syntax error with its line, no step, a runtime error, an
    // endless loop stopped, no file access.
    {
        std::string error;
        check(!ScriptDistributor::load("function step(c, dt)\n  c.tc = = 1\nend\n", "bad.luau", error) &&
                  error.find("bad.luau:2") != std::string::npos,
              "a syntax error names the file and the line: " + error);
        error.clear();
        check(!ScriptDistributor::load("local x = 1\n", "nostep.luau", error) && error.find("step") != std::string::npos,
              "a script without step(c, dt) is refused");
        auto failing = ScriptDistributor::load("function step(c, dt)\n  error('сломано')\nend\n", "fails.luau", error);
        check(failing != nullptr, "a script that fails only when run loads");
        if (failing) {
            DistributorState state;
            check(!failing->step(state, car_of(DistributorKind::vr292), 0.0f, -1.0f, 0.01f, error) &&
                      error.find("сломано") != std::string::npos,
                  "its error comes back: " + error);
        }
        auto endless = ScriptDistributor::load("function step(c, dt)\n  while true do end\nend\n", "loop.luau", error);
        check(endless != nullptr, "an endless loop loads");
        if (endless) {
            DistributorState state;
            check(!endless->step(state, car_of(DistributorKind::vr292), 0.0f, -1.0f, 0.01f, error) &&
                      error.find("time limit") != std::string::npos,
                  "an endless loop is stopped by the time limit: " + error);
        }
        check(!ScriptDistributor::load("local f = io.open('x')\nfunction step(c, dt) end\n", "io.luau", error),
              "no io: files cannot be opened");
    }

    // A 292 brakes on a first step of 0.3 from any charging pressure (until
    // 2026-11-17 not above a pipe of 4.9: from 5.2 a 0.3 step braked nothing).
    for (const float charged : {5.0f, 5.2f, 5.4f}) {
        auto car = car_of(DistributorKind::vr292);
        DistributorState s;
        s.pipe = s.auxiliary = s.reference = charged;
        constexpr float h = 1.0f / 300.0f;
        for (float t = 0.0f; t < 15.0f; t += h) {
            s.pipe = (std::max)(charged - 0.25f * t, charged - 0.3f); // V at 0.25 atm/s, then a lap
            step_distributor(s, car, t < 1.2f ? -0.25f : 0.0f, -1.0f, h);
        }
        check(s.braking && s.cylinder > 0.5f, "292: a 0.3 step from " + std::to_string(charged) + " brakes (" + std::to_string(s.cylinder) + ")");
    }

    // ЭВР 305 (Крылов, «Автоматические тормоза подвижного состава», 1983,
    // §39): 3 atm in 2.5-3.5 s; released from 3.5 to 0.4 in 8-10 s (305-000,
    // 1.3 mm) or 3.5-4.5 s (305-001, 2.0 mm), whatever the 292's plug; the
    // wire dead, its valves let go; a lap holds; the 292 still answers the pipe.
    {
        constexpr float h = 1.0f / 300.0f;
        struct Times {
            float fill = -1.0f, release = -1.0f, held{}, dead = -1.0f;
        };
        const auto run = [&](const DistributorKind kind, const DistributorPlug plug, const float pace = 1.0f) {
            auto car = car_of(kind);
            car.ep = true;
            car.plug = plug;
            car.ep_fill = pace;
            DistributorState s;
            s.pipe = s.auxiliary = s.reference = 5.0f;
            Times out;
            const auto step = [&](const EpCommand command, const float ep, const float seconds) {
                for (float t = 0.0f; t < seconds; t += h) {
                    s.pipe = 5.0f; // a locomotive's valve holds the pipe charged
                    step_distributor(s, car, 0.0f, ep, h, command);
                }
            };
            float t = 0.0f;
            while (s.cylinder < 3.0f && t < 10.0f) {
                s.pipe = 5.0f;
                step_distributor(s, car, 0.0f, 0.0f, h, EpCommand::apply);
                t += h;
            }
            out.fill = t;
            while (s.cylinder < 3.5f && t < 20.0f) {
                s.pipe = 5.0f;
                step_distributor(s, car, 0.0f, 0.0f, h, EpCommand::apply);
                t += h;
            }
            step(EpCommand::hold, 0.0f, 5.0f);
            out.held = s.cylinder;
            t = 0.0f;
            while (s.cylinder > 0.4f && t < 30.0f) {
                s.pipe = 5.0f;
                step_distributor(s, car, 0.0f, 0.0f, h, EpCommand::release);
                t += h;
            }
            out.release = t;
            // Applied again, then the wire dead (no command, no target).
            step(EpCommand::release, 0.0f, 15.0f);
            step(EpCommand::apply, 0.0f, 3.0f);
            t = 0.0f;
            while (s.cylinder > 0.4f && t < 30.0f) {
                s.pipe = 5.0f;
                step_distributor(s, car, 0.0f, -1.0f, h, EpCommand::target);
                t += h;
            }
            out.dead = t;
            return out;
        };
        const auto coach = run(DistributorKind::evr305, DistributorPlug::short_train);
        const auto coach_long = run(DistributorKind::evr305, DistributorPlug::long_train);
        const auto emu = run(DistributorKind::evr305_001, DistributorPlug::short_train);
        check(coach.fill >= 2.5f && coach.fill <= 3.5f, "305: 3 atm in 2.5-3.5 s (" + std::to_string(coach.fill) + " s)");
        // A train's own pace (Train::scatter_ep_fill): 1.2 atm/s fills faster.
        const auto quick = run(DistributorKind::evr305, DistributorPlug::short_train, 1.2f);
        check(quick.fill < coach.fill * 0.9f, "305: a quicker train's ЭВР fill faster (" + std::to_string(quick.fill) + " s)");
        check(std::abs(coach.held - 3.5f) < 0.05f, "305: a lap holds the cylinder (" + std::to_string(coach.held) + ")");
        check(coach.release >= 8.0f && coach.release <= 10.0f, "305-000: 3.5 to 0.4 in 8-10 s (" + std::to_string(coach.release) + " s)");
        check(std::abs(coach_long.release - coach.release) < 0.1f, "305: the release is the 305's, not the 292 plug's (Д " +
                                                                       std::to_string(coach_long.release) + " s)");
        check(emu.release >= 3.5f && emu.release <= 4.5f, "305-001: 3.5 to 0.4 in 3.5-4.5 s (" + std::to_string(emu.release) + " s)");
        check(coach.dead > 0.0f && coach.dead < 12.0f, "305: the wire dead, the brakes release (" + std::to_string(coach.dead) + " s)");

        // A DLL's target: the cylinder goes to it and back down to a lower one.
        auto car = car_of(DistributorKind::evr305);
        car.ep = true;
        DistributorState s;
        s.pipe = s.auxiliary = s.reference = 5.0f;
        const auto hold_for = [&](const float ep, const float seconds) {
            for (float t = 0.0f; t < seconds; t += h) {
                s.pipe = 5.0f;
                step_distributor(s, car, 0.0f, ep, h);
            }
        };
        hold_for(2.0f, 5.0f);
        check(std::abs(s.cylinder - 2.0f) < 0.05f, "305: a DLL's target of 2.0 (" + std::to_string(s.cylinder) + ")");
        hold_for(1.0f, 8.0f);
        check(std::abs(s.cylinder - 1.0f) < 0.05f, "305: down to a target of 1.0 (" + std::to_string(s.cylinder) + ")");
        // The 292 behind it answers a pipe reduction with the ЭПТ holding.
        for (float t = 0.0f; t < 10.0f; t += h) {
            s.pipe = (std::max)(5.0f - t * 0.25f, 4.2f);
            step_distributor(s, car, -0.25f, 1.0f, h, EpCommand::hold);
        }
        // (The ЗР not yet charged again: the 292 gives 3 x (ЗР - pipe).)
        check(s.cylinder > 1.4f, "305: a pipe reduction brakes through its 292 above what the ЭПТ holds (" + std::to_string(s.cylinder) + ")");
    }

    // The 395's ЭПТ (driver_valve.hpp): commands with the realistic air, and
    // its demand gone when switched off.
    {
        ValveState state;
        ValveInputs in;
        in.pipe = 5.0f;
        in.main_reservoir = 8.0f;
        in.realistic = true;
        in.ep_on = true;
        const auto at = [&](const ValvePosition position) { return step_valve_395(state, position, in, 0.1f); };
        check(at(ValvePosition::ep_service).ep_command == EpCommand::apply, "VЭ: apply");
        check(at(ValvePosition::service).ep_command == EpCommand::apply, "V: apply");
        check(at(ValvePosition::lap).ep_command == EpCommand::hold && at(ValvePosition::lap_feed).ep_command == EpCommand::hold,
              "III and IV: hold");
        check(at(ValvePosition::running).ep_command == EpCommand::release && at(ValvePosition::charge).ep_command == EpCommand::release,
              "I and II: release");
        for (int k = 0; k < 40; ++k) at(ValvePosition::ep_service);
        in.ep_on = false;
        check(at(ValvePosition::lap).ep_value < 0.0f, "the ЭПТ off: no demand");
        in.ep_on = true;
        check(at(ValvePosition::lap).ep_value == 0.0f, "switched on again in a lap: the old demand does not come back");
        in.realistic = false;
        check(at(ValvePosition::ep_service).ep_command == EpCommand::target, "the RTS model: a target");
    }

    if (failures == 0) std::cout << "distributor: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
