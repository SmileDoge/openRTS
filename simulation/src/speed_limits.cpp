#include <openrts/simulation/speed_limits.hpp>

#include <algorithm>
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

float without_tolerance(const float speed) noexcept {
    if (speed <= 0.0f) return speed;
    const float kmh = speed * 3.6f;
    const float whole = std::round(kmh);
    if (std::abs(kmh - whole) > 0.05f || whole < 8.0f) return speed;
    if (static_cast<int>(whole) % 5 != 3) return speed;
    return (whole - 3.0f) / 3.6f;
}

float SpeedLimits::speed_of(const SpeedPostOnTrack& on) const noexcept {
    if (on.post.resume) return route_limit_;
    return without_tolerance_ ? simulation::without_tolerance(on.post.speed) : on.post.speed;
}

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
        return speed_of(*on);
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
        result.emplace_back(speed_of(*on), found.distance);
        if (result.size() >= count) break;
    }
    return result;
}

void SpeedLimits::limits_on(const std::uint32_t node, const double entry, const double exit, const bool forward, const double at,
                            const TrainCategory category, std::vector<std::pair<float, double>>& out) const {
    std::vector<std::pair<float, double>> found;
    for (const auto& on : posts_) {
        if (on.node != node || !applies(on, forward, category)) continue;
        const bool inside = forward ? on.offset > entry && on.offset <= exit : on.offset < entry && on.offset >= exit;
        if (inside) found.emplace_back(speed_of(on), at + std::abs(on.offset - entry));
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    out.insert(out.end(), found.begin(), found.end());
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

std::vector<std::pair<double, double>> SpeedLimits::mileposts_along(const TrackTraveller& lead, const double ahead,
                                                                      const double behind) const {
    std::vector<std::pair<double, double>> result;
    if (mileposts_.empty()) return result;
    const auto is_post = [&](const formats::TrackItem& item) { return mileposts_.contains(item.id); };
    const auto back = look_ahead(*network_, lead.node(), lead.offset(), !lead.forward(), behind, is_post, 64);
    for (auto it = back.items.rbegin(); it != back.items.rend(); ++it) result.emplace_back(mileposts_.at(it->item), -it->distance);
    const auto front = look_ahead(*network_, lead.node(), lead.offset(), lead.forward(), ahead, is_post, 256);
    for (const auto& found : front.items) {
        // A post right under the end is found both ways.
        if (found.distance <= 1e-6 && !result.empty() && result.back().second >= -1e-6) continue;
        result.emplace_back(mileposts_.at(found.item), found.distance);
    }
    return result;
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
        result.ahead = speed_of(*on);
        result.ahead_distance = found.distance;
        break;
    }
    return result;
}

} // namespace openrts::simulation
