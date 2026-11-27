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

#include <openrts/simulation/auto_mode.hpp>
#include <openrts/simulation/distributor.hpp>
#include <openrts/simulation/train.hpp>

namespace openrts::simulation::detail {

struct AirCar {
    DistributorKind type = DistributorKind::vr292;
    bool braking = false;     // an application is in progress (quick service done)
    bool emergency = false;   // the accelerator fired
    float reference{};        // 483: control chamber РК; 292: the ЗР when the application began
    float zk{};               // 483: the spool chamber's pressure, atm
    float kdr{};              // 483: the extra chamber's pressure, atm
    float last_pipe{};        // pipe at the last sub-step (rate detection)
    float wave = -1.0f;       // s until the emergency front reaches this car (< 0: not coming)
    // s its emergency accelerator still vents the pipe: it opens when the
    // car goes into emergency and closes after a few seconds; the car stays
    // in emergency (its distributor full on) until the pipe is charged again.
    float accelerator{};
    // ЭВР 305: its РК and the switching valve's side (DistributorState).
    float ep_chamber{};
    bool ep_side = false;
    float pipe_low{}; // 483: the pipe's lowest while braking
    // The 418's channels: an application's one additional discharge, and the
    // ТЦ channel that keeps its target with the cylinder let off outside.
    bool discharge = false;
    float channel{};
    // ORTSBrakeDistributor: the car's own distributor in Luau (null: the
    // built-in one; also after a script error, reported once).
    std::shared_ptr<ScriptDistributor> script;
    // The freight 483's auto mode (АР-265), between the distributor and the
    // real brake cylinder. Uninitialized on the other cars.
    AutoMode265 auto_mode;
};

// What the model needs to know about car i's neighbours and engines.
struct AirInputs {
    const std::vector<EngineDemands>* demands = nullptr;
    const std::vector<bool>* has_controller = nullptr;
    // Car i and i + 1 are joined by connected hoses and open cocks.
    std::vector<bool> joined;
};

// An engine's main reservoir, m³ (AirBrakesMainResVolume: litres above 50,
// else cubic feet as MSTS gives it).
[[nodiscard]] float main_reservoir_volume(const formats::EngineDefinition& engine);

class RealisticAir {
public:
    void reset(const std::vector<CarState>& cars);
    // One train step of `dt` seconds over every car's pipe, reservoirs and cylinder.
    void step(std::vector<CarState>& cars, const AirInputs& inputs, float dt);
    [[nodiscard]] const std::vector<AirCar>& state() const noexcept { return air_; }
    // A saved game: each car's distributor state ("a3.braking" ...).
    void save(StateWriter& out) const;
    void restore(const StateReader& in);

private:
    std::vector<AirCar> air_;
    // The pipe's flow between car i and i + 1, m³·atm/s towards i + 1: its
    // air has inertia, so a change goes down the pipe no faster than sound.
    std::vector<float> link_flow_;
};

} // namespace openrts::simulation::detail
