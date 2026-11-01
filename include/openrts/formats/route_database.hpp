#pragma once

#include <cstdint>
#include <map>
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
struct RouteDatabase {
    std::vector<RouteSignalUnit> signal_units;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

[[nodiscard]] RouteDatabase parse_route_database(std::string_view bytes);

struct TrackDatabase;
// The TDB signal item each named signal unit belongs to: first as RTS
// matches (0x4903DC: same tile, x/y/z within 0.2 m), then each unit still
// without one takes the nearest signal item no other unit has, within
// `reach` metres (signals moved after the database was written; the RTS
// server reports such "Signal ID changes (shifts) in tdb"). Item -> unit.
[[nodiscard]] std::map<std::uint32_t, std::size_t> match_signal_units(const TrackDatabase& tdb, const RouteDatabase& database,
                                                                      double reach = 30.0);
// `<route>/<RouteID>.rtsdb` (RouteID from the .trk, as RTS builds the name),
// else the only .rtsdb of the folder; an empty database when there is none.
[[nodiscard]] RouteDatabase load_route_database(const core::Vfs& vfs, const core::VirtualPath& route);

} // namespace openrts::formats
