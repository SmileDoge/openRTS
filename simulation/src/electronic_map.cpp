#include <openrts/simulation/electronic_map.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <queue>
#include <utility>

namespace openrts::simulation {

namespace {

// A vector node reached in a search: its travel direction, where the way
// came onto it, how far that is from the start, the visit it came from.
struct Visit {
    std::uint32_t node{};
    bool forward = true;
    double entry{};
    double distance{};
    std::int32_t parent = -1;
};

struct Arrival {
    std::uint32_t node{};
    bool forward = true;
};

// The vector nodes reached leaving `from` at its end in `forward`, through
// the junctions between: every branch of a facing junction (only its
// MainRoute with `main_only`), the trunk of a trailing one. Pins as
// track_traveller.cpp's follow() reads them: to a vector node, direction 1
// onto its start going forward; to a junction, 1 onto its trunk (facing).
void arrivals(const TrackNetwork& network, const std::uint32_t from, const bool forward, const bool main_only, std::vector<Arrival>& out) {
    out.clear();
    const auto* node = network.node(from);
    if (!node || node->pins.size() < 2) return;
    struct Pending {
        formats::TrackPin pin;
        int depth{};
    };
    std::vector<Pending> stack{{node->pins[forward ? 1 : 0], 0}};
    while (!stack.empty()) {
        const auto pending = stack.back();
        stack.pop_back();
        if (pending.depth > 32) continue;
        const auto* target = network.node(pending.pin.node);
        if (!target) continue;
        if (target->kind == formats::TrackNodeKind::vector) {
            out.push_back({target->index, pending.pin.direction == 1});
            continue;
        }
        if (target->kind != formats::TrackNodeKind::junction || target->pins.size() < 2) continue;
        if (pending.pin.direction == 1) {
            const auto* state = network.junction(target->index);
            const std::uint32_t main = state ? state->main_route : 0;
            for (std::size_t b = 1; b < target->pins.size(); ++b) {
                if (main_only && b - 1 != main) continue;
                stack.push_back({target->pins[b], pending.depth + 1});
            }
        } else {
            stack.push_back({target->pins[0], pending.depth + 1});
        }
    }
}

[[nodiscard]] double node_length(const TrackNetwork& network, const std::uint32_t node) {
    const auto* n = network.node(node);
    return n ? n->length : 0.0;
}

// Dijkstra over the vector nodes from `start` within `horizon`: `goal`
// says whether the visit's node holds the goal and how far along the way
// it is. The nearest goal's visit, its distance in `best`.
template <class Goal>
[[nodiscard]] std::optional<std::size_t> nearest(const TrackNetwork& network, std::vector<Visit>& visits, const std::vector<Visit>& starts,
                                                 const double horizon, Goal goal, double& best) {
    visits.clear();
    best = horizon;
    std::optional<std::size_t> found;
    using Entry = std::pair<double, std::size_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> queue;
    for (const auto& s : starts) {
        visits.push_back(s);
        queue.push({s.distance, visits.size() - 1});
    }
    std::set<std::pair<std::uint32_t, bool>> done;
    std::vector<Arrival> next;
    while (!queue.empty()) {
        const auto [distance, index] = queue.top();
        queue.pop();
        if (distance > best) break;
        const Visit visit = visits[index];
        if (!done.insert({visit.node, visit.forward}).second) continue;
        if (double at = 0.0; goal(visit, at) && at <= best) {
            best = at;
            found = index;
        }
        const double length = node_length(network, visit.node);
        const double leave = visit.distance + (visit.forward ? length - visit.entry : visit.entry);
        if (leave > best) continue;
        arrivals(network, visit.node, visit.forward, false, next);
        for (const auto& a : next) {
            if (done.contains({a.node, a.forward})) continue;
            visits.push_back({.node = a.node,
                              .forward = a.forward,
                              .entry = a.forward ? 0.0 : node_length(network, a.node),
                              .distance = leave,
                              .parent = static_cast<std::int32_t>(index)});
            queue.push({leave, visits.size() - 1});
        }
    }
    return found;
}

// The visits from the start to `last`.
[[nodiscard]] std::vector<Visit> way_to(const std::vector<Visit>& visits, std::size_t last) {
    std::vector<Visit> way;
    for (std::int32_t i = static_cast<std::int32_t>(last); i >= 0; i = visits[static_cast<std::size_t>(i)].parent) {
        way.push_back(visits[static_cast<std::size_t>(i)]);
        if (way.size() > visits.size()) break;
    }
    std::reverse(way.begin(), way.end());
    return way;
}

[[nodiscard]] TrackPoint point_of(const formats::RoutePosition& p) {
    return TrackPoint{.tile_x = p.tile_x, .tile_z = p.tile_z, .x = p.x, .y = p.y, .z = p.z};
}

} // namespace

std::uint32_t ElectronicMap::track_number(const std::string_view name) {
    std::uint32_t number = 0;
    std::size_t digits = 0;
    for (const char c : name) {
        if (c < '0' || c > '9') break;
        number = number * 10 + static_cast<std::uint32_t>(c - '0');
        if (++digits > 3) return 0;
    }
    return digits > 0 && number >= 1 && number <= 98 ? number : 0;
}

ElectronicMap::ElectronicMap(const TrackNetwork& network, const formats::RouteDatabase& database, std::vector<MapSignal> signals) {
    for (const auto& s : signals) signals_[s.node].push_back({.object = s.object, .offset = s.offset, .forward = s.forward});
    for (auto& [node, list] : signals_) std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.offset < b.offset; });

    constexpr double reach = 10.0; // a route point off its track at most, m
    std::vector<Visit> visits;
    for (const auto& point : database.points) {
        if (point.type != 2 || !point.point2) continue;
        const auto number = track_number(point.name);
        if (number == 0) continue;
        const auto a = network.locate(point_of(point.point1.position), reach, 5.0);
        const auto b = network.locate(point_of(point.point2->position), reach, 5.0);
        if (!a || !b) continue;
        MapTrack track{.station = point.station, .name = point.name, .number = number};
        if (a->node == b->node) {
            track.spans.push_back({a->node, (std::min)(a->offset, b->offset), (std::max)(a->offset, b->offset)});
        } else {
            // From one end to the other, either way off the first.
            double best = 0.0;
            const std::vector<Visit> starts{{.node = a->node, .forward = true, .entry = a->offset},
                                            {.node = a->node, .forward = false, .entry = a->offset}};
            const auto last = nearest(network, visits, starts, 6000.0,
                                      [&](const Visit& v, double& at) {
                                          if (v.node != b->node) return false;
                                          at = v.distance + std::abs(b->offset - v.entry);
                                          return true;
                                      },
                                      best);
            if (!last) continue;
            const auto way = way_to(visits, *last);
            for (std::size_t i = 0; i < way.size(); ++i) {
                const auto& v = way[i];
                const double length = node_length(network, v.node);
                const double end = i + 1 == way.size() ? b->offset : (v.forward ? length : 0.0);
                track.spans.push_back({v.node, (std::min)(v.entry, end), (std::max)(v.entry, end)});
            }
        }
        station_numbers_.insert({track.station, number});
        numbers_.insert(number);
        tracks_.push_back(std::move(track));
    }
    for (std::size_t t = 0; t < tracks_.size(); ++t)
        for (const auto& span : tracks_[t].spans) spans_[span.node].push_back({t, span.from, span.to});

    // A signal on a track or at its end (an exit or a route signal, within
    // 60 m past it): that track's.
    constexpr double end_reach = 60.0;
    for (const auto& [node, list] : signals_) {
        const auto spans = spans_.find(node);
        if (spans == spans_.end()) continue;
        for (const auto& s : list) {
            double nearest_gap = end_reach + 1.0;
            std::optional<std::size_t> track;
            for (const auto& span : spans->second) {
                const double gap = s.offset < span.from ? span.from - s.offset : s.offset > span.to ? s.offset - span.to : 0.0;
                if (gap <= end_reach && gap < nearest_gap) {
                    nearest_gap = gap;
                    track = span.track;
                }
            }
            if (track) signal_track_[s.object] = *track;
        }
    }
    // A station's entry signals: those within 40 m of its ends at the lines.
    constexpr double entry_reach = 40.0;
    for (const auto& point : database.points) {
        if (point.type != 1 || !(point.flags & 2u)) continue;
        const auto at = network.locate(point_of(point.point1.position), reach, 5.0);
        if (!at) continue;
        const auto list = signals_.find(at->node);
        if (list == signals_.end()) continue;
        for (const auto& s : list->second) {
            if (std::abs(s.offset - at->offset) <= entry_reach && !signal_track_.contains(s.object)) signal_entry_[s.object] = point.station;
        }
    }
}

const MapTrack* ElectronicMap::track_of(const std::uint32_t object) const {
    const auto it = signal_track_.find(object);
    return it == signal_track_.end() ? nullptr : &tracks_[it->second];
}

ElectronicMap::Found ElectronicMap::along_main(const TrackNetwork& network, std::uint32_t node, double offset, bool forward,
                                               double already, bool strictly_ahead) const {
    Found found;
    std::set<std::pair<std::uint32_t, bool>> seen;
    std::vector<Arrival> next;
    double distance = already;
    while (distance <= horizon && seen.insert({node, forward}).second) {
        if (const auto list = signals_.find(node); list != signals_.end()) {
            const Signal* first = nullptr;
            for (const auto& s : list->second) {
                if (s.forward != forward) continue;
                const bool ahead = forward ? (strictly_ahead ? s.offset > offset : s.offset >= offset)
                                           : (strictly_ahead ? s.offset < offset : s.offset <= offset);
                if (!ahead) continue;
                if (!first || (forward ? s.offset < first->offset : s.offset > first->offset)) first = &s;
            }
            if (first) {
                found.has_target = true;
                found.object = first->object;
                found.distance = distance + std::abs(first->offset - offset);
                return found;
            }
        }
        const double length = node_length(network, node);
        distance += forward ? length - offset : offset;
        arrivals(network, node, forward, true, next);
        if (next.empty()) break;
        node = next.front().node;
        forward = next.front().forward;
        offset = forward ? 0.0 : node_length(network, node);
        strictly_ahead = false;
    }
    return found;
}

ElectronicMap::Found ElectronicMap::search(const TrackNetwork& network, const std::uint32_t node, const double offset, const bool forward,
                                           const std::uint32_t number) const {
    Found found;
    std::vector<Visit> visits;
    double best = 0.0;
    const auto last = nearest(network, visits, {{.node = node, .forward = forward, .entry = offset}}, horizon,
                              [&](const Visit& v, double& at) {
                                  const auto spans = spans_.find(v.node);
                                  if (spans == spans_.end()) return false;
                                  bool any = false;
                                  for (const auto& span : spans->second) {
                                      if (tracks_[span.track].number != number) continue;
                                      // The span's part ahead on this node.
                                      double onto = 0.0;
                                      if (v.forward) {
                                          if (span.to < v.entry) continue;
                                          onto = (std::max)(span.from, v.entry);
                                      } else {
                                          if (span.from > v.entry) continue;
                                          onto = (std::min)(span.to, v.entry);
                                      }
                                      const double d = v.distance + std::abs(onto - v.entry);
                                      if (!any || d < at) {
                                          at = d;
                                          any = true;
                                      }
                                  }
                                  return any;
                              },
                              best);
    if (!last) return found;
    found.found = true;
    const double goal_distance = best;
    // Which track it is: the nearest of the number on that node.
    {
        const auto& v = visits[*last];
        double nearest_d = -1.0;
        for (const auto& span : spans_.at(v.node)) {
            if (tracks_[span.track].number != number) continue;
            const double onto = v.forward ? (std::max)(span.from, v.entry) : (std::min)(span.to, v.entry);
            const double d = std::abs(onto - v.entry);
            if (nearest_d < 0.0 || d < nearest_d) {
                nearest_d = d;
                found.track = span.track;
            }
        }
    }
    // The train signals along the way in order (every node of it to its
    // end: the way goes on from there), the first the target; those before
    // the track counted.
    const auto way = way_to(visits, *last);
    for (std::size_t i = 0; i < way.size(); ++i) {
        const auto& v = way[i];
        const auto list = signals_.find(v.node);
        if (list == signals_.end()) continue;
        std::vector<const Signal*> ahead;
        for (const auto& s : list->second) {
            if (s.forward != v.forward) continue;
            const bool is_ahead = i == 0 ? (v.forward ? s.offset > v.entry : s.offset < v.entry)
                                         : (v.forward ? s.offset >= v.entry : s.offset <= v.entry);
            if (is_ahead) ahead.push_back(&s);
        }
        std::sort(ahead.begin(), ahead.end(),
                  [&](const Signal* a, const Signal* b) { return v.forward ? a->offset < b->offset : a->offset > b->offset; });
        for (const auto* s : ahead) {
            const double d = v.distance + std::abs(s->offset - v.entry);
            if (d < goal_distance) ++found.signals_before;
            if (!found.has_target) {
                found.has_target = true;
                found.object = s->object;
                found.distance = d;
            }
        }
    }
    if (found.has_target) return found;
    // None up to the track's node's end: on along MainRoute from there.
    const auto& v = way.back();
    const double length = node_length(network, v.node);
    const double leave = v.distance + (v.forward ? length - v.entry : v.entry);
    std::vector<Arrival> next;
    arrivals(network, v.node, v.forward, true, next);
    if (next.empty()) return found;
    const auto on = along_main(network, next.front().node, next.front().forward ? 0.0 : node_length(network, next.front().node),
                               next.front().forward, leave, false);
    found.has_target = on.has_target;
    found.object = on.object;
    found.distance = on.distance;
    return found;
}

EkTarget ElectronicMap::update(EkState& state, const TrackNetwork& network, const std::uint32_t node, const double offset, const bool forward,
                               const std::uint32_t number, const std::span<const std::uint32_t> passed) const {
    EkTarget out;
    const bool fresh = !state.started || state.number != number;
    if (fresh) state = EkState{.number = number, .started = true};
    // 0: the КЛУБ's own "без ЭК"; 99: the instructions' way into it.
    if (number == 0 || number > 98 || empty()) {
        state.matched = false;
        return out;
    }
    // The signals passed: another track's (its exit or route signal) or the
    // entry of a station without the entered track: off its way.
    for (const auto object : passed) {
        if (const auto it = signal_track_.find(object); it != signal_track_.end()) {
            if (tracks_[it->second].number != number) {
                state.matched = false;
                state.latched_off = true;
                state.left_at = tracks_[it->second].station;
            }
        } else if (const auto entry = signal_entry_.find(object); entry != signal_entry_.end()) {
            if (!station_numbers_.contains({entry->second, number})) {
                state.matched = false;
                state.latched_off = true;
                state.left_at = entry->second;
            }
        }
    }
    const auto found = search(network, node, offset, forward, number);
    if (found.found) {
        // Left off its way: back before the entry signal of another
        // station with the track (the target its entry, or no more than
        // one train signal before the track where the entries are not
        // known).
        const auto& station = tracks_[found.track].station;
        const auto entry = found.has_target ? signal_entry_.find(found.object) : signal_entry_.end();
        const bool before_entry = (entry != signal_entry_.end() && entry->second == station) || found.signals_before <= 1;
        if (!state.latched_off || (before_entry && station != state.left_at)) {
            state.matched = true;
            state.latched_off = false;
        }
    } else if (fresh) {
        // Entered with no station track of it ahead (on a line): the ЭК
        // takes it while some station of the route has the number.
        state.matched = numbers_.contains(number);
    }
    if (!state.matched) return out;
    out.matched = true;
    const auto target = found.found ? found : along_main(network, node, offset, forward, 0.0, true);
    out.has_target = target.has_target;
    out.object = target.object;
    out.distance = target.distance;
    return out;
}

} // namespace openrts::simulation
