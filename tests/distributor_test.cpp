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
        check(r.charged > 0.0f && r.charged < 300.0f, name + ": the ЗР charges (" + std::to_string(r.charged) + " s)");
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
        auto empty = car_of(DistributorKind::vr483);
        empty.load = DistributorLoad::empty;
        empty.full_cylinder = distributor_full_cylinder(empty.kind, empty.load);
        const auto re = run_stand(empty, nullptr);
        check(re.full_cylinder < 1.6f, "483 empty: full service gives the empty ТЦ (" + std::to_string(re.full_cylinder) + ")");
        // Charge, a 1.0 step, then the pipe back up by 0.5 and held.
        const auto partial_release = [](const bool mountain) {
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
            hold(4.5f, 40.0f);
            return std::pair{applied, s.cylinder};
        };
        const auto [flat_applied, flat_after] = partial_release(false);
        const auto [mountain_applied, mountain_after] = partial_release(true);
        check(flat_applied > 2.0f && std::abs(flat_after - flat_applied) < 0.05f,
              "483 flat: a partial pipe rise releases nothing (" + std::to_string(flat_applied) + " -> " + std::to_string(flat_after) + ")");
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

    if (failures == 0) std::cout << "distributor: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
