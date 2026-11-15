#include <openrts/simulation/track_profile.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <openrts/formats/track_database.hpp>
#include <openrts/simulation/signals.hpp>
#include <openrts/simulation/train.hpp>

namespace openrts::simulation {
namespace {

constexpr double tile_size = 2000.0; // MSTS tile, metres
constexpr double max_grade = 250.0;  // per mille, clamps a broken joint
constexpr double signal_mark_slack = 5.0;

double plan_distance(const TrackPoint& a, const TrackPoint& b) noexcept {
    const double dx = static_cast<double>(b.tile_x - a.tile_x) * tile_size + (b.x - a.x);
    const double dz = static_cast<double>(b.tile_z - a.tile_z) * tile_size + (b.z - a.z);
    return std::sqrt(dx * dx + dz * dz);
}

// A walk along the rails from a starting point: the sampled poses and the
// distance travelled to each of them.
struct Walk {
    std::vector<TrackPoint> points;
    std::vector<double> distances;
    double reached{};
    bool cut = false;

    void run(const TrackTraveller& start, const double distance, const double step) {
        auto traveller = start;
        points.push_back(traveller.pose());
        distances.push_back(0.0);
        const double stride = step > 0.0 ? step : 10.0;
        while (reached < distance - 1e-9) {
            const double left = (std::min)(stride, distance - reached);
            const auto moved = traveller.move(left, JunctionMode::probe);
            reached += moved.moved;
            if (moved.moved > 0.0) {
                points.push_back(traveller.pose());
                distances.push_back(reached);
            }
            if (!moved.ok()) {
                cut = true;
                break;
            }
            if (moved.moved <= 1e-9) break;
        }
    }
};

bool is_mark_item(const formats::TrackItem& item) {
    return item.kind == formats::TrackItemKind::signal || item.kind == formats::TrackItemKind::platform
        || item.kind == formats::TrackItemKind::siding;
}

} // namespace

TrackProfile build_profile(const Train& train, const TrackNetwork& network, const SpeedLimits* limits,
                           const TrainCategory category, const SignalSystem* signals,
                           const std::vector<const Train*>& others, const ProfileRequest& request) {
    TrackProfile profile;
    const auto head = train.leading_end();
    if (!head) return profile;
    const double step = request.step > 0.0 ? request.step : 10.0;

    // One map for the names and stations of the items the walks report.
    std::unordered_map<std::uint32_t, const formats::TrackItem*> by_id;
    for (const auto& item : network.database().items) by_id[item.id] = &item;

    // Behind the leading end, then ahead: both walks go the way the train
    // last travelled, so the profile reads as the train sees the way.
    auto behind = *head;
    behind.reverse();
    Walk back;
    back.run(behind, request.behind, step);
    Walk fore;
    fore.run(*head, request.ahead, step);

    // The vertices run from the far end behind back to the head and then on
    // ahead, so a drawing walks the vector in order; `samples` keeps the
    // pose of each of them.
    std::vector<TrackPoint> samples;
    const auto add_vertex = [&](const double distance, const TrackPoint& pose) {
        profile.vertices.push_back(ProfileVertex{.distance = distance, .elevation = pose.y});
        samples.push_back(pose);
    };
    for (std::size_t i = back.points.size(); i-- > 0;) {
        // The last point of the walk behind is the head, which the walk ahead
        // adds.
        if (i + 1 == back.points.size() && back.points.size() > 1) continue;
        add_vertex(-back.distances[i], back.points[i]);
    }
    const std::size_t first_ahead = profile.vertices.size();
    for (std::size_t i = 0; i < fore.points.size(); ++i) add_vertex(fore.distances[i], fore.points[i]);

    // The grade of a sample: the way it rises towards the next one (the last
    // one keeps the grade it came with).
    for (std::size_t i = 0; i + 1 < profile.vertices.size(); ++i) {
        const double length = profile.vertices[i + 1].distance - profile.vertices[i].distance;
        if (length <= 1e-9) continue;
        const double grade = 1000.0 * (profile.vertices[i + 1].elevation - profile.vertices[i].elevation) / length;
        profile.vertices[i].grade = static_cast<float>(std::clamp(grade, -max_grade, max_grade));
    }
    if (profile.vertices.size() > 1) profile.vertices.back().grade = profile.vertices[profile.vertices.size() - 2].grade;

    profile.behind = back.reached;
    profile.ahead = fore.reached;
    profile.cut_behind = back.cut;
    profile.cut_ahead = fore.cut;
    if (first_ahead < profile.vertices.size()) {
        profile.elevation_at_head = profile.vertices[first_ahead].elevation;
        profile.grade_at_head = profile.vertices[first_ahead].grade;
    }
    if (limits) profile.current_limit = limits->limit_at(head->node(), head->offset(), head->forward(), category);

    if (!request.marks) return profile;

    const auto add_item_marks = [&](const LookAhead& found, const double sign) {
        for (const auto& entry : found.items) {
            const auto it = by_id.find(entry.item);
            if (it == by_id.end() || !it->second) continue;
            const auto& item = *it->second;
            ProfileMark mark{.distance = sign * entry.distance};
            switch (item.kind) {
            case formats::TrackItemKind::signal: mark.kind = ProfileMarkKind::signal; break;
            case formats::TrackItemKind::platform:
                mark.kind = item.station.empty() ? ProfileMarkKind::platform : ProfileMarkKind::station;
                break;
            case formats::TrackItemKind::siding: mark.kind = ProfileMarkKind::siding; break;
            default: continue;
            }
            mark.name = mark.kind == ProfileMarkKind::station ? item.station : item.name;
            if (mark.name.empty()) mark.name = item.keyword;
            mark.facing = sign < 0.0 ? !entry.facing_forward : entry.facing_forward;
            profile.marks.push_back(std::move(mark));
        }
    };
    add_item_marks(look_ahead(network, head->node(), head->offset(), head->forward(), request.ahead, is_mark_item), 1.0);
    add_item_marks(look_ahead(network, head->node(), head->offset(), !head->forward(), request.behind, is_mark_item),
                   -1.0);

    // The signal the train is running up to: whether it stands at danger
    // decides how it looks on the profile.
    if (signals) {
        const auto next = signals->ahead(train);
        if (next.valid) {
            for (auto& mark : profile.marks) {
                if (mark.kind != ProfileMarkKind::signal || mark.distance < 0.0) continue;
                if (std::abs(mark.distance - next.distance) > signal_mark_slack) continue;
                mark.at_danger = next.aspect <= formats::SignalAspect::stop_and_proceed;
                if (next.speed > 0.0f) mark.speed = next.speed;
            }
        }
    }

    // The speed posts: the limits themselves, so the post behind the head
    // shows the limit the train is running under.
    if (limits) {
        if (profile.current_limit > 0.0f) {
            profile.marks.push_back(
                ProfileMark{.kind = ProfileMarkKind::speed_post, .distance = 0.0, .speed = profile.current_limit});
        }
        for (const auto& [speed, distance] : limits->limits_ahead(train, category, request.ahead, 32)) {
            profile.marks.push_back(
                ProfileMark{.kind = ProfileMarkKind::speed_post, .distance = distance, .speed = speed});
        }
    }

    std::sort(profile.marks.begin(), profile.marks.end(),
              [](const ProfileMark& a, const ProfileMark& b) { return a.distance < b.distance; });

    if (!request.occupancy) return profile;

    // The cars of the other trains, each a stretch around its centre: the
    // nearest sample in the plan is where that car stands.
    const double car_length =
        train.cars().empty() ? 24.0 : train.length() / static_cast<double>(train.cars().size());
    for (const auto* other : others) {
        if (!other || other == &train) continue;
        for (const auto& centre : other->car_centres()) {
            const auto pose = centre.pose();
            std::size_t best = samples.size();
            double nearest = std::numeric_limits<double>::infinity();
            for (std::size_t i = 0; i < samples.size(); ++i) {
                const double distance = plan_distance(samples[i], pose);
                if (distance < nearest) {
                    nearest = distance;
                    best = i;
                }
            }
            if (best == samples.size() || nearest > car_length) continue; // not on this stretch
            const double at = profile.vertices[best].distance;
            profile.occupied.push_back(ProfileSpan{.from = at - car_length * 0.5, .to = at + car_length * 0.5});
        }
    }
    std::sort(profile.occupied.begin(), profile.occupied.end(),
              [](const ProfileSpan& a, const ProfileSpan& b) { return a.from < b.from; });
    return profile;
}

} // namespace openrts::simulation
