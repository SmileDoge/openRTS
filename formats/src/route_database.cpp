#include <openrts/formats/route_database.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <optional>
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
    for (TextRange from{0, t.size()}; const auto type = rts_find(t, "signaltypeshownasnormal", from);) {
        if (auto name = rts_token(o, type->begin, type->end); !name.empty()) database.shown_as_normal.push_back(std::move(name));
        from.begin = type->end + 1;
        if (from.begin >= from.end) break;
    }
    return database;
}

namespace {

// The whole of the first `keyword ( ... )` from `from`: the keyword's first
// character to past its closing bracket (quotes respected).
[[nodiscard]] std::optional<TextRange> block_extent(const std::string_view t, const std::string_view keyword, const std::size_t from) {
    for (std::size_t i = from; i + keyword.size() < t.size(); ++i) {
        if (i > 0 && !detail::rts_space(t[i - 1])) continue;
        if (t.compare(i, keyword.size(), keyword) != 0) continue;
        auto j = i + keyword.size();
        while (j < t.size() && detail::rts_space(t[j])) ++j;
        if (j >= t.size() || t[j] != '(') continue;
        int depth = 0;
        bool quoted = false;
        for (++j; j < t.size(); ++j) {
            const char c = t[j];
            if (c == '"') quoted = !quoted;
            if (quoted) continue;
            if (c == '(') {
                ++depth;
            } else if (c == ')') {
                if (depth == 0) return TextRange{i, j + 1};
                --depth;
            }
        }
        return TextRange{i, t.size()};
    }
    return std::nullopt;
}

[[nodiscard]] std::string lowered(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// UTF-8 to UTF-16LE with a BOM.
[[nodiscard]] std::string utf16_file(const std::string_view text) {
    std::string out("\xFF\xFE", 2);
    out.reserve(text.size() * 2 + 2);
    const auto put = [&](const std::uint32_t unit) {
        out.push_back(static_cast<char>(unit & 0xFF));
        out.push_back(static_cast<char>((unit >> 8) & 0xFF));
    };
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::uint32_t code = c;
        std::size_t length = 1;
        if (c >= 0xF0) {
            code = c & 0x07u;
            length = 4;
        } else if (c >= 0xE0) {
            code = c & 0x0Fu;
            length = 3;
        } else if (c >= 0xC0) {
            code = c & 0x1Fu;
            length = 2;
        }
        if (i + length > text.size()) length = 1;
        for (std::size_t k = 1; k < length; ++k) code = (code << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3Fu);
        i += length;
        if (code >= 0x10000) {
            code -= 0x10000;
            put(0xD800 + (code >> 10));
            put(0xDC00 + (code & 0x3FF));
        } else {
            put(code);
        }
    }
    return out;
}

} // namespace

std::string write_route_database(const std::string_view original, const RouteDatabase& database, std::string& error) {
    std::string text;
    if (!original.empty()) {
        auto decoded = detail::decode_text(original, error);
        if (!decoded) return {};
        text = std::move(*decoded);
    } else {
        text = "SIMISA@@@@@@@@@@JINX1R0t______\r\n\r\n";
    }
    // The old type lines out, each with its line.
    for (;;) {
        const auto block = block_extent(lowered(text), "signaltypeshownasnormal", 0);
        if (!block) break;
        auto begin = block->begin;
        while (begin > 0 && (text[begin - 1] == ' ' || text[begin - 1] == '\t')) --begin;
        auto end = block->end;
        while (end < text.size() && (text[end] == ' ' || text[end] == '\t')) ++end;
        if (end < text.size() && text[end] == '\r') ++end;
        if (end < text.size() && text[end] == '\n') ++end;
        text.erase(begin, end - begin);
    }
    // The units where the old ones were, else at the end.
    std::string units;
    char line[256];
    std::snprintf(line, sizeof line, "SignalUnits ( %zu \r\n", database.signal_units.size());
    units += line;
    for (const auto& unit : database.signal_units) {
        units += "\tSignalUnit ( \r\n";
        std::snprintf(line, sizeof line, "\t\tPosition ( %d %d %.4f %.4f %.4f )\r\n", unit.position.tile_x, unit.position.tile_z,
                      unit.position.x, unit.position.y, unit.position.z);
        units += line;
        std::snprintf(line, sizeof line, "\t\tNumSigItems ( %u )\r\n", unit.items);
        units += line;
        if (!unit.name.empty()) units += "\t\tName ( \"" + unit.name + "\")\r\n";
        std::snprintf(line, sizeof line, "\t\tUnitFlags ( %08x )\r\n", unit.flags);
        units += line;
        units += "\t\tDefaultAspect ( " + (unit.default_aspect.empty() ? std::string("STOP") : unit.default_aspect) + " )\r\n";
        units += "\t)\r\n";
    }
    units += ")";
    if (const auto old = block_extent(lowered(text), "signalunits", 0)) {
        text.replace(old->begin, old->end - old->begin, units);
    } else {
        if (!text.empty() && text.back() != '\n') text += "\r\n";
        text += units;
    }
    // The types after the header line (and the blank lines after it).
    std::string types;
    for (const auto& name : database.shown_as_normal) types += "SignalTypeShownAsNormal ( \"" + name + "\" )\r\n";
    if (!types.empty()) {
        std::size_t at = text.find('\n');
        at = at == std::string::npos ? text.size() : at + 1;
        while (at < text.size() && (text[at] == '\r' || text[at] == '\n')) ++at;
        text.insert(at, types);
    }
    return utf16_file(text);
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

std::optional<core::VirtualPath> route_database_file(const core::Vfs& vfs, const core::VirtualPath& route) {
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
    return file;
}

RouteDatabase load_route_database(const core::Vfs& vfs, const core::VirtualPath& route) {
    const auto file = route_database_file(vfs, route);
    if (!file) return {};
    std::string error;
    const auto bytes = vfs.read(*file, &error);
    if (!bytes) return {.error = error};
    return parse_route_database(*bytes);
}

} // namespace openrts::formats
