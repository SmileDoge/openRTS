#pragma once

// Native openRTS locomotive plugins (include/openrts/plugin/train_plugin.h):
// a 64-bit DLL in the game process. NativeLocomotive is the simulation's
// controller for one locomotive running such a plugin; CabModel is the cab
// state it works on (the same .sd model the legacy bridge mirrors).

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <openrts/formats/cab_description.hpp>
#include <openrts/formats/rolling_stock.hpp>
#include <openrts/plugin/train_plugin.h>
#include <openrts/simulation/train.hpp>

namespace openrts::trainplugin {

// Cab switches and indicators by their .sd names, with RTS's movement: a
// switch asked to a new state gets there after its AnimTime, then counts as
// changed (Switched).
class CabModel {
public:
    explicit CabModel(const formats::CabDescription* description);

    struct Control {
        std::string name;
        std::uint32_t states = 2;
        float anim_time{};
        float state{};      // where it is
        float target{};     // where it goes
        float frame{};      // animation position
        bool key = false;
    };
    struct Indicator {
        std::string name;
        float value{};
        std::uint32_t state{};
    };

    [[nodiscard]] OrtsHandle find_control(std::string_view name) const;
    [[nodiscard]] OrtsHandle find_indicator(std::string_view name) const;
    [[nodiscard]] std::vector<Control>& controls() noexcept { return controls_; }
    [[nodiscard]] const std::vector<Control>& controls() const noexcept { return controls_; }
    [[nodiscard]] std::vector<Indicator>& indicators() noexcept { return indicators_; }
    [[nodiscard]] const std::vector<Indicator>& indicators() const noexcept { return indicators_; }
    // Moves the controls; returns those that arrived (handle, previous state).
    std::vector<std::pair<OrtsHandle, float>> step(float dt);

private:
    std::vector<Control> controls_;
    std::vector<Indicator> indicators_;
};

class NativeLocomotive final : public simulation::LocomotiveController {
public:
    // Loads `dll` and creates the plugin for `engine`; nullptr with `error`.
    [[nodiscard]] static std::unique_ptr<NativeLocomotive> load(const std::filesystem::path& dll,
                                                                const formats::WagonDefinition& engine,
                                                                const formats::CabDescription* cab, std::string* error);
    ~NativeLocomotive() override;

    void step(double dt, const simulation::EngineInputs& inputs, simulation::EngineDemands& demands) override;
    // The driver moves control `handle` to `state`: the plugin may refuse.
    bool operate(OrtsHandle handle, float state);
    void set_player(bool player) noexcept { player_ = player; }

    [[nodiscard]] CabModel& cab() noexcept { return cab_; }
    [[nodiscard]] const OrtsTrainOutputs& outputs() const noexcept { return outputs_; }
    [[nodiscard]] std::vector<std::uint32_t> take_sounds(bool cab);
    [[nodiscard]] std::vector<std::string> take_messages();
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const formats::WagonDefinition* definition() const noexcept { return engine_; }

private:
    explicit NativeLocomotive(const formats::CabDescription* cab) : cab_(cab) {}

    void* module_ = nullptr;
    const OrtsTrainPlugin* plugin_ = nullptr;
    void* context_ = nullptr;
    OrtsHost host_{};
    CabModel cab_;
    const formats::WagonDefinition* engine_ = nullptr;
    OrtsTrainOutputs outputs_{};
    double time_{};
    bool player_ = true;
    std::vector<std::uint32_t> sounds_ext_, sounds_cab_;
    std::vector<std::string> messages_;
    std::string name_;

    friend struct NativeHostCalls;
};

} // namespace openrts::trainplugin
