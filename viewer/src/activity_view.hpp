#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <openrts/formats/activity.hpp>
#include <openrts/simulation/signals.hpp>

namespace openrts::viewer {

// A train of the scenario: its service and its number in it (TrainID ( s t )).
struct ActivityTrainId {
    std::uint32_t service{};
    std::uint32_t train{};
    friend bool operator==(const ActivityTrainId&, const ActivityTrainId&) = default;
    friend bool operator<(const ActivityTrainId& a, const ActivityTrainId& b) {
        return a.service != b.service ? a.service < b.service : a.train < b.train;
    }
};

// A car: by its place in a train now (from the head from 0, negative from
// the tail: -1 the last), or by its scenario ID (ssss-tttt-nnnn: the service
// and train it started in and its consist UiD; `by_id`).
struct ActivityCarRef {
    ActivityTrainId train{};
    std::int32_t number = 0;
    bool by_id = false;
};

// A scenario's train cut in two (UncoupleCars): the id it had (the part
// that keeps it), the cars of its head and tail parts, the IDs of the cars
// of the part that went (the cut).
struct ActivityUncoupling {
    ActivityTrainId train{};
    std::size_t head{}, tail{};
    std::vector<ActivityCarRef> cut;
};

// SetDefaultUncouple: what a cut from the train becomes (its NewID and
// NewType); MainWagon, the car that always stays in the train's own part.
struct ActivityUncoupleRule {
    std::optional<ActivityTrainId> new_id;
    std::string new_type; // STATIC, TRAFFIC, ... as written (empty: as it was)
    std::optional<ActivityCarRef> main_wagon;
};

// The world a scenario's events look at and act on: the trains by their
// scenario ids, the player, the signals, the sounds (the route scene's
// TrainView and SignalSystem; a test's fake).
class ActivityWorld {
public:
    virtual ~ActivityWorld() = default;
    // The player is in the cab of the train (PlayerBoardsTrain).
    [[nodiscard]] virtual bool in_cab(ActivityTrainId train) const = 0;
    // The train at `at` within `radius` m on the ground (ReachLocation),
    // `reach` as written: WAGON_ANY (any car; the default), WAGON_ALL,
    // WAGON_FRONT, WAGON_BACK, WAGON_PLAYER (the player's car), PLAYER.
    [[nodiscard]] virtual bool near(ActivityTrainId train, const formats::ActivityPoint& at, double radius, std::string_view reach) const = 0;
    // A car's state variable for WagonState (activity_WagonStateVariables:
    // Area WAGON / CABIN / SALON, the variable, Idx, SubIdx; pressures in
    // kgf/cm2, speeds in m/s); nullopt: not known here.
    [[nodiscard]] virtual std::optional<double> car_value(const ActivityCarRef& car, std::string_view area, std::string_view name,
                                                          int idx, int sub) const = 0;
    // Couplings since the last call: the trains that came together (CoupleCars).
    [[nodiscard]] virtual std::vector<std::pair<ActivityTrainId, ActivityTrainId>> take_couplings() = 0;
    // The train `from` is `to` from now on (AssignService).
    virtual void reassign(ActivityTrainId from, ActivityTrainId to) = 0;
    // The signal at `at` (its world object) held as `value`, or back to its
    // script (nullopt; SignalOverride's Cancel). False: no signal there.
    virtual bool override_signal(const formats::ActivityPoint& at, const std::optional<simulation::SignalOverride>& value) = 0;
    // SendSoundTrigger: the code to scenario object `object`'s sound (-1:
    // to the sounds of `car` itself).
    virtual void sound_trigger(std::int32_t object, std::uint32_t code, const ActivityCarRef& car) = 0;
    // ModifyWagons: a WagonGroup on the train's cars (RTS 0x4FA374): chocks,
    // hoses and end cocks at the group's ends, handbrakes on its cars.
    virtual void modify_wagons(ActivityTrainId train, const formats::ActivityWagonGroup& group) = 0;
    // ReachReversePoint: the reverse points of its path the train has
    // reached; nullopt without a path.
    [[nodiscard]] virtual std::optional<std::size_t> reverse_points_reached(ActivityTrainId /*train*/) const { return std::nullopt; }
    // The cars of the train now (a WagonState without WagonNum / WagonID
    // looks at each of them).
    [[nodiscard]] virtual std::size_t car_count(ActivityTrainId /*train*/) const { return 1; }
    // The scenario's trains cut since the last call (UncoupleCars).
    [[nodiscard]] virtual std::vector<ActivityUncoupling> take_uncouplings() { return {}; }
    // SetDefaultUncouple: the ids of the cuts from `train` from now on.
    virtual void set_default_uncouple(ActivityTrainId /*train*/, const ActivityUncoupleRule& /*rule*/) {}
    // ChangeServiceStartupType: a service's trains not started yet start at
    // their times (`starts`) or not at all (NONE).
    virtual void set_service_startup(std::uint32_t /*service*/, bool /*starts*/) {}
    // SpawnTrain ( ServiceID TrainID ): that traffic train starts now.
    virtual void spawn_train(ActivityTrainId /*train*/) {}
    // SetSpeedLimit: the train's speed limit (m/s) till another; not
    // positive: none.
    virtual void set_speed_limit(ActivityTrainId /*train*/, float /*limit*/) {}
    // Traffic (docs/traffic.md). The trains that came into play since the
    // last call (a traffic train started at its time: OnStart).
    [[nodiscard]] virtual std::vector<ActivityTrainId> take_started() { return {}; }
    // ReleaseBlockPoint / EnableBlockPoint: service 0 the global points,
    // point -1 every one of them; an enabled one does not hold a train
    // within `distance` m of it whose way runs past it already.
    virtual void set_block_point(std::uint32_t /*service*/, std::int32_t /*point*/, bool /*enabled*/, double /*distance*/) {}
    // ModifyVelocity: the train's maximum speed (m/s) and its rates (m/s^2);
    // not positive leaves one as it is.
    virtual void modify_velocity(ActivityTrainId /*train*/, float /*speed*/, float /*acceleration*/, float /*deceleration*/) {}
    // WaitHere: a traffic train stands for `time` s (not positive: till told otherwise).
    virtual void wait_here(ActivityTrainId /*train*/, double /*time*/) {}
};

// An RTS scenario while it runs (docs/activities.md; RTS's activity.pdf):
// its events and what they say. Once the trains stand, the start's events
// (ActivityStarts) run; then every frame the armed events (enabled, their
// activation level above 0, their service in play) whose condition holds:
// ReachLocation, PlayerBoardsTrain, CoupleCars, timers, WagonState (while
// its train's state is tracked, or once by CheckWagonState); the menu items
// picked (OnMenuSelect, F8), the messages closed (OnCloseMessageBox) and
// the services' starts (OnStart) fire theirs. An event runs its outcomes in
// order, Suspend pausing it in game time (ActivateEvent resumes it), a modal
// message holding it and the game till it is closed; it fires once unless
// Permanent. A traffic train coming into play (ActivityWorld::take_started)
// brings its service's events in; block points are released and enabled,
// traffic slowed (ModifyVelocity) and stopped (WaitHere) through the world.
// What is not run yet (SpawnTrain, tasks, saves...) the log names.
class ActivityView {
public:
    explicit ActivityView(std::shared_ptr<const formats::Activity> activity);

    [[nodiscard]] const formats::Activity& activity() const noexcept { return *activity_; }
    // The start's events, once (`time_of_day`: s into the day).
    void start(double time_of_day, ActivityWorld& world);
    [[nodiscard]] bool started() const noexcept { return started_; }
    // One frame of `elapsed` s of game time.
    void update(float elapsed, double time_of_day, ActivityWorld& world);
    void toggle_journal() noexcept { journal_open_ = !journal_open_; }
    // F8: the scenario's menu (SetMainMenu's) open or shut.
    void toggle_menu() noexcept { menu_open_ = !menu_open_; }
    // An item of a menu picked: its OnMenuSelect events run at the next update.
    void choose(std::int32_t menu, std::int32_t item);
    // A modal message is open: the game waits for it.
    [[nodiscard]] bool holds() const;
    // The open message closed by a button (ID_OK, ID_CANCEL, ID_YES, ID_NO).
    void close_message(std::string_view button = "ID_OK");
    // ImGui: the message windows, the hint, the menu, the journal.
    void draw();

    struct Entry {
        double time{}; // s into the day
        std::string title;
        std::string text;
    };
    [[nodiscard]] const std::vector<Entry>& journal() const noexcept { return journal_; }
    // The conditions and outcomes met that openRTS does not run yet.
    [[nodiscard]] const std::set<std::string>& not_run() const noexcept { return not_run_; }
    [[nodiscard]] std::int32_t main_menu() const noexcept { return main_menu_; }
    enum class Outcome : std::uint8_t { running, succeeded, failed };
    [[nodiscard]] Outcome outcome() const noexcept { return outcome_; }
    [[nodiscard]] std::size_t open_messages() const noexcept { return windows_.size(); }
    [[nodiscard]] ActivityTrainId player() const noexcept { return player_; }
    // A saved game: the scenario where it stands - its clock, each event's
    // state, the scripts waiting, the services in play, the cars tracked,
    // the journal and the messages open. load() puts it back over this
    // scenario made anew (not started: its start events do not run again).
    [[nodiscard]] std::string save() const;
    void load(const std::string& text);

private:
    struct Window {
        Entry entry;
        std::int32_t id = -1;             // MsgID
        std::vector<std::string> buttons; // ID_OK, ID_CANCEL, ID_YES, ID_NO
        bool modal = false;
        float expires = 0.0f; // s left; 0: till closed
    };
    struct EventState {
        std::uint32_t service{}; // 0: a global event
        const formats::ActivityEvent* event = nullptr;
        bool enabled = true;
        std::int32_t level = 1;  // ActivationLevel: armed above 0
        double armed_at{};       // the time it was enabled (ActivateByTimer)
        bool held = false;       // its condition held last time (a Permanent one fires as it comes true)
        bool level_changed = false; // its level changed since the last look (OnActivationLevel)
        // WagonState: each relation's last value (the dynamic ones), by car
        // (by_id, the train, the number) and the relation.
        std::map<std::tuple<bool, std::uint32_t, std::uint32_t, std::int32_t, std::size_t>, double> last;
    };
    // StartWagonStateTracking / CheckWagonState: the cars looked at (its
    // WagonRange or WagonList), none given: every car of the train.
    struct CarSet {
        std::optional<std::vector<ActivityCarRef>> cars;
    };
    struct Script {
        const formats::ActivityEvent* event = nullptr;
        std::uint32_t service{};
        ActivityTrainId train{};
        std::size_t next{};
        double wait{}; // s of Suspend left (infinity: till ActivateEvent)
    };
    [[nodiscard]] ActivityTrainId train_of(std::uint32_t service) const;
    [[nodiscard]] bool armed(const EventState& state) const;
    // Service numbers as written: -1 the global events, -2 `own`.
    [[nodiscard]] std::uint32_t service_in(double written, std::uint32_t own) const;
    [[nodiscard]] bool holds_now(EventState& state, ActivityWorld& world,
                                 const std::vector<std::pair<ActivityTrainId, ActivityTrainId>>& couplings, bool check_once);
    [[nodiscard]] std::optional<bool> wagon_state(EventState& state, const formats::ActivityNode& condition, ActivityTrainId own,
                                                  ActivityWorld& world, bool check_once);
    void fire(EventState& state);
    void start_service(std::uint32_t service);
    // Runs the scripts' outcomes till each waits or ends.
    void step_scripts(ActivityWorld& world);
    // Returns false when the script waits (a Suspend).
    bool run_outcome(Script& script, const formats::ActivityNode& outcome, ActivityWorld& world);
    void say(const formats::ActivityNode& outcome);
    // The events of the trains whose state CheckWagonState looks at now.
    void check_wagons(ActivityTrainId train, const CarSet& cars, ActivityWorld& world);
    // The cars a WagonRange / WagonList under `node` names in `train`.
    [[nodiscard]] static CarSet car_set(const formats::ActivityNode& node, ActivityTrainId train);
    // The cars a WagonState without WagonNum / WagonID looks at.
    [[nodiscard]] std::vector<ActivityCarRef> cars_of(ActivityTrainId train, bool check_once, ActivityWorld& world) const;

    std::shared_ptr<const formats::Activity> activity_;
    bool started_ = false;
    double now_ = 0.0;
    std::vector<EventState> events_;
    std::vector<Script> scripts_;
    std::set<std::uint32_t> services_;      // services with a train in play
    std::map<ActivityTrainId, CarSet> tracked_; // StartWagonStateTracking
    const CarSet* checking_ = nullptr;          // CheckWagonState's cars while it looks
    std::vector<ActivityUncoupling> uncouplings_; // this update's (UncoupleCars)
    ActivityTrainId player_{};               // the player's train
    std::int32_t main_menu_ = -1;
    std::vector<std::pair<std::int32_t, std::int32_t>> choices_;      // menu items picked, for the next update
    std::vector<std::pair<std::int32_t, std::string>> closed_;       // messages closed (MsgID, button)
    Outcome outcome_ = Outcome::running;
    std::vector<Entry> journal_;
    std::vector<Window> windows_; // the first is shown
    std::string hint_;
    float hint_left_ = 0.0f;
    std::string notice_;
    float notice_left_ = 0.0f;
    bool journal_open_ = false;
    bool menu_open_ = false;
    std::set<std::string> not_run_;
};

} // namespace openrts::viewer
