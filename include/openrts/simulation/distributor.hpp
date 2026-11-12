#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <openrts/formats/rolling_stock.hpp>

// A car's brake distributor (ВР) in openRTS's realistic air model
// (docs/train.md): the built-in ВР 483, ВР 292 and ЭВР 305, or one written
// in Luau (ORTSBrakeDistributor, docs/scripting.md). The model steps it in
// sub-steps of a couple of milliseconds; the pipe's flow between the cars,
// the driver's valve and the emergency wave stay the model's.
namespace openrts::simulation {

enum class DistributorKind : std::uint8_t { vr483, vr292, evr305 };

// The switches a hand sets at the car (the car menu at a bogie): the 483's
// load mode and flat / mountain, the 292's plug К / Д / УВ (the 305's
// pneumatic part is a 292). docs/train.md, "Distributor switches".
enum class DistributorLoad : std::uint8_t { by_mass, empty, medium, loaded };
enum class DistributorPlug : std::uint8_t {
    short_train,   // К: up to about 15 cars; fills and releases fastest
    long_train,    // Д: fills, releases and fills in emergency about twice as slow
    no_accelerator // УВ: as Д, and the car's emergency accelerator does not vent the pipe
};
struct DistributorModes {
    DistributorLoad load = DistributorLoad::by_mass; // by_mass: by the car's mass, until a hand sets it
    bool mountain = false;
    DistributorPlug plug = DistributorPlug::short_train;
};

// The load mode a car's 483 is on: `load`, or by `mass` (kg; over 60 t
// loaded, over 35 t medium, else empty).
[[nodiscard]] DistributorLoad effective_load(DistributorLoad load, float mass) noexcept;
// ТЦ of a full application: a 483 by its load mode (1.5 / 3.0 / 3.8 atm),
// 3.8 for the others.
[[nodiscard]] float distributor_full_cylinder(DistributorKind kind, DistributorLoad load) noexcept;
// The distributor a car gets: EP-equipped cars 305 (with a 292 behind it),
// `Type ( Freight )` 483, everything else 292.
[[nodiscard]] DistributorKind distributor_kind(const formats::WagonDefinition& definition) noexcept;

// The distributor's air and its state at one car, atm.
struct DistributorState {
    float pipe{};       // ТМ at the car: a distributor takes air from it (charging, quick service)
    float auxiliary{};  // ЗР
    float cylinder{};   // ТЦ
    float reference{};  // the level remembered: 483 its РК, 292 the ЗР when the application began
    bool braking = false;   // an application is on
    bool emergency = false; // the emergency front has reached the car
};

// What the car gives its distributor.
struct DistributorCar {
    DistributorKind kind = DistributorKind::vr292;
    float full_cylinder = 3.8f; // ТЦ of a full application, atm (483: by the car's load)
    float ratio = 3.0f;         // TripleValveRatio
    float pipe_volume = 0.016f;      // m³ of pipe at the car
    float auxiliary_volume = 0.055f; // m³
    float cylinder_volume = 0.008f;  // m³
    float mass{};               // kg
    bool ep = false;            // the ЭПТ wired (305 follows its target)
    // The switches (DistributorModes), the load resolved.
    DistributorLoad load = DistributorLoad::loaded;
    bool mountain = false;
    DistributorPlug plug = DistributorPlug::short_train;
};

// ЗР volumes of the built-in distributors, m³.
[[nodiscard]] float distributor_auxiliary_volume(DistributorKind kind) noexcept;
inline constexpr float distributor_cylinder_volume = 0.008f;

// One sub-step of `h` s of a built-in distributor; the pipe changing at
// `rate` atm/s, `ep` the ЭПТ target (atm, < 0: none).
void step_distributor(DistributorState& state, const DistributorCar& car, float rate, float ep, float h);

// A distributor in Luau: the script's `step(c, dt)` with the car's table
// (docs/scripting.md). One per car: its `c.state` table is the car's own.
class ScriptDistributor {
public:
    ~ScriptDistributor();
    // nullptr and the message when the script cannot be compiled, run, or
    // has no `step` function.
    [[nodiscard]] static std::unique_ptr<ScriptDistributor> load(const std::string& source, const std::string& name,
                                                                 std::string& error);
    // false and the message on a script error (the car's built-in one takes over).
    bool step(DistributorState& state, const DistributorCar& car, float rate, float ep, float h, std::string& error);

private:
    ScriptDistributor();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The test stand (openrts-brake-stand): the pipe held to a program, as a
// stand's driver's valve and supply would, and the distributor's answer.
// The program, from empty: charge to 5.0 (to 300 s); a service step of 0.5
// (to 360 s); release to 5.0 (to 450 s); full service to 3.5 (to 510 s);
// release with an overcharge to 5.6 (to 570 s), then its slow removal at
// 0.15 atm a minute down to 5.0, as a stabiliser does (to 840 s); emergency
// (to 900 s).
struct StandSample {
    float time{}, pipe{}, auxiliary{}, cylinder{}, reference{};
    bool braking = false;
};
struct StandReport {
    std::vector<StandSample> samples; // one a second
    float charged = -1.0f;      // s until the ЗР is within 0.1 of 5.0 (-1: never)
    float step_cylinder{};      // ТЦ after the 0.5 step, at its end
    float step_fill = -1.0f;    // s from the step until 95 % of that
    float release = -1.0f;      // s from the release until ТЦ < 0.4
    float full_cylinder{};      // ТЦ after the full service
    float creep{};              // the most ТЦ rises while the overcharge is removed (0: stable)
    float emergency_fill = -1.0f; // s until 95 % of the full ТЦ in emergency
    float emergency_cylinder{}; // ТЦ at the end
    std::string error;          // a script's error (the run stops there)
};
// `script` null: the built-in distributor of `car.kind`.
[[nodiscard]] StandReport run_stand(const DistributorCar& car, ScriptDistributor* script);

} // namespace openrts::simulation
