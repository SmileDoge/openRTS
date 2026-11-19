#include <openrts/server/station_routes.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <queue>
#include <set>

#include <openrts/simulation/track_geometry.hpp>
#include <openrts/simulation/track_traveller.hpp>

namespace openrts::server {
namespace {

[[nodiscard]] simulation::TrackPoint track_point(const formats::RoutePosition& p) {
    return {.tile_x = p.tile_x, .tile_z = p.tile_z, .x = p.x, .y = p.y, .z = p.z};
}

[[nodiscard]] std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Within this of the track a route point stands on it, and a TrainPath's
// point on its junction (RTS places both with 0x428790).
constexpr double place_reach = 3.0;

// Every vector node reached leaving `from` at its start (end 0) or end (1),
// through junctions thrown any way: facing, each branch; trailing, the
// trunk (as simulation's follow(), 0x47ECD0, without the junctions' state).
void next_nodes(const simulation::TrackNetwork& network, const std::uint32_t from, const int end,
                std::vector<std::pair<std::uint32_t, bool>>& out) {
    const auto* node = network.node(from);
    if (!node || node->pins.size() < 2) return;
    struct Step {
        formats::TrackPin pin;
        std::uint32_t previous{};
        bool previous_is_vector = true;
        int previous_end{};
        int depth{};
    };
    std::vector<Step> stack{{node->pins[static_cast<std::size_t>(end)], from, true, end, 0}};
    while (!stack.empty()) {
        const auto s = stack.back();
        stack.pop_back();
        const auto* target = network.node(s.pin.node);
        if (!target || s.depth > 64) continue;
        if (target->kind == formats::TrackNodeKind::vector) {
            out.emplace_back(target->index, s.pin.direction == 1);
            continue;
        }
        if (target->kind != formats::TrackNodeKind::junction || target->pins.size() < 2) continue;
        if (s.pin.direction == 1) {
            for (std::size_t k = 1; k < target->pins.size(); ++k) stack.push_back({target->pins[k], target->index, false, 0, s.depth + 1});
            continue;
        }
        // In on a branch: out the trunk.
        for (std::size_t k = 1; k < target->pins.size(); ++k) {
            const auto& back = target->pins[k];
            if (back.node != s.previous) continue;
            if (s.previous_is_vector && back.direction != static_cast<std::uint32_t>(s.previous_end == 1 ? 0 : 1)) continue;
            stack.push_back({target->pins[0], target->index, false, 0, s.depth + 1});
            break;
        }
    }
}

// The way a place faces on the map (radians), `forward` along its node.
[[nodiscard]] std::optional<float> heading_at(const simulation::TrackNetwork& network, const simulation::TrackLocation& at) {
    const auto pose = network.point(at.node, at.offset);
    if (!pose) return std::nullopt;
    return pose->heading + (at.forward ? 0.0f : 3.14159265f);
}

} // namespace

StationRoutes StationRoutes::build(const simulation::TrackNetwork& network, const formats::RouteDatabase& database) {
    StationRoutes out;
    // The route points on the track.
    std::size_t placed = 0;
    for (const auto& data : database.points) {
        Point point{.data = &data};
        const auto locate = [&](const formats::RouteTrackPlace& place) -> std::optional<simulation::TrackLocation> {
            const auto at = network.locate(track_point(place.position), place_reach, 1.0);
            if (!at) return std::nullopt;
            return simulation::TrackLocation{at->node, at->offset, true};
        };
        point.place1 = locate(data.point1);
        if (data.point2) point.place2 = locate(*data.point2);
        if (point.place1 || point.place2) ++placed;
        out.points_.push_back(point);
    }
    for (const auto& point : out.points_) {
        for (const auto& place : {point.place1, point.place2}) {
            if (place) out.places_by_node_[place->node].emplace_back(&point - out.points_.data(), place->offset);
        }
    }
    // A track's length: from one place to the other along the track.
    for (auto& point : out.points_) {
        if (!point.place1 || !point.place2) continue;
        const auto index = static_cast<std::size_t>(&point - out.points_.data());
        for (const bool forward : {true, false}) {
            out.walk(network, {point.place1->node, point.place1->offset, forward}, 5000.0,
                     [&](const std::size_t met, const double distance, const simulation::TrackLocation&) {
                         if (met != index || distance < 1.0) return false;
                         if (point.length == 0.0 || distance < point.length) point.length = distance;
                         return true;
                     });
        }
    }
    // Points by station and name (0x449BF8: the station's point of that name).
    std::map<std::pair<std::string, std::string>, std::size_t> point_of;
    for (std::size_t i = 0; i < out.points_.size(); ++i) {
        point_of.emplace(std::pair{lower(out.points_[i].data->station), lower(out.points_[i].data->name)}, i);
    }
    // Junctions by their place.
    std::vector<std::pair<std::uint32_t, simulation::TrackPoint>> junctions;
    for (std::uint32_t n = 1; n < network.node_count(); ++n) {
        const auto* node = network.node(n);
        if (!node || node->kind != formats::TrackNodeKind::junction) continue;
        if (const auto at = network.node_position(n)) junctions.emplace_back(n, *at);
    }
    const auto junction_at = [&](const formats::RoutePosition& position) -> std::optional<std::uint32_t> {
        const auto target = track_point(position);
        std::optional<std::uint32_t> best;
        double nearest = place_reach;
        for (const auto& [n, at] : junctions) {
            if (std::abs(at.tile_x - target.tile_x) > 1 || std::abs(at.tile_z - target.tile_z) > 1) continue;
            const double d = simulation::distance(at, target);
            if (d <= nearest) {
                nearest = d;
                best = n;
            }
        }
        return best;
    };
    for (const auto& data : database.routes) {
        Route route{.data = &data};
        if (const auto it = point_of.find({lower(data.station), lower(data.start)}); it != point_of.end()) route.start = it->second;
        if (const auto it = point_of.find({lower(data.station), lower(data.end)}); it != point_of.end()) route.end = it->second;
        out.routes_.push_back(route);
    }
    std::map<std::uint32_t, std::size_t> route_of;
    for (std::size_t i = 0; i < out.routes_.size(); ++i) route_of.emplace(out.routes_[i].data->id, i);

    // The paths: their junctions, then the way through them each way.
    std::size_t laid = 0;
    for (std::size_t p = 0; p < database.paths.size(); ++p) {
        const auto& data = database.paths[p];
        Path path{.data = &data, .index = p};
        for (const auto& place : data.junctions) {
            const auto node = junction_at(place.position);
            if (!node) {
                path.problem = "a junction of its path is not on the track";
                break;
            }
            path.junctions.emplace_back(*node, place.value);
        }
        const auto route = route_of.find(data.route);
        if (route == route_of.end()) {
            path.problem = "no route " + std::to_string(data.route);
        } else if (path.problem.empty()) {
            const auto& r = out.routes_[route->second];
            if (!r.start || !r.end) {
                path.problem = "its route's start or end point is not in the database";
            } else {
                const auto& from_point = out.points_[*r.start];
                const auto& to_point = out.points_[*r.end];
                std::map<std::uint32_t, std::uint32_t> branches;
                for (const auto& [node, branch] : path.junctions) branches[node] = branch;
                // The shortest way from one point's places to the other's
                // through the junctions in that order, either way along the
                // start's node. Start bit 2: from the start point's Point2.
                const auto lay = [&](const Point& a, const Point& b, std::vector<std::uint32_t> through, const bool second_first)
                    -> std::optional<simulation::RouteLeg> {
                    std::vector<simulation::TrackLocation> starts;
                    for (const auto& place : second_first ? std::vector{a.place2, a.place1} : std::vector{a.place1, a.place2}) {
                        if (place) starts.push_back(*place);
                    }
                    std::optional<simulation::RouteLeg> best;
                    for (const auto& start : starts) {
                        for (const bool forward : {true, false}) {
                            // To a track: on to its far place (its points
                            // stand by its exit signals, the PDF: "чтобы
                            // корректно расположить точку окончания
                            // маршрута"), when the way to it runs through
                            // the near one.
                            std::optional<simulation::RouteLeg> near, far;
                            for (const auto& end : {b.place1, b.place2}) {
                                if (!end) continue;
                                simulation::PathPlan plan;
                                plan.name = a.data->name + " => " + b.data->name;
                                plan.start = {start.node, start.offset, forward};
                                plan.points.push_back({.kind = simulation::PlanPointKind::end,
                                                       .node = end->node,
                                                       .offset = end->offset,
                                                       .through = through,
                                                       .branches = branches});
                                auto built = simulation::build_route(network, plan);
                                if (!built.ok() || built.legs.size() != 1) continue;
                                if (!near || built.legs[0].length() < near->length()) {
                                    if (near) far = std::move(near);
                                    near = std::move(built.legs[0]);
                                } else {
                                    far = std::move(built.legs[0]);
                                }
                            }
                            if (!near) continue;
                            auto leg = std::move(*near);
                            if (far && b.length > 0.0 && std::abs(far->length() - leg.length() - b.length) < 10.0) leg = std::move(*far);
                            if (!best || leg.length() < best->length()) best = std::move(leg);
                        }
                        if (best) break;
                    }
                    return best;
                };
                std::vector<std::uint32_t> through;
                for (const auto& [node, _] : path.junctions) through.push_back(node);
                path.forward = lay(from_point, to_point, through, (data.start & 2u) != 0);
                std::reverse(through.begin(), through.end());
                path.backward = lay(to_point, from_point, through, false);
                if (!path.forward && !path.backward) path.problem = "no way through its junctions between its points";
            }
            out.routes_[route->second].paths.push_back(p);
        }
        if (path.forward || path.backward) ++laid;
        out.paths_.push_back(std::move(path));
    }
    for (auto& route : out.routes_) {
        std::stable_sort(route.paths.begin(), route.paths.end(),
                         [&](const std::size_t a, const std::size_t b) { return out.paths_[a].data->priority < out.paths_[b].data->priority; });
    }
    // Where each route's way starts, by its first point.
    out.starts_by_point_.assign(out.points_.size(), {});
    for (std::size_t r = 0; r < out.routes_.size(); ++r) {
        const auto& route = out.routes_[r];
        for (const bool reverse : {false, true}) {
            if (reverse && (route.data->flags & route_flag::one_way)) continue;
            const auto first = reverse ? route.end : route.start;
            const auto* leg = out.leg_of(route, reverse);
            const auto at = leg ? leg->at(0.0) : std::nullopt;
            if (first && at) out.starts_by_point_[*first].push_back({r, reverse, *at});
        }
    }
    out.summary_ = std::to_string(out.points_.size()) + " route points (" + std::to_string(placed) + " on the track), " +
                   std::to_string(out.routes_.size()) + " station routes, " + std::to_string(out.paths_.size()) + " paths (" +
                   std::to_string(laid) + " laid)";
    return out;
}

const StationRoutes::Route* StationRoutes::route(const std::uint32_t id) const {
    const auto index = route_index(id);
    return index ? &routes_[*index] : nullptr;
}

std::optional<std::size_t> StationRoutes::route_index(const std::uint32_t id) const {
    for (std::size_t i = 0; i < routes_.size(); ++i) {
        if (routes_[i].data->id == id) return i;
    }
    return std::nullopt;
}

const StationRoutes::Path* StationRoutes::path_of(const Route& route, const bool reverse) const {
    for (const auto p : route.paths) {
        const auto& path = paths_[p];
        if (reverse ? path.backward.has_value() : path.forward.has_value()) return &path;
    }
    return nullptr;
}

const simulation::RouteLeg* StationRoutes::leg_of(const Route& route, const bool reverse) const {
    const auto* path = path_of(route, reverse);
    if (!path) return nullptr;
    const auto& leg = reverse ? path->backward : path->forward;
    return leg ? &*leg : nullptr;
}

void StationRoutes::walk(const simulation::TrackNetwork& network, const simulation::TrackLocation& from, const double limit,
                         const std::function<bool(std::size_t, double, const simulation::TrackLocation&)>& meet) const {
    struct Way {
        double walked{};
        std::uint32_t node{};
        double offset{};
        bool forward = true;
        bool first = false;
        bool operator>(const Way& other) const { return walked > other.walked; }
    };
    std::priority_queue<Way, std::vector<Way>, std::greater<>> ways;
    ways.push({0.0, from.node, from.offset, from.forward, true});
    std::set<std::pair<std::uint32_t, bool>> visited;
    std::vector<std::pair<std::uint32_t, bool>> next;
    while (!ways.empty()) {
        const auto way = ways.top();
        ways.pop();
        if (way.walked > limit || !visited.insert({way.node, way.forward}).second) continue;
        const auto* info = network.node(way.node);
        if (!info || info->kind != formats::TrackNodeKind::vector) continue;
        // The places on this node ahead, nearest first (the one `from`
        // stands at too, within a metre back).
        std::vector<std::pair<double, std::size_t>> ahead;
        if (const auto places = places_by_node_.find(way.node); places != places_by_node_.end()) {
            for (const auto& [point, offset] : places->second) {
                const double d = way.forward ? offset - way.offset : way.offset - offset;
                if (d >= (way.first ? -1.0 : -1e-6)) ahead.emplace_back(d, point);
            }
        }
        std::sort(ahead.begin(), ahead.end());
        bool ended = false;
        for (const auto& [d, point] : ahead) {
            const double distance = way.walked + (std::max)(d, 0.0);
            if (distance > limit) break;
            const double offset = way.forward ? way.offset + d : way.offset - d;
            if (meet(point, distance, {way.node, offset, way.forward})) {
                ended = true;
                break;
            }
        }
        if (ended) continue;
        const double rest = way.forward ? info->length - way.offset : way.offset;
        next.clear();
        next_nodes(network, way.node, way.forward ? 1 : 0, next);
        for (const auto& [node, forward] : next) {
            const auto* target = network.node(node);
            if (!target) continue;
            ways.push({way.walked + rest, node, forward ? 0.0 : target->length, forward, false});
        }
    }
}

std::vector<StationRoutes::Choice> StationRoutes::available(const simulation::TrackNetwork& network, const simulation::TrackLocation& from,
                                                            const double limit, const double behind) const {
    std::vector<Choice> out;
    std::set<std::pair<std::size_t, bool>> taken;
    // The routes starting at `point` the way `at` faces.
    const auto ours = [&](const std::size_t point, const simulation::TrackLocation& at, const double distance) {
        const auto here = heading_at(network, at);
        if (!here || point >= starts_by_point_.size()) return false;
        bool any = false;
        for (const auto& start : starts_by_point_[point]) {
            const auto there = heading_at(network, start.at);
            if (!there || std::cos(*there - *here) < 0.3f) continue;
            any = true;
            if (taken.insert({start.route, start.reverse}).second) out.push_back({start.route, start.reverse, distance});
        }
        return any;
    };
    walk(network, from, limit, [&](const std::size_t point, const double distance, const simulation::TrackLocation& at) {
        if (ours(point, at, distance)) return true;
        // A point routes start from, none our way, ends the way; but the one
        // `from` stands at (an exit onto a stretch: on to the next station)
        // and one nothing starts from (half a track, Kuban's 4АП before 4БП).
        return distance > 1.0 && !starts_by_point_[point].empty();
    });
    if (behind > 0.0) {
        walk(network, {from.node, from.offset, !from.forward}, behind, [&](const std::size_t point, const double distance, const simulation::TrackLocation& at) {
            ours(point, {at.node, at.offset, !at.forward}, -distance);
            return true;
        });
    }
    std::stable_sort(out.begin(), out.end(), [](const Choice& a, const Choice& b) { return a.distance < b.distance; });
    return out;
}

std::vector<std::pair<std::string, std::size_t>> StationRoutes::stations() const {
    std::map<std::string, std::size_t> count;
    for (const auto& route : routes_) ++count[route.data->station];
    return {count.begin(), count.end()};
}

std::optional<std::size_t> StationRoutes::point_beyond(const simulation::TrackNetwork& network, const simulation::TrackLocation& from,
                                                       const double limit, const std::optional<std::size_t> skip) const {
    auto node = from.node;
    double offset = from.offset;
    bool forward = from.forward;
    double walked = 0.0;
    std::set<std::pair<std::uint32_t, bool>> visited;
    while (walked <= limit) {
        const auto* info = network.node(node);
        if (!info || !visited.insert({node, forward}).second) break;
        std::optional<std::size_t> best;
        double nearest = 0.0;
        for (std::size_t i = 0; i < points_.size(); ++i) {
            if (skip && *skip == i) continue;
            for (const auto& place : {points_[i].place1, points_[i].place2}) {
                if (!place || place->node != node) continue;
                const double ahead = forward ? place->offset - offset : offset - place->offset;
                if (ahead <= 0.5 || (best && ahead >= nearest)) continue;
                best = i;
                nearest = ahead;
            }
        }
        if (best) return best;
        walked += forward ? info->length - offset : offset;
        simulation::MoveStop stop = simulation::MoveStop::completed;
        const auto step = simulation::next_vector_node(network, node, forward, stop);
        if (!step) break;
        node = step->node;
        forward = step->forward;
        const auto* next = network.node(node);
        offset = forward ? 0.0 : (next ? next->length : 0.0);
    }
    return std::nullopt;
}

} // namespace openrts::server
