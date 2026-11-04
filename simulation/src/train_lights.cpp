#include <openrts/simulation/train_lights.hpp>

#include <algorithm>
#include <cmath>

namespace openrts::simulation {

bool light_condition(const formats::LightDefinition& light, const LightUnit unit, const TrainKind kind, const bool cold_engine,
                     const LightWorld& world) {
    const auto k = static_cast<int>(kind);
    // Control: 1 not the player's train, 2 the player's (types 1 and 4).
    const bool players = k == 1 || k == 4;
    if (light.control == 1 && players) return false;
    if (light.control == 2 && !players) return false;
    // Service: 1 a train out of service (STATIC / LOOSE, or a cold engine),
    // 2 one working.
    const bool out_of_service = k == 0 || k == 3;
    if (light.service == 1 && !cold_engine && !out_of_service) return false;
    if (light.service == 2 && (cold_engine || out_of_service)) return false;
    if (light.unit != 0 && light.unit != static_cast<std::uint8_t>(unit)) return false;
    // TimeOfDay: 1 day, 2 night. RTS leaves a day light on until it is
    // both night and dim, and a night light until it is both day and bright.
    if (light.time_of_day == 1 && world.night && world.world_lit < 0.5f) return false;
    if (light.time_of_day == 2 && !world.night && world.world_lit >= 0.5f) return false;
    // Weather: 1 clear, 2 rain, 3 snow (the world's 0 clear, 1 snow, 2 rain).
    if (light.weather == 1 && world.weather != 0) return false;
    if (light.weather == 2 && world.weather != 2) return false;
    if (light.weather == 3 && world.weather != 1) return false;
    // Headlight: 1 day (and a cold engine), 2 dusk, 3 night, 4 dusk or night.
    if (light.headlight != 0) {
        int phase = world.night ? 3 : world.world_lit < 0.5f ? 2 : 1;
        if (cold_engine) phase = 1;
        if (light.headlight == 4) {
            if (phase < 2) return false;
        } else if (light.headlight != phase) {
            return false;
        }
    }
    return true;
}

std::vector<bool> car_lights(const formats::WagonDefinition& car, const std::size_t index, const std::size_t cars,
                             const TrainKind kind, const bool reversed, const bool cold_engine, const LightWorld& world) {
    std::vector<bool> on(car.lights.size(), false);
    // STATIC and LOOSE trains show no lights (0x4F07D4, without an activity
    // service record).
    if (kind == TrainKind::static_train || kind == TrainKind::loose) return on;
    for (std::size_t j = 0; j < car.lights.size(); ++j) {
        const auto& light = car.lights[j];
        if (light.states.empty()) continue;
        bool lit = false;
        // The first car takes Unit 2 (4 reversed), the last 3 (5); a single
        // car both; the others 1.
        if (index == 0) lit = lit || light_condition(light, reversed ? LightUnit::first_reversed : LightUnit::first, kind, cold_engine, world);
        if (index + 1 == cars) lit = lit || light_condition(light, reversed ? LightUnit::last_reversed : LightUnit::last, kind, cold_engine, world);
        if (index > 0 && index + 1 < cars) lit = lit || light_condition(light, LightUnit::middle, kind, cold_engine, world);
        on[j] = lit;
    }
    return on;
}

void LightRunner::step(const formats::LightDefinition& light, const bool on, const float dt) {
    const float time = on ? light.fade_in : light.fade_out;
    const float goal = on ? 1.0f : 0.0f;
    level = time <= 0.0f ? goal : goal > level ? (std::min)(goal, level + dt / time) : (std::max)(goal, level - dt / time);
    const auto n = light.states.size();
    if (n <= 1 || level <= 0.0f) {
        state = 0;
        clock = 0.0f;
        return;
    }
    clock += dt;
    // 0x4FF9B8: with a Cycle the states share it evenly; without, each
    // shows for its Duration (none: the first stays).
    if (light.cycle > 0.0f) {
        if (clock > light.cycle) clock = std::fmod(clock, light.cycle);
        state = (std::min)(static_cast<std::size_t>(clock / (light.cycle / static_cast<float>(n))), n - 1);
    } else if (light.states.front().duration != 0.0f) {
        float total = 0.0f;
        for (const auto& s : light.states) total += (std::max)(s.duration, 0.0f);
        if (total > 0.0f && clock >= total) clock = std::fmod(clock, total);
        float sum = 0.0f;
        state = n - 1;
        for (std::size_t j = 0; j < n; ++j) {
            sum += (std::max)(light.states[j].duration, 0.0f);
            if (sum > clock) {
                state = j;
                break;
            }
        }
    }
}

} // namespace openrts::simulation
