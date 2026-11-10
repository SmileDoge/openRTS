#include <openrts/simulation/traffic.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace openrts::simulation {

const std::vector<TrafficCurveLimit>& rts_traffic_limits() {
    static const std::vector<TrafficCurveLimit> table{
        {6000.0f, 75.0f}, {5000.0f, 55.0f}, {3500.0f, 45.0f}, {2500.0f, 38.0f}, {2000.0f, 32.0f},
        {1500.0f, 28.0f}, {1000.0f, 25.0f}, {600.0f, 17.0f},  {300.0f, 11.0f},  {200.0f, 7.0f},
    };
    return table;
}

float traffic_curve_speed(const std::span<const TrafficCurveLimit> table, const double radius) {
    float speed = -1.0f;
    if (radius <= 0.0) return speed;
    for (const auto& entry : table) {
        if (entry.speed > 0.0f && radius < entry.radius) speed = entry.speed;
    }
    return speed;
}

std::string TrafficSettings::problem() const {
    if (!(max_speed > 0.0f)) return "TrainMaxSpeed must be positive";
    if (!(acceleration > 0.0f)) return "TrainAcceleration must be positive";
    return {};
}

TrafficDriver::TrafficDriver(TrafficSettings settings) : settings_(std::move(settings)) {}

// RTS 0x424488, the TRAFFIC branch. `v` is the speed along the way it runs;
// the speed it may run at is the least of the route's limit, its maximum
// and the limits here; of the targets below that, the one needing the
// strongest braking, (v - v_t) / d, is planned for: with the plain braking
// distance d0 = (v^2 - v_t^2) / 2 dec it runs on beyond 4 d0, brakes from
// 0.25 to 1.0 of its deceleration between 4 d0 and d0, and twice it nearer.
void TrafficDriver::step(const double dt_, Train& train, const TrafficOrders& orders) {
    const auto dt = static_cast<float>(dt_);
    const auto& s = settings_;
    const float acc = s.acceleration;
    const float dec = s.braking();
    const float direction = orders.direction >= 0 ? 1.0f : -1.0f;
    const float v = static_cast<float>(train.speed()) * direction;

    float allowed = s.max_speed;
    if (s.route_limit > 0.0f) allowed = (std::min)(allowed, s.route_limit);
    if (orders.limit >= 0.0f) allowed = (std::min)(allowed, orders.limit);
    if (orders.hold) allowed = 0.0f;
    float target = -1.0f;
    double target_distance = 0.0;
    bool behind_train = false;
    float strongest = 0.0f;
    for (const auto& t : orders.targets) {
        if (t.speed < 0.0f || t.speed >= allowed) continue;
        if (t.distance <= 0.0) {
            allowed = t.speed;
            continue;
        }
        const auto rate = static_cast<float>((v - t.speed) / t.distance);
        if (rate > strongest) {
            strongest = rate;
            target = t.speed;
            target_distance = t.distance;
            behind_train = t.train;
        }
    }
    // A target reached meanwhile (passed by a later one at d <= 0) is the limit now.
    if (target >= 0.0f && target >= allowed) target = -1.0f;

    float a = 0.0f;
    if (v >= 0.0f && dec > 0.0f) {
        if (v < allowed) {
            a = acc;
            if (v + a * dt > allowed) a = (allowed - v) / dt;
        }
        if (v > allowed) {
            a = -dec;
            if (v + a * dt < allowed) a = (allowed - v) / dt;
        }
        if (target >= 0.0f) {
            const float dv = v - target;
            float near = (dv * 0.5f + target) * dv / dec;
            if (near > target_distance && behind_train) near = (dv * 0.5f + target) * (dv * 0.5f) / dec;
            const float far = (dv * 0.5f + target) * (dv * 4.0f) / dec;
            const auto d = static_cast<float>(target_distance);
            if (d > far) {
                // Not yet: run on, but not past the 4 d0 mark this step.
                if (a > 0.0f) {
                    const float travel = (a * dt + v) * dt;
                    if (travel > 0.0f && d - far <= travel) a = (1.0f - (travel - d + far) / travel) * acc;
                }
            } else if (d <= near) {
                a = -dec * (behind_train ? 4.0f : 2.0f);
            } else {
                const float scale = behind_train ? dec * 2.0f : dec;
                a = -(((1.0f - (d - near) / (far - near)) * 0.75f) + 0.25f) * scale;
            }
        }
    } else if (v < 0.0f) {
        a = dec * 2.0f; // running the wrong way: back to a stand
    }
    acceleration_ = a;
    allowed_ = allowed;

    // The seeming throttle for the engines (RTS: from the acceleration, less
    // at speed).
    float throttle = acc > 0.0f ? a / acc : 0.0f;
    if (v < 0.0f || throttle <= 0.1f) {
        throttle = 0.1f;
    } else {
        const float top = (std::max)(s.max_speed, 0.1f);
        if (v >= top * 0.2f)
            throttle *= 0.5f - v / (top * 2.25f) + 0.044f;
        else
            throttle *= 1.0f - 2.5f * v / top;
        throttle = std::clamp(throttle, 0.1f, 1.0f);
    }
    throttle_ = throttle;
    train.set_drive_acceleration(a * direction);
}

namespace {

// Calls `curve(distance, radius)` for every curved section of the leg
// between two leg distances (from where it starts, or `from` when the
// stretch is already in it).
template <typename Curve>
void curves_between(const TrackNetwork& network, const RouteLeg& leg, const double from, const double to, Curve&& curve) {
    for (const auto& span : leg.spans) {
        const double span_end = span.start + span.length();
        if (span_end < from || span.start > to) continue;
        const auto* node = network.node(span.node);
        if (!node) continue;
        for (const auto& section : node->sections) {
            if (!section.definition || !section.definition->curved || section.definition->radius <= 0.0f) continue;
            const double a = span.forward ? span.start + (section.start - span.from)
                                          : span.start + (span.from - section.start - section.length);
            const double b = a + section.length;
            const double low = (std::max)(a, span.start), high = (std::min)(b, span_end);
            if (high < from || low > to || high <= low) continue;
            curve((std::max)(low, from), static_cast<double>(section.definition->radius));
        }
    }
}

} // namespace

TrafficOrders traffic_orders(const Train& train, Dispatcher& dispatcher, const SignalSystem* signals, const SpeedLimits& limits,
                             const TrainCategory category, const TrafficSettings& settings, float& wait_left,
                             const std::span<Train* const> trains) {
    TrafficOrders orders;
    const auto id = train.id();
    const auto* progress = dispatcher.progress(id);
    const auto* route = dispatcher.route(id);
    if (!progress || !route || !progress->on_route || progress->finished || train.derailed() || !train.network()) {
        orders.hold = true;
        return orders;
    }
    const auto& leg = route->legs[progress->leg];
    orders.direction = progress->front_leads ? 1 : -1;
    const double head = progress->head;
    const float efficiency = settings.efficiency;
    const double speed = std::abs(train.speed());
    const double dec = (std::max)(static_cast<double>(settings.braking()), 0.05);
    // How far ahead the limits matter: four braking distances from its top
    // speed and some (RTS looks 3 km ahead for curves).
    const double top = (std::max)(speed, static_cast<double>(settings.max_speed));
    const double look = (std::min)(3000.0, 4.0 * top * top / (2.0 * dec) + 300.0);

    // Where its way ends: the leg's end (a reverse point, or the path's end
    // at FinishSpeed) when it is reserved that far, else 10 m short of the
    // reservation's end.
    const bool last_leg = progress->leg + 1 >= route->legs.size();
    const bool path_end = last_leg && !leg.marks.empty() && leg.marks.back().kind == RouteMarkKind::end;
    if (progress->authority >= leg.length() - 0.5) {
        const float finish = path_end ? (std::max)(settings.finish_speed, 0.0f) : 0.0f;
        orders.targets.push_back({finish, leg.length() - head - (finish > 0.0f ? 0.0 : traffic_point_margin)});
    } else {
        orders.targets.push_back({0.0f, progress->authority - head - traffic_stop_margin});
    }
    // A waiting point: stand at it for its time, then the dispatcher passes it.
    if (progress->next_mark < leg.marks.size()) {
        const auto& mark = leg.marks[progress->next_mark];
        if (mark.kind == RouteMarkKind::wait) {
            const double to_mark = mark.distance - head;
            orders.targets.push_back({0.0f, to_mark - traffic_point_margin});
            if (to_mark < traffic_point_margin + 15.0 && speed < 0.05) {
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
    // Signals: one at danger is the end of the way; its speed a limit ahead.
    if (signals && !settings.ignore_signals) {
        const auto next = signals->ahead(train);
        if (next.valid && next.aspect <= formats::SignalAspect::stop_and_proceed)
            orders.targets.push_back({0.0f, next.distance - traffic_stop_margin});
        if (next.valid && next.speed > 0.0f) orders.targets.push_back({next.speed * efficiency, next.distance});
    }
    // Speed posts: the limit here and the next ones.
    if (!settings.ignore_speed_limits) {
        const auto track = limits.evaluate(train, category, look);
        if (track.current >= 0.0f) orders.limit = track.current * efficiency;
        for (const auto& [limit, distance] : limits.limits_ahead(train, category, look, 4))
            orders.targets.push_back({limit * efficiency, distance});
    }
    // Curves under the train and ahead (TrafficLimitsTable).
    if (!settings.curve_limits.empty()) {
        for (const auto& car : train.cars()) {
            const float v = traffic_curve_speed(settings.curve_limits, car.radius);
            if (v > 0.0f && (orders.limit < 0.0f || v * efficiency < orders.limit)) orders.limit = v * efficiency;
        }
        float last_speed = -1.0f;
        curves_between(*train.network(), leg, head, head + look, [&](const double distance, const double radius) {
            const float v = traffic_curve_speed(settings.curve_limits, radius);
            if (v <= 0.0f || v == last_speed) return;
            last_speed = v;
            orders.targets.push_back({v * efficiency, (std::max)(0.0, distance - head)});
        });
    }
    // Another train's end ahead on the way it runs.
    if (!trains.empty()) {
        const double reach = 4.0 * speed * speed / (2.0 * dec) + 100.0;
        if (const auto contact = end_contact(train, progress->front_leads, trains, reach))
            orders.targets.push_back({0.0f, contact->gap - traffic_stop_margin, true});
    }
    return orders;
}

TrafficSettings default_traffic_settings(const Train& train, const float route_limit) {
    TrafficSettings settings;
    float top = 0.0f;
    for (const auto& car : train.cars()) {
        if (!car.definition || !car.definition->engine) continue;
        const float v = car.definition->engine->max_velocity;
        if (v > 0.0f && (top <= 0.0f || v < top)) top = v;
    }
    if (top <= 0.0f) top = route_limit > 0.0f ? route_limit : 33.0f;
    settings.max_speed = top;
    settings.acceleration = 0.4f;
    settings.deceleration = 0.5f;
    settings.route_limit = route_limit;
    return settings;
}

} // namespace openrts::simulation
