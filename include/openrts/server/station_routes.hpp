#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <openrts/formats/route_database.hpp>
#include <openrts/simulation/track_network.hpp>
#include <openrts/simulation/train_route.hpp>

// The route database's station routes on the server's network
// (docs/server.md, "Station routes"; rts_server.exe: RoutePoints 0x4640F0,
// TrainRoutes 0x464674, TrainPaths 0x464890 and 0x4361F4). A station route
// runs from one route point to another through the junctions its path
// lists, each at its branch; the dispatcher sets them for trains as
// segments of their routes (GameServer::add_segment).
namespace openrts::server {

// The flags as rts_server.exe's route form ("Маршруты поездов", the PDF
// "Поездные маршруты" by its checkboxes' order; PROBABLE but bit 4 and 8 of
// a route and bit 2 of a point, CONFIRMED by the code): a route's
namespace route_flag {
constexpr std::uint32_t main_track = 1; // "Главный путь": first in the list, green
constexpr std::uint32_t one_way = 2;    // "В одну сторону": only from its start
constexpr std::uint32_t shunting = 4;   // "Маневровый": no occupancy checks
constexpr std::uint32_t composite = 8;  // "Составной": may be set part of the way
} // namespace route_flag
// and a route point's.
namespace point_flag {
constexpr std::uint32_t wrong_way = 1; // "Неправильный путь": last in the list, lilac
constexpr std::uint32_t stretch = 2;   // "Проверять встречный маршрут": leads onto a stretch
constexpr std::uint32_t shunting = 4;  // "Маневровый": onto sidings, no occupancy checks
constexpr std::uint32_t platform = 8;  // a passenger platform ("[П]")
} // namespace point_flag

class StationRoutes {
public:
    struct Point {
        const formats::RoutePoint* data = nullptr;
        // Point1 and Point2 on the track (the direction: along the node).
        std::optional<simulation::TrackLocation> place1, place2;
        double length{}; // a track's, from one place to the other (0: one place)
    };
    struct Path {
        const formats::StationPath* data = nullptr;
        std::size_t index{}; // in the database's TrainPaths (0x450B gives index + 1)
        std::vector<std::pair<std::uint32_t, std::uint32_t>> junctions; // junction node, branch, in order
        // The way from the start point to the end point through the
        // junctions, and from the end back to the start (a segment run from
        // its end); nothing when it could not be laid.
        std::optional<simulation::RouteLeg> forward, backward;
        std::string problem;
    };
    struct Route {
        const formats::StationRoute* data = nullptr;
        std::optional<std::size_t> start, end; // route points
        std::vector<std::size_t> paths;        // by priority
    };

    StationRoutes() = default;
    // `database` must outlive the result (it points into it).
    [[nodiscard]] static StationRoutes build(const simulation::TrackNetwork& network, const formats::RouteDatabase& database);

    [[nodiscard]] const std::vector<Point>& points() const noexcept { return points_; }
    [[nodiscard]] const std::vector<Route>& routes() const noexcept { return routes_; }
    [[nodiscard]] const std::vector<Path>& paths() const noexcept { return paths_; }
    [[nodiscard]] const Route* route(std::uint32_t id) const;
    [[nodiscard]] std::optional<std::size_t> route_index(std::uint32_t id) const;
    // The first of the route's paths laid in that direction.
    [[nodiscard]] const Path* path_of(const Route& route, bool reverse) const;
    // The station routes' stations, with how many routes each has.
    [[nodiscard]] std::vector<std::pair<std::string, std::size_t>> stations() const;
    // The route point first met beyond `from` the way it faces, along the
    // junctions as they stand, within `limit` m (rts_server.exe 0x436510: a
    // stretch's far end); `skip`: a point not to count.
    [[nodiscard]] std::optional<std::size_t> point_beyond(const simulation::TrackNetwork& network, const simulation::TrackLocation& from,
                                                          double limit, std::optional<std::size_t> skip) const;
    // A station route the train can take next (rts_server.exe's "Собрать
    // маршрут": "все маршруты в выбранном направлении, доступные от текущей
    // точки").
    struct Choice {
        std::size_t route{};  // in routes()
        bool reverse = false; // run from its end
        double distance{};    // along the track to its first point (< 0 behind)
    };
    // The routes that can be taken from `from` the way it faces: those
    // starting the way it faces at the first route point met on each way on
    // (through junctions thrown any way, `limit` m at most; the point
    // `from` stands at counts, an exit with nothing our way is passed), and
    // at the first point within `behind` m back (a train standing past its
    // track's point). One-way routes only from their start. Nearest first.
    [[nodiscard]] std::vector<Choice> available(const simulation::TrackNetwork& network, const simulation::TrackLocation& from, double limit,
                                                double behind = 0.0) const;
    // The way a route runs: its path's leg (nothing when not laid).
    [[nodiscard]] const simulation::RouteLeg* leg_of(const Route& route, bool reverse) const;
    // What was laid: "182 points (180 on the track), 828 routes, 828 paths (820 laid)".
    [[nodiscard]] const std::string& summary() const noexcept { return summary_; }

private:
    // Walks on from `from` the way it faces, through junctions thrown any
    // way (facing: each branch; trailing: the trunk), shortest ways first,
    // calling `meet(point, distance, place)` at each route point place met
    // (`place` facing the way walked); true from it ends that way there.
    void walk(const simulation::TrackNetwork& network, const simulation::TrackLocation& from, double limit,
              const std::function<bool(std::size_t, double, const simulation::TrackLocation&)>& meet) const;

    struct LegStart {
        std::size_t route{};
        bool reverse = false;
        simulation::TrackLocation at;
    };
    std::vector<Point> points_;
    std::vector<Route> routes_;
    std::vector<Path> paths_;
    std::map<std::uint32_t, std::vector<std::pair<std::size_t, double>>> places_by_node_; // node: (point, offset)
    std::vector<std::vector<LegStart>> starts_by_point_;                                   // the legs starting at each point
    std::string summary_;
};

} // namespace openrts::server
