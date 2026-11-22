#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

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
    // `pipe_needle_fall` + `pipe_needle_per_wagon` × the wagons (up to
    // 100) atm/s, in II `pipe_needle_fall_running` + `..._per_wagon_running`
    // × them, in III .. VI `pipe_needle_fall_braking` (Brakes.dll's: its k
    // is RTSGetInteger 2, the train's wagons). 0: the pressure as it is.
    float cylinder_needle_near{}, cylinder_needle_far{}, cylinder_needle_band{};
    float pipe_needle_fall{}, pipe_needle_per_wagon{};
    float pipe_needle_fall_running{}, pipe_needle_per_wagon_running{};
    float pipe_needle_fall_braking{};
    // The 418 (the pipe's break, lamp ТМ). With the distributor's
    // channels (the realistic air) as the real one (Афонин §4.7, Асадченко
    // §4.3.5; testers 2026-11-20), on a locomotive with the ВР 483 only (a
    // 292 has none): its ДДР closes when that 483 starts an application
    // with its additional discharge; with its ДТЦ still closed (the
    // distributor's ТЦ channel under `sensor418_dtc` atm) the relay picks
    // up and holds itself: the lamp and no traction, until that channel is
    // over `sensor418_dtc` (a service application: a flash; a break: lit
    // while the channel fills, out once it is over). The engine's own
    // cylinder (the 254's release) does not count, nor a slow fall (an
    // overcharge going), nor a release. Without them
    // (Brakes.dll's way): the pipe falling faster than `sensor418_rate`
    // atm/s for longer than `sensor418_time` s without the ЭПТ, out again
    // once the cylinder is over `sensor418_cylinder` atm.
    float sensor418_rate{}, sensor418_time{}, sensor418_cylinder{};
    float sensor418_dtc{};

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
    // The position is the КПТ's (the ЭП2Д's controller moved last): the
    // 395's own air sounds stay silent, as Brakes.dll's (the DLL sounds the
    // КПТ itself: the ЭП2Д's kptR1 / kptR2; testers heard both at I).
    bool by_controller = false;
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
    // The train's wagons, not engines (RTSGetInteger 2): the ТМ needle's fall.
    std::uint32_t wagons{};
    // The air sounds' codes: BR_130MODE (the valve's, the ЭПТ's, the pipe
    // gauge's) and BR_224MODE (the independent brake's) not 0: the 31xx ones.
    bool sounds_130 = false, sounds_224 = false;
    // BR_254ID1 given: the independent brake's sounds by its cylinder and value.
    bool independent_sounds = false;
};

// The air as the model has it at the engine.
struct CabBrakeAir {
    float pipe{};
    float main_reservoir{};
    float cylinder{};    // the train brake's
    float independent{}; // the independent brake's
    // How fast the valve's relay fed the pipe at the engine over the last
    // step, atm/s (Train: CarState::valve_feed); Brakes.dll's air sounds
    // take it for its TrainPipeRate.
    float valve_feed{};
    // The engine's distributor (the realistic air): its ТЦ channel and an
    // application begun with its additional discharge since the last step
    // (the 418's ДТЦ and ДДР); `distributor_known` false: none. With it,
    // `sensor418_fitted`: the engine has the 418 at all (a locomotive's 483,
    // simulation::sensor418_fitted); without, no lamp ТМ from it.
    bool distributor_known = false;
    bool sensor418_fitted = false;
    float distributor_channel{};
    bool distributor_discharge = false;
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
    std::uint32_t sounds{}; // the air sounds playing, a bit each (docs/reverse/brakes-dll.md §10)
};

struct CabBrakeOutput {
    // The УР the pipe follows (< 0: the valve is off the pipe) and the УР
    // it keeps all the same (its gauge, F5).
    float equalising = -1.0f;
    float equalising_kept{};
    bool emergency = false; // the pipe opened at the engine
    bool braking = false;   // the 395 in a braking position (V, VЭ), for the train's DLL
    bool feed = true;       // the relay may feed the pipe (not in III)
    bool direct = false;    // I: the main reservoir straight into the pipe (ValveOutput::direct)
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
    // The air sounds' triggers this step for Brakes.sms (on / off pairs:
    // 124 / 125 I feeding, 126 / 127 II feeding, 128 / 129 V, 130 / 131 the
    // pipe vented, 132 / 133 II at the charge, 134 / 135 the ЭПТ applying,
    // 122 / 123 its release, 136 / 137 the pipe gauge over the pipe, 120 ..
    // 123 the 4ВК), as Brakes.dll posts them.
    std::vector<std::uint16_t> sounds;
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
// the 4ВК's 0..2 (BR_FLIPCABS 1: 2 applies, 0 lets off; 0: the other
// way). Out of range: none.
[[nodiscard]] std::optional<ValvePosition> valve_position_of(int state, bool flipped);
[[nodiscard]] std::optional<IndependentPosition> independent_position_of(int state, bool flipped);
[[nodiscard]] std::optional<Vk4Position> vk4_position_of(int state, bool flipped = true);

} // namespace openrts::simulation
