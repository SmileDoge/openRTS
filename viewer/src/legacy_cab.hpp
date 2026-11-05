#pragma once

// The player's train when its locomotives run legacy RTS DLLs
// (docs/train-dll.md): owns the LegacyConsist, puts its controllers on the
// DLL engines, steps it before the simulation, routes the cab keys of the
// .sd (AssignKey) to CanSwitch, and shows the cab state in a window until
// the 3-D cab draws it.

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <map>
#include <optional>
#include <vector>

#include <openrts/core/vfs.hpp>
#include <openrts/simulation/train.hpp>
#include <openrts/trainplugin/legacy_consist.hpp>

#include "viewer_scene.hpp"

namespace openrts::viewer {

struct LegacyCarRef {
    const formats::WagonDefinition* definition = nullptr;
    std::string folder; // TRAINSET subfolder
    std::uint32_t uid{};
    std::uint32_t key{}; // unique in the game: follows the car through couplings
};

class LegacyCab {
public:
    // nullptr when no car has a DLL (`status` empty) or the DLLs cannot run
    // (`status` says why).
    // `start_state`: the DLLs' Init state as an activity's TrainState (0 cold,
    // 1 on, 3 running), -1 running (the default).
    // `route`: for the library sounds RTS finds in the route's SOUND first (ROUTEFORWAGON).
    [[nodiscard]] static std::unique_ptr<LegacyCab> attach(const core::Vfs& vfs, const std::vector<LegacyCarRef>& cars,
                                                           simulation::Train& train, std::string& status, int start_state = -1,
                                                           const core::VirtualPath* route = nullptr);

    // Before train.step(): one round trip with the host. Sound triggers per
    // car are added to `triggers` (exterior SMS), messages to the log.
    // `aux_triggers[car][library]`: what each AuxLibrary posted to its own
    // SMS (AuxLibrarySound).
    // Several trains: the player's first, then the parts cut off from it
    // (as the last reconsist ordered them); per train what its cars' DLLs
    // posted.
    struct Triggers {
        std::vector<std::vector<std::uint32_t>> exterior, cab;
        std::vector<std::vector<std::vector<std::uint32_t>>> aux;
    };
    void step(const std::vector<const simulation::Train*>& trains, std::vector<Triggers>& posted);
    void step(const simulation::Train& train, std::vector<std::vector<std::uint32_t>>& triggers,
              std::vector<std::vector<std::uint32_t>>& cab_triggers,
              std::vector<std::vector<std::vector<std::uint32_t>>>& aux_triggers);
    // SMS Var1..Var8 the DLL set for car `car`'s cab sound, if any.
    [[nodiscard]] const std::array<float, 8>* cab_sound_variables(std::size_t car) const;
    // Var1..Var8 of AuxLibrary `library`'s own SMS on car `car`, if any.
    [[nodiscard]] const std::array<float, 8>* aux_sound_variables(std::size_t car, std::size_t library) const;
    // Cab key presses and releases (when keyboard() is on).
    void key(const KeyEvent& event);
    // The keys no longer reach the cab (the engineer left the car): the
    // buttons a key holds (NONFIXED) go back to their FixedState, as if let go.
    void release_keys();
    // Switch `id` of the player's cab to `state` (through the DLL's
    // CanSwitch / Switched, as a key would; --capture-switch).
    void set_switch(std::uint32_t id, std::uint32_t state) { operate(id, state); }
    // The mouse on switch `id` of the player's cab: a click (0, RTS's
    // ping-pong step) or the wheel (+1 / -1). A click on a NONFIXED switch
    // (a button) presses it: one step up, held until release() (RTS 0x4121BC).
    // `car`: whose interior (a salon's stop valve); SIZE_MAX: the player's cab.
    void click(std::uint32_t id, int direction, bool shift, std::size_t car = SIZE_MAX);
    // Whether car `car` has an interior the host works (its switches answer clicks).
    [[nodiscard]] bool has_cab(std::size_t car) const;
    // The left button let go: a held button returns to its FixedState.
    void release();
    // A press on a CONTROLLERX / CONTROLLERY switch takes hold of it instead
    // of clicking it (RTS 0x4121BC; false: not such a switch).
    // `frames_per_state`: CabView::switch_frames_per_state (0: no animated node).
    bool grab(std::uint32_t id, float frames_per_state, std::size_t car = SIZE_MAX);
    [[nodiscard]] bool dragging() const noexcept { return drag_.has_value(); }
    // The cursor moved `dx`, `dy` pixels (y down) with the switch in hand:
    // with SwitchPrecision its frame follows (PrecX * dx + PrecY * dy frames,
    // 0x437E64); without, one step in the move's direction (right or up: +1)
    // and the hold ends; with `shift` a KEY switch's key comes out (RTS
    // 0x437AA4: at InitialState + 1).
    void drag(float dx, float dy, bool shift = false);
    // Let go (0x527190): a controller takes the nearest position, a PRECISE
    // switch stays; a NONFIXED one returns to its FixedState.
    void drag_end();
    // `extra`: more of the window, drawn under the cab keys checkbox.
    void draw_window(const std::function<void()>& extra = {});

    // Cars coupled on or cut off: the host's trains as they are now (the
    // player's first, then the parts cut off from it, whose DLLs go on
    // running). The DLLs keep their state and regroup
    // (LegacyConsist::reconsist); the player's train's DLL engines get
    // their controllers (the other trains' are the caller's to set). When
    // the player's engine is not in the first train, player() is none
    // until enter(). False: the host failed (attach again).
    bool reconsist(const core::Vfs& vfs, const std::vector<std::vector<LegacyCarRef>>& trains,
                   const std::vector<simulation::Train*>& sims);
    [[nodiscard]] std::size_t trains() const { return consist_ ? consist_->trains() : 0; }
    [[nodiscard]] std::size_t train_size(std::size_t train) const { return consist_ ? consist_->train_size(train) : 0; }
    // Train `train`'s car `car` (0: the player's train).
    [[nodiscard]] const dllhost::CarOutput* output(std::size_t train, std::size_t car) const {
        return consist_ ? consist_->output(train, car) : nullptr;
    }
    [[nodiscard]] bool has_dll(std::size_t train, std::size_t car) const { return consist_ && consist_->has_dll(train, car); }
    [[nodiscard]] std::unique_ptr<simulation::LocomotiveController> controller(std::size_t train, std::size_t car) const {
        return consist_ ? consist_->controller(train, car) : nullptr;
    }
    [[nodiscard]] const std::array<float, 8>* sound_variables(std::size_t train, std::size_t car) const;
    [[nodiscard]] const std::array<float, 8>* aux_sound_variables(std::size_t train, std::size_t car, std::size_t library) const;
    // The player takes locomotive `car` (the engineer walked into its cab):
    // the host enters it (Init, ChangeLoco, the multiple-unit groups) and its
    // cab's keys are bound. False when it has no DLL or the host refuses.
    bool enter(std::size_t car, const simulation::Train& train);
    [[nodiscard]] bool keyboard() const noexcept { return keyboard_; }
    void set_keyboard(bool on) noexcept { keyboard_ = on; }
    [[nodiscard]] bool alive() const { return consist_ && consist_->alive(); }
    [[nodiscard]] std::size_t player() const { return consist_ ? consist_->player() : SIZE_MAX; }
    // The farthest RTSGetTrackItems asked for in the last step, m.
    [[nodiscard]] float items_wanted() const { return consist_ ? consist_->items_wanted() : 0.0f; }
    [[nodiscard]] bool has_dll(std::size_t car) const { return consist_ && consist_->has_dll(car); }
    [[nodiscard]] const dllhost::CarOutput* output(std::size_t car) const { return consist_ ? consist_->output(car) : nullptr; }
    // SMS Var1..Var8 the DLL set for car `car` (exterior), if any.
    [[nodiscard]] const std::array<float, 8>* sound_variables(std::size_t car) const;

private:
    struct Binding {
        std::uint32_t id{};
        int key{};          // GLFW key code
        bool shift = false, ctrl = false;
        int direction{};    // +1 / -1 (two keys), 0 (one key: as a click)
    };
    // RTS's switch activation (0x437AA4): the next state of switch `id` for
    // a click (0) or a step up / down (+1 / -1).
    [[nodiscard]] std::uint32_t next_state(std::uint32_t id, int direction, bool shift, std::size_t car = SIZE_MAX);
    void operate(std::uint32_t id, std::uint32_t state, std::size_t car = SIZE_MAX);
    void bind_keys(std::size_t car);
    [[nodiscard]] std::size_t car_or_player(std::size_t car) const;
    void log(std::string line);

    std::unique_ptr<trainplugin::LegacyConsist> consist_;
    std::vector<Binding> bindings_;
    std::map<std::uint64_t, std::uint8_t> going_down_; // ping-pong direction per car << 32 | switch id
    std::optional<std::pair<std::size_t, std::uint32_t>> held_; // the button the mouse holds down: car, id
    struct Drag {
        std::size_t car{};
        std::uint32_t id{};
        float frames_per_state{}; // > 0: dragged smoothly (SwitchPrecision)
        float precision[2]{};
        std::uint32_t flags{};
    };
    std::optional<Drag> drag_; // the switch the mouse holds
    std::deque<std::string> log_;
public:
    // What the DLLs see of the world (signals, speed limits), set before each step.
    trainplugin::LegacyWorld& world() noexcept { return world_; }
    [[nodiscard]] const trainplugin::LegacyWorld& world() const noexcept { return world_; }

private:
    trainplugin::LegacyWorld world_;
    bool keyboard_ = false;
    char filter_[64]{};
};

// GLFW key code (and modifiers) of an RTS AssignKey text ("A", "Shift+;",
// "Ctrl+5", "Space", "<"); 0 when unknown.
struct CabKey {
    int key = 0;
    bool shift = false, ctrl = false;
};
[[nodiscard]] CabKey parse_cab_key(std::string_view text);

} // namespace openrts::viewer
