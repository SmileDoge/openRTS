#include <openrts/simulation/track_profile.hpp>

#include <algorithm>
#include <cmath>

namespace openrts::simulation {

namespace {

// Metres from the traveller to the next joint of sections the way it goes
// (0 at the end of its node: the move goes on into the next one).
double to_joint(const TrackNetwork& network, const TrackTraveller& t) {
    const auto* node = network.node(t.node());
    if (!node || node->sections.empty()) return 0.0;
    const auto& sections = node->sections;
    const auto s = (std::min)(t.section(), sections.size() - 1);
    // At a joint section() gives the section that starts there.
    if (t.forward()) {
        double left = sections[s].start + sections[s].length - t.offset();
        if (left < 1e-4 && s + 1 < sections.size()) left = sections[s + 1].start + sections[s + 1].length - t.offset();
        return (std::max)(left, 0.0);
    }
    double left = t.offset() - sections[s].start;
    if (left < 1e-4 && s > 0) left = t.offset() - sections[s - 1].start;
    return (std::max)(left, 0.0);
}

// From `start` the way it faces, joint by joint, to `limit` m: the points
// at (sign × distance), the last one at the end.
double walk(const TrackNetwork& network, TrackTraveller walker, const double limit, const double sign,
            std::vector<TrackProfilePoint>& points) {
    double travelled = 0.0;
    while (travelled < limit) {
        double step = to_joint(network, walker);
        // A joint under a centimetre (or a node's end): a centimetre on.
        if (step < 0.01) step = 0.01;
        step = (std::min)(step, limit - travelled);
        const auto moved = walker.move(step);
        travelled += moved.moved;
        if (moved.moved <= 1e-9) break;
        const TrackProfilePoint point{sign * travelled, walker.pose().y};
        // Within a few centimetres of the last one: the same joint.
        if (!points.empty() && std::abs(points.back().distance - point.distance) < 0.05) points.back() = point;
        else points.push_back(point);
        if (!moved.ok()) break;
    }
    return travelled;
}

} // namespace

TrackProfile track_profile(const TrackNetwork& network, const TrackTraveller& lead, const double ahead, const double behind) {
    TrackProfile profile;
    std::vector<TrackProfilePoint> back;
    TrackTraveller turned = lead;
    turned.reverse();
    profile.behind = walk(network, turned, behind, -1.0, back);
    for (auto it = back.rbegin(); it != back.rend(); ++it) profile.points.push_back(*it);
    if (profile.points.empty() || std::abs(profile.points.back().distance) >= 0.05) profile.points.push_back({0.0, lead.pose().y});
    std::vector<TrackProfilePoint> front;
    profile.ahead = walk(network, lead, ahead, 1.0, front);
    for (const auto& point : front)
        if (point.distance > profile.points.back().distance + 0.05) profile.points.push_back(point);
    return profile;
}

double TrackProfile::height_at(const double distance) const {
    if (points.empty()) return 0.0;
    if (distance <= points.front().distance) return points.front().height;
    if (distance >= points.back().distance) return points.back().height;
    const auto after = std::lower_bound(points.begin(), points.end(), distance,
                                        [](const TrackProfilePoint& p, const double d) { return p.distance < d; });
    const auto before = after - 1;
    const double span = after->distance - before->distance;
    if (span <= 1e-9) return after->height;
    const double t = (distance - before->distance) / span;
    return before->height + (after->height - before->height) * t;
}

double TrackProfile::grade(const double from, const double to) const {
    const double low = (std::min)(from, to), high = (std::max)(from, to);
    if (high - low < 1.0) return 0.0;
    // The way the train goes is + distance: rising there is +.
    return (height_at(high) - height_at(low)) / (high - low) * 1000.0;
}

std::vector<TrackProfileElement> TrackProfile::elements(const double tolerance) const {
    std::vector<TrackProfileElement> result;
    for (std::size_t i = 1; i < points.size(); ++i) {
        const double from = points[i - 1].distance, to = points[i].distance;
        if (to - from <= 1e-6) continue;
        const double step = (points[i].height - points[i - 1].height) / (to - from) * 1000.0;
        if (!result.empty() && (std::abs(step - result.back().grade) <= tolerance || to - from < 1.0)) {
            result.back().to = to;
            result.back().grade = grade(result.back().from, to);
        } else if (!result.empty() && result.back().to - result.back().from < 1.0) {
            // The one before was under a metre: this one takes it.
            result.back().to = to;
            result.back().grade = grade(result.back().from, to);
        } else {
            result.push_back({from, to, step});
        }
    }
    return result;
}

} // namespace openrts::simulation
