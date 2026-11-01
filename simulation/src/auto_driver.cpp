#include <openrts/simulation/auto_driver.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

namespace openrts::simulation {

AutoDriver::AutoDriver(std::shared_ptr<DriverHandles> handles) : handles_(std::move(handles)) {}

void AutoDriver::step(const double dt, const Train& train, const DriveOrders& orders) {
    if (!handles_ || train.cars().empty()) return;
    using Brake = DriverHandles::BrakeHandle;
    auto& h = *handles_;
    const auto time = static_cast<float>(dt);
    const float speed = std::abs(static_cast<float>(train.speed()));
    // Positive when moving the way it should drive.
    const float along = static_cast<float>(train.speed()) * static_cast<float>(orders.direction);
    float cylinder = 0.0f;
    for (const auto& car : train.cars()) cylinder = (std::max)(cylinder, car.cylinder);

    // The speed it may run at: the limit, the next limit as a braking curve,
    // the stop point as a braking curve, a little under all of them.
    float limit = orders.speed_limit > 0.0f ? orders.speed_limit : 22.0f;
    if (orders.next_limit >= 0.0f) {
        const double room = (std::max)(0.0, orders.next_limit_distance - 30.0);
        const double curve = std::sqrt(static_cast<double>(orders.next_limit) * orders.next_limit + 2.0 * planned_deceleration * room);
        limit = (std::min)(limit, static_cast<float>(curve));
    }
    double room = orders.stop_distance - stop_margin;
    if (std::isfinite(orders.stop_distance)) {
        limit = (std::min)(limit, static_cast<float>(room > 0.0 ? std::sqrt(2.0 * planned_deceleration * room) : 0.0));
    }
    if (orders.hold) limit = 0.0f;
    target_ = limit > 1.5f ? limit - 1.0f : (limit > 0.3f ? limit * 0.5f : 0.0f);
    standing_ = speed < 0.05f;
    const bool must_stop = target_ < 0.2f;
    notch_timer_ = (std::max)(0.0f, notch_timer_ - time);

    if (must_stop || along < -0.3f) {
        h.throttle = 0;
        if (standing_) {
            h.independent = 3.0f;
            brake_target_ = (std::min)(brake_target_, 4.5f);
        } else {
            // Brake for the room left: a heavier reduction the harder it must stop.
            const double needed = room > 1.0 ? speed * speed / (2.0 * room) : 1.0;
            const float reduction = std::clamp(0.4f + static_cast<float>(needed) * 2.0f, 0.4f, 1.6f);
            brake_target_ = (std::min)(brake_target_, 5.0f - reduction);
            if (room < -15.0 && speed > 3.0f) h.brake = Brake::emergency;
        }
    } else {
        if (standing_ && h.reverser != orders.direction) {
            h.throttle = 0;
            h.reverser = orders.direction;
        }
        h.independent = 0.0f;
        const float excess = along - target_;
        if (excess > 2.0f) {
            h.throttle = 0;
            brake_target_ = (std::min)(brake_target_, 5.0f - std::clamp(excess * 0.15f, 0.4f, 1.4f));
        } else if (excess > 0.3f) {
            if (h.throttle > 0 && notch_timer_ <= 0.0f) {
                --h.throttle;
                notch_timer_ = 0.3f;
            }
            if (excess < 1.0f) brake_target_ = 5.0f;
        } else {
            brake_target_ = 5.0f;
            if (excess < -1.0f && cylinder < 0.3f && h.throttle < h.notches && notch_timer_ <= 0.0f) {
                ++h.throttle;
                notch_timer_ = 0.5f;
            }
        }
    }
    if (h.brake == Brake::emergency && !(room < -15.0 && speed > 3.0f)) h.brake = Brake::lap;
    if (h.brake != Brake::emergency) {
        if (brake_target_ < 4.95f) {
            h.brake = h.equalising > brake_target_ + 0.02f ? Brake::service : Brake::lap;
        } else {
            h.brake = Brake::running;
        }
    }
}

double curve_speed(const Train& train, const double radius) {
    double worst = std::numeric_limits<double>::infinity();
    for (const auto& car : train.cars()) {
        if (!car.definition || car.definition->derail_rail_force <= 0.0f || car.definition->mass <= 0.0f) continue;
        worst = (std::min)(worst, static_cast<double>(car.definition->derail_rail_force / car.definition->mass));
    }
    if (!std::isfinite(worst) || radius <= 0.0) return std::numeric_limits<double>::infinity();
    return 0.85 * std::sqrt(worst * radius);
}

std::optional<CurveAhead> tightest_curve(const TrackNetwork& network, const RouteLeg& leg, const double from, const double to) {
    std::optional<CurveAhead> best;
    for (const auto& span : leg.spans) {
        const double span_end = span.start + span.length();
        if (span_end < from || span.start > to) continue;
        const auto* node = network.node(span.node);
        if (!node) continue;
        for (const auto& section : node->sections) {
            if (!section.definition || !section.definition->curved || section.definition->radius <= 0.0f) continue;
            // The section's stretch in leg distances.
            const double a = span.forward ? span.start + (section.start - span.from) : span.start + (span.from - section.start - section.length);
            const double b = a + section.length;
            const double low = (std::max)(a, span.start), high = (std::min)(b, span_end);
            if (high < from || low > to || high <= low) continue;
            const double radius = section.definition->radius;
            if (!best || radius < best->radius) best = CurveAhead{(std::max)(low, from), radius};
        }
    }
    return best;
}

DriveOrders path_orders(const Train& train, Dispatcher& dispatcher, const SignalSystem* signals, const SpeedLimits& limits,
                        const bool freight, float& wait_left) {
    DriveOrders orders;
    const auto id = train.id();
    const auto* progress = dispatcher.progress(id);
    const auto* route = dispatcher.route(id);
    if (!progress || !route || !progress->on_route || progress->finished || train.derailed() || !train.network()) {
        orders.hold = true;
        return orders;
    }
    const auto& leg = route->legs[progress->leg];
    orders.direction = progress->front_leads ? 1 : -1;
    double stop = (std::min)(progress->authority, leg.length()) - progress->head;
    if (progress->next_mark < leg.marks.size()) {
        const auto& mark = leg.marks[progress->next_mark];
        if (mark.kind == RouteMarkKind::wait) {
            const double to_mark = mark.distance - progress->head;
            stop = (std::min)(stop, to_mark);
            if (to_mark < AutoDriver::stop_margin + 15.0 && std::abs(train.speed()) < 0.05) {
                if (wait_left < 0.0f) wait_left = mark.wait;
                wait_left -= static_cast<float>(train_step);
                orders.hold = true;
                if (wait_left <= 0.0f) {
                    wait_left = -1.0f;
                    dispatcher.pass_mark(id);
                }
                return orders;
            }
        }
    }
    // The limits ahead: the speed posts', the next signal's, the curves';
    // the one with the lowest braking curve is the one to plan for.
    const auto track = limits.evaluate(train, freight);
    float limit = track.current;
    struct Ahead {
        float speed;
        double distance;
    };
    std::vector<Ahead> ahead;
    if (track.ahead >= 0.0f) ahead.push_back({track.ahead, track.ahead_distance});
    if (signals) {
        const auto next = signals->ahead(train);
        if (next.valid && next.aspect <= formats::SignalAspect::stop_and_proceed) stop = (std::min)(stop, next.distance);
        if (next.valid && next.speed > 0.0f) ahead.push_back({next.speed, next.distance});
    }
    // Curves under the train now and within braking distance ahead.
    for (const auto& car : train.cars()) {
        if (car.radius > 0.0f) {
            const auto v = static_cast<float>(curve_speed(train, car.radius));
            if (std::isfinite(v) && (limit < 0.0f || v < limit)) limit = v;
        }
    }
    const double speed = std::abs(train.speed());
    const double look = speed * speed / (2.0 * AutoDriver::planned_deceleration) + 300.0;
    if (const auto curve = tightest_curve(*train.network(), leg, progress->head, progress->head + look)) {
        const auto v = static_cast<float>(curve_speed(train, curve->radius));
        if (std::isfinite(v)) ahead.push_back({v, (std::max)(0.0, curve->distance - progress->head)});
    }
    double best = std::numeric_limits<double>::infinity();
    for (const auto& candidate : ahead) {
        const double curve = std::sqrt(static_cast<double>(candidate.speed) * candidate.speed
                                       + 2.0 * AutoDriver::planned_deceleration * (std::max)(0.0, candidate.distance - 30.0));
        if (curve < best) {
            best = curve;
            orders.next_limit = candidate.speed;
            orders.next_limit_distance = candidate.distance;
        }
    }
    orders.speed_limit = limit;
    orders.stop_distance = stop;
    return orders;
}

} // namespace openrts::simulation
