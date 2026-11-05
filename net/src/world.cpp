#include <openrts/net/world.hpp>

#include <algorithm>
#include <cctype>
#include <unordered_map>

#include <openrts/simulation/signals.hpp>
#include <openrts/simulation/track_network.hpp>
#include <openrts/simulation/track_traveller.hpp>

namespace openrts::net {

WorldApplied apply_junctions(simulation::TrackNetwork& network, const Snapshot& snapshot) {
    WorldApplied result;
    for (const auto& j : snapshot.junctions) {
        const auto* node = network.node(j.node());
        if (!node || node->kind != formats::TrackNodeKind::junction) {
            result.problems.push_back("junction index " + std::to_string(j.index) + ": node " + std::to_string(j.node()) +
                                      " is no junction");
            continue;
        }
        if (j.branch >= node->branch_count()) {
            result.problems.push_back("junction " + std::to_string(j.node()) + ": no branch " + std::to_string(j.branch));
            continue;
        }
        if (network.throw_junction(j.node(), j.branch)) {
            ++result.applied;
        } else {
            result.problems.push_back("junction " + std::to_string(j.node()) + " would not move (occupied or held)");
        }
    }
    return result;
}

WorldApplied apply_signals(simulation::SignalSystem& signals, const Snapshot& snapshot) {
    WorldApplied result;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> by_item;
    const auto& heads = signals.heads();
    for (std::uint32_t i = 0; i < heads.size(); ++i) by_item[heads[i].track_item].push_back(i);
    for (const auto& h : snapshot.signals) {
        const auto it = by_item.find(h.item);
        if (it == by_item.end()) {
            result.problems.push_back("signal item " + std::to_string(h.item) + ": no head");
            continue;
        }
        simulation::SignalOverride value;
        const auto aspect = static_cast<int>(h.state);
        if (aspect >= 0 && aspect <= static_cast<int>(formats::SignalAspect::clear_2)) {
            value.aspect = static_cast<formats::SignalAspect>(aspect);
        }
        value.draw_state = static_cast<int>(h.draw_state);
        value.speed = h.speed;
        for (const auto head : it->second) signals.set_override(head, value);
        ++result.applied;
    }
    return result;
}

std::optional<simulation::TrackLocation> train_front(simulation::TrackNetwork& network, const SnapshotTrain& train,
                                                     std::string* error) {
    if (!train.pos) {
        if (error) *error = "no position";
        return std::nullopt;
    }
    const auto& pos = *train.pos;
    const auto* node = network.node(pos.node);
    if (!node || node->kind != formats::TrackNodeKind::vector) {
        if (error) *error = "node " + std::to_string(pos.node) + " is no vector node";
        return std::nullopt;
    }
    if (pos.offset < -0.5f || pos.offset > node->length + 0.5) {
        if (error) *error = "offset " + std::to_string(pos.offset) + " is off node " + std::to_string(pos.node);
        return std::nullopt;
    }
    simulation::TrackTraveller traveller(network, pos.node, std::clamp<double>(pos.offset, 0.0, node->length), pos.forward());
    const auto moved = traveller.move(front_distance(train));
    if (!moved.ok()) {
        if (error) *error = "the track ends before the train's front";
        return std::nullopt;
    }
    return simulation::TrackLocation{traveller.node(), traveller.offset(), traveller.forward()};
}

CarFile car_file(const TrainCar& car) {
    auto path = to_utf8(car.file);
    std::replace(path.begin(), path.end(), '/', '\\');
    CarFile out;
    const auto slash = path.rfind('\\');
    out.folder = slash == std::string::npos ? std::string{} : path.substr(0, slash);
    out.name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (const auto dot = out.name.rfind('.'); dot != std::string::npos) {
        auto extension = out.name.substr(dot + 1);
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        out.engine = extension == "eng";
        out.name.resize(dot);
    } else {
        out.engine = car.kind != 0;
    }
    return out;
}

bool set_junction(simulation::TrackNetwork& network, const JunctionState& junction) {
    const auto* node = network.node(junction.node());
    if (!node || node->kind != formats::TrackNodeKind::junction || junction.branch >= node->branch_count()) return false;
    if (network.throw_junction(junction.node(), junction.branch)) return true;
    auto* state = network.junction(junction.node());
    if (!state) return false;
    state->selected = junction.branch;
    return true;
}

SignalHeads::SignalHeads(const simulation::SignalSystem& signals) {
    const auto& heads = signals.heads();
    by_item_.reserve(heads.size());
    for (std::uint32_t i = 0; i < heads.size(); ++i) by_item_.emplace_back(heads[i].track_item, i);
    std::sort(by_item_.begin(), by_item_.end());
}

bool SignalHeads::apply(simulation::SignalSystem& signals, const SignalHeadState& head) const {
    auto it = std::lower_bound(by_item_.begin(), by_item_.end(), std::pair{head.item, std::uint32_t{0}});
    if (it == by_item_.end() || it->first != head.item) return false;
    simulation::SignalOverride value;
    const auto aspect = static_cast<int>(head.state);
    if (aspect >= 0 && aspect <= static_cast<int>(formats::SignalAspect::clear_2)) {
        value.aspect = static_cast<formats::SignalAspect>(aspect);
    }
    value.draw_state = static_cast<int>(head.draw_state);
    value.speed = head.speed;
    for (; it != by_item_.end() && it->first == head.item; ++it) signals.set_override(it->second, value);
    return true;
}

namespace {

// Walks from `from` the way it faces until `to`'s node and offset; metres.
std::optional<double> walk_to(simulation::TrackNetwork& network, const simulation::TrackLocation& from,
                              const simulation::TrackLocation& to, const double limit) {
    simulation::TrackTraveller t(network, from.node, from.offset, from.forward);
    double travelled = 0.0;
    for (int nodes = 0; nodes < 64; ++nodes) {
        const auto* node = network.node(t.node());
        if (!node) return std::nullopt;
        if (t.node() == to.node) {
            const double ahead = t.forward() ? to.offset - t.offset() : t.offset() - to.offset;
            if (ahead >= -1e-6) {
                if (t.forward() != to.forward) return std::nullopt;
                const double total = travelled + std::max(0.0, ahead);
                return total <= limit ? std::optional<double>(total) : std::nullopt;
            }
        }
        const double rest = t.forward() ? node->length - t.offset() : t.offset();
        if (travelled + rest > limit) return std::nullopt;
        // A little past the node's end puts the traveller on the next one.
        const auto moved = t.move(rest + 1e-3);
        if (!moved.ok()) return std::nullopt;
        travelled += rest + 1e-3;
    }
    return std::nullopt;
}

} // namespace

std::optional<double> track_distance(simulation::TrackNetwork& network, const simulation::TrackLocation& from,
                                     const simulation::TrackLocation& to, const double limit) {
    if (const auto ahead = walk_to(network, from, to, limit)) return *ahead;
    // Behind: walk the other way, facing back, to the target facing back.
    const simulation::TrackLocation back{from.node, from.offset, !from.forward};
    const simulation::TrackLocation target{to.node, to.offset, !to.forward};
    if (const auto behind = walk_to(network, back, target, limit)) return -*behind;
    return std::nullopt;
}

void RemoteMotion::add(simulation::TrackNetwork& network, const double time, const simulation::TrackLocation& front,
                       const float speed) {
    Sample sample{time, front, speed, std::nullopt};
    if (!samples_.empty()) {
        const auto& last = samples_.back();
        const double gap = time - last.time;
        // The interval: what the sender keeps to, smoothed.
        if (gap > 0.05 && gap < 10.0) interval_ = std::clamp(0.7 * interval_ + 0.3 * gap, 0.2, 3.0);
        // As far as the train can go in twice the gap at 400 km/h, with room.
        const double limit = 50.0 + 2.0 * std::max(gap, 0.0) * 112.0;
        sample.from_previous = track_distance(network, last.front, front, limit);
    }
    standing_ = false;
    samples_.push_back(sample);
    if (samples_.size() > 8) samples_.erase(samples_.begin());
}

void RemoteMotion::stand(const double time) {
    standing_ = true;
    if (!samples_.empty()) {
        auto last = samples_.back();
        last.time = std::max(time, last.time);
        last.speed = 0.0f;
        last.from_previous = 0.0;
        samples_.push_back(last);
        if (samples_.size() > 8) samples_.erase(samples_.begin());
    }
}

std::optional<simulation::TrackLocation> RemoteMotion::at(simulation::TrackNetwork& network, const double time) const {
    if (samples_.empty()) return std::nullopt;
    const double drawn = time - interval_;
    if (drawn <= samples_.front().time) return samples_.front().front;
    for (std::size_t i = 1; i < samples_.size(); ++i) {
        const auto& a = samples_[i - 1];
        const auto& b = samples_[i];
        if (drawn >= b.time) continue;
        if (!b.from_previous) return a.front; // not joined: wait for b and jump there
        const double span = b.time - a.time;
        const double share = span > 1e-6 ? (drawn - a.time) / span : 1.0;
        simulation::TrackTraveller t(network, a.front.node, a.front.offset, a.front.forward);
        t.move(*b.from_previous * share);
        return simulation::TrackLocation{t.node(), t.offset(), t.forward()};
    }
    // Past the last position: on at its speed for one more interval.
    const auto& last = samples_.back();
    if (standing_ || last.speed == 0.0f) return last.front;
    const double ahead = std::min(drawn - last.time, interval_) * last.speed;
    simulation::TrackTraveller t(network, last.front.node, last.front.offset, last.front.forward);
    t.move(ahead);
    return simulation::TrackLocation{t.node(), t.offset(), t.forward()};
}

float RemoteMotion::speed_at(const double time) const {
    if (samples_.empty()) return 0.0f;
    const double drawn = time - interval_;
    for (std::size_t i = 1; i < samples_.size(); ++i) {
        if (drawn < samples_[i].time) {
            const auto& a = samples_[i - 1];
            const auto& b = samples_[i];
            const double span = b.time - a.time;
            const double share = span > 1e-6 ? std::clamp((drawn - a.time) / span, 0.0, 1.0) : 1.0;
            return static_cast<float>(a.speed + (b.speed - a.speed) * share);
        }
    }
    return standing_ ? 0.0f : samples_.back().speed;
}

std::string train_name(const SnapshotTrain& train) {
    if (train.names && !(*train.names)[0].empty()) return to_utf8((*train.names)[0]);
    if (train.assign && train.assign->number != 0) return std::to_string(train.assign->number);
    return "train " + std::to_string(train.id);
}

} // namespace openrts::net
