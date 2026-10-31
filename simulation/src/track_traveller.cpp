#include <openrts/simulation/track_traveller.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>

namespace openrts::simulation {
namespace {

struct Arrival {
    std::uint32_t node{};
    bool forward = true;
    double offset{};
};

// Called for each junction on the way: (junction, facing, branch). Returns
// false to stop (obstructed).
using JunctionVisitor = std::function<bool(std::uint32_t, bool, std::uint32_t)>;

// Leaves vector node `from` at its start (end = 0) or end (end = 1) and
// follows pins through any junctions to the next vector node (RTS 0x47ECD0).
// Pin directions (checked on Kuban_RTS): a pin to a vector node names its
// start (1) or end (0); a pin to a junction names its trunk (1) or a branch
// side (0).
[[nodiscard]] std::optional<Arrival> follow(const TrackNetwork& network, const std::uint32_t from, const int end,
                                            const JunctionVisitor& visit, MoveStop& stop) {
    const auto* node = network.node(from);
    if (!node || node->pins.size() < 2) {
        stop = MoveStop::end_of_track;
        return std::nullopt;
    }
    auto pin = node->pins[static_cast<std::size_t>(end)];
    std::uint32_t previous = from;
    bool previous_is_vector = true;
    int previous_end = end;

    // A chain of junctions is bounded by the node count.
    for (std::size_t guard = 0; guard <= network.node_count(); ++guard) {
        const auto* target = network.node(pin.node);
        if (!target) break;
        switch (target->kind) {
        case formats::TrackNodeKind::vector:
            return Arrival{target->index, pin.direction == 1, pin.direction == 1 ? 0.0 : target->length};
        case formats::TrackNodeKind::end:
            stop = MoveStop::end_of_track;
            return std::nullopt;
        case formats::TrackNodeKind::junction: {
            const auto* state = network.junction(target->index);
            if (!state || target->pins.size() < 2) break;
            const bool facing = pin.direction == 1;
            std::uint32_t branch = 0;
            formats::TrackPin next{};
            if (facing) {
                branch = state->selected;
                if (1 + branch >= target->pins.size()) break;
                next = target->pins[1 + branch];
            } else {
                // Which branch we came from: the branch pin pointing back at
                // the node we left, at the end we left it.
                std::optional<std::uint32_t> found;
                for (std::size_t k = 1; k < target->pins.size(); ++k) {
                    const auto& back = target->pins[k];
                    if (back.node != previous) continue;
                    if (previous_is_vector && back.direction != static_cast<std::uint32_t>(previous_end == 1 ? 0 : 1)) {
                        continue;
                    }
                    found = static_cast<std::uint32_t>(k - 1);
                    break;
                }
                if (!found) break;
                branch = *found;
                next = target->pins[0];
            }
            if (!visit(target->index, facing, branch)) {
                stop = MoveStop::junction_obstructed;
                return std::nullopt;
            }
            previous = target->index;
            previous_is_vector = false;
            previous_end = 0;
            pin = next;
            continue;
        }
        }
        break;
    }
    stop = MoveStop::end_of_track;
    return std::nullopt;
}

} // namespace

TrackTraveller::TrackTraveller(TrackNetwork& network, const std::uint32_t node, const double offset, const bool forward)
    : network_(&network), node_(node), offset_(offset), forward_(forward) {
    if (const auto* info = network.node(node)) offset_ = std::clamp(offset_, 0.0, info->length);
}

bool TrackTraveller::leave(const bool forward, const JunctionMode mode, const std::uint64_t owner, MoveResult& result) {
    auto& network = *network_;
    const auto visit = [&](const std::uint32_t junction, const bool facing, const std::uint32_t branch) {
        auto* state = network.junction(junction);
        const auto* info = network.node(junction);
        JunctionPass pass{.junction = junction, .facing = facing, .branch = branch};
        if (state) {
            const bool held_by_other = !state->reservations.empty() && state->reservations.front().first != owner;
            switch (mode) {
            case JunctionMode::occupy:
                pass.forced = held_by_other;
                state->occupied = true;
                if (!facing) state->selected = branch; // run-through: the switch is pushed over
                break;
            case JunctionMode::release:
                if (facing) {
                    // The tail keeps the junction for ClearanceDist more metres.
                    state->clearance[owner] = info && info->shape ? info->shape->clearance_distance : 0.0;
                } else {
                    state->occupied = false;
                    state->clearance.erase(owner);
                    std::erase_if(state->reservations, [&](const auto& r) { return r.first == owner; });
                    state->selected = state->reservations.empty() ? state->main_route : state->reservations.front().second;
                }
                break;
            case JunctionMode::probe:
                if (!facing && state->selected != branch) return false;
                break;
            case JunctionMode::plain:
                break;
            }
        }
        result.junctions.push_back(pass);
        return true;
    };

    MoveStop stop = MoveStop::completed;
    const auto arrival = follow(network, node_, forward ? 1 : 0, visit, stop);
    if (!arrival) {
        result.stop = stop;
        return false;
    }
    node_ = arrival->node;
    offset_ = arrival->offset;
    // The direction on the new node replaces `forward` (callers fix up for
    // backward moves).
    forward_ = arrival->forward;
    return true;
}

MoveResult TrackTraveller::move(const double distance, const JunctionMode mode, const std::uint64_t owner) {
    MoveResult result;
    const bool backwards = distance < 0.0;
    double remaining = std::abs(distance);
    // Direction of motion on the current node.
    bool motion = backwards ? !forward_ : forward_;
    std::size_t idle_transitions = 0;

    while (remaining > 0.0) {
        const auto* node = network_->node(node_);
        if (!node) {
            result.stop = MoveStop::end_of_track;
            break;
        }
        const double available = motion ? node->length - offset_ : offset_;
        if (remaining <= available) {
            offset_ += motion ? remaining : -remaining;
            result.moved += remaining;
            remaining = 0.0;
            break;
        }
        offset_ = motion ? node->length : 0.0;
        result.moved += available;
        remaining -= available;

        // Zero-length nodes can follow each other; do not spin forever.
        idle_transitions = available > 0.0 ? 0 : idle_transitions + 1;
        if (idle_transitions > network_->node_count()) {
            result.stop = MoveStop::end_of_track;
            break;
        }
        if (!leave(motion, mode, owner, result)) {
            forward_ = backwards ? !motion : motion;
            return result;
        }
        motion = forward_;
        forward_ = backwards ? !motion : motion;
    }
    return result;
}

std::size_t TrackTraveller::section() const {
    const auto* node = network_->node(node_);
    if (!node || node->sections.empty()) return 0;
    const auto it = std::upper_bound(node->sections.begin(), node->sections.end(), offset_,
                                     [](const double value, const NetworkSection& s) { return value < s.start; });
    return static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, (it - node->sections.begin()) - 1));
}

TrackPoint TrackTraveller::pose() const {
    auto point = network_->point(node_, offset_).value_or(TrackPoint{});
    if (!forward_) {
        point.heading += std::numbers::pi_v<float>;
        point.pitch = -point.pitch;
    }
    return point;
}

LookAhead look_ahead(const TrackNetwork& network, std::uint32_t node, double offset, bool forward,
                     const double max_distance, const std::function<bool(const formats::TrackItem&)>& filter,
                     const std::size_t max_items) {
    LookAhead result;
    const auto start = std::pair{node, forward};
    const double start_offset = offset;
    std::set<std::pair<std::uint32_t, bool>> visited{start};
    bool returned_to_start = false;

    const auto accept = [&](const NetworkItem& entry) {
        const auto& item = network.database().items[entry.item];
        return !filter || filter(item);
    };

    while (true) {
        const auto* info = network.node(node);
        if (!info) {
            result.stop = LookAheadStop::end_of_track;
            return result;
        }
        // Items ahead on this node, nearest first.
        const double limit = returned_to_start ? start_offset : (forward ? info->length : 0.0);
        std::vector<ItemAhead> here;
        for (const auto& entry : info->items) {
            const bool ahead = forward ? (entry.offset >= offset && entry.offset <= limit)
                                       : (entry.offset <= offset && entry.offset >= limit);
            if (!ahead || !accept(entry)) continue;
            const double d = result.searched + std::abs(entry.offset - offset);
            if (d > max_distance) continue;
            here.push_back({entry.item, node, d, forward});
        }
        std::sort(here.begin(), here.end(), [](const auto& a, const auto& b) { return a.distance < b.distance; });
        for (const auto& item : here) {
            result.items.push_back(item);
            if (result.items.size() >= max_items) return result;
        }
        if (returned_to_start) {
            result.searched = (std::min)(result.searched + std::abs(start_offset - offset), max_distance);
            result.stop = LookAheadStop::loop;
            return result;
        }

        result.searched += forward ? info->length - offset : offset;
        if (result.searched >= max_distance) {
            result.searched = max_distance;
            result.stop = LookAheadStop::distance;
            return result;
        }

        MoveStop stop = MoveStop::completed;
        const auto arrival = follow(network, node, forward ? 1 : 0,
                                    [&](const std::uint32_t junction, const bool facing, const std::uint32_t branch) {
                                        const auto* state = network.junction(junction);
                                        return facing || !state || state->selected == branch;
                                    },
                                    stop);
        if (!arrival) {
            result.stop = stop == MoveStop::junction_obstructed ? LookAheadStop::junction_obstructed
                                                                : LookAheadStop::end_of_track;
            return result;
        }
        node = arrival->node;
        forward = arrival->forward;
        offset = arrival->offset;
        const std::pair key{node, forward};
        if (key == start) {
            returned_to_start = true; // one lap: collect up to the start offset, then stop
        } else if (!visited.insert(key).second) {
            result.stop = LookAheadStop::loop;
            return result;
        }
    }
}

} // namespace openrts::simulation
