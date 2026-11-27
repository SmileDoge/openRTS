#pragma once

#include <openrts/simulation/driver_valve.hpp>

namespace openrts::simulation {

// The 395's own air, as the RRS `krm395` model (maisvendoo/RRS, develop,
// GPL-2.0) describes it: the equalising reservoir (УР, `Ver` = 0.02 m³) and
// the small brake-pipe chamber at the valve (`Vbp` = 1e-4 m³). Pressures are
// atm; the volumes are m³; the K coefficients keep RRS's m³/s dimension, so
// the equations are the same numbers in atm as in RRS's MPa (only the
// pressure scale changes, not the physics).
//
// The crane works the equalising reservoir and gives the pressure its local
// brake-pipe chamber asks for. openRTS's realistic air keeps the train's pipe
// itself, so this class is not a second pipe model: `boundary_pipe()` is the
// pressure the engine's pipe node follows for the coming step (RRS's
// `getBrakePipeInitPressure()`); `brake_pipe()` is the chamber's state after it.
// The 1e-4 m³ chamber is stiff (K2/Vbp ~ 3000 1/s), hence the sub-stepped RK4.
struct BrakeCrane395Config {
    float eq_reservoir_volume = 0.02f; // Ver, m³
    float brake_pipe_volume = 1e-4f;   // Vbp, m³

    float k1 = 0.95e-3f; // I/II: the УР from the feed line, m³/s
    float k2 = 0.3f;     // II: the pipe from the feed line through the relay
    float k4 = 2.5e-4f;  // the pipe vented by the equalising piston
    float k5 = 0.1e-3f;  // VЭ: the УР vents
    float k6 = 1.0e-3f;  // V: the УР vents
    float k7 = 0.1e-3f;  // VI: the pipe vents at the valve
    float k8 = 0.08f;    // I: the wide channel from the feed line to the pipe
    float k9 = 1.5e-3f;  // VI: the УР vents

    float k_stab = 0.0368e-4f; // II: the stabiliser takes the overcharge out
    float k1_stab = 100.0f;    // its cut() argument (k1 * УР)
    float k2_relay = 1.3f;     // the relay's opening by the pipe below the УР
    float k3_relay = 1.0f;     // the equalising piston's vent opening
    float k4_relay = 10.0f;    // its extra opening with the pressure difference
    float a1 = 1.0f;           // the equalising piston's area factor

    // RK4's internal step. RRS integrates the same chamber at its solver's
    // step; 0.5 ms keeps K2/Vbp inside RK4's stability region.
    float max_substep = 0.0005f;
};

class BrakeCrane395 {
public:
    explicit BrakeCrane395(BrakeCrane395Config config = {});

    // Both pressures start at the pipe's (RRS `init(pTM, pFL)` sets Y[0] and
    // Y[1] to the pipe). openRTS's УР gauge is already at its charging value,
    // so callers pass it separately when they have one.
    void reset(float brake_pipe, float equalising);
    void set_state(float brake_pipe, float equalising);

    void set_charge_pressure(float atm) noexcept { charge_pressure_ = atm; }
    void set_feed_line(float atm) noexcept { feed_line_ = atm; }

    // One driver's-valve step. Internally RK4 with `max_substep`.
    void step(ValvePosition position, float dt);

    [[nodiscard]] float brake_pipe() const noexcept { return brake_pipe_; }
    // The pressure the valve gives the pipe for the coming step, taken after
    // preStep and before RK4 (RRS's getBrakePipeInitPressure()).
    [[nodiscard]] float boundary_pipe() const noexcept { return boundary_pipe_; }
    [[nodiscard]] float equalising() const noexcept { return equalising_; }
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }
    [[nodiscard]] ValvePosition position() const noexcept { return position_; }
    [[nodiscard]] bool emergency() const noexcept { return position_ == ValvePosition::emergency; }
    [[nodiscard]] bool feed() const noexcept { return position_ != ValvePosition::lap; }
    [[nodiscard]] bool direct() const noexcept { return position_ == ValvePosition::charge; }
    [[nodiscard]] const BrakeCrane395Config& config() const noexcept { return config_; }

private:
    struct Derivative {
        float pipe;
        float equalising;
    };

    [[nodiscard]] Derivative derivative(ValvePosition position, float pipe, float equalising) const;
    void pre_step(ValvePosition position);

    BrakeCrane395Config config_;
    float brake_pipe_ = 5.0f;
    float boundary_pipe_ = 5.0f;
    float equalising_ = 5.0f;
    float charge_pressure_ = 5.0f;
    float feed_line_ = 0.0f;
    ValvePosition position_ = ValvePosition::running;
    bool initialized_ = false;
};

} // namespace openrts::simulation
