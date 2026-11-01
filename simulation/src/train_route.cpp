#include <openrts/simulation/train_route.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <limits>
#include <queue>
#include <set>

namespace openrts::simulation {
namespace {

struct Step {
    std::uint32_t junction{};
    bool facing = true;
    std::uint32_t branch{};
};

// One way out of a vector node to the next vector node.
struct Transition {
    std::uint32_t node{};
    bool forward = true;
    double offset{}; // where it enters the new node
    std::vector<Step> steps;
};

// Every way out of vector node `from` at its start (end 0) or end (end 1),
// through any chain of junctions: all branches of a facing junction (only
// the chosen one when `branches` names it), the trunk of a trailing one.
// Pins as in TrackTraveller: to a vector node 1 = its start, 0 = its end;
// to a junction 1 = the trunk side, 0 = a branch side.
void transitions(const TrackNetwork& network, const std::uint32_t from, const int end,
                 const std::map<std::uint32_t, std::uint32_t>& branches, std::vector<Transition>& out) {
    const auto* node = network.node(from);
    if (!node || node->pins.size() < 2) return;
    std::vector<Step> steps;
    const std::function<void(const formats::TrackPin&, std::uint32_t, bool, int, int)> walk =
        [&](const formats::TrackPin& pin, const std::uint32_t previous, const bool previous_is_vector, const int previous_end,
            const int depth) {
            if (depth > 32) return;
            const auto* target = network.node(pin.node);
            if (!target) return;
            switch (target->kind) {
            case formats::TrackNodeKind::vector:
                out.push_back({target->index, pin.direction == 1, pin.direction == 1 ? 0.0 : target->length, steps});
                return;
            case formats::TrackNodeKind::end:
                return;
            case formats::TrackNodeKind::junction: {
                if (target->pins.size() < 2) return;
                if (pin.direction == 1) {
                    const auto forced = branches.find(target->index);
                    for (std::uint32_t b = 0; b + 1 < target->pins.size(); ++b) {
                        if (forced != branches.end() && forced->second != b) continue;
                        steps.push_back({target->index, true, b});
                        walk(target->pins[1 + b], target->index, false, 0, depth + 1);
                        steps.pop_back();
                    }
                } else {
                    std::optional<std::uint32_t> arrived;
                    for (std::size_t k = 1; k < target->pins.size(); ++k) {
                        const auto& back = target->pins[k];
                        if (back.node != previous) continue;
                        if (previous_is_vector && back.direction != static_cast<std::uint32_t>(previous_end == 1 ? 0 : 1)) {
                            continue;
                        }
                        arrived = static_cast<std::uint32_t>(k - 1);
                        break;
                    }
                    if (!arrived) return;
                    steps.push_back({target->index, false, *arrived});
                    walk(target->pins[0], target->index, false, 0, depth + 1);
                    steps.pop_back();
                }
                return;
            }
            }
        };
    walk(node->pins[static_cast<std::size_t>(end)], from, true, end, 0);
}

// The way between two places, ready to append to a leg.
struct Way {
    std::vector<RouteSpan> spans; // `start` relative to the way's start
    std::vector<std::pair<Step, double>> junctions;
    TrackLocation arrival;
    double length{};
};

// A facing junction taken off its main route costs this much more than
// its length, m. Measured on the installed routes (openrts-path-check
// --replan: every .pat planned again from its start and end only): without
// it 2 457 of the authors' 4 781 junction passes come out the same, with
// 25 m 3 401, with 100 m 3 409, with 300 m 3 427 (the rest are mostly the
// station tracks the authors chose). Penalising running over a node that has
// signals for the other direction only (the other track of a double line)
// made it worse on some routes and better on others, so it is not used.
constexpr double diverging_cost = 100.0;

// Way from `from` to (node, offset), arriving in any direction: the
// cheapest, a facing junction off its main route costing `diverging_cost`
// more (straight through a station and on the line's own track rather than
// a marginally shorter way over crossovers). RTS re-routes between Track
// control's points along the junctions' main routes (0x48BBF0). With
// `rules.through` the way must pass exactly those junctions in order.
std::optional<Way> find_way(const TrackNetwork& network, const TrackLocation& from, const std::uint32_t target_node,
                            const double target_offset, const PlanPoint& rules) {
    const auto* start = network.node(from.node);
    const auto* goal = network.node(target_node);
    if (!start || !goal || start->kind != formats::TrackNodeKind::vector || goal->kind != formats::TrackNodeKind::vector) {
        return std::nullopt;
    }
    const auto& through = rules.through;
    // How far along `through` a transition gets from k; nothing if it breaks the list.
    const auto advance = [&](const std::vector<Step>& steps, std::size_t k) -> std::optional<std::size_t> {
        if (through.empty()) return k;
        for (const auto& step : steps) {
            if (k >= through.size() || through[k] != step.junction) return std::nullopt;
            ++k;
        }
        return k;
    };

    struct State {
        std::uint32_t node{};
        bool forward = true;
        std::size_t k{};
        double entry{}; // offset where the way enters the node
        double distance{};
        std::ptrdiff_t parent = -1;
        std::vector<Step> steps; // junctions passed to get here
    };
    std::vector<State> states;
    using Entry = std::pair<double, std::size_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
    std::map<std::tuple<std::uint32_t, bool, std::size_t>, double> settled;

    double best = std::numeric_limits<double>::infinity();
    std::ptrdiff_t best_state = -2; // -1: directly on the start node
    // On the start node itself.
    if (from.node == target_node && through.empty()) {
        const bool ahead = from.forward ? target_offset >= from.offset - 1e-6 : target_offset <= from.offset + 1e-6;
        if (ahead) {
            best = std::abs(target_offset - from.offset);
            best_state = -1;
        }
    }
    std::vector<Transition> out;
    const auto expand = [&](const std::uint32_t node, const bool forward, const std::size_t k, const double leave,
                            const std::ptrdiff_t parent) {
        out.clear();
        transitions(network, node, forward ? 1 : 0, rules.branches, out);
        for (auto& t : out) {
            const auto next_k = advance(t.steps, k);
            if (!next_k) continue;
            double cost = leave;
            for (const auto& step : t.steps) {
                const auto* state = network.junction(step.junction);
                if (step.facing && state && step.branch != state->main_route) cost += diverging_cost;
            }
            states.push_back({t.node, t.forward, *next_k, t.offset, cost, parent, std::move(t.steps)});
            open.push({cost, states.size() - 1});
        }
    };
    expand(from.node, from.forward, 0, from.forward ? start->length - from.offset : from.offset, -1);
    for (std::size_t guard = 0; !open.empty() && guard < 200000; ++guard) {
        const auto [cost, index] = open.top();
        open.pop();
        if (cost > best) break;
        const auto key = std::tuple{states[index].node, states[index].forward, states[index].k};
        if (const auto it = settled.find(key); it != settled.end() && it->second <= cost) continue;
        settled[key] = cost;
        const auto& state = states[index];
        const auto* info = network.node(state.node);
        if (!info) continue;
        if (state.node == target_node && state.k == through.size()) {
            const double along = state.forward ? target_offset - state.entry : state.entry - target_offset;
            const double total = cost + (std::max)(along, 0.0);
            if (along >= -1e-6 && total < best) {
                best = total;
                best_state = static_cast<std::ptrdiff_t>(index);
            }
        }
        expand(state.node, state.forward, state.k, cost + info->length,
               static_cast<std::ptrdiff_t>(index));
    }
    if (best_state == -2) return std::nullopt;

    Way way;
    if (best_state == -1) {
        way.spans.push_back({from.node, from.forward, from.offset, target_offset, 0.0});
        way.arrival = {target_node, target_offset, from.forward};
        way.length = std::abs(target_offset - from.offset);
        return way;
    }
    std::vector<std::size_t> chain;
    for (auto at = best_state; at >= 0; at = states[static_cast<std::size_t>(at)].parent) {
        chain.push_back(static_cast<std::size_t>(at));
    }
    std::reverse(chain.begin(), chain.end());
    way.spans.push_back({from.node, from.forward, from.offset, from.forward ? start->length : 0.0, 0.0});
    double at = way.spans.back().length();
    for (std::size_t c = 0; c < chain.size(); ++c) {
        const auto& state = states[chain[c]];
        for (const auto& step : state.steps) way.junctions.push_back({step, at});
        const auto* info = network.node(state.node);
        const bool last = c + 1 == chain.size();
        const double to = last ? target_offset : (state.forward ? info->length : 0.0);
        way.spans.push_back({state.node, state.forward, state.entry, to, at});
        at += way.spans.back().length();
    }
    way.arrival = {target_node, target_offset, states[chain.back()].forward};
    way.length = at;
    return way;
}

[[nodiscard]] RouteMarkKind mark_kind(const PlanPointKind kind) {
    switch (kind) {
    case PlanPointKind::wait: return RouteMarkKind::wait;
    case PlanPointKind::reverse: return RouteMarkKind::reverse;
    case PlanPointKind::end: return RouteMarkKind::end;
    case PlanPointKind::via: break;
    }
    return RouteMarkKind::via;
}

[[nodiscard]] TrackPoint track_point(const formats::RoutePosition& position) {
    return {.tile_x = position.tile_x, .tile_z = position.tile_z, .x = position.x, .y = position.y, .z = position.z};
}

[[nodiscard]] formats::RoutePosition route_position(const TrackPoint& point) {
    return {.tile_x = point.tile_x, .tile_z = point.tile_z, .x = point.x, .y = point.y, .z = point.z};
}

} // namespace

double RouteLeg::length() const noexcept {
    return spans.empty() ? 0.0 : spans.back().start + spans.back().length();
}

std::optional<double> RouteLeg::locate(const std::uint32_t node, const double offset, const std::size_t hint,
                                       std::size_t* span) const {
    constexpr double tolerance = 0.5;
    const auto n = spans.size();
    for (std::size_t i = 0; i < n; ++i) {
        const auto k = (hint + i) % n;
        const auto& s = spans[k];
        if (s.node != node) continue;
        const double low = (std::min)(s.from, s.to);
        const double high = (std::max)(s.from, s.to);
        if (offset < low - tolerance || offset > high + tolerance) continue;
        if (span) *span = k;
        const double along = std::clamp(s.forward ? offset - s.from : s.from - offset, 0.0, s.length());
        return s.start + along;
    }
    return std::nullopt;
}

std::optional<TrackLocation> RouteLeg::at(double distance) const {
    if (spans.empty()) return std::nullopt;
    distance = std::clamp(distance, 0.0, length());
    for (const auto& s : spans) {
        if (distance > s.start + s.length() + 1e-9 && &s != &spans.back()) continue;
        const double along = std::clamp(distance - s.start, 0.0, s.length());
        return TrackLocation{s.node, s.forward ? s.from + along : s.from - along, s.forward};
    }
    return std::nullopt;
}

TrainRoute build_route(const TrackNetwork& network, const PathPlan& plan) {
    TrainRoute route;
    route.name = plan.name;
    const auto* start = network.node(plan.start.node);
    if (!start || start->kind != formats::TrackNodeKind::vector) {
        route.error = "the start is not on a track";
        return route;
    }
    if (plan.points.empty()) {
        route.error = "the path has no end point";
        return route;
    }
    RouteLeg leg;
    double distance = 0.0;
    TrackLocation at = plan.start;
    for (std::size_t i = 0; i < plan.points.size(); ++i) {
        const auto& point = plan.points[i];
        const auto way = find_way(network, at, point.node, point.offset, point);
        if (!way) {
            route.error = "no way to point " + std::to_string(i + 1) + " (node " + std::to_string(point.node) + ")";
            return route;
        }
        for (const auto& span : way->spans) {
            auto shifted = span;
            shifted.start += distance;
            if (!leg.spans.empty()) {
                auto& last = leg.spans.back();
                if (last.node == shifted.node && last.forward == shifted.forward && std::abs(last.to - shifted.from) < 1e-6) {
                    last.to = shifted.to;
                    continue;
                }
            }
            if (shifted.length() < 1e-9 && !leg.spans.empty()) continue; // zero-length entries at node ends
            leg.spans.push_back(shifted);
        }
        for (const auto& [step, along] : way->junctions) {
            leg.junctions.push_back({step.junction, step.branch, step.facing, distance + along, i});
        }
        distance += way->length;
        leg.marks.push_back({mark_kind(point.kind), distance, point.wait, i});
        at = way->arrival;
        if (point.kind == PlanPointKind::end) break;
        if (point.kind == PlanPointKind::reverse && i + 1 < plan.points.size()) {
            route.legs.push_back(std::move(leg));
            leg = {};
            distance = 0.0;
            at.forward = !at.forward;
        }
    }
    if (!leg.spans.empty()) route.legs.push_back(std::move(leg));
    if (route.legs.empty()) route.error = "the path has no track";
    return route;
}

std::optional<std::uint32_t> junction_near(const TrackNetwork& network, const TrackPoint& point, const double tolerance) {
    std::optional<std::uint32_t> best;
    double best_distance = tolerance;
    for (std::uint32_t n = 1; n < network.node_count(); ++n) {
        const auto* node = network.node(n);
        if (!node || node->kind != formats::TrackNodeKind::junction) continue;
        const auto position = network.node_position(n);
        if (!position) continue;
        const double d = distance(*position, point);
        if (d <= best_distance) {
            best_distance = d;
            best = n;
        }
    }
    return best;
}

std::optional<PathPlan> plan_from_path(const TrackNetwork& network, const formats::TrainPath& path, std::string* error) {
    const auto fail = [&](std::string message) -> std::optional<PathPlan> {
        if (error) *error = std::move(message);
        return std::nullopt;
    };
    if (!path.ok()) return fail(path.error.empty() ? "empty path" : path.error);
    // The main line of nodes from the first one.
    std::vector<std::uint32_t> order;
    std::set<std::uint32_t> seen;
    for (std::uint32_t at = 0; at < path.nodes.size() && seen.insert(at).second; at = path.nodes[at].next) {
        order.push_back(at);
        if (path.nodes[at].next == formats::PathNode::invalid) break;
    }
    struct Resolved {
        bool junction = false;
        std::uint32_t node{};
        double offset{};
        const formats::PathNode* source = nullptr;
    };
    std::vector<Resolved> points;
    for (const auto index : order) {
        const auto& source = path.nodes[index];
        const auto& pdp = path.points[source.point];
        const auto where = track_point(pdp.position);
        Resolved resolved{.source = &source};
        if (pdp.junction()) {
            auto junction = junction_near(network, where, 3.0);
            if (!junction) junction = junction_near(network, where, 8.0);
            if (!junction) return fail("no junction at path point " + std::to_string(index));
            resolved.junction = true;
            resolved.node = *junction;
        } else {
            const auto location = network.locate(where, 10.0);
            if (!location) return fail("no track at path point " + std::to_string(index));
            resolved.node = location->node;
            resolved.offset = location->offset;
        }
        points.push_back(resolved);
    }
    if (points.size() < 2) return fail("the path has fewer than two points");
    if (points.front().junction) return fail("the path starts at a junction");
    if (points.back().junction) return fail("the path ends at a junction");

    PathPlan plan;
    plan.name = path.name.empty() ? (path.id.empty() ? path.file : path.id) : path.name;
    plan.start = {points.front().node, points.front().offset, true};
    std::vector<std::uint32_t> through;
    std::map<std::uint32_t, std::uint32_t> branches;
    for (std::size_t i = 1; i < points.size(); ++i) {
        const auto& point = points[i];
        if (point.junction) {
            through.push_back(point.node);
            // The branch towards the next point, as RTS picks it: the last
            // branch whose track reaches that point (0x4886EC keeps looking
            // after a match, so the highest index wins).
            const auto* junction = network.node(point.node);
            const auto& next = points[i + 1];
            for (std::uint32_t b = 0; junction && b + 1 < junction->pins.size(); ++b) {
                const auto* track = network.node(junction->pins[1 + b].node);
                if (!track || track->kind != formats::TrackNodeKind::vector) continue;
                bool reaches = !next.junction && track->index == next.node;
                if (next.junction) {
                    for (const auto& far : track->pins) reaches = reaches || far.node == next.node;
                }
                if (reaches) branches[point.node] = b;
            }
            continue;
        }
        PlanPoint plan_point;
        plan_point.node = point.node;
        plan_point.offset = point.offset;
        plan_point.through = std::move(through);
        plan_point.branches = std::move(branches);
        through.clear();
        branches.clear();
        if (i + 1 == points.size()) {
            plan_point.kind = PlanPointKind::end;
        } else if (point.source->reverse()) {
            plan_point.kind = PlanPointKind::reverse;
        } else if (point.source->wait()) {
            plan_point.kind = PlanPointKind::wait;
            plan_point.wait = static_cast<float>(point.source->wait_seconds());
        }
        plan.points.push_back(std::move(plan_point));
    }
    // The start direction: the one that reaches the first point.
    std::optional<double> forward_length, backward_length;
    for (const bool forward : {true, false}) {
        TrackLocation from = plan.start;
        from.forward = forward;
        if (const auto way = find_way(network, from, plan.points.front().node, plan.points.front().offset, plan.points.front())) {
            (forward ? forward_length : backward_length) = way->length;
        }
    }
    if (!forward_length && !backward_length) return fail("the path's first leg does not connect");
    plan.start.forward = forward_length && (!backward_length || *forward_length <= *backward_length);
    return plan;
}

formats::TrainPath path_from_route(const TrackNetwork& network, const PathPlan& plan, const TrainRoute& route) {
    formats::TrainPath path;
    path.file = plan.name;
    path.id = plan.name;
    path.name = plan.name;
    const auto add = [&](const TrackPoint& where, const std::uint32_t kind, const std::uint32_t flags) {
        path.points.push_back({route_position(where), kind, 0});
        const auto index = static_cast<std::uint32_t>(path.nodes.size());
        if (!path.nodes.empty()) path.nodes.back().next = index;
        path.nodes.push_back({.flags = flags, .next = formats::PathNode::invalid, .next_siding = formats::PathNode::invalid,
                              .point = static_cast<std::uint32_t>(path.points.size() - 1)});
    };
    const auto add_on_leg = [&](const RouteLeg& leg, const double distance, const std::uint32_t flags) {
        const auto location = leg.at(distance);
        if (!location) return;
        if (const auto where = network.point(location->node, location->offset)) add(*where, 1, flags);
    };
    if (const auto start = network.point(plan.start.node, plan.start.offset)) add(*start, 1, 0);
    for (const auto& leg : route.legs) {
        // Junctions and marks in order of distance.
        struct Event {
            double distance{};
            bool junction = false;
            std::size_t index{};
        };
        std::vector<Event> events;
        for (std::size_t j = 0; j < leg.junctions.size(); ++j) events.push_back({leg.junctions[j].distance, true, j});
        for (std::size_t m = 0; m < leg.marks.size(); ++m) events.push_back({leg.marks[m].distance, false, m});
        std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
            return a.distance < b.distance || (a.distance == b.distance && a.junction && !b.junction);
        });
        for (std::size_t e = 0; e < events.size(); ++e) {
            const auto& event = events[e];
            if (!event.junction) {
                const auto& mark = leg.marks[event.index];
                std::uint32_t flags = 4;
                if (mark.kind == RouteMarkKind::reverse) flags = 1;
                if (mark.kind == RouteMarkKind::wait) flags = 2u | (static_cast<std::uint32_t>(mark.wait) << 16);
                if (mark.kind == RouteMarkKind::end) flags = 0;
                add_on_leg(leg, mark.distance, flags);
                continue;
            }
            const auto& pass = leg.junctions[event.index];
            if (const auto where = network.node_position(pass.junction)) add(*where, 2, 0);
            if (!pass.facing || e + 1 >= events.size()) continue;
            // RTS reads the branch from the next point (the last branch that
            // reaches it); add a point on the branch taken when that would
            // read differently.
            const auto& next = events[e + 1];
            const auto* junction = network.node(pass.junction);
            std::optional<std::uint32_t> read;
            const auto next_location = next.junction ? std::nullopt : leg.at(next.distance);
            for (std::uint32_t b = 0; junction && b + 1 < junction->pins.size(); ++b) {
                const auto* track = network.node(junction->pins[1 + b].node);
                if (!track || track->kind != formats::TrackNodeKind::vector) continue;
                bool reaches = false;
                if (next.junction) {
                    for (const auto& far : track->pins) reaches = reaches || far.node == leg.junctions[next.index].junction;
                } else {
                    reaches = next_location && next_location->node == track->index;
                }
                if (reaches) read = b;
            }
            if (read != pass.branch) {
                add_on_leg(leg, pass.distance + (std::min)(5.0, (next.distance - pass.distance) * 0.5), 4);
            }
        }
    }
    return path;
}

} // namespace openrts::simulation
