#include <openrts/simulation/speed_limits.hpp>

#include <cmath>
#include <numbers>

#include <openrts/simulation/track_traveller.hpp>
#include <openrts/simulation/train.hpp>

namespace openrts::simulation {
namespace {

// The sign of a post faces the trains it is for. Its angle is a
// mathematical angle in the ground plane: the sign faces the heading
// 3/2 pi - angle, and it is for travel against that facing. Checked on the
// installed routes (openrts-track-check): warning boards come before the
// limit post of their speed 11 times of 11, posts ending a restriction come
// after a limit for the same direction 193 times of 209; the other three
// readings of the angle fail one test or both (docs/paths.md). PROBABLE.
[[nodiscard]] double sign_facing(const float angle) {
    return 1.5 * std::numbers::pi - static_cast<double>(angle);
}

} // namespace

SpeedLimits::SpeedLimits(const TrackNetwork& network, const float route_limit)
    : network_(&network), route_limit_(route_limit > 0.0f ? route_limit : -1.0f) {
    const auto& items = network.database().items;
    for (std::uint32_t n = 1; n < network.node_count(); ++n) {
        const auto* node = network.node(n);
        if (!node || node->kind != formats::TrackNodeKind::vector) continue;
        for (const auto& entry : node->items) {
            if (entry.item >= items.size()) continue;
            const auto post = formats::speed_post(items[entry.item]);
            if (post && !post->limit && !post->warning) mileposts_[entry.item] = post->number;
            if (!post || !post->limit) continue;
            SpeedPostOnTrack on{.item = entry.item, .node = n, .offset = entry.offset, .post = *post};
            if (const auto where = network.point(n, entry.offset)) {
                // cos of the angle between the sign's facing and the node's
                // direction: facing back along the node = for travel along it.
                const double c = std::cos(sign_facing(post->angle) - static_cast<double>(where->heading));
                if (std::abs(c) > 0.2) {
                    on.along = c < 0.0;
                    on.against = !on.along;
                }
            }
            by_item_[entry.item] = posts_.size();
            posts_.push_back(on);
        }
    }
}

const SpeedPostOnTrack* SpeedLimits::post(const std::uint32_t item) const {
    const auto it = by_item_.find(item);
    return it == by_item_.end() ? nullptr : &posts_[it->second];
}

bool SpeedLimits::applies(const SpeedPostOnTrack& post, const bool forward, const TrainCategory category) {
    if (forward ? !post.along : !post.against) return false;
    if (category == TrainCategory::other) return true;
    const auto flags = post.post.flags;
    if ((flags & 0x20u) != 0 && category != TrainCategory::passenger) return false;
    return (flags & 0x40u) == 0 || category == TrainCategory::freight;
}

float SpeedLimits::limit_at(const std::uint32_t node, const double offset, const bool forward, const TrainCategory category) const {
    // Walk back from the point: the first post that applies to this travel.
    const auto behind = look_ahead(*network_, node, offset, !forward, 20000.0, [&](const formats::TrackItem& item) {
        return by_item_.contains(item.id);
    });
    for (const auto& found : behind.items) {
        const auto* on = post(found.item);
        // The walk goes the other way: travel on that node is the opposite.
        if (!on || !applies(*on, !found.facing_forward, category)) continue;
        return on->post.resume ? route_limit_ : on->post.speed;
    }
    return route_limit_;
}

std::vector<std::pair<float, double>> SpeedLimits::limits_ahead(const Train& train, const TrainCategory category, const double distance,
                                                                const std::size_t count) const {
    std::vector<std::pair<float, double>> result;
    const auto lead = train.leading_end();
    if (!lead) return result;
    const auto ahead = look_ahead(*network_, lead->node(), lead->offset(), lead->forward(), distance,
                                  [&](const formats::TrackItem& item) { return by_item_.contains(item.id); });
    for (const auto& found : ahead.items) {
        const auto* on = post(found.item);
        if (!on || !applies(*on, found.facing_forward, category)) continue;
        result.emplace_back(on->post.resume ? route_limit_ : on->post.speed, found.distance);
        if (result.size() >= count) break;
    }
    return result;
}

std::optional<double> SpeedLimits::ordinate(const Train& train) const {
    const auto lead = train.leading_end();
    if (!lead || mileposts_.empty()) return std::nullopt;
    const auto is_post = [&](const formats::TrackItem& item) { return mileposts_.contains(item.id); };
    const auto behind = look_ahead(*network_, lead->node(), lead->offset(), !lead->forward(), 5000.0, is_post, 1);
    if (behind.items.empty()) return std::nullopt;
    const double last = mileposts_.at(behind.items.front().item);
    const double from_last = behind.items.front().distance;
    const auto ahead = look_ahead(*network_, lead->node(), lead->offset(), lead->forward(), 5000.0, is_post, 4);
    double direction = 1.0;
    for (const auto& found : ahead.items) {
        const double next = mileposts_.at(found.item);
        if (next != last) {
            direction = next > last ? 1.0 : -1.0;
            break;
        }
    }
    return last + direction * from_last / 1000.0;
}

SpeedLimit SpeedLimits::evaluate(const Train& train, const TrainCategory category, const double distance) const {
    SpeedLimit result;
    const auto lead = train.leading_end();
    if (!lead) return result;
    // The trailing end facing the way of travel.
    const auto front = train.front_end();
    const auto rear = train.rear_end();
    std::optional<TrackTraveller> trail;
    if (front && rear) {
        const bool front_leads = front->node() == lead->node() && std::abs(front->offset() - lead->offset()) < 1e-3;
        trail = front_leads ? *rear : *front;
        if (!front_leads) trail->reverse();
    }
    const float at_head = limit_at(lead->node(), lead->offset(), lead->forward(), category);
    float current = at_head;
    if (trail) {
        const float at_tail = limit_at(trail->node(), trail->offset(), trail->forward(), category);
        if (at_tail > 0.0f && (current < 0.0f || at_tail < current)) current = at_tail;
    }
    result.current = current;
    const auto ahead = look_ahead(*network_, lead->node(), lead->offset(), lead->forward(), distance,
                                  [&](const formats::TrackItem& item) { return by_item_.contains(item.id); });
    for (const auto& found : ahead.items) {
        const auto* on = post(found.item);
        if (!on || !applies(*on, found.facing_forward, category)) continue;
        result.ahead = on->post.resume ? route_limit_ : on->post.speed;
        result.ahead_distance = found.distance;
        break;
    }
    return result;
}

} // namespace openrts::simulation
