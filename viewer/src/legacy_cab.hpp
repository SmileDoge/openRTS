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
#include <set>
#include <optional>
#include <vector>

#include <openrts/core/vfs.hpp>
#include <openrts/simulation/train.hpp>
#include <openrts/trainplugin/legacy_consist.hpp>

#include "viewer_scene.hpp"

namespace openrts::formats {
struct CabSwitch;
}

namespace openrts::viewer {

// RTS (docs/reverse/train-dll.md section 11): a key works a switch flagged
// CHECKDISTANCEONPRESS only with the engineer within its Radius of its
// Position. A cab of two ends in one shape (the VL60's: both desks, the
// same AssignKey at each) answers at the desk he is at. The eye is taken,
// in cab coordinates and the .sd's Position as it is, as a click's pick
// does (0x528EA0); no eye (no 3-D cab): the key works it.
[[nodiscard]] bool key_reaches(const formats::CabSwitch& control, const std::optional<glm::vec3>& eye);

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
    // `fresh`: a train just made (loaded, put back): its air as RTS's
    // (empty cars, the pressures Init writes, then every car the player's
    // engine's); else the train keeps its own (the DLLs again after a
    // consist change).
    [[nodiscard]] static std::unique_ptr<LegacyCab> attach(const core::Vfs& vfs, const std::vector<LegacyCarRef>& cars,
                                                           simulation::Train& train, std::string& status, int start_state = -1,
                                                           const core::VirtualPath* route = nullptr, bool fresh = false);

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
    // Cab key presses and releases (when keyboard() is on). `eye`: the
    // engineer's eye in cab coordinates, when he is in the 3-D cab
    // (key_reaches).
    void key(const KeyEvent& event, const std::optional<glm::vec3>& eye);
    // The keys no longer reach the cab (the engineer left the car): the
    // buttons a key holds (NONFIXED) go back to their FixedState, as if let go.
    void release_keys();
    // Each frame: a NONFIXED switch whose key was let go is asked back to its
    // FixedState again until it is there (RTS 0x414340 steps it back once per
    // key delay; the DLL may refuse while another is held: the whistle and
    // the horn pressed together stuck on, testers 2026-11-26).
    // `dt`: the frame's s; a PRECISE switch's key held turns it as a drag
    // (the handbrake's wheel: its State is not what the DLL reads).
    void return_released(float dt = 0.0f);
    // A key of the player's cab bound to switch `id` the way `direction`
    // (+1 up, -1 down; 0 either) is down now (Brakes.dll's springs, as its
    // GetAsyncKeyState on the switch's AssignKey).
    [[nodiscard]] bool key_held(std::uint32_t id, int direction) const;
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
    // The switch the mouse holds, if any.
    [[nodiscard]] std::optional<std::uint32_t> dragged_switch() const noexcept {
        return drag_ ? std::optional<std::uint32_t>{drag_->id} : std::nullopt;
    }
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
    void set_train_types(std::vector<std::uint32_t> types) {
        if (consist_) consist_->set_train_types(std::move(types));
    }
    void set_train_starts(std::vector<std::int16_t> starts) {
        if (consist_) consist_->set_train_starts(std::move(starts));
    }
    // The DLL trains' air under the realistic air model (LegacyConsist::set_air).
    void set_air(const trainplugin::DllAir mode) {
        if (consist_) consist_->set_air(mode);
    }
    [[nodiscard]] trainplugin::DllAir air() const { return consist_ ? consist_->air() : trainplugin::DllAir::rts; }
    // openRTS's cab brake devices (LegacyConsist::set_own_brakes).
    void set_own_brakes(const bool own, const bool books) {
        if (consist_) consist_->set_own_brakes(own, books);
    }
    [[nodiscard]] bool own_brakes() const { return consist_ && consist_->own_brakes(); }
    // openRTS's КЛУБ-У (LegacyConsist::set_own_klub).
    void set_own_klub(const bool own) {
        if (consist_) consist_->set_own_klub(own);
    }
    [[nodiscard]] std::size_t klub_cars() const { return consist_ ? consist_->klub_cars() : 0; }
    [[nodiscard]] std::size_t without_brakes_dll() const { return consist_ ? consist_->without_brakes_dll() : 0; }
    [[nodiscard]] std::size_t take_restored() { return consist_ ? consist_->take_restored() : 0; }
    bool reconsist(const core::Vfs& vfs, const std::vector<std::vector<LegacyCarRef>>& trains,
                   const std::vector<simulation::Train*>& sims);
    [[nodiscard]] std::size_t trains() const { return consist_ ? consist_->trains() : 0; }
    [[nodiscard]] std::size_t train_size(std::size_t train) const { return consist_ ? consist_->train_size(train) : 0; }
    // Train `train`'s car `car` (0: the player's train).
    [[nodiscard]] const dllhost::CarOutput* output(std::size_t train, std::size_t car) const {
        return consist_ ? consist_->output(train, car) : nullptr;
    }
    [[nodiscard]] bool has_dll(std::size_t train, std::size_t car) const { return consist_ && consist_->has_dll(train, car); }
    // The car's Brakes.dll controls as openRTS's valve reads them
    // (LegacyConsist::cab_brakes).
    [[nodiscard]] trainplugin::CabBrakes cab_brakes(std::size_t train, std::size_t car) const {
        return consist_ ? consist_->cab_brakes(train, car) : trainplugin::CabBrakes{};
    }
    [[nodiscard]] std::unique_ptr<simulation::CarModule> controller(std::size_t train, std::size_t car) const {
        return consist_ ? consist_->controller(train, car) : nullptr;
    }
    [[nodiscard]] const std::array<float, 8>* sound_variables(std::size_t train, std::size_t car) const;
    [[nodiscard]] const std::array<float, 8>* aux_sound_variables(std::size_t train, std::size_t car, std::size_t library) const;
    // The player takes locomotive `car` (the engineer walked into its cab):
    // the host enters it (Init, ChangeLoco, the multiple-unit groups) and its
    // cab's keys are bound. False when it has no DLL or the host refuses.
    // `state`: an activity's TrainState for its DLL's Init (0 cold, 1 on), -1 running.
    bool enter(std::size_t car, const simulation::Train& train, int state = -1);
    // The cab keys have the keyboard: chosen (with the free camera, whose
    // WASD they take), or always while the engineer is on foot (he walks
    // with the arrows, as in RTS, where a cab's keys work at once).
    [[nodiscard]] bool keyboard() const noexcept { return keyboard_ || on_foot_; }
    void set_keyboard(bool on) noexcept { keyboard_ = on; }
    // The engineer on foot (in a chair, walking about, outside), each frame.
    void set_on_foot(bool on) noexcept { on_foot_ = on; }
    [[nodiscard]] bool on_foot() const noexcept { return on_foot_; }
    [[nodiscard]] bool alive() const { return consist_ && consist_->alive(); }
    // A saved game: the DLLs' state by car key (LegacyConsist::save_state /
    // restore_state). False: no DLLs running, or the host failed.
    [[nodiscard]] bool save_state(std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>>& states) {
        return consist_ && consist_->save_state(states);
    }
    bool restore_state(const std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>>& states) {
        return consist_ && consist_->restore_state(states);
    }

    // A helper's hands on the engineer's cab (docs/net.md, "Players and
    // helpers"): no DLLs here; the cab is what the engineer's DLLs showed
    // last (`output`), its .sd this side's (`cab`), and what the hands do
    // (a switch to a state, a drag) goes to the engineer. The clicks, the
    // drags and the keys work as on a cab of one's own.
    struct Remote {
        std::function<const dllhost::CarOutput*(std::size_t car)> output;
        std::function<const formats::CabDescription*(std::size_t car)> cab;
        std::function<void(std::size_t car, std::uint32_t id, std::uint32_t state)> operate;
        std::function<void(std::size_t car, std::uint32_t id, float delta, bool release)> drag;
    };
    [[nodiscard]] static std::unique_ptr<LegacyCab> remote(Remote target, std::size_t player);
    // The cab the helper is at (its keys bound).
    void set_remote_player(std::size_t car);
    // The engineer's side: a helper's hand, done here as his own would be.
    bool operate_switch(std::size_t car, std::uint32_t id, std::uint32_t state);
    void drag_switch(std::size_t car, std::uint32_t id, float delta, bool release);
    [[nodiscard]] std::size_t player() const { return consist_ ? consist_->player() : remote_ ? remote_player_ : SIZE_MAX; }
    // The farthest RTSGetTrackItems asked for in the last step, m.
    [[nodiscard]] float items_wanted() const { return consist_ ? consist_->items_wanted() : 0.0f; }
    // What the DLLs asked of the tape in the last step, once.
    [[nodiscard]] trainplugin::LegacyConsist::TapeRequest take_tape_request() {
        return consist_ ? consist_->take_tape_request() : trainplugin::LegacyConsist::TapeRequest{};
    }
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
    // The cab's state and .sd, and the hand's way to the DLLs: the consist
    // here, or the engineer's for a helper (remote_).
    [[nodiscard]] const dllhost::CarOutput* out_of(std::size_t car) const;
    [[nodiscard]] const formats::CabDescription* cab_of(std::size_t car) const;
    void apply_operate(std::size_t car, std::uint32_t id, std::uint32_t state);
    void apply_drag(std::size_t car, std::uint32_t id, float delta, bool release);
    void take_log();
    [[nodiscard]] bool working() const noexcept { return consist_ != nullptr || remote_.has_value(); }
    std::optional<Remote> remote_;
    std::size_t remote_player_ = SIZE_MAX;
    [[nodiscard]] std::size_t car_or_player(std::size_t car) const;
    void log(std::string line);

    std::unique_ptr<trainplugin::LegacyConsist> consist_;
    std::vector<Binding> bindings_;
    std::set<int> keys_down_; // the GLFW keys down, as the cab's key events left them
    std::set<std::uint32_t> returning_; // NONFIXED switches let go, not yet back (return_released)
    // The NONFIXED switches each key's press moved: its release returns these
    // alone (testers 2026-11-26: letting go of the whistle's Space returned
    // the ЭД4М's brake switch on Shift+Space too, its brakes let off).
    std::map<int, std::set<std::uint32_t>> pressed_by_key_;
    // When each NONFIXED switch was pressed (clock_): one let go sooner than
    // min_hold after stays pressed till then, so the DLL's step sees it (a
    // quick tap of the РБ fell between two steps; testers 2026-11-26).
    std::map<std::uint32_t, double> pressed_at_;
    double clock_ = 0.0;
    static constexpr double min_hold = 0.2;
    std::map<std::uint32_t, int> precise_held_; // PRECISE switches turned by a key held: the way
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
    bool on_foot_ = false;
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
