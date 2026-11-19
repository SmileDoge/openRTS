#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <openrts/core/vfs.hpp>
#include <openrts/formats/route_world.hpp>

namespace openrts::formats {

// One signal of the route database: the console's per-signal data, matched
// to the world signal by position (RTS 0x4903DC: tile and x/y/z within
// 0.2 m of the TDB signal item).
struct RouteSignalUnit {
    RoutePosition position;
    std::uint32_t items{};          // NumSigItems
    std::string name;               // the signal's name ("литера"), may be empty
    std::uint32_t flags{};          // UnitFlags
    std::string default_aspect;     // DefaultAspect, e.g. STOP
};

// The RTS route database (`<route>/<RouteID>.rtsdb`, UTF-16 text,
// SIMISA@@@@@@@@@@JINX1R0t; docs/reverse/dispatcher.md): the game reads the
// signal names from it (Ctrl+F7), the dispatcher console also its station
// routes. Only the signal units are read so far.
// A place of the track in the route database: a position and the number
// after it (a route point's end: its direction flag; a junction: the
// branch its route takes).
struct RouteTrackPlace {
    RoutePosition position;
    std::uint32_t value{};
};

// RoutePoint (rts_server.exe 0x4640F0): a place a station route starts or
// ends at. PointType 1 a point (Point1), 2 a track between Point1 and
// Point2; Flags bit 2 it leads onto a stretch (an oncoming route there is
// refused, 0x469804), bit 4 no occupancy checks; Junctions: those within
// the track, with their branches.
struct RoutePoint {
    std::string name;
    std::uint32_t type{};
    std::uint32_t flags{};
    std::string station;
    RouteTrackPlace point1;
    std::optional<RouteTrackPlace> point2;
    std::vector<RouteTrackPlace> junctions;
};

// TrainRoute (0x464674): a station route from one route point to another
// of its station. Flags bit 4: no occupancy checks; bit 8: it may be set
// part of the way (junction after junction, 0x46A348).
struct StationRoute {
    std::uint32_t id{};
    std::string name;
    std::uint32_t flags{};
    std::string station;
    std::string start; // route point names
    std::string end;
};

// TrainPath (0x464890): a station route's way, the junctions it passes in
// order with the branch each is set to (0x4361F4 walks them from the start
// point). Start bit 2: from the start point's Point2 (else Point1).
struct StationPath {
    std::uint32_t route{};
    std::string name;
    std::uint32_t priority{};
    std::uint32_t start{};
    std::vector<RouteTrackPlace> junctions;
};

struct RouteDatabase {
    std::vector<RoutePoint> points;     // RoutePoints
    std::vector<StationRoute> routes;   // RouteTrainRoutes
    std::vector<StationPath> paths;     // RouteTrainPaths
    std::vector<RouteSignalUnit> signal_units;
    // SignalTypeShownAsNormal ( "<type>" ): signal types (sigcfg names) the
    // console draws as train signals, whose heads also count for their
    // unit's aspect (rts_server.exe: the type's flag 0x100).
    std::vector<std::string> shown_as_normal;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

[[nodiscard]] RouteDatabase parse_route_database(std::string_view bytes);

// The route database `original` (its file's bytes; empty: a new one) with
// its SignalTypeShownAsNormal and SignalUnits as `database` has them, as
// rts_server.exe's "Сохранить всё" writes them (0x448914): the types after
// the header, the units (Position, NumSigItems, Name when there is one,
// UnitFlags, DefaultAspect) in their order where the old ones were, the
// rest of the file as it was. UTF-16LE with a BOM, CRLF lines.
[[nodiscard]] std::string write_route_database(std::string_view original, const RouteDatabase& database, std::string& error);

struct TrackDatabase;
// The TDB signal item each named signal unit belongs to: first as RTS
// matches (0x4903DC: same tile, x/y/z within 0.2 m), then each unit still
// without one takes the nearest signal item no other unit has, within
// `reach` metres (signals moved after the database was written; the RTS
// server reports such "Signal ID changes (shifts) in tdb"). Item -> unit.
[[nodiscard]] std::map<std::uint32_t, std::size_t> match_signal_units(const TrackDatabase& tdb, const RouteDatabase& database,
                                                                      double reach = 30.0);
// The unit RTS hangs on each TDB signal item (its RTSSignalData, 0x4903DC):
// units in file order, each taken by every signal item still without one on
// the same tile within 0.2 m in x, y and z. Named or not; no fallback.
// By item number; -1 none.
[[nodiscard]] std::vector<std::int32_t> rts_signal_units(const TrackDatabase& tdb, const RouteDatabase& database);
// `<route>/<RouteID>.rtsdb` (RouteID from the .trk, as RTS builds the name),
// else the only .rtsdb of the folder; nothing when there is none.
[[nodiscard]] std::optional<core::VirtualPath> route_database_file(const core::Vfs& vfs, const core::VirtualPath& route);
// That file read; an empty database when there is none.
[[nodiscard]] RouteDatabase load_route_database(const core::Vfs& vfs, const core::VirtualPath& route);

} // namespace openrts::formats
