#include <openrts/simulation/dispatcher.hpp>

#include <algorithm>
#include <cmath>
#include <set>

#include <openrts/simulation/train.hpp>

namespace openrts::simulation {
namespace {

// Comfortable stopping distance for a holder that may lose its junction, m.
[[nodiscard]] double stopping_distance(const double speed) {
    return speed * speed / (2.0 * 0.3) + 100.0;
}

} // namespace

struct Dispatcher::Follower {
    std::uint64_t id{};
    PathPlan plan;
    TrainRoute route;
    RouteProgress progress;
    std::size_t span_hint{};
    std::vector<std::size_t> held; // indices into the current leg's junctions reserved ahead
    // Vector nodes the train holds (under it and reserved ahead), with the
    // travel direction: another train's way the other way stops before them.
    std::vector<std::pair<std::uint32_t, bool>> sections;
    double stood{}; // s standing still
    double speed{}; // m/s
};

Dispatcher::Dispatcher(TrackNetwork& network, DispatcherSettings settings) : network_(&network), settings_(settings) {}

Dispatcher::~Dispatcher() = default;

void Dispatcher::assign(const std::uint64_t id, PathPlan plan, TrainRoute route) {
    clear(id);
    auto follower = std::make_unique<Follower>();
    follower->id = id;
    follower->plan = std::move(plan);
    follower->route = std::move(route);
    followers_[id] = std::move(follower);
}

void Dispatcher::clear(const std::uint64_t id) {
    const auto it = followers_.find(id);
    if (it == followers_.end()) return;
    for (const auto& leg : it->second->route.legs) {
        for (const auto& pass : leg.junctions) network_->cancel_reservation(pass.junction, id);
    }
    followers_.erase(it);
}

void Dispatcher::cancel(Follower& follower, const std::size_t from_junction) {
    if (follower.route.legs.empty()) return;
    const auto& leg = follower.route.legs[follower.progress.leg];
    std::vector<std::size_t> kept;
    for (const auto index : follower.held) {
        if (index >= from_junction && index < leg.junctions.size()) {
            network_->cancel_reservation(leg.junctions[index].junction, follower.id);
        } else {
            kept.push_back(index);
        }
    }
    follower.held = std::move(kept);
}

const PathPlan* Dispatcher::plan(const std::uint64_t id) const {
    const auto it = followers_.find(id);
    return it == followers_.end() ? nullptr : &it->second->plan;
}

const TrainRoute* Dispatcher::route(const std::uint64_t id) const {
    const auto it = followers_.find(id);
    return it == followers_.end() ? nullptr : &it->second->route;
}

const RouteProgress* Dispatcher::progress(const std::uint64_t id) const {
    const auto it = followers_.find(id);
    return it == followers_.end() ? nullptr : &it->second->progress;
}

std::optional<double> Dispatcher::authority_ahead(const std::uint64_t id) const {
    const auto it = followers_.find(id);
    if (it == followers_.end() || !it->second->progress.on_route) return std::nullopt;
    const auto& p = it->second->progress;
    return (std::max)(0.0, p.authority - p.head);
}

bool Dispatcher::next_leg(const std::uint64_t id) {
    const auto it = followers_.find(id);
    if (it == followers_.end()) return false;
    auto& f = *it->second;
    if (f.progress.leg + 1 >= f.route.legs.size()) return false;
    cancel(f, 0);
    ++f.progress.leg;
    f.progress.next_mark = 0;
    f.span_hint = 0;
    f.held.clear();
    f.stood = 0.0;
    return true;
}

void Dispatcher::pass_mark(const std::uint64_t id) {
    const auto it = followers_.find(id);
    if (it != followers_.end()) ++it->second->progress.next_mark;
}

std::vector<std::uint64_t> Dispatcher::routed() const {
    std::vector<std::uint64_t> ids;
    for (const auto& [id, _] : followers_) ids.push_back(id);
    return ids;
}

std::optional<double> Dispatcher::eta(const Follower& follower, const std::uint32_t junction, const double speed) const {
    if (!follower.progress.on_route || follower.route.legs.empty()) return std::nullopt;
    const auto& leg = follower.route.legs[follower.progress.leg];
    for (const auto& pass : leg.junctions) {
        if (pass.junction != junction || pass.distance <= follower.progress.head) continue;
        return (pass.distance - follower.progress.head) / (std::max)(speed, 2.0);
    }
    return std::nullopt;
}

void Dispatcher::update(const std::span<const Train* const> trains) {
    std::map<std::uint64_t, const Train*> by_id;
    for (const auto* train : trains) {
        if (train) by_id[train->id()] = train;
    }
    for (auto it = followers_.begin(); it != followers_.end();) {
        if (by_id.contains(it->first)) {
            ++it;
            continue;
        }
        const auto id = it->first;
        ++it;
        clear(id);
    }
    for (auto& [id, follower] : followers_) {
        auto& f = *follower;
        const auto* train = by_id[id];
        auto& p = f.progress;
        f.speed = std::abs(train->speed());
        f.stood = f.speed < 0.1 ? f.stood + train_step : 0.0;
        if (!train->placed() || f.route.legs.empty()) {
            p.on_route = false;
            continue;
        }
        // Where the train is on its leg.
        const auto locate = [&](const RouteLeg& leg, std::size_t& hint, double& head, double& tail, bool& front_leads) {
            const auto front = train->front_end();
            const auto rear = train->rear_end();
            std::optional<double> at_front, at_rear;
            if (front) at_front = leg.locate(front->node(), front->offset(), hint, &hint);
            if (rear) at_rear = leg.locate(rear->node(), rear->offset(), hint);
            if (!at_front && !at_rear) return false;
            if (at_front && at_rear) {
                front_leads = *at_front >= *at_rear;
                head = (std::max)(*at_front, *at_rear);
                tail = (std::min)(*at_front, *at_rear);
            } else {
                front_leads = at_front.has_value();
                head = at_front ? *at_front : *at_rear;
                tail = (std::max)(0.0, head - train->length());
            }
            return true;
        };
        p.on_route = locate(f.route.legs[p.leg], f.span_hint, p.head, p.tail, p.front_leads);
        if (!p.on_route) {
            cancel(f, 0);
            f.sections.clear();
            continue;
        }
        const auto& leg = f.route.legs[p.leg];
        // Marks reached: points to run through as the head passes them,
        // waiting points once passed (the driver passes them after the wait).
        while (p.next_mark < leg.marks.size()) {
            const auto& mark = leg.marks[p.next_mark];
            const bool passed = mark.kind == RouteMarkKind::via ? p.head >= mark.distance - 1.0 : p.head > mark.distance + 5.0;
            if (!passed || &mark == &leg.marks.back()) break;
            ++p.next_mark;
        }
        // The leg's end: standing near a reverse point continues on the next
        // leg; standing at the end point finishes the path.
        const bool at_end = p.head >= leg.length() - 30.0 && f.stood >= 1.0;
        if (at_end && !leg.marks.empty()) {
            if (leg.marks.back().kind == RouteMarkKind::reverse && p.leg + 1 < f.route.legs.size()) {
                next_leg(id);
                p.on_route = locate(f.route.legs[p.leg], f.span_hint, p.head, p.tail, p.front_leads);
                if (!p.on_route) continue;
            } else if (leg.marks.back().kind == RouteMarkKind::end && p.head >= leg.length() - 20.0) {
                p.finished = true;
                p.next_mark = leg.marks.size();
            }
        }
        reserve(f, *train, trains);
    }
}

void Dispatcher::reserve(Follower& f, const Train& train, const std::span<const Train* const> trains) {
    auto& p = f.progress;
    const auto& leg = f.route.legs[p.leg];
    const auto& junctions = leg.junctions;
    p.authority = leg.length();
    p.blocked_at.reset();
    p.blocked_by = 0;
    const double reach = p.head + settings_.horizon;
    const auto first = static_cast<std::size_t>(
        std::upper_bound(junctions.begin(), junctions.end(), p.head + 0.01,
                         [](const double d, const RouteJunction& pass) { return d < pass.distance; })
        - junctions.begin());

    // The nodes of the leg between two leg distances (spans that only touch
    // an end do not count).
    const auto section = [&](const double from, const double to) {
        std::vector<std::pair<std::uint32_t, bool>> nodes;
        for (const auto& span : leg.spans) {
            const double end = span.start + span.length();
            if (end <= from + 1e-3 || span.start >= to - 1e-3) continue;
            nodes.emplace_back(span.node, span.forward);
        }
        return nodes;
    };
    // Another routed train holds or is reserved over one of these nodes the
    // other way.
    const auto opposed = [&](const std::vector<std::pair<std::uint32_t, bool>>& nodes) -> std::optional<std::uint64_t> {
        for (const auto& [other_id, other] : followers_) {
            if (other_id == f.id) continue;
            for (const auto& [node, forward] : nodes) {
                for (const auto& [held_node, held_forward] : other->sections) {
                    if (held_node == node && held_forward != forward) return other_id;
                }
            }
        }
        return std::nullopt;
    };
    // The holder's way runs over these nodes the other way (it will want them).
    const auto wants_against = [&](const std::uint64_t holder, const std::vector<std::pair<std::uint32_t, bool>>& nodes) {
        const auto it = followers_.find(holder);
        if (it == followers_.end() || it->second->route.legs.empty()) return false;
        const auto& g = *it->second;
        const auto& other_leg = g.route.legs[g.progress.leg];
        for (const auto& span : other_leg.spans) {
            if (span.start + span.length() < g.progress.tail) continue;
            for (const auto& [node, forward] : nodes) {
                if (span.node == node && span.forward != forward) return true;
            }
        }
        return false;
    };
    const auto speed_of = [&](const std::uint64_t id) {
        for (const auto* t : trains) {
            if (t && t->id() == id) return std::abs(t->speed());
        }
        return 0.0;
    };

    std::vector<std::size_t> now_held;
    std::optional<std::size_t> blocked;
    bool section_in_use = false; // blocked by a train on the way beyond: give the junction up
    for (std::size_t j = first; j < junctions.size(); ++j) {
        const auto& pass = junctions[j];
        if (pass.distance > reach) {
            p.authority = pass.distance;
            break;
        }
        auto* state = network_->junction(pass.junction);
        if (!state) continue;
        // The way beyond this junction, up to the next one.
        const double next = j + 1 < junctions.size() ? junctions[j + 1].distance : leg.length();
        const auto beyond = section(pass.distance, next);
        if (const auto against = opposed(beyond)) {
            p.authority = pass.distance;
            p.blocked_at = pass.junction;
            p.blocked_by = *against;
            blocked = j;
            section_in_use = true;
            break;
        }
        auto& queue = state->reservations;
        auto mine = std::find_if(queue.begin(), queue.end(), [&](const auto& r) { return r.first == f.id; });
        if (mine == queue.end()) {
            queue.emplace_back(f.id, pass.branch);
        } else {
            mine->second = pass.branch;
        }
        if (queue.front().first != f.id && !state->occupied) {
            // Take the junction over when this train gets there clearly
            // earlier and the holder is still far off (0x48C924).
            const auto holder = queue.front().first;
            const auto holder_it = followers_.find(holder);
            const double my_eta = (pass.distance - p.head) / (std::max)(f.speed, 2.0);
            const double holder_speed = speed_of(holder);
            const auto holder_eta = holder_it == followers_.end() ? std::nullopt : eta(*holder_it->second, pass.junction, holder_speed);
            bool take = !holder_eta; // a stale entry: the holder no longer comes here
            if (holder_eta && my_eta + settings_.takeover_margin < *holder_eta) {
                const auto& g = holder_it->second->progress;
                double holder_distance = 0.0;
                for (const auto& other_pass : holder_it->second->route.legs[g.leg].junctions) {
                    if (other_pass.junction == pass.junction && other_pass.distance > g.head) {
                        holder_distance = other_pass.distance - g.head;
                        break;
                    }
                }
                take = holder_distance > stopping_distance(holder_speed);
            }
            if (take) {
                const auto entry = std::find_if(queue.begin(), queue.end(), [&](const auto& r) { return r.first == f.id; });
                const auto moved = *entry;
                queue.erase(entry);
                queue.insert(queue.begin(), moved);
            }
        }
        if (queue.front().first != f.id) {
            p.authority = pass.distance;
            p.blocked_at = pass.junction;
            p.blocked_by = queue.front().first;
            blocked = j;
            break;
        }
        if (!state->occupied) {
            state->selected = pass.branch;
        } else if (state->selected != pass.branch) {
            // Under another train and set the other way.
            p.authority = pass.distance;
            p.blocked_at = pass.junction;
            blocked = j;
            break;
        }
        now_held.push_back(j);
    }
    // Blocked by a train whose way comes through the stretch before the
    // blocking junction the other way: do not enter that stretch (a single
    // line between two junctions), stop at the junction before it.
    if (blocked && p.blocked_by != 0 && *blocked > first) {
        const auto before = *blocked - 1;
        if (junctions[before].distance > p.head
            && wants_against(p.blocked_by, section(junctions[before].distance, junctions[*blocked].distance))) {
            network_->cancel_reservation(junctions[before].junction, f.id);
            std::erase(now_held, before);
            p.authority = junctions[before].distance;
            p.blocked_at = junctions[before].junction;
        }
    }
    // Reservations ahead that the way no longer reaches are given up; the
    // queue place at the blocking junction is kept.
    for (const auto index : f.held) {
        if (index < first || index >= junctions.size()) continue;
        if (std::find(now_held.begin(), now_held.end(), index) != now_held.end()) continue;
        if (blocked && index == *blocked && !section_in_use && p.blocked_at == junctions[index].junction) continue;
        network_->cancel_reservation(junctions[index].junction, f.id);
    }
    // A train waiting for the way beyond a junction lets others have it
    // meanwhile (they may need it to clear that way).
    if (blocked && section_in_use) network_->cancel_reservation(junctions[*blocked].junction, f.id);
    f.held = std::move(now_held);
    if (blocked && !section_in_use && p.blocked_at == junctions[*blocked].junction) f.held.push_back(*blocked);
    f.sections = section(p.tail, p.authority);
    (void)train;
}

} // namespace openrts::simulation
