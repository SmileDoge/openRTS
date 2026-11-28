#include <openrts/net/world.hpp>

#include <algorithm>
#include <cctype>
#include <unordered_map>

#include <openrts/simulation/signals.hpp>
#include <openrts/simulation/track_network.hpp>
#include <openrts/simulation/track_traveller.hpp>
#include <openrts/simulation/train.hpp>

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

std::vector<std::uint32_t> junctions_under(simulation::TrackNetwork& network, const simulation::Train& train) {
    const double ahead = 5.0 + std::min(std::abs(train.speed()) * 4.0, 150.0);
    std::vector<std::uint32_t> out;
    const auto rear = train.rear_end();
    if (!rear || !train.placed()) return out;
    // From the rear towards the front (as position_of and train_front take it).
    simulation::TrackTraveller t(network, rear->node(), rear->offset(), rear->forward());
    const auto moved = t.move(train.length() + ahead, simulation::JunctionMode::plain);
    for (const auto& pass : moved.junctions) out.push_back(pass.junction);
    return out;
}

std::optional<TrainPos> position_of(const simulation::Train& train) {
    const auto rear = train.rear_end();
    const auto& cars = train.cars();
    if (!rear || cars.empty() || !cars.back().definition) return std::nullopt;
    TrainPos pos;
    pos.node = rear->node();
    pos.offset = static_cast<float>(rear->offset());
    pos.direction = rear->forward() ? 0 : 1;
    pos.speed = cars.front().velocity;
    pos.acceleration = cars.front().acceleration;
    // openRTS counts car middles from the last car's middle; RTS from the
    // rear end, half the last car further back.
    const double half_last = 0.5 * cars.back().definition->length;
    for (const auto& car : cars) pos.cars.push_back({car.offset + half_last, car.velocity, car.acceleration});
    return pos;
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
    // After timed ones (the server's fast positions switched off): kept, the
    // clock eased back to the longer delay instead of jumping.
    const bool after_timed = timed_ || last_timed_;
    untimed();
    last_timed_ = false;
    Sample sample{time, front, speed, 0.0f, std::nullopt};
    if (!samples_.empty()) {
        const auto& last = samples_.back();
        const double gap = time - last.time;
        // The interval: what the sender keeps to, smoothed. RTS also sends a
        // position out of turn (stopping, starting: 0.06 s after the last in
        // the live capture); those gaps say nothing of the interval. A
        // server driving TRAFFIC every 0.5 s still counts. Nor does a gap
        // from a timed one, or from where it stood (a train that stood a
        // minute and set off is not sent every minute: the delay grew to
        // 3 s).
        const bool counts = !after_timed && last.speed != 0.0f;
        if (counts && gap > 0.45 && gap < 10.0) interval_ = std::clamp(0.7 * interval_ + 0.3 * gap, 0.4, 3.0);
        // As far as the train can go in twice the gap at 400 km/h, with room.
        const double limit = 50.0 + 2.0 * std::max(gap, 0.0) * 112.0;
        sample.from_previous = track_distance(network, last.front, front, limit);
        // The time it was there: by the way covered at the speeds both
        // packages give, not by when the package came (it comes early or
        // late by a few tenths of a second: the train sped up and slowed
        // down between them, 74 to 98 km/h at 86; testers 2026-11-21),
        // pulled a little towards the arrival so the clocks do not drift.
        const double mean = 0.5 * (std::abs(last.speed) + std::abs(speed));
        if (sample.from_previous && mean > 1.0 && *sample.from_previous * speed >= 0.0 && *sample.from_previous * last.speed >= 0.0) {
            const double by_way = last.time + std::abs(*sample.from_previous) / mean;
            if (std::abs(by_way - time) < 1.0) sample.time = by_way + 0.15 * (time - by_way);
        }
    }
    if (!samples_.empty()) sample.time = std::max(sample.time, samples_.back().time + 1e-3);
    standing_ = false;
    samples_.push_back(sample);
    if (samples_.size() > 8) samples_.erase(samples_.begin());
}

void RemoteMotion::add_timed(simulation::TrackNetwork& network, double time, const simulation::TrackLocation& front, const float speed,
                             const float acceleration) {
    // After RTS ones (switched on, or the train set off again): kept, the
    // clock eased forward to the shorter delay instead of jumping.
    if (!timed_) {
        timed_ = true;
        easing_ = !samples_.empty();
    }
    last_timed_ = true;
    // Late (by UDP): put among the others in time, the ways from and to its
    // neighbours measured again; too old to be drawn any more, dropped.
    if (!samples_.empty() && time < samples_.back().time - 1e-4) {
        if (time <= samples_.front().time) return;
        auto at = std::upper_bound(samples_.begin(), samples_.end(), time, [](const double t, const Sample& s) { return t < s.time; });
        const auto& before = *(at - 1);
        if (std::abs(before.time - time) < 1e-4) return;
        Sample late{time, front, speed, acceleration, std::nullopt};
        late.from_previous = track_distance(network, before.front, front, 50.0 + 2.0 * (time - before.time) * 112.0);
        at = samples_.insert(at, late);
        auto& next = *(at + 1);
        next.from_previous = track_distance(network, front, next.front, 50.0 + 2.0 * (next.time - time) * 112.0);
        return;
    }
    if (!samples_.empty() && std::abs(samples_.back().time - time) < 1e-4) return;
    if (!samples_.empty()) time = std::max(time, samples_.back().time + 1e-3);
    Sample sample{time, front, speed, acceleration, std::nullopt};
    if (!samples_.empty()) {
        const auto& last = samples_.back();
        // As far as the train can go in twice the gap at 400 km/h, with room.
        const double limit = 50.0 + 2.0 * (time - last.time) * 112.0;
        sample.from_previous = track_distance(network, last.front, front, limit);
    }
    standing_ = false;
    samples_.push_back(sample);
    // Three seconds of them (20 a second by UDP): the delay stays under it,
    // also while it eases down from the RTS positions' 1.75 s.
    if (samples_.size() > 64) samples_.erase(samples_.begin());
}

void RemoteMotion::untimed() {
    if (!timed_) return;
    timed_ = false;
    easing_ = true;
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

namespace {

// The way between two positions `span` s apart, `distance` m, left at
// `speed_a` and reached at `speed_b` (m/s): a cubic (Hermite) curve, so the
// speed runs on smoothly from one package to the next instead of jumping at
// each. Speeds that do not fit the way (the other sign) give a straight
// line. Metres from the first position and the speed at `share` 0..1.
std::pair<double, double> between(const double distance, const double speed_a, const double speed_b, const double span,
                                   const double share) {
    const double s = std::clamp(share, 0.0, 1.0);
    if (span <= 1e-6) return {distance, speed_b};
    const bool fits = std::abs(distance) < 0.05 || (distance * speed_a >= 0.0 && distance * speed_b >= 0.0);
    if (!fits) return {distance * s, distance / span};
    const double s2 = s * s;
    const double s3 = s2 * s;
    const double h10 = s3 - 2.0 * s2 + s;
    const double h01 = -2.0 * s3 + 3.0 * s2;
    const double h11 = s3 - s2;
    const double way = h10 * span * speed_a + h01 * distance + h11 * span * speed_b;
    const double d10 = 3.0 * s2 - 4.0 * s + 1.0;
    const double d01 = -6.0 * s2 + 6.0 * s;
    const double d11 = 3.0 * s2 - 2.0 * s;
    const double speed = d10 * speed_a + d01 * distance / span + d11 * speed_b;
    return {way, speed};
}

} // namespace

void RemoteMotion::tick(const double now) {
    const double target = now - delay();
    if (!ticked_ || now < tick_now_) {
        drawn_ = target;
        easing_ = false;
    } else {
        const double step = now - tick_now_;
        const double off = target - (drawn_ + step);
        if (easing_ && std::abs(off) < 5.0) {
            // Between timed and RTS positions: 10 % faster or slower until
            // the clock trails by the new delay (1.55 s: some 15 s).
            drawn_ += step * std::clamp(1.0 + off / std::max(step, 1e-3), 0.9, 1.1);
            if (std::abs(off) < 0.02) easing_ = false;
            rate_ = 1.0;
        } else if (std::abs(off) > 0.5) {
            drawn_ = target;
            easing_ = false;
        } else if (timed_) {
            // Timed positions: their delay moves by hundredths of a second.
            // The clock eases towards it, at most 2 % off (a second's off
            // a percent), its pace changing over half a second: a steady
            // 12 m/s was drawn 11.8..12.7, jerking by 0.24 m/s in a frame.
            const double target = 1.0 + std::clamp(off, -0.02, 0.02);
            rate_ += (target - rate_) * std::min(step / 0.5, 1.0);
            drawn_ += step * rate_;
        } else {
            const double rate = std::clamp(1.0 + off / std::max(step, 1e-3), 0.9, 1.1);
            drawn_ += step * rate;
        }
    }
    ticked_ = true;
    tick_now_ = now;
}

std::optional<simulation::TrackLocation> RemoteMotion::at(simulation::TrackNetwork& network, const double time) const {
    if (samples_.empty()) return std::nullopt;
    const double drawn = this->drawn(time);
    if (drawn <= samples_.front().time) return samples_.front().front;
    for (std::size_t i = 1; i < samples_.size(); ++i) {
        const auto& a = samples_[i - 1];
        const auto& b = samples_[i];
        if (drawn >= b.time) continue;
        if (!b.from_previous) return a.front; // not joined: wait for b and jump there
        const auto [way, speed] = between(*b.from_previous, a.speed, b.speed, b.time - a.time, (drawn - a.time) / (b.time - a.time));
        simulation::TrackTraveller t(network, a.front.node, a.front.offset, a.front.forward);
        t.move(way);
        return simulation::TrackLocation{t.node(), t.offset(), t.forward()};
    }
    // Past the last position: on at its speed for one more interval (timed:
    // half a second, at its acceleration too, never turning back).
    const auto& last = samples_.back();
    if (standing_ || last.speed == 0.0f) return last.front;
    const double past = std::min(drawn - last.time, timed_ ? 0.5 : interval_);
    double ahead = past * last.speed;
    if (timed_) {
        const double with = ahead + 0.5 * last.acceleration * past * past;
        ahead = with * ahead > 0.0 ? with : 0.0;
    }
    simulation::TrackTraveller t(network, last.front.node, last.front.offset, last.front.forward);
    t.move(ahead);
    return simulation::TrackLocation{t.node(), t.offset(), t.forward()};
}

float RemoteMotion::speed_at(const double time) const {
    if (samples_.empty()) return 0.0f;
    const double drawn = this->drawn(time);
    for (std::size_t i = 1; i < samples_.size(); ++i) {
        const auto& a = samples_[i - 1];
        const auto& b = samples_[i];
        if (drawn >= b.time) continue;
        if (drawn <= a.time || !b.from_previous) return a.speed;
        const auto [way, speed] = between(*b.from_previous, a.speed, b.speed, b.time - a.time, (drawn - a.time) / (b.time - a.time));
        static_cast<void>(way);
        return static_cast<float>(speed);
    }
    return standing_ ? 0.0f : samples_.back().speed;
}

std::string train_name(const SnapshotTrain& train) {
    if (train.names && !(*train.names)[0].empty()) return to_utf8((*train.names)[0]);
    if (train.assign && train.assign->number != 0) return std::to_string(train.assign->number);
    return "train " + std::to_string(train.id);
}

} // namespace openrts::net
