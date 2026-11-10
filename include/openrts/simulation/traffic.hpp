#pragma once

#include <span>
#include <string>
#include <vector>

#include <openrts/simulation/dispatcher.hpp>
#include <openrts/simulation/signals.hpp>
#include <openrts/simulation/speed_limits.hpp>
#include <openrts/simulation/train.hpp>

// Traffic trains as RTS runs them (docs/traffic.md; the analysis in
// docs/reverse/traffic.md). A traffic train is one rigid body following its
// path (Train::set_kinematic): no traction, brakes, air or couplers, so many
// of them cost little. Each step RTS's controller (0x424488) picks the
// acceleration from the speed the train may run at and the targets ahead
// (where it must stand, or be down to a lower speed). Nothing here depends
// on the viewer, so a server can run traffic the same way.
namespace openrts::simulation {

// A TrafficLimitsTable entry: curves tighter than `radius` m at `speed` m/s.
struct TrafficCurveLimit {
    float radius{};
    float speed{};
};

// RTS's table when a scenario has none (0x407588): below 6000 m 75 m/s,
// 5000 55, 3500 45, 2500 38, 2000 32, 1500 28, 1000 25, 600 17, 300 11,
// 200 7.
[[nodiscard]] const std::vector<TrafficCurveLimit>& rts_traffic_limits();
// The speed for a curve of `radius` m (RTS 0x424488: the entries in turn,
// the last one with a larger radius wins); -1: none applies.
[[nodiscard]] float traffic_curve_speed(std::span<const TrafficCurveLimit> table, double radius);

struct TrafficSettings {
    float max_speed{};         // TrainMaxSpeed, m/s; must be positive
    float acceleration{};      // TrainAcceleration MaxAcc, m/s^2; must be positive
    float deceleration{};      // MaxDec; not positive: the acceleration (RTS 0x4EF740)
    float efficiency = 1.0f;   // TrainEfficiency: the limits it keeps to are times this
    float finish_speed{};      // FinishSpeed: the speed it runs off its path's end at
    float route_limit = -1.0f; // the route's SpeedLimit (.trk): RTS's cap over all, m/s
    bool ignore_signals = false;      // TrainFlags IGNORE_SIGNALS
    bool ignore_speed_limits = false; // TrainFlags IGNORE_SPEED_LIMITS
    std::vector<TrafficCurveLimit> curve_limits = rts_traffic_limits();

    [[nodiscard]] float braking() const noexcept { return deceleration > 0.0f ? deceleration : acceleration; }
    // Empty when the train can run as traffic, else why not (RTS: message
    // 0x88, the train is not placed).
    [[nodiscard]] std::string problem() const;
};

// Something ahead the train must be down to `speed` at, `distance` m from
// its leading end.
struct TrafficTarget {
    float speed{};
    double distance{};
    bool train = false; // the end of another train ahead (RTS brakes harder for it)
};

struct TrafficOrders {
    int direction = 1;   // +1: run towards the train front, -1: backwards
    bool hold = false;   // stand (waiting, no path)
    float limit = -1.0f; // m/s it may run at here (already times the efficiency); -1: its own maximum only
    std::vector<TrafficTarget> targets;
};

class TrafficDriver {
public:
    explicit TrafficDriver(TrafficSettings settings);
    // Once per train step, before Train::step: picks the acceleration and
    // sets it on the (kinematic) train.
    void step(double dt, Train& train, const TrafficOrders& orders);
    [[nodiscard]] TrafficSettings& settings() noexcept { return settings_; }
    [[nodiscard]] const TrafficSettings& settings() const noexcept { return settings_; }
    [[nodiscard]] float acceleration() const noexcept { return acceleration_; } // m/s^2 along its way
    [[nodiscard]] float target_speed() const noexcept { return allowed_; }      // what it may run at now
    // The seeming throttle 0.1 (idle) .. 1 that RTS works out from the
    // acceleration for the engines' sounds and diesels (0x424488).
    [[nodiscard]] float throttle() const noexcept { return throttle_; }

private:
    TrafficSettings settings_;
    float acceleration_ = 0.0f;
    float allowed_ = 0.0f;
    float throttle_ = 0.1f;
};

// How short of where it must stand a traffic train stops: the end of its
// reserved way or a signal at danger (RTS: the reservation end less 10 m),
// and a waiting, reverse or end point of its path (RTS runs up to it).
inline constexpr double traffic_stop_margin = 10.0;
inline constexpr double traffic_point_margin = 1.0;

// What a traffic train following its dispatcher path must keep to this
// step: the end of its reserved way, its waiting points (for their time,
// `wait_left` keeps the count, then the dispatcher passes the point), the
// end of its path at FinishSpeed, signals, speed posts, the curves ahead
// (TrafficLimitsTable, 3 km as RTS), and another train ahead on its way
// (`trains`: the others).
[[nodiscard]] TrafficOrders traffic_orders(const Train& train, Dispatcher& dispatcher, const SignalSystem* signals,
                                           const SpeedLimits& limits, TrainCategory category, const TrafficSettings& settings,
                                           float& wait_left, std::span<Train* const> trains = {});

// Settings for a train with no scenario values (Track control's AI
// trains): its engines' lowest MaxVelocity (else the route's limit, else
// 33 m/s), 0.4 m/s^2 up and 0.5 down (openRTS's values; RTS asks the
// dispatcher for none).
[[nodiscard]] TrafficSettings default_traffic_settings(const Train& train, float route_limit);

} // namespace openrts::simulation
