#pragma once

// The exterior lights of cars no DLL drives, as rts1.exe decides them
// (0x4F07D4 for a train, 0x4FFDF8 for one light; docs/train-systems.md):
// each light's Conditions ( Headlight Control Service Unit TimeOfDay
// Weather ) against the train, the car's place in it and the world. And a
// light's state over time (FadeIn / FadeOut, its States cycling, 0x4FF9B8).

#include <cstdint>
#include <vector>

#include <openrts/formats/rolling_stock.hpp>

namespace openrts::simulation {

// RTS train types as the light conditions read them (train +0).
enum class TrainKind : std::uint8_t {
    static_train = 0, // STATIC: a consist standing (no lights at all)
    player = 1,
    ai = 2,
    loose = 3,        // LOOSE: uncoupled, rolling or standing (no lights)
    remote = 4,       // another player's train (multiplayer)
};

// The world the conditions look at: RTS's is_night, the light level
// (dword_61D60C: the sun's ambient and direct colours averaged, below 0.5
// is dusk) and the weather (byte_61D62D: 0 clear, 1 snow, 2 rain).
struct LightWorld {
    bool night = false;
    float world_lit = 1.0f;
    std::uint8_t weather = 0;
};

// A car's place in its train for the Unit condition: 1 in the middle, 2
// first, 3 last, 4 first and 5 last when the train runs reversed.
enum class LightUnit : std::uint8_t { middle = 1, first = 2, last = 3, first_reversed = 4, last_reversed = 5 };

// 0x4FFDF8: whether `light` is on for a car at `unit` of a train of `kind`.
// `cold_engine`: the car is an engine started cold (LocoFlags bit 3).
[[nodiscard]] bool light_condition(const formats::LightDefinition& light, LightUnit unit, TrainKind kind, bool cold_engine,
                                   const LightWorld& world);

// 0x4F07D4: which lights of car `index` of a train of `cars` cars are to be
// on (one entry per light of the car). A STATIC or LOOSE train has none on;
// `reversed`: the train runs with its rear leading (train +4 bit 0x80).
[[nodiscard]] std::vector<bool> car_lights(const formats::WagonDefinition& car, std::size_t index, std::size_t cars,
                                           TrainKind kind, bool reversed, bool cold_engine, const LightWorld& world);

// One light's state over time (0x4FF9B8): fading in and out over FadeIn /
// FadeOut, and which of its States shows (by their Durations, or the Cycle
// spread evenly over them).
struct LightRunner {
    float level{};          // 0 off .. 1 on
    float clock{};          // s into the States' cycle
    std::size_t state{};    // the State shown
    void step(const formats::LightDefinition& light, bool on, float dt);
};

} // namespace openrts::simulation
