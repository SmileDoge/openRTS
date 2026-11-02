#pragma once

// A consist whose locomotives run legacy RTS DLLs (docs/train-dll.md): owns
// the x86 host for the consist, builds its mirrors from the .eng / .wag /
// cab .sd files, and per simulation step sends the cars' state and collects
// what the DLLs did. Locomotive controllers made by controller() turn those
// outputs into the simulation's EngineDemands.
//
// Use: create() -> enter(player car) -> every step: step(train) before
// train.step(). The consist must outlive the train's controllers' use of it
// (they turn inert when it is gone).

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <openrts/core/vfs.hpp>
#include <openrts/dllhost/protocol.hpp>
#include <openrts/formats/cab_description.hpp>
#include <openrts/formats/rolling_stock.hpp>
#include <openrts/simulation/train.hpp>
#include <openrts/trainplugin/legacy_host.hpp>

namespace openrts::trainplugin {

struct LegacyCar {
    const formats::WagonDefinition* definition = nullptr;
    core::VirtualPath file; // its .eng / .wag (relative DLL and cab paths start here)
    std::uint32_t uid{};
};

// What the world tells the DLLs each step.
struct LegacyWorld {
    double day_time = 12.0 * 3600.0; // s since midnight
    bool night = false;
    float air_temperature = 20.0f;   // °C
    // V and Hz at the pantographs for cars whose CarState carries no line
    // (the route's electrification not modelled).
    float line_voltage = 3000.0f;
    float line_frequency = 0.0f;
    dllhost::SpeedLimitInput route, signal, event; // m/s, m
    std::vector<dllhost::SignalInput> signals_ahead;
};

class LegacyConsist {
public:
    struct Outputs {
        std::vector<dllhost::CarOutput> cars;
        bool valid = false;
    };

    // nullptr (with `error`) when no car has a DLL or the host cannot start.
    [[nodiscard]] static std::unique_ptr<LegacyConsist> create(const core::Vfs& vfs,
                                                               const std::filesystem::path& cache_root,
                                                               std::vector<LegacyCar> cars,
                                                               const std::filesystem::path& rts_root,
                                                               std::string* error,
                                                               const std::filesystem::path& host_executable = {});
    ~LegacyConsist();

    // The player takes locomotive `car` (Init of it and its multiple-unit group).
    // `state`: Init's start state as an activity's TrainState (0 cold, 1 on,
    // 2 / 3 running), -1 from the train (`running`).
    bool enter(std::size_t car, const simulation::Train& train, bool running = true, int state = -1);
    // One round trip: the train's state in, the DLLs' outputs out.
    bool step(double dt, const simulation::Train& train, const LegacyWorld& world);
    // The player moves a cab switch of `car` to `state`: CanSwitch, then the
    // host moves it (Switched fires when it gets there). False if refused.
    bool operate(std::size_t car, std::uint32_t id, std::uint32_t state);
    // Drags switch `id` (DragSwitchRequest): `delta` in states, or lets it go.
    bool drag(std::size_t car, std::uint32_t id, float delta, bool release);

    [[nodiscard]] const dllhost::CarOutput* output(std::size_t car) const;
    [[nodiscard]] const formats::CabDescription* cab(std::size_t car) const;
    [[nodiscard]] bool has_dll(std::size_t car) const;
    [[nodiscard]] const std::vector<dllhost::ModuleReport>& modules() const noexcept { return modules_; }
    [[nodiscard]] std::size_t player() const noexcept { return player_; }
    [[nodiscard]] bool alive() const;
    [[nodiscard]] std::string error() const;
    [[nodiscard]] std::string take_messages();
    // A controller for `car` reading this consist's outputs.
    [[nodiscard]] std::unique_ptr<simulation::LocomotiveController> controller(std::size_t car) const;

private:
    LegacyConsist() = default;
    std::vector<dllhost::CarInput> inputs(const simulation::Train& train, const LegacyWorld* world) const;

    std::unique_ptr<LegacyHost> host_;
    std::vector<LegacyCar> cars_;
    std::vector<formats::CabDescription> cabs_;
    std::vector<bool> dll_;
    std::vector<dllhost::ModuleReport> modules_;
    std::shared_ptr<Outputs> outputs_ = std::make_shared<Outputs>();
    std::size_t player_ = SIZE_MAX;
    std::string messages_;
};

// Engine outputs of a legacy DLL as the simulation's demands.
void apply_engine_output(const dllhost::EngineOutput& out, simulation::EngineDemands& demands);

} // namespace openrts::trainplugin
