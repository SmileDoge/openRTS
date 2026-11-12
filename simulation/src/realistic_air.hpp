#pragma once

// The "realistic air" brake model (openRTS option, never with Legacy on;
// docs/train.md "Proposal: realistic pneumatics"): the brake pipe as a flow
// between per-car volumes with its own sub-steps, a distributor per car
// (ВР 483 freight, ВР 292 passenger, ЭВР 305 with a 292 behind it), the
// driver's valve 395 feeding the pipe through its relay from the main
// reservoir, and emergency accelerators that carry the emergency wave at
// 250-300 m/s. Internal to openrts_simulation.

#include <cstdint>
#include <memory>
#include <vector>

#include <openrts/simulation/distributor.hpp>
#include <openrts/simulation/train.hpp>

namespace openrts::simulation::detail {

struct AirCar {
    DistributorKind type = DistributorKind::vr292;
    bool braking = false;     // an application is in progress (quick service done)
    bool emergency = false;   // the accelerator fired
    float reference{};        // 483: control chamber РК; 292: the ЗР when the application began
    float last_pipe{};        // pipe at the last sub-step (rate detection)
    float wave = -1.0f;       // s until the emergency front reaches this car (< 0: not coming)
    // ORTSBrakeDistributor: the car's own distributor in Luau (null: the
    // built-in one; also after a script error, reported once).
    std::shared_ptr<ScriptDistributor> script;
};

// What the model needs to know about car i's neighbours and engines.
struct AirInputs {
    const std::vector<EngineDemands>* demands = nullptr;
    const std::vector<bool>* has_controller = nullptr;
    // Car i and i + 1 are joined by connected hoses and open cocks.
    std::vector<bool> joined;
};

class RealisticAir {
public:
    void reset(const std::vector<CarState>& cars);
    // One train step of `dt` seconds over every car's pipe, reservoirs and cylinder.
    void step(std::vector<CarState>& cars, const AirInputs& inputs, float dt);
    [[nodiscard]] const std::vector<AirCar>& state() const noexcept { return air_; }

private:
    std::vector<AirCar> air_;
};

} // namespace openrts::simulation::detail
