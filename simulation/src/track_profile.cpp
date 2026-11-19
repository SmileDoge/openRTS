#include <openrts/simulation/track_profile.hpp>

#include <algorithm>
#include <cmath>

namespace openrts::simulation {

TrackProfile track_profile(const TrackTraveller& lead, const double ahead, const double behind, const double step) {
    TrackProfile profile;
    const double pace = (std::max)(step, 1.0);
    std::vector<TrackProfilePoint> back;
    {
        // Behind: the same end turned round, its distances negative.
        TrackTraveller walker = lead;
        walker.reverse();
        double travelled = 0.0;
        while (travelled < behind) {
            const auto moved = walker.move((std::min)(pace, behind - travelled));
            travelled += moved.moved;
            if (moved.moved <= 1e-6) break;
            back.push_back({-travelled, walker.pose().y});
            if (!moved.ok()) break;
        }
        profile.behind = travelled;
    }
    profile.points.reserve(back.size() + static_cast<std::size_t>(ahead / pace) + 2);
    for (auto it = back.rbegin(); it != back.rend(); ++it) profile.points.push_back(*it);
    profile.points.push_back({0.0, lead.pose().y});
    TrackTraveller walker = lead;
    double travelled = 0.0;
    while (travelled < ahead) {
        const auto moved = walker.move((std::min)(pace, ahead - travelled));
        travelled += moved.moved;
        if (moved.moved <= 1e-6) break;
        profile.points.push_back({travelled, walker.pose().y});
        if (!moved.ok()) break;
    }
    profile.ahead = travelled;
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

} // namespace openrts::simulation
