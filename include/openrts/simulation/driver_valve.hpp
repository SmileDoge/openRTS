#pragma once

#include <cstdint>

// The driver's brake valve 395 and the boundary between a valve and the air
// (docs/train.md, "The driver's valve"). A valve keeps the equalising
// reservoir (УР) and says what it asks of the pipe: the realistic air model
// holds the pipe at the УР through its relay, opens it at the engine in VI,
// and takes the ЭПТ demand; the RTS model takes the pipe rate. Whoever
// works the handle (the keyboard, a cab's control under a DLL) steps it.
namespace openrts::simulation {

// Positions I, II, III, IV, VЭ (Va), V, VI, in that order (a cab's 395 control
// has them as its states 0..6).
enum class ValvePosition : std::uint8_t {
    charge,     // I: charge and release
    running,    // II
    lap,        // III: lap without feed
    lap_feed,   // IV: lap with feed
    ep_service, // VЭ: the ЭПТ applies; without it see ValveInputs::slow_service
    service,    // V
    emergency,  // VI
};

// What the valve keeps between steps.
struct ValveState {
    float equalising = 5.0f; // УР, atm
    float ep_target{};       // ЭПТ demand, atm
};

struct ValveInputs {
    float pipe{};           // atm at the engine
    float main_reservoir{}; // atm
    float cylinder{};       // the engine's train brake cylinder, atm
    float charging_pressure = 5.0f; // II charges the УР to this
    // The train runs the realistic air: no overcharge to push a release
    // through RTS's triple valve, and the real stabiliser.
    bool realistic = false;
    bool ep_on = false; // the ЭПТ switched on
    // VЭ without the ЭПТ: as Va, a slow service (the УР down 0.03 atm/s,
    // MaDaDev's Brakes.dll); otherwise a lap without feed (the ЭР9Т's).
    bool slow_service = false;
};

struct ValveOutput {
    float pipe_rate{};      // the RTS model's TrainPipeRate, atm/s (> 0 feed, < 0 vent)
    float equalising{};     // the УР the pipe is to follow
    bool emergency = false; // VI: the pipe opened at the engine
    float ep_value = -1.0f; // the ЭПТ demand, < 0 none
    std::uint16_t systems = 1; // BrakeSystemsEngaged: 1, | 2 with the ЭПТ
};

// One step of `dt` s of a 395 as the ВЛ10к DLL works it (vl10k_main.cpp),
// the ЭПТ as the ЭР9Т's (docs/train.md, the table of positions).
ValveOutput step_valve_395(ValveState& state, ValvePosition position, const ValveInputs& in, float dt);

} // namespace openrts::simulation
