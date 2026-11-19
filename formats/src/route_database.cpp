#include <openrts/formats/route_database.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <tuple>
#include <cstdlib>

#include <openrts/formats/track_database.hpp>

#include "rts_text.hpp"

namespace openrts::formats {
namespace {

using detail::rts_find;
using detail::rts_next;
using detail::rts_token;
using detail::TextRange;

[[nodiscard]] std::vector<std::string> tokens(std::string_view text, const TextRange range) {
    std::vector<std::string> result;
    for (auto position = range.begin; position < range.end; position = rts_next(text, position, range.end)) {
        auto token = rts_token(text, position, range.end);
        if (token.empty()) break;
        result.push_back(std::move(token));
    }
    return result;
}

[[nodiscard]] std::optional<RoutePlace> place(const std::string_view text, const TextRange range) {
    const auto v = tokens(text, range);
    if (v.size() < 6) return std::nullopt;
    RoutePlace result;
    result.tile_x = std::atoi(v[0].c_str());
    result.tile_z = std::atoi(v[1].c_str());
    result.x = std::strtof(v[2].c_str(), nullptr);
    result.y = std::strtof(v[3].c_str(), nullptr);
    result.z = std::strtof(v[4].c_str(), nullptr);
    result.flag = static_cast<std::uint32_t>(std::strtoul(v[5].c_str(), nullptr, 10));
    return result;
}

} // namespace

RouteDatabase parse_route_database(const std::string_view bytes) {
    RouteDatabase database;
    std::string error;
    const auto original = detail::decode_text(bytes, error);
    if (!original) return {.error = error};
    // Keywords on a lower-cased copy; names from the original at the same
    // positions (lower-casing keeps the length).
    std::string lower = *original;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::string_view t = lower;
    const std::string_view o = *original;
    const auto units = rts_find(t, "signalunits");
    if (!units) return database;
    auto within = *units;
    while (const auto block = rts_find(t, "signalunit", within)) {
        RouteSignalUnit unit;
        if (const auto position = rts_find(t, "position", *block)) {
            const auto v = tokens(t, *position);
            if (v.size() >= 5) {
                unit.position = {.tile_x = std::atoi(v[0].c_str()),
                                 .tile_z = std::atoi(v[1].c_str()),
                                 .x = std::atof(v[2].c_str()),
                                 .y = std::atof(v[3].c_str()),
                                 .z = std::atof(v[4].c_str())};
            }
        }
        if (const auto items = rts_find(t, "numsigitems", *block)) unit.items = static_cast<std::uint32_t>(std::atoi(rts_token(t, items->begin, items->end).c_str()));
        if (const auto name = rts_find(t, "name", *block)) unit.name = rts_token(o, name->begin, name->end);
        if (const auto flags = rts_find(t, "unitflags", *block)) unit.flags = static_cast<std::uint32_t>(std::strtoul(rts_token(t, flags->begin, flags->end).c_str(), nullptr, 16));
        if (const auto aspect = rts_find(t, "defaultaspect", *block)) unit.default_aspect = rts_token(o, aspect->begin, aspect->end);
        database.signal_units.push_back(std::move(unit));
        within.begin = block->end + 1;
        if (within.begin >= within.end) break;
    }
    if (const auto points = rts_find(t, "routepoints")) {
        auto within = *points;
        while (const auto block = rts_find(t, "routepoint", within)) {
            RoutePoint point;
            if (const auto name = rts_find(t, "name", *block)) point.name = rts_token(o, name->begin, name->end);
            if (const auto type = rts_find(t, "pointtype", *block))
                point.point_type = static_cast<std::uint32_t>(std::atoi(rts_token(t, type->begin, type->end).c_str()));
            if (const auto flags = rts_find(t, "flags", *block))
                point.flags = static_cast<std::uint32_t>(std::strtoul(rts_token(t, flags->begin, flags->end).c_str(), nullptr, 16));
            if (const auto station = rts_find(t, "stationname", *block)) point.station = rts_token(o, station->begin, station->end);
            if (const auto first = rts_find(t, "point1", *block)) {
                if (const auto parsed = place(t, *first)) point.first = *parsed;
            }
            if (const auto second = rts_find(t, "point2", *block)) {
                if (const auto parsed = place(t, *second)) {
                    point.second = *parsed;
                    point.has_second = true;
                }
            }
            database.points.push_back(std::move(point));
            within.begin = block->end + 1;
            if (within.begin >= within.end) break;
        }
    }
    if (const auto routes = rts_find(t, "routetrainroutes")) {
        auto within = *routes;
        while (const auto block = rts_find(t, "trainroute", within)) {
            RouteTrainRoute route;
            if (const auto id = rts_find(t, "id", *block))
                route.id = static_cast<std::uint32_t>(std::atoi(rts_token(t, id->begin, id->end).c_str()));
            if (const auto name = rts_find(t, "name", *block)) route.name = rts_token(o, name->begin, name->end);
            if (const auto flags = rts_find(t, "flags", *block))
                route.flags = static_cast<std::uint32_t>(std::strtoul(rts_token(t, flags->begin, flags->end).c_str(), nullptr, 16));
            if (const auto station = rts_find(t, "stationname", *block)) route.station = rts_token(o, station->begin, station->end);
            if (const auto start = rts_find(t, "start", *block)) route.start = rts_token(o, start->begin, start->end);
            if (const auto end = rts_find(t, "end", *block)) route.end = rts_token(o, end->begin, end->end);
            database.routes.push_back(std::move(route));
            within.begin = block->end + 1;
            if (within.begin >= within.end) break;
        }
    }
    if (const auto paths = rts_find(t, "routetrainpaths")) {
        auto within = *paths;
        while (const auto block = rts_find(t, "trainpath", within)) {
            RouteTrainPath path;
            if (const auto id = rts_find(t, "routeid", *block))
                path.route_id = static_cast<std::uint32_t>(std::atoi(rts_token(t, id->begin, id->end).c_str()));
            if (const auto priority = rts_find(t, "priority", *block))
                path.priority = static_cast<std::uint32_t>(std::atoi(rts_token(t, priority->begin, priority->end).c_str()));
            if (const auto points = rts_find(t, "points", *block)) {
                auto rows = *points;
                while (const auto row = rts_find(t, "point", rows)) {
                    if (const auto parsed = place(t, *row)) path.points.push_back(*parsed);
                    rows.begin = row->end + 1;
                    if (rows.begin >= rows.end) break;
                }
            }
            database.paths.push_back(std::move(path));
            within.begin = block->end + 1;
            if (within.begin >= within.end) break;
        }
    }
    return database;
}

std::vector<std::int32_t> rts_signal_units(const TrackDatabase& tdb, const RouteDatabase& database) {
    std::vector<std::int32_t> units(tdb.items.size(), -1);
    for (std::size_t u = 0; u < database.signal_units.size(); ++u) {
        const auto& unit = database.signal_units[u];
        for (std::size_t k = 0; k < tdb.items.size(); ++k) {
            const auto& item = tdb.items[k];
            if (item.kind != TrackItemKind::signal || units[k] >= 0 || !item.has_position) continue;
            if (item.tile_x != unit.position.tile_x || item.tile_z != unit.position.tile_z) continue;
            if (std::abs(item.x - unit.position.x) <= 0.2 && std::abs(item.y - unit.position.y) <= 0.2
                && std::abs(item.z - unit.position.z) <= 0.2)
                units[k] = static_cast<std::int32_t>(u);
        }
    }
    return units;
}

std::map<std::uint32_t, std::size_t> match_signal_units(const TrackDatabase& tdb, const RouteDatabase& database, const double reach) {
    std::map<std::uint32_t, std::size_t> matched;
    std::vector<std::uint32_t> signals;
    for (std::uint32_t k = 0; k < tdb.items.size(); ++k) {
        if (tdb.items[k].kind == TrackItemKind::signal && tdb.items[k].has_position) signals.push_back(k);
    }
    const auto distance = [&](const TrackItem& item, const RouteSignalUnit& unit) {
        const double dx = (item.tile_x - unit.position.tile_x) * 2048.0 + item.x - unit.position.x;
        const double dz = (item.tile_z - unit.position.tile_z) * 2048.0 + item.z - unit.position.z;
        const double dy = item.y - unit.position.y;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    std::vector<char> unit_done(database.signal_units.size(), 0);
    // As RTS: the same place.
    for (std::size_t u = 0; u < database.signal_units.size(); ++u) {
        const auto& unit = database.signal_units[u];
        if (unit.name.empty()) continue;
        for (const auto k : signals) {
            const auto& item = tdb.items[k];
            if (item.tile_x != unit.position.tile_x || item.tile_z != unit.position.tile_z || matched.contains(k)) continue;
            if (std::abs(item.x - unit.position.x) <= 0.2 && std::abs(item.y - unit.position.y) <= 0.2
                && std::abs(item.z - unit.position.z) <= 0.2) {
                matched[k] = u;
                unit_done[u] = 1;
                break;
            }
        }
    }
    // The rest: nearest pairs first, each item and unit once.
    std::vector<std::tuple<double, std::uint32_t, std::size_t>> candidates;
    for (std::size_t u = 0; u < database.signal_units.size(); ++u) {
        const auto& unit = database.signal_units[u];
        if (unit.name.empty() || unit_done[u]) continue;
        for (const auto k : signals) {
            if (matched.contains(k)) continue;
            const double d = distance(tdb.items[k], unit);
            if (d <= reach) candidates.emplace_back(d, k, u);
        }
    }
    std::sort(candidates.begin(), candidates.end());
    for (const auto& [d, k, u] : candidates) {
        if (unit_done[u] || matched.contains(k)) continue;
        matched[k] = u;
        unit_done[u] = 1;
    }
    return matched;
}

RouteDatabase load_route_database(const core::Vfs& vfs, const core::VirtualPath& route) {
    std::optional<core::VirtualPath> file;
    const auto definition = load_route_definition(vfs, route);
    if (definition.ok() && !definition.route_id.empty()) {
        if (const auto candidate = route.resolve(definition.route_id + ".rtsdb"); candidate && vfs.exists(*candidate)) file = candidate;
    }
    if (!file) {
        for (const auto& entry : vfs.list(route)) {
            if (entry.directory) continue;
            std::string name(entry.path.filename());
            for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (name.size() > 6 && name.ends_with(".rtsdb")) {
                file = entry.path;
                break;
            }
        }
    }
    if (!file) return {};
    std::string error;
    const auto bytes = vfs.read(*file, &error);
    if (!bytes) return {.error = error};
    return parse_route_database(*bytes);
}

} // namespace openrts::formats
