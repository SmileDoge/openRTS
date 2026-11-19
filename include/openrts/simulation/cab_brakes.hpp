#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include <openrts/simulation/driver_valve.hpp>

// openRTS's own cab brake devices in place of MaDaDev's Brakes.dll's
// (docs/brakes-dll-replacement.md, step 2; what Brakes.dll does:
// docs/reverse/brakes-dll.md §10): the driver's valve (395, 130), the
// independent brake (254, 224, 4ВК), the 367 and the cocks, the ЭПТ's
// commands, the cab's air gauges and lamps. They work the realistic air:
// the УР the pipe follows, the ЭПТ's command to the cars' ЭВР, the
// independent brake's target. Their numbers come in two sets
// (CabBrakeNumbers): Brakes.dll's, and openRTS's from the books, which
// differ only where Brakes.dll's fall outside what the books give.
namespace openrts::simulation {

// The independent brake valve's positions (254; the 224 mirrored): I lets
// the engine's own train brake off, II lets the independent brake off,
// III .. VI apply it in steps (CabBrakeNumbers::independent_steps).
enum class IndependentPosition : std::uint8_t { release, running, step1, step2, step3, step4 };
// The 4ВК's (BR_4VKID): it lets the independent brake off, holds it, applies it.
enum class Vk4Position : std::uint8_t { release, hold, apply };

struct CabBrakeNumbers {
    // The driver's valve, atm/s unless named otherwise.
    float charge_ur_rate{};   // I: the УР from the main reservoir at most this,
    float charge_ur_gain{};   //    and this x (MR - УР) when less, 1/s
    float running_ur_rate{};  // II: the УР up to the charging pressure at most this,
    float running_ur_gain{};  //     and this x (T - УР) when less, 1/s
    float stabiliser{};       // II: the УР down always (an overcharge goes)
    float charging_drop{};    // T: the charging pressure named less this, atm
    float lap_ur_rate{};      // III: the УР down to the pipe
    float slow_service{};     // VЭ (Va): the УР down while the handle rests there
    float service{};          // V: the same
    float emergency_ur{};     // VI: the УР out
    // The independent brake: its III .. VI, atm (at most the main reservoir).
    std::array<float, 4> independent_steps{};
    float independent_release{}; // II: its target down, atm/s
    // Its I lets the engine's own train brake off with BR_BUFERKVT only.
    bool independent_release_needs_buffer = true;
    float vk4_apply{}, vk4_release{}, vk4_max{}; // the 4ВК, atm/s and atm
    // The ЭПТ's voltmeter and ammeter (BR_VOLT, BR_AMP): V in I and II,
    // V in III .. VI; A in a lap, A braking.
    float ept_volts_release{}, ept_volts_brake{}, ept_amps_lap{}, ept_amps_brake{};
    // The ЭПТ's level for cars on RTS's air (CabBrakeOutput::ep_value): up
    // applying, down released, atm/s, to 4.3 atm.
    float ep_apply{}, ep_release{};
    // The needles: the ТЦ's moves at most `cylinder_needle_near` atm/s
    // within `cylinder_needle_band` of the pressure, `cylinder_needle_far`
    // beyond; the ТМ's follows a rise at once and a fall at most
    // `pipe_needle_fall` atm/s. 0: the pressure as it is.
    float cylinder_needle_near{}, cylinder_needle_far{}, cylinder_needle_band{};
    float pipe_needle_fall{};
    // The 418 (the pipe's break, lamp ТМ): the pipe falling faster than
    // `sensor418_rate` atm/s for longer than `sensor418_time` s without the
    // ЭПТ; out again once the cylinder is over `sensor418_cylinder` atm.
    float sensor418_rate{}, sensor418_time{}, sensor418_cylinder{};

    [[nodiscard]] static CabBrakeNumbers brakes_dll();
    [[nodiscard]] static CabBrakeNumbers openrts();
};

// The working cab's devices as its switches stand (the caller reads them
// from the cab's BR_* parameters).
struct CabBrakeControls {
    // The driver's valve's position (its handle, its КПТ, or the train's
    // DLL taking it as IV); none: no valve in this cab.
    std::optional<ValvePosition> valve = ValvePosition::running;
    // The handle rests in its position (not passing through): VЭ and V lower
    // the УР only then.
    bool valve_resting = true;
    // The charging pressure named (BR_ZARPRESS, the reducer's label), atm.
    float charging = 5.0f;
    // The valve off the pipe: the 367 out of its working state, the
    // combined valve in double traction, the separating cock shut. Its УР
    // stays as it was.
    bool cut_off = false;
    // The pipe opened at the engine: the combined valve in emergency, ЭТ, a
    // stop valve, the ЭПК tripped.
    bool emergency = false;
    // The train's DLL keeps the УР where it was this step (var + 32 0x4000).
    bool keep_equalising = false;
    // The independent brake valves (the higher one counts), the 4ВК.
    std::optional<IndependentPosition> independent, independent2;
    std::optional<Vk4Position> vk4;
    bool buffer_kvt = false;          // BR_BUFERKVT
    bool release_train_brake = false; // ОТ (BR_OTID) on
    // The ЭПТ: switched on; its buttons (BR_EPTAPL, BR_EPTREL); the train's
    // DLL's hold and apply (var + 32 0x10, 0x20).
    bool ep_on = false;
    bool ep_apply_button = false, ep_release_button = false;
    bool ep_hold_asked = false, ep_apply_asked = false;
    bool multiple_unit = false; // BR_MODE 1: the П lamp in III .. VI
    bool sensor418 = false;     // the 418 switched on (BR_418PID)
};

// The air as the model has it at the engine.
struct CabBrakeAir {
    float pipe{};
    float main_reservoir{};
    float cylinder{};    // the train brake's
    float independent{}; // the independent brake's
};

// What the devices keep between steps.
struct CabBrakeState {
    ValveState valve;          // the УР (ep_target: the ЭПТ's level for RTS's cars)
    float independent{};       // the independent brake's target
    float cylinder_needle{}, pipe_needle{};
    float sensor418_time{};
    bool sensor418 = false;
    float last_pipe = -1.0f;
    bool started = false;
};

struct CabBrakeOutput {
    // The УР the pipe follows (< 0: the valve is off the pipe) and the УР
    // it keeps all the same (its gauge, F5).
    float equalising = -1.0f;
    float equalising_kept{};
    bool emergency = false; // the pipe opened at the engine
    bool feed = true;       // the relay may feed the pipe (not in III)
    // The ЭПТ: its level (>= 0 while on) and its command to the cars' ЭВР;
    // BrakeSystemsEngaged.
    float ep_value = -1.0f;
    EpCommand ep_command = EpCommand::target;
    std::uint16_t systems = 1;
    float independent_brake{};        // the independent brake's target, atm
    bool release_train_brake = false; // the engine's own train brake let off
    // The gauges (atm) and lamps.
    float gauge_cylinder{}, gauge_pipe{}, gauge_main_reservoir{}, gauge_equalising{};
    bool lamp_ep_on = false, lamp_ep_hold = false, lamp_ep_brake = false;
    bool lamp_pipe = false; // the 418
    float ep_volts{}, ep_amps{};
};

// One step of `dt` s. Only the working cab's devices count: the caller
// steps the cab whose 367 stands working.
CabBrakeOutput step_cab_brakes(CabBrakeState& state, const CabBrakeControls& controls, const CabBrakeAir& air,
                               const CabBrakeNumbers& numbers, float dt);

// A handle's state as the cab's control has it, in the order with I first
// (BR_FLIPCABS 1, the ЭД9М's); BR_FLIPCABS 0 (the ЭП20's 130 and 224)
// numbers them from the other end. `last`: the control's last state.
[[nodiscard]] constexpr int direct_state(const int state, const int last, const bool flipped) noexcept {
    return flipped ? state : last - state;
}
// The 395's (130's) position for its state 0..6; the 254's (224's) 0..5;
// the 4ВК's 0..2. Out of range: none.
[[nodiscard]] std::optional<ValvePosition> valve_position_of(int state, bool flipped);
[[nodiscard]] std::optional<IndependentPosition> independent_position_of(int state, bool flipped);
[[nodiscard]] std::optional<Vk4Position> vk4_position_of(int state);

} // namespace openrts::simulation
