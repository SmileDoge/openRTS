#pragma once

#include <limits>
#include <memory>

#include <openrts/simulation/dispatcher.hpp>
#include <openrts/simulation/signals.hpp>
#include <openrts/simulation/speed_limits.hpp>
#include <openrts/simulation/train.hpp>

// A simple driver for test trains (docs/paths.md): it works the same handles
// as the player (throttle, reverser, the 395 brake valve, the independent
// brake), drives up to the speed it may run at and stops where it must
// stand. Not RTS's AI (that drives traffic by its own rules); a stand-in to
// load signals and blocks and for remote trains.
namespace openrts::simulation {

struct DriveOrders {
    // Metres from the leading end to where the train must stand: a signal
    // at danger, the end of its reserved way, a waiting / reverse / end
    // point. Infinity: nowhere.
    double stop_distance = std::numeric_limits<double>::infinity();
    float speed_limit = -1.0f;      // m/s now, -1: none
    float next_limit = -1.0f;       // m/s ahead, -1: none
    double next_limit_distance = 0.0;
    int direction = 1;              // +1 drive towards the train front, -1 backwards
    bool hold = false;              // stand still (waiting)
};

class AutoDriver {
public:
    explicit AutoDriver(std::shared_ptr<DriverHandles> handles);
    // Once per train step, before Train::step.
    void step(double dt, const Train& train, const DriveOrders& orders);
    [[nodiscard]] float target_speed() const noexcept { return target_; }
    [[nodiscard]] bool standing() const noexcept { return standing_; }
    [[nodiscard]] const std::shared_ptr<DriverHandles>& handles() const noexcept { return handles_; }

    static constexpr float planned_deceleration = 0.35f; // m/s^2 the driver plans its stops with
    static constexpr double stop_margin = 8.0;           // m short of a stop point

private:
    std::shared_ptr<DriverHandles> handles_;
    float target_ = 0.0f;
    float notch_timer_ = 0.0f;
    float brake_target_ = 5.0f; // equalising reservoir aim while braking, atm
    bool standing_ = true;
};

// The highest speed the train takes a curve of `radius` m at: 0.85 of the
// speed where a car's m v^2 / R reaches its DerailRailForce (the derailment
// rule, docs/train.md). Infinity when no car has a DerailRailForce.
[[nodiscard]] double curve_speed(const Train& train, double radius);

// The tightest curve of the leg between two leg distances.
struct CurveAhead {
    double distance{}; // leg distance where it starts (or `from` when already in it)
    double radius{};
};
[[nodiscard]] std::optional<CurveAhead> tightest_curve(const TrackNetwork& network, const RouteLeg& leg, double from,
                                                       double to);

// What a test driver following its path must do this step: stand at the end
// of its reserved way, at a waiting point (for its time, `wait_left` keeps
// the count, then the dispatcher passes the point), at a signal at danger;
// keep to the speed limit, the next limit, the next signal's speed and the
// curves ahead.
[[nodiscard]] DriveOrders path_orders(const Train& train, Dispatcher& dispatcher, const SignalSystem* signals,
                                      const SpeedLimits& limits, bool freight, float& wait_left);

} // namespace openrts::simulation
