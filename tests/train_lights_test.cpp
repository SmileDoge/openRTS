// The lights of cars no DLL drives (simulation/train_lights, RTS 0x4F07D4 /
// 0x4FFDF8 / 0x4FF9B8): each Conditions value against the train, the car's
// place and the world; the fade and the States' cycle.
#include <cmath>
#include <iostream>
#include <string>

#include <openrts/simulation/train_lights.hpp>

namespace {

using namespace openrts;
using simulation::LightUnit;
using simulation::TrainKind;

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

formats::LightDefinition light(std::uint8_t headlight, std::uint8_t control, std::uint8_t service, std::uint8_t unit,
                               std::uint8_t time_of_day, std::uint8_t weather) {
    formats::LightDefinition l;
    l.headlight = headlight;
    l.control = control;
    l.service = service;
    l.unit = unit;
    l.time_of_day = time_of_day;
    l.weather = weather;
    l.states.push_back({});
    return l;
}

void test_conditions() {
    const simulation::LightWorld day{.night = false, .world_lit = 0.9f, .weather = 0};
    const simulation::LightWorld dusk{.night = false, .world_lit = 0.3f, .weather = 0};
    const simulation::LightWorld night{.night = true, .world_lit = 0.1f, .weather = 0};
    const auto on = [](const formats::LightDefinition& l, LightUnit u, TrainKind k, bool cold, const simulation::LightWorld& w) {
        return simulation::light_condition(l, u, k, cold, w);
    };
    // Headlight: 2 dusk, 3 night, 4 dusk or night; a cold engine is "day".
    check(on(light(3, 0, 0, 0, 0, 0), LightUnit::first, TrainKind::ai, false, night), "Headlight 3 at night");
    check(!on(light(3, 0, 0, 0, 0, 0), LightUnit::first, TrainKind::ai, false, dusk), "Headlight 3 not at dusk");
    check(on(light(2, 0, 0, 0, 0, 0), LightUnit::first, TrainKind::ai, false, dusk), "Headlight 2 at dusk");
    check(on(light(4, 0, 0, 0, 0, 0), LightUnit::first, TrainKind::ai, false, dusk) &&
              on(light(4, 0, 0, 0, 0, 0), LightUnit::first, TrainKind::ai, false, night) &&
              !on(light(4, 0, 0, 0, 0, 0), LightUnit::first, TrainKind::ai, false, day), "Headlight 4: dusk or night");
    check(on(light(1, 0, 0, 0, 0, 0), LightUnit::first, TrainKind::ai, true, night), "Headlight 1: a cold engine at night");
    // Control: 1 not the player's, 2 the player's.
    check(!on(light(0, 1, 0, 0, 0, 0), LightUnit::first, TrainKind::player, false, day), "Control 1 off on the player's train");
    check(on(light(0, 2, 0, 0, 0, 0), LightUnit::first, TrainKind::player, false, day), "Control 2 on the player's train");
    check(!on(light(0, 2, 0, 0, 0, 0), LightUnit::first, TrainKind::ai, false, day), "Control 2 off on an AI train");
    // Service: 1 out of service (a cold engine, STATIC / LOOSE), 2 working.
    check(on(light(0, 0, 1, 0, 0, 0), LightUnit::first, TrainKind::ai, true, day), "Service 1 on a cold engine");
    check(!on(light(0, 0, 1, 0, 0, 0), LightUnit::first, TrainKind::ai, false, day), "Service 1 off on a working train");
    check(!on(light(0, 0, 2, 0, 0, 0), LightUnit::first, TrainKind::ai, true, day), "Service 2 off on a cold engine");
    // Unit, TimeOfDay (the day light stays on until night AND dim), Weather.
    check(!on(light(0, 0, 0, 3, 0, 0), LightUnit::first, TrainKind::ai, false, day), "Unit 3 is the last car");
    check(on(light(0, 0, 0, 1, 0, 0), LightUnit::middle, TrainKind::ai, false, day), "Unit 1 in the middle");
    check(on(light(0, 0, 0, 0, 1, 0), LightUnit::first, TrainKind::ai, false, dusk), "TimeOfDay 1 at dusk (not yet night)");
    check(!on(light(0, 0, 0, 0, 1, 0), LightUnit::first, TrainKind::ai, false, night), "TimeOfDay 1 off at night");
    check(on(light(0, 0, 0, 0, 2, 0), LightUnit::first, TrainKind::ai, false, dusk), "TimeOfDay 2 at dusk");
    const simulation::LightWorld snow{.night = false, .world_lit = 0.9f, .weather = 1};
    check(on(light(0, 0, 0, 0, 0, 3), LightUnit::first, TrainKind::ai, false, snow) &&
              !on(light(0, 0, 0, 0, 0, 2), LightUnit::first, TrainKind::ai, false, snow), "Weather 3 is snow, 2 rain");
}

void test_train() {
    formats::WagonDefinition car;
    car.lights.push_back(light(0, 0, 0, 2, 0, 0)); // the head light
    car.lights.push_back(light(0, 0, 0, 3, 0, 0)); // the tail light
    car.lights.push_back(light(0, 0, 0, 4, 0, 0)); // head light when reversed
    const simulation::LightWorld day{};
    auto lit = simulation::car_lights(car, 0, 3, TrainKind::ai, false, false, day);
    check(lit[0] && !lit[1] && !lit[2], "the first car of three: Unit 2");
    lit = simulation::car_lights(car, 2, 3, TrainKind::ai, false, false, day);
    check(!lit[0] && lit[1], "the last car: Unit 3");
    lit = simulation::car_lights(car, 0, 1, TrainKind::ai, false, false, day);
    check(lit[0] && lit[1], "a single car is first and last");
    lit = simulation::car_lights(car, 0, 3, TrainKind::ai, true, false, day);
    check(!lit[0] && lit[2], "reversed: Unit 4");
    lit = simulation::car_lights(car, 0, 3, TrainKind::static_train, false, false, day);
    check(!lit[0] && !lit[1] && !lit[2], "a STATIC train shows none");
}

void test_runner() {
    auto l = light(0, 0, 0, 0, 0, 0);
    l.fade_in = 1.0f;
    l.fade_out = 0.5f;
    l.states.resize(2);
    l.states[0].duration = 0.5f;
    l.states[1].duration = 0.5f;
    simulation::LightRunner r;
    r.step(l, true, 0.5f);
    check(std::abs(r.level - 0.5f) < 1e-5f, "fading in over FadeIn");
    r.step(l, true, 0.5f);
    r.step(l, true, 0.3f);
    const auto first = r.state;
    r.step(l, true, 0.4f);
    check(r.level == 1.0f && first != r.state, "the States change by their Durations");
    r.step(l, false, 0.25f);
    check(std::abs(r.level - 0.5f) < 1e-5f, "fading out over FadeOut");
}

} // namespace

int main() {
    test_conditions();
    test_train();
    test_runner();
    std::cout << (failures ? "FAILED" : "train lights tests passed") << '\n';
    return failures ? 1 : 0;
}
