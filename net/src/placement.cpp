#include <openrts/net/placement.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <sstream>

#include <openrts/net/wire.hpp>

namespace openrts::net {

namespace {

std::string escaped(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c != '\r') out += c;
    }
    return out;
}

std::string unescaped(const std::string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            if (text[i + 1] == 'n') out += "\r\n";
            else out += text[i + 1];
            ++i;
        } else {
            out += text[i];
        }
    }
    return out;
}

char lower_ascii(const char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

std::string trimmed(std::string text) {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.pop_back();
    std::size_t start = 0;
    while (start < text.size() && (text[start] == ' ' || text[start] == '\t')) ++start;
    return text.substr(start);
}

// The single player's placements before 2026-11-21 (TrainView's own key =
// value file): its trains standing (STATIC, as they were put down), the
// number the server's lowest free.
Placement parse_old(std::istringstream& in) {
    Placement out;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "[train]") {
            out.trains.push_back(PlacementTrain{.kind = 0, .state = 2});
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos || line.starts_with('#')) continue;
        const auto key = line.substr(0, equals);
        const auto value = line.substr(equals + 1);
        if (key == "route") {
            out.route = value;
            continue;
        }
        if (out.trains.empty()) continue;
        auto& train = out.trains.back();
        if (key == "name") {
            train.name = from_utf8(value);
        } else if (key == "front") {
            std::istringstream fields(value);
            int forward = 1;
            if (fields >> train.node >> train.offset >> forward) train.forward = forward != 0;
        } else if (key == "car") {
            std::vector<std::string> fields;
            std::size_t start = 0;
            for (;;) {
                const auto tab = value.find('\t', start);
                fields.push_back(value.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
                if (tab == std::string::npos) break;
                start = tab + 1;
            }
            if (fields.size() < 5) continue;
            PlacementCar car;
            car.flags = fields[3] == "1" ? 1u : 0u;
            car.file = from_utf8(fields[0] + "\\" + fields[1] + (fields[2] == "1" ? ".eng" : ".wag"));
            if (fields.size() > 5) car.shoes = fields[5];
            train.cars.push_back(std::move(car));
        }
    }
    return out;
}

} // namespace

std::string write_placement(const Placement& placement) {
    std::string out(placement_header);
    out += '\n';
    if (!placement.route.empty()) out += "route " + placement.route + "\n";
    for (const auto& name : placement.includes) out += "include " + name + "\n";
    for (const auto& t : placement.trains) {
        char line[200];
        std::snprintf(line, sizeof line, "train %u %u %u %u %.3f %u %.3f %d\n", t.number, t.kind, t.subtype, t.state, t.top_speed, t.node,
                      t.offset, t.forward ? 1 : 0);
        out += line;
        if (!t.name.empty()) out += "name " + escaped(to_utf8(t.name)) + "\n";
        if (!t.notes.empty()) out += "notes " + escaped(to_utf8(t.notes)) + "\n";
        if (!t.from.empty()) out += "from " + t.from + "\n";
        for (const auto& car : t.cars) {
            char head[64];
            if (car.handbrake) std::snprintf(head, sizeof head, "car %u %.3f ", car.flags, *car.handbrake);
            else std::snprintf(head, sizeof head, "car %u - ", car.flags);
            out += head + to_utf8(car.file) + "\n";
            if (!car.shoes.empty()) out += "shoes " + car.shoes + "\n";
        }
        out += "end\n";
    }
    return out;
}

std::string write_placement(const std::vector<PlacementTrain>& trains) { return write_placement(Placement{.trains = trains}); }

Placement parse_placement(const std::string_view text, std::string& error) {
    Placement out;
    std::istringstream in{std::string(text)};
    std::string line;
    std::getline(in, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF && static_cast<unsigned char>(line[1]) == 0xBB
        && static_cast<unsigned char>(line[2]) == 0xBF) {
        line.erase(0, 3); // a BOM from a text editor
    }
    if (line.rfind("# openRTS placement", 0) == 0) return parse_old(in);
    if (line != placement_header && line != "openRTS server placement 1") {
        error = "not a placement";
        return out;
    }
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.rfind("route ", 0) == 0) {
            out.route = trimmed(line.substr(6));
        } else if (line.rfind("include ", 0) == 0) {
            auto name = trimmed(line.substr(8));
            if (placement_name_ok(name) && std::find(out.includes.begin(), out.includes.end(), name) == out.includes.end()) {
                out.includes.push_back(std::move(name));
            }
        } else if (line.rfind("train ", 0) == 0) {
            std::istringstream words(line.substr(6));
            PlacementTrain t;
            unsigned kind = 0, subtype = 0, state = 0;
            int forward = 1;
            if (!(words >> t.number >> kind >> subtype >> state >> t.top_speed >> t.node >> t.offset >> forward)) continue;
            t.kind = static_cast<std::uint16_t>(kind);
            t.subtype = static_cast<std::uint16_t>(subtype);
            t.state = static_cast<std::uint8_t>(state);
            t.forward = forward != 0;
            out.trains.push_back(std::move(t));
        } else if (out.trains.empty()) {
            continue;
        } else if (line.rfind("name ", 0) == 0) {
            out.trains.back().name = from_utf8(unescaped(std::string_view(line).substr(5)));
        } else if (line.rfind("notes ", 0) == 0) {
            out.trains.back().notes = from_utf8(unescaped(std::string_view(line).substr(6)));
        } else if (line.rfind("from ", 0) == 0) {
            out.trains.back().from = trimmed(line.substr(5));
        } else if (line.rfind("car ", 0) == 0) {
            std::istringstream words(line.substr(4));
            PlacementCar car;
            std::string handbrake;
            if (!(words >> car.flags >> handbrake)) continue;
            if (handbrake != "-") car.handbrake = std::strtof(handbrake.c_str(), nullptr);
            std::string file;
            std::getline(words >> std::ws, file);
            if (file.empty()) continue;
            car.file = from_utf8(file);
            out.trains.back().cars.push_back(std::move(car));
        } else if (line.rfind("shoes ", 0) == 0 && !out.trains.back().cars.empty()) {
            out.trains.back().cars.back().shoes = trimmed(line.substr(6));
        }
    }
    return out;
}

std::vector<PlacementTrain> read_placement(const std::string_view text, std::string& error) { return parse_placement(text, error).trains; }

std::vector<PlacementTrain> resolve_placement(const Placement& placement, const std::string_view self, const PlacementOpener& open,
                                              std::vector<std::string>& problems) {
    std::vector<PlacementTrain> out = placement.trains;
    std::set<std::string> seen;
    if (!self.empty()) seen.insert(std::string(self));
    for (const auto& t : out)
        if (!t.from.empty()) seen.insert(t.from);
    // Depth first, each placement once: one already in it again is a loop
    // or a second way to the same one, either way not put twice.
    std::function<void(const std::vector<std::string>&, int)> include = [&](const std::vector<std::string>& names, const int depth) {
        for (const auto& name : names) {
            if (seen.contains(name)) continue;
            seen.insert(name);
            if (depth > 16) {
                problems.push_back(name + ": included too deep");
                continue;
            }
            const auto text = open ? open(name) : std::nullopt;
            if (!text) {
                problems.push_back(name + ": no such placement");
                continue;
            }
            std::string error;
            auto inner = parse_placement(*text, error);
            if (!error.empty()) {
                problems.push_back(name + ": " + error);
                continue;
            }
            for (auto& t : inner.trains) {
                if (t.from.empty()) t.from = name;
                out.push_back(std::move(t));
            }
            include(inner.includes, depth + 1);
        }
    };
    include(placement.includes, 1);
    return out;
}

bool placement_route_fits(const std::string_view placement_route, const std::string_view route) {
    if (placement_route.empty() || route.empty()) return true;
    if (placement_route.size() != route.size()) return false;
    for (std::size_t i = 0; i < route.size(); ++i)
        if (lower_ascii(placement_route[i]) != lower_ascii(route[i])) return false;
    return true;
}

ConsolePlace place_of(const PlacementTrain& train) {
    ConsolePlace place;
    place.consist = train.name.empty() ? u"№" + from_utf8(std::to_string(train.number)) : train.name;
    place.max_velocity = train.top_speed;
    place.number = train.number;
    place.node = train.node;
    place.offset = train.offset;
    place.forward = train.forward;
    place.name = train.name;
    place.notes = train.notes;
    place.kind = train.kind;
    place.state = train.state;
    for (const auto& car : train.cars) {
        const auto slash = car.file.find_last_of(u"\\/");
        const auto dot = car.file.find_last_of(u'.');
        if (slash == std::u16string::npos || dot == std::u16string::npos || dot < slash) continue;
        auto ext = car.file.substr(dot);
        for (auto& c : ext)
            if (c >= u'A' && c <= u'Z') c = static_cast<char16_t>(c - u'A' + u'a');
        place.cars.push_back({.folder = car.file.substr(0, slash),
                              .name = car.file.substr(slash + 1, dot - slash - 1),
                              .engine = ext == u".eng",
                              .flipped = (car.flags & 1u) != 0,
                              .cold = (car.flags & 2u) != 0,
                              .handbrake = car.handbrake ? *car.handbrake * 100.0f : -1.0f,
                              .chocks = car.chocks()});
    }
    return place;
}

bool placement_name_ok(const std::string_view name) {
    if (name.empty() || name.size() > 80 || name.front() == '.' || name.front() == ' ' || name.back() == ' ') return false;
    for (const char c : name)
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c == '\n'
            || c == '\r' || c == '\t')
            return false;
    return true;
}

std::string placement_to_line(const std::string_view text) {
    std::string out;
    out.reserve(text.size() + text.size() / 8);
    for (const char c : text) {
        if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == ' ') out += "\\s";
        else if (c == '"') out += "\\q";
        else if (c == '\t') out += "\\t";
        else if (c != '\r') out += c;
    }
    return out;
}

std::string placement_from_line(const std::string_view line) {
    std::string out;
    out.reserve(line.size());
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] != '\\' || i + 1 >= line.size()) {
            out += line[i];
            continue;
        }
        const char c = line[++i];
        out += c == 'n' ? '\n' : c == 's' ? ' ' : c == 'q' ? '"' : c == 't' ? '\t' : c;
    }
    return out;
}

std::vector<std::string> placement_upload_commands(const std::string_view name, const std::string_view text) {
    // A console command goes as UTF-16 (two bytes a unit) in a package of
    // at most 0x3C00 bytes: 4000 bytes of UTF-8 a part, never cut inside a
    // character (each part is a line of its own).
    const auto line = placement_to_line(text);
    std::vector<std::string_view> parts;
    constexpr std::size_t most = 4000;
    for (std::size_t at = 0; at < line.size();) {
        std::size_t end = std::min(line.size(), at + most);
        while (end < line.size() && end > at && (static_cast<unsigned char>(line[end]) & 0xC0u) == 0x80u) --end;
        parts.push_back(std::string_view(line).substr(at, end - at));
        at = end;
    }
    if (parts.empty()) parts.push_back({});
    std::vector<std::string> out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        out.push_back("placement upload \"" + std::string(name) + "\" " + std::to_string(i + 1) + " " + std::to_string(parts.size()) + " "
                      + std::string(parts[i]));
    }
    return out;
}

} // namespace openrts::net
