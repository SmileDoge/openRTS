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
void arrivals(const TrackNetwork& network, const std::uint32_t from, const bool forward, const bool main_only, std::vector<Arrival>& out,
              const bool as_set = false) {
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
            // `as_set`: the branch the junction is set for (the train's way).
            const std::uint32_t main = state ? (as_set ? state->selected : state->main_route) : 0;
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

// The vector node on from `from`'s end in `forward` along MainRoute: a
// facing junction's MainRoute branch, a trailing one's trunk when entered
// by its MainRoute branch; none where it is entered off it (a side track
// joining the track the way goes onto) or the track ends.
[[nodiscard]] std::optional<Arrival> main_onward(const TrackNetwork& network, const std::uint32_t from, const bool forward) {
    const auto* node = network.node(from);
    if (!node || node->pins.size() < 2) return std::nullopt;
    auto pin = node->pins[forward ? 1 : 0];
    std::uint32_t came = from; // the node the way comes onto the junction from
    for (int depth = 0; depth < 32; ++depth) {
        const auto* target = network.node(pin.node);
        if (!target) return std::nullopt;
        if (target->kind == formats::TrackNodeKind::vector) return Arrival{target->index, pin.direction == 1};
        if (target->kind != formats::TrackNodeKind::junction || target->pins.size() < 2) return std::nullopt;
        const auto* state = network.junction(target->index);
        const std::uint32_t main = state ? state->main_route : 0;
        formats::TrackPin next{};
        if (pin.direction == 1) {
            if (1 + main >= target->pins.size()) return std::nullopt;
            next = target->pins[1 + main];
        } else {
            std::optional<std::size_t> branch;
            for (std::size_t b = 1; b < target->pins.size() && !branch; ++b)
                if (target->pins[b].node == came) branch = b;
            if (!branch || *branch - 1 != main) return std::nullopt;
            next = target->pins[0];
        }
        came = target->index;
        pin = next;
    }
    return std::nullopt;
}

// World metres of a track point, and its 64 m cell.
[[nodiscard]] double world_x(const TrackPoint& p) { return p.tile_x * 2048.0 + p.x; }
[[nodiscard]] double world_z(const TrackPoint& p) { return p.tile_z * 2048.0 + p.z; }
constexpr double line_cell = 64.0;
[[nodiscard]] std::uint64_t cell_key(const std::int64_t cx, const std::int64_t cz) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx)) << 32) | static_cast<std::uint32_t>(cz);
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

std::string ElectronicMap::track_park(const std::string_view name) {
    std::size_t i = 0;
    while (i < name.size() && name[i] >= '0' && name[i] <= '9') ++i;
    if (i == 0 || i >= name.size()) return {};
    const auto c = static_cast<unsigned char>(name[i]);
    if (c >= 'A' && c <= 'Z') return std::string(name.substr(i, 1));
    // UTF-8 А..Я (D0 90 .. D0 AF) and Ё (D0 81).
    if (c == 0xD0 && i + 1 < name.size()) {
        const auto d = static_cast<unsigned char>(name[i + 1]);
        if ((d >= 0x90 && d <= 0xAF) || d == 0x81) return std::string(name.substr(i, 2));
    }
    return {};
}

bool ElectronicMap::in_park(const std::string& station, const std::string& park, const std::uint32_t number) const {
    return park_numbers_.contains({station, park, number}) || (number <= 2 && park_numbers_.contains({station, std::string(), number}));
}

ElectronicMap::ElectronicMap(const TrackNetwork& network, const formats::RouteDatabase& database, std::vector<MapSignal> signals) {
    for (const auto& s : signals) {
        signals_[s.node].push_back({.object = s.object, .offset = s.offset, .forward = s.forward});
        signal_place_[s.object] = s;
    }
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
        MapTrack track{.station = point.station, .name = point.name, .number = number, .park = track_park(point.name)};
        if (a->node == b->node) {
            track.spans.push_back({a->node, (std::min)(a->offset, b->offset), (std::max)(a->offset, b->offset), a->offset <= b->offset});
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
                track.spans.push_back({v.node, (std::min)(v.entry, end), (std::max)(v.entry, end), v.forward});
            }
        }
        station_numbers_.insert({track.station, number});
        park_numbers_.insert({track.station, track.park, number});
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
    // The lines' tracks: from each end of a station's main track (1, 2: a
    // side track's way out, numbered, made the throat a "line" of its
    // number and the main ones' ambiguous where they join) outwards along
    // MainRoute (not on where it joins another track by a side branch) up
    // to the next station's track, its number carried.
    for (const auto& track : tracks_) {
        if (track.number > 2) continue;
        for (const bool last_end : {true, false}) {
            const auto& span = last_end ? track.spans.back() : track.spans.front();
            std::uint32_t node = span.node;
            bool forward = last_end ? span.forward : !span.forward;
            double carried = 0.0;
            std::set<std::uint32_t> seen;
            while (carried <= line_carry) {
                const auto on = main_onward(network, node, forward);
                if (!on || spans_.contains(on->node) || !seen.insert(on->node).second) break;
                const auto [it, added] = line_numbers_.insert({on->node, track.number});
                if (!added && it->second != track.number) it->second = 0;
                carried += node_length(network, on->node);
                node = on->node;
                forward = on->forward;
            }
        }
    }
    for (const auto& [node, number] : line_numbers_) {
        if (number == 0) continue;
        if (const auto list = signals_.find(node); list != signals_.end()) {
            int way = 0;
            for (const auto& s : list->second) way += s.forward ? 1 : -1;
            if (way != 0) line_ways_[node] = way > 0 ? 1 : -1;
        }
        const double length = node_length(network, node);
        for (double at = 0.0;; at += 10.0) {
            const double offset = (std::min)(at, length);
            if (const auto p = network.point(node, offset)) {
                const auto cx = static_cast<std::int64_t>(std::floor(world_x(*p) / line_cell));
                const auto cz = static_cast<std::int64_t>(std::floor(world_z(*p) / line_cell));
                line_cells_[cell_key(cx, cz)].push_back({node, offset});
            }
            if (offset >= length) break;
        }
    }
}

std::uint32_t ElectronicMap::line_number(const std::uint32_t node) const {
    const auto it = line_numbers_.find(node);
    return it == line_numbers_.end() ? 0 : it->second;
}

ElectronicMap::Found ElectronicMap::line_abreast(const TrackNetwork& network, const std::uint32_t node, const double offset,
                                                 const bool forward, const std::uint32_t number) const {
    Found found;
    const auto front = network.point(node, offset);
    if (!front) return found;
    const double fx = world_x(*front), fz = world_z(*front);
    // The nearest point of a line track of the number within line_reach.
    const auto reach_cells = static_cast<std::int64_t>(std::ceil(line_reach / line_cell));
    const auto cx = static_cast<std::int64_t>(std::floor(fx / line_cell));
    const auto cz = static_cast<std::int64_t>(std::floor(fz / line_cell));
    std::optional<LinePoint> best;
    double best_d2 = line_reach * line_reach;
    const auto take = [&](const std::uint32_t n, const double at) {
        const auto p = network.point(n, at);
        if (!p) return;
        const double dx = world_x(*p) - fx, dz = world_z(*p) - fz;
        if (const double d2 = dx * dx + dz * dz; d2 <= best_d2) {
            best_d2 = d2;
            best = LinePoint{n, at};
        }
    };
    for (std::int64_t ix = cx - reach_cells; ix <= cx + reach_cells; ++ix) {
        for (std::int64_t iz = cz - reach_cells; iz <= cz + reach_cells; ++iz) {
            const auto cell = line_cells_.find(cell_key(ix, iz));
            if (cell == line_cells_.end()) continue;
            for (const auto& lp : cell->second)
                if (lp.node != node && line_number(lp.node) == number) take(lp.node, lp.offset);
        }
    }
    if (!best) return found;
    {
        const LinePoint coarse = *best;
        const double length = node_length(network, coarse.node);
        for (double at = (std::max)(coarse.offset - 10.0, 0.0); at <= (std::min)(coarse.offset + 10.0, length); at += 1.0) take(coarse.node, at);
    }
    // The way the front runs, along that track; its next train signal on
    // (as the switches' MainRoute lie), either facing.
    const auto p = network.point(best->node, best->offset);
    if (!p) return found;
    constexpr double pi = 3.14159265358979323846;
    const double heading = static_cast<double>(front->heading) + (forward ? 0.0 : pi);
    const bool up = std::cos(static_cast<double>(p->heading) - heading) >= 0.0;
    // Where that track begins ahead (the nearest point its first, past a
    // station's throat): the metres up to it, straight on, as in a station.
    const double lead = (std::max)((world_x(*p) - fx) * std::sin(heading) + (world_z(*p) - fz) * std::cos(heading), 0.0);
    found = along_main(network, best->node, best->offset, up, lead, true, true);
    found.found = found.has_target;
    return found;
}

const MapTrack* ElectronicMap::track_of(const std::uint32_t object) const {
    const auto it = signal_track_.find(object);
    return it == signal_track_.end() ? nullptr : &tracks_[it->second];
}

ElectronicMap::Found ElectronicMap::along_main(const TrackNetwork& network, std::uint32_t node, double offset, bool forward,
                                               double already, bool strictly_ahead, const bool line_way, const bool as_set) const {
    Found found;
    std::set<std::pair<std::uint32_t, bool>> seen;
    std::vector<Arrival> next;
    double distance = already;
    while (distance <= horizon && seen.insert({node, forward}).second) {
        if (const auto list = signals_.find(node); list != signals_.end()) {
            const Signal* first = nullptr;
            for (const auto& s : list->second) {
                if (line_way) {
                    const auto way = line_ways_.find(node);
                    if (way != line_ways_.end() && (way->second > 0) != s.forward) continue;
                } else if (s.forward != forward) {
                    continue;
                }
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
        arrivals(network, node, forward, true, next, as_set);
        if (next.empty()) break;
        node = next.front().node;
        forward = next.front().forward;
        offset = forward ? 0.0 : node_length(network, node);
        strictly_ahead = false;
    }
    return found;
}

std::optional<std::size_t> ElectronicMap::track_at(const std::uint32_t node, const double offset) const {
    const auto spans = spans_.find(node);
    if (spans == spans_.end()) return std::nullopt;
    for (const auto& span : spans->second)
        if (offset >= span.from && offset <= span.to) return span.track;
    return std::nullopt;
}

ElectronicMap::Found ElectronicMap::abreast(const TrackNetwork& network, const std::uint32_t node, const double offset, const bool forward,
                                            const std::string& station, const std::uint32_t number, const std::string* park) const {
    Found found;
    const auto front = network.point(node, offset);
    if (!front) return found;
    const auto distance2 = [&](const TrackPoint& p) {
        const double dx = (p.tile_x - front->tile_x) * 2048.0 + (p.x - front->x);
        const double dz = (p.tile_z - front->tile_z) * 2048.0 + (p.z - front->z);
        return dx * dx + dz * dz;
    };
    // Along a track: metres from its first end (a point on span `k`, or
    // past its ends on that span's node).
    const auto along_track = [](const MapTrack& track, const std::size_t k, const double at) {
        double base = 0.0;
        for (std::size_t i = 0; i < k; ++i) base += track.spans[i].to - track.spans[i].from;
        const auto& span = track.spans[k];
        return base + (span.forward ? at - span.from : span.to - at);
    };
    constexpr double pi = 3.14159265358979323846;
    const double heading = static_cast<double>(front->heading) + (forward ? 0.0 : pi);
    // Of the station's tracks of the number ("2", "2А"), the nearest to
    // the front with a signal ahead.
    double best_d2 = -1.0;
    double nearest_d2 = -1.0;
    bool nearest_passed = false;
    for (std::size_t t = 0; t < tracks_.size(); ++t) {
        const auto& track = tracks_[t];
        if (track.number != number || track.station != station || track.spans.empty()) continue;
        // Within the train's park (the testers, 2026-11-21: not the next
        // park's); the main ones also on the station's unlettered tracks.
        if (park && track.park != *park && !(number <= 2 && track.park.empty())) continue;
        // Its point nearest the front: every 10 m, then to the metre.
        std::size_t near_span = 0;
        double near_at = 0.0, near_d2 = -1.0;
        const auto take = [&](const std::size_t k, const double at) {
            const auto p = network.point(track.spans[k].node, at);
            if (!p) return;
            const double d2 = distance2(*p);
            if (near_d2 < 0.0 || d2 < near_d2) {
                near_span = k;
                near_at = at;
                near_d2 = d2;
            }
        };
        for (std::size_t k = 0; k < track.spans.size(); ++k) {
            for (double at = track.spans[k].from; at < track.spans[k].to; at += 10.0) take(k, at);
            take(k, track.spans[k].to);
        }
        if (near_d2 < 0.0) continue;
        {
            const auto& span = track.spans[near_span];
            const double coarse = near_at;
            for (double at = (std::max)(coarse - 10.0, span.from); at <= (std::min)(coarse + 10.0, span.to); at += 1.0) take(near_span, at);
        }
        const double here = along_track(track, near_span, near_at);
        double length = 0.0;
        for (const auto& span : track.spans) length += span.to - span.from;
        // The way the front runs, along the track.
        const auto p = network.point(track.spans[near_span].node, near_at);
        if (!p) continue;
        const double track_heading = static_cast<double>(p->heading) + (track.spans[near_span].forward ? 0.0 : pi);
        const bool up = std::cos(track_heading - heading) >= 0.0;
        // Before its near end (the throat) the metres up to it, straight
        // on. Past its far end (its signal nearer than the train's own) the
        // metres the front is on past it, along the track's way: its
        // signals behind the front passed, the next one on that way (the
        // testers' 2026-11-21: to then the distance stood at the metres
        // from its end to its exit signal, 2, past 50 m off its end the
        // track was dropped). However far aside (a park of the station's,
        // the testers' 2026-11-21: to then no more than 500 m off).
        const bool far_end = up ? here >= length - 0.5 : here <= 0.5;
        const bool near_end = up ? here <= 0.5 : here >= length - 0.5;
        const double dx = (front->tile_x - p->tile_x) * 2048.0 + (front->x - p->x);
        const double dz = (front->tile_z - p->tile_z) * 2048.0 + (front->z - p->z);
        double lead = 0.0, past = 0.0;
        if (near_end && near_d2 > 1.0) {
            // TrackPoint headings: 0 along +z, growing towards +x.
            const double hx = std::sin(heading), hz = std::cos(heading);
            lead = -(dx * hx + dz * hz);
            if (lead < 0.0) continue;
        } else if (far_end && near_d2 > 1.0) {
            const double way = track_heading + (up ? 0.0 : pi);
            past = (std::max)(dx * std::sin(way) + dz * std::cos(way), 0.0);
        }
        // Its signals facing that way ahead of the point: the nearest.
        Found on_track;
        for (const auto& [object, index] : signal_track_) {
            if (index != t) continue;
            const auto place = signal_place_.find(object);
            if (place == signal_place_.end()) continue;
            const auto& s = place->second;
            std::optional<std::size_t> k;
            double gap = 0.0;
            for (std::size_t i = 0; i < track.spans.size(); ++i) {
                const auto& span = track.spans[i];
                if (span.node != s.node) continue;
                const double g = s.offset < span.from ? span.from - s.offset : s.offset > span.to ? s.offset - span.to : 0.0;
                if (!k || g < gap) {
                    k = i;
                    gap = g;
                }
            }
            if (!k) continue;
            const auto& span = track.spans[*k];
            const bool faces_up = s.forward == span.forward; // trains running up the track approach it
            if (faces_up != up) continue;
            const double at = along_track(track, *k, s.offset);
            const double ahead = (up ? at - here : here - at) - past;
            if (ahead < 0.0) continue;
            const double d = ahead + lead;
            if (!on_track.has_target || d < on_track.distance) {
                on_track.found = on_track.has_target = true;
                on_track.object = object;
                on_track.distance = d;
                on_track.track = t;
            }
        }
        // The nearest track of the number: behind the front, its signals
        // passed (the testers' 2026-11-21: then the first block signal, not
        // another park's track of the number further on).
        if (nearest_d2 < 0.0 || near_d2 < nearest_d2) {
            nearest_d2 = near_d2;
            nearest_passed = far_end && past > 1.0 && !on_track.has_target;
        }
        if (best_d2 >= 0.0 && near_d2 >= best_d2) continue;
        // None of its own ahead (its exit past 60 m from its end, on
        // another node): the first train signal on from the point along
        // the track's way ahead of the front, unless another track's.
        if (!on_track.has_target) {
            const bool node_forward = up == track.spans[near_span].forward;
            auto on = along_main(network, track.spans[near_span].node, near_at, node_forward, lead - past, false);
            for (int k = 0; k < 16 && on.has_target && on.distance < 0.0; ++k) {
                const auto place = signal_place_.find(on.object);
                if (place == signal_place_.end()) break;
                on = along_main(network, place->second.node, place->second.offset, place->second.forward, on.distance, true);
            }
            if (on.has_target && on.distance < 0.0) on = {};
            const auto owner = on.has_target ? signal_track_.find(on.object) : signal_track_.end();
            if (on.has_target && (owner == signal_track_.end() || owner->second == t)) {
                on_track = on;
                on_track.found = true;
                on_track.track = t;
            }
        }
        if (!on_track.has_target) continue;
        found = on_track;
        best_d2 = near_d2;
    }
    // A side track's (not the main tracks' 1, 2: the line's of theirs then).
    if (nearest_passed && number > 2) return Found{.passed = true};
    return found;
}

ElectronicMap::Found ElectronicMap::first_block(const TrackNetwork& network, const std::uint32_t node, const double offset,
                                                const bool forward, const std::string& station) const {
    const auto station_signal = [&](const std::uint32_t object) {
        if (const auto t = signal_track_.find(object); t != signal_track_.end()) return tracks_[t->second].station == station;
        if (const auto e = signal_entry_.find(object); e != signal_entry_.end()) return e->second == station;
        return false;
    };
    // Along the junctions as they are set: the train's own way.
    auto on = along_main(network, node, offset, forward, 0.0, true, false, true);
    for (int k = 0; k < 32 && on.has_target && station_signal(on.object); ++k) {
        const auto place = signal_place_.find(on.object);
        if (place == signal_place_.end()) break;
        on = along_main(network, place->second.node, place->second.offset, place->second.forward, on.distance, true, false, true);
    }
    if (on.has_target && station_signal(on.object)) on = {};
    on.found = on.has_target;
    return on;
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
    // the track counted, and those before its first turn off MainRoute.
    const auto way = way_to(visits, *last);
    std::optional<double> turn;
    {
        std::vector<Arrival> main;
        for (std::size_t i = 0; i + 1 < way.size() && !turn; ++i) {
            arrivals(network, way[i].node, way[i].forward, true, main);
            const bool on_main = std::any_of(main.begin(), main.end(), [&](const Arrival& a) {
                return a.node == way[i + 1].node && a.forward == way[i + 1].forward;
            });
            if (!on_main) turn = way[i + 1].distance;
        }
    }
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
            if (turn && d < *turn) ++found.signals_before_turn;
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
    // The signals passed (testers, 2026-11-21). A track other than the
    // main ones (1, 2: the line's track of theirs then, below) is the ЭК's
    // up to the first signal on past its own along its way: past its route
    // signal (НМ3) the exit after the crossover (Н1), past its exit (Н3)
    // the first block signal; "без ЭК" past that. The train on another
    // track: its station's signals passed on (its exits, several heads
    // too), "без ЭК" past the first block signal. The entry of a station
    // without the entered track: off its way.
    const auto leave = [&](const std::string& station) {
        state.matched = false;
        state.latched_off = true;
        state.left_at = station;
        state.leaving.clear();
        state.leaving_own = false;
        state.done_at.clear();
    };
    for (const auto object : passed) {
        if (const auto it = signal_track_.find(object); it != signal_track_.end()) {
            state.inside.clear(); // a track's signal: on its way out (or onto the entered one's)
            const auto& track = tracks_[it->second];
            if (number <= 2) continue;
            if (track.number == number) {
                // Its own (each of its heads): the next signal past it ends it.
                state.leaving = track.station;
                state.leaving_own = true;
            } else if (state.leaving_own) {
                leave(track.station);
            } else {
                state.leaving = track.station;
            }
        } else if (const auto entry = signal_entry_.find(object); entry != signal_entry_.end()) {
            state.leaving.clear();
            state.leaving_own = false;
            if (state.done_at != entry->second) state.done_at.clear();
            if (!station_numbers_.contains({entry->second, number})) {
                state.matched = false;
                state.latched_off = true;
                state.left_at = entry->second;
                state.inside.clear();
            } else {
                state.inside = entry->second;
            }
        } else if (!state.leaving.empty() || !state.done_at.empty()) {
            leave(state.leaving.empty() ? state.done_at : state.leaving);
        }
    }
    // Past another track's signal of a station with the entered track, that
    // track's signal still ahead abreast: the ЭК keeps it, by its coordinate
    // (testers 2026-11-26: Новокузнецк-Пасс., 6 entered, the train on 10:
    // past Ч10, Ч6 some 300 m on, the КЛУБ took the next signal as it lay).
    if (number > 2 && !state.leaving.empty() && !state.leaving_own && state.inside.empty() && state.done_at != state.leaving
        && station_numbers_.contains({state.leaving, number})) {
        const auto beside = abreast(network, node, offset, forward, state.leaving, number);
        if (beside.has_target && !beside.passed) {
            state.inside = state.leaving;
            state.leaving.clear();
        }
    }
    // The entered track abreast behind: the first block signal along the
    // train's way.
    const auto to_first_block = [&]() {
        // Past its own signal: the next signal on, whatever it is (НМ3, then
        // the exit Н1 after the crossover); else the first block signal.
        const auto block = state.leaving_own ? along_main(network, node, offset, forward, 0.0, true, false, true)
                                             : first_block(network, node, offset, forward, state.done_at);
        out.matched = state.matched;
        out.has_target = state.matched && block.has_target;
        out.object = block.object;
        out.distance = block.distance;
        return out;
    };
    // On another track of a station with the entered one: that track's
    // signal (the real КЛУБ-У's map, by its coordinate), unless the train
    // left the entered track's way there.
    if (const auto here = track_at(node, offset)) {
        const auto& station = tracks_[*here].station;
        const auto& park = tracks_[*here].park;
        // The entered number nowhere in the train's park (the station's
        // tracks without a letter one park), or not at the station at all:
        // "без ЭК", back before a station with it (the testers, 2026-11-21:
        // to then the signal ahead, or the next park's track).
        if (tracks_[*here].number != number && !state.leaving_own && state.done_at != station && !in_park(station, park, number)
            && (!state.latched_off || state.left_at != station)) {
            leave(station);
            return out;
        }
        if (tracks_[*here].number != number && station_numbers_.contains({station, number})
            && (!state.latched_off || state.left_at != station)) {
            state.inside = station;
            if (state.leaving_own) return to_first_block(); // past its own: the next signal on
            if (state.done_at != station) {
                const auto beside = abreast(network, node, offset, forward, station, number, &park);
                if (beside.passed) {
                    state.done_at = station;
                } else if (beside.has_target) {
                    state.matched = true;
                    state.latched_off = false;
                    out.matched = true;
                    out.has_target = true;
                    out.object = beside.object;
                    out.distance = beside.distance;
                    return out;
                }
            }
            if (state.done_at == station) return to_first_block();
        }
    }
    // On a line's track of another number (not in a station with the
    // entered track, not left off its way): the entered one's abreast.
    if (number <= 2 && state.inside.empty() && !state.latched_off && !track_at(node, offset)) {
        if (const auto line = line_number(node); line != 0 && line != number) {
            const auto beside = line_abreast(network, node, offset, forward, number);
            if (beside.has_target) {
                state.matched = true;
                out.matched = true;
                out.has_target = true;
                out.object = beside.object;
                out.distance = beside.distance;
                return out;
            }
        }
    }
    const auto found = search(network, node, offset, forward, number);
    // In its throat past its entry signal: that track's signal from abreast
    // too, by its coordinate, whichever way the switches lead.
    if (!state.inside.empty() && (!state.latched_off || state.left_at != state.inside)) {
        if (state.leaving_own) return to_first_block();
        if (state.done_at != state.inside) {
            const auto beside = abreast(network, node, offset, forward, state.inside, number);
            if (beside.passed) {
                state.done_at = state.inside;
            } else if (beside.has_target) {
                state.matched = true;
                state.latched_off = false;
                out.matched = true;
                out.has_target = true;
                out.object = beside.object;
                out.distance = beside.distance;
                return out;
            }
        }
        if (state.done_at == state.inside) return to_first_block();
    }
    // Its station left behind (its signals passed) with the track abreast
    // behind: the first block signal still.
    if (!state.done_at.empty() && !state.latched_off) return to_first_block();
    if (found.found) {
        // Left off its way: back before the entry signal of another
        // station with the track (the target its entry, or no more than
        // one train signal before the track where the entries are not
        // known). Taken (not yet the ЭК's) only in front of the signal its
        // turn off MainRoute lies beyond.
        const auto& station = tracks_[found.track].station;
        const auto entry = found.has_target ? signal_entry_.find(found.object) : signal_entry_.end();
        const bool before_entry = (entry != signal_entry_.end() && entry->second == station) || found.signals_before <= 1;
        const bool at_turn = found.signals_before_turn <= 1;
        if (!state.latched_off) {
            if (at_turn) state.matched = true;
        } else if (before_entry && at_turn && station != state.left_at) {
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
