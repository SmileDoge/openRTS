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

std::vector<TrackProfileElement> TrackProfile::elements(const double tolerance, const double min_length) const {
    std::vector<TrackProfileElement> result;
    if (points.size() < 2) return result;
    const auto chord = [&](const double from, const double to) { return grade(from, to); };
    for (std::size_t i = 1; i < points.size(); ++i) {
        const double from = points[i - 1].distance, to = points[i].distance;
        if (to - from <= 1e-6) continue;
        const double step = (points[i].height - points[i - 1].height) / (to - from) * 1000.0;
        if (!result.empty() && std::abs(step - result.back().grade) <= tolerance) {
            result.back().to = to;
            result.back().grade = chord(result.back().from, to);
        } else {
            result.push_back({from, to, step});
        }
    }
    // The short ones into a neighbour, the shortest first.
    while (result.size() > 1) {
        std::size_t shortest = 0;
        for (std::size_t k = 1; k < result.size(); ++k)
            if (result[k].to - result[k].from < result[shortest].to - result[shortest].from) shortest = k;
        if (result[shortest].to - result[shortest].from >= min_length) break;
        std::size_t other;
        if (shortest == 0) other = 1;
        else if (shortest + 1 == result.size()) other = shortest - 1;
        else {
            const double g = result[shortest].grade;
            other = std::abs(result[shortest - 1].grade - g) <= std::abs(result[shortest + 1].grade - g) ? shortest - 1 : shortest + 1;
        }
        const std::size_t low = (std::min)(shortest, other), high = (std::max)(shortest, other);
        result[low].to = result[high].to;
        result[low].grade = chord(result[low].from, result[low].to);
        result.erase(result.begin() + static_cast<std::ptrdiff_t>(high));
        // Neighbours of one grade now: one element.
        if (low + 1 < result.size() && std::abs(result[low + 1].grade - result[low].grade) <= tolerance) {
            result[low].to = result[low + 1].to;
            result[low].grade = chord(result[low].from, result[low].to);
            result.erase(result.begin() + static_cast<std::ptrdiff_t>(low + 1));
        }
        if (low > 0 && std::abs(result[low - 1].grade - result[low].grade) <= tolerance) {
            result[low - 1].to = result[low].to;
            result[low - 1].grade = chord(result[low - 1].from, result[low - 1].to);
            result.erase(result.begin() + static_cast<std::ptrdiff_t>(low));
        }
    }
    return result;
}

} // namespace openrts::simulation
