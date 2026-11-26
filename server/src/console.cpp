#include <openrts/server/console.hpp>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include <openrts/core/virtual_path.hpp>
#include <openrts/formats/signal_config.hpp>
#include <openrts/net/placement.hpp>
#include <openrts/net/wire.hpp>

namespace openrts::server {

namespace {

[[nodiscard]] std::optional<std::uint32_t> number_of(const std::string_view text) {
    std::uint32_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

[[nodiscard]] std::string lower(const std::string_view text) { return core::fold_case(text); }

const char* kind_name(const std::uint16_t kind) {
    switch (kind) {
    case 0: return "STATIC";
    case 1: return "PLAYER";
    case 2: return "TRAFFIC";
    case 3: return "LOOSE";
    default: return "?";
    }
}

constexpr const char* help_text =
    "commands:\n"
    "  status                         the game: route, time, players, trains\n"
    "  users                          the players\n"
    "  trains                         the trains\n"
    "  paths [text]                   the route's paths (.pat)\n"
    "  consists [text]                the consists\n"
    "  add \"<consist>\" <path> [number] a train standing at the path's start (STATIC)\n"
    "  place \"<consist>\" <node> <offset> <forward|back> [number]  a train (STATIC) with its front at a place of the track\n"
    "  assign <train> <nick>          give a player a train (they drive it)\n"
    "  unassign <train>               take it back (it stands, STATIC)\n"
    "  remove <train>                 delete a train\n"
    "  weather [clear|snow|rain|storm|dynamic [seed]]  the weather while the game runs (openRTS players)\n"
    "  placement save <name>          every train on the track saved (its cars, place, number, name, kind, state, chocks)\n"
    "  placement load <name> [add]    the saved trains put back, with those of the placements it includes (the ones on the track removed first, unless add)\n"
    "  placement list | delete <name> the placements saved; get [name] one's file (none: the trains now); clear: every train removed\n"
    "  placement include|exclude <name> <other>  another placement put with it, or no more\n"
    "  move <train> <node> <offset> <0|1>  a standing train put again there (its front, facing along the node with 1)\n"
    "  chocks <train> none|head|tail|both  chocks under a standing train's ends (it is put again)\n"
    "  number <train> <number>        its train number (RTS numbers its cars by it; not another train's)\n"
    "  name <train> [text]            its name (the map shows it); notes <train> [text] its notes; none clears\n"
    "  couple <train> <train>         the second joins the first where they stand end to end\n"
    "  cut <train> <car>              cut before car <car> (from 0): its own train, the same owner\n"
    "  say <nick|*> <text>            a message to a player or everyone\n"
    "  pause | resume                 stop or start the game\n"
    "  time [HH:MM[:SS]]              show or set the time of day\n"
    "  speed [1|60|600|3600]          show or set how fast the clock runs (openRTS players follow)\n"
    "  season [spring|summer|autumn|winter]  show or set the season (openRTS players at once)\n"
    "  route <train> <signal> [signal...]  set and hold the junctions from the train to a signal\n"
    "                                 (its name, \"литера\", from the route database), through the others in turn\n"
    "  route <train> cancel           free the route's junctions\n"
    "  signals [text]                 the signals' names and how many of each\n"
    "  stations [text]                the stations of the route database's station routes, with how many\n"
    "  routes <station> [text]        a station's routes: ID, name, from, to, flags, whether its way is laid\n"
    "  heads                          every signal head as the players get it: item, aspect, draw state, mode\n"
    "  next <train> [back]            the station routes the train can take next (\"Собрать маршрут\"): from its last\n"
    "                                 segment's end, else its head (back: the other way); then segment <train> <ID> ...\n"
    "  segment <train> <route ID> [back] [hold] [rear] [priority N]  a segment of the train's route: the\n"
    "                                 station route (back: from its end; hold: the train's next segment waits;\n"
    "                                 rear: the train runs it rear first), set when its turn comes\n"
    "  segment cancel <id>            a segment cancelled (the train's route back to its previous one's end)\n"
    "  segments                       the queue: id, train, station route, state, junctions set\n"
    "  junction <node> [branch] [lock|unlock]  throw or lock a junction (TDB node)\n"
    "  signal <item|name> [what]      a signal (TDB item, or its name if only one has it): its mode and aspect,\n"
    "                                 or: <aspect> hold it there (manual, a route leaves it so), open (until a\n"
    "                                 train passes it), auto (automatic block), close (manual), default\n"
    "                                 aspects: stop, stop_and_proceed, restricting, approach_1..3, clear_1, clear_2\n"
    "  signal type <type> <manual|open|auto>  every signal with a head of that sigcfg type\n"
    "  signal type <type> normal on|off  the type shown as normal (its heads count for their signal's aspect)\n"
    "  signal defaults                every signal back to its defaults from the route database\n"
    "  signal set-defaults            every signal's defaults become its mode and aspect now\n"
    "  signal save                    the defaults and the types shown as normal into the server's copy of\n"
    "                                 the route database (its data folder; the installation is not touched)\n"
    "  helper <nick> <train|off>      make a player the helper (помощник машиниста) on a train's cab, or not\n"
    "  kick <nick>                    drop a player\n"
    "  admit on|off                   take new players or not\n"
    "  fastpos on|off                 openRTS players send their trains' motion 10 times a second, or as RTS every 1.5 s\n"
    "  quit                          stop the server\n";

std::string time_text(const double seconds) {
    const auto s = static_cast<int>(seconds);
    char text[16];
    std::snprintf(text, sizeof text, "%02d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    return text;
}

} // namespace

namespace {

// The cars made a train, its subtype from them: passenger cars make it
// "Пассажирский", else freight cars "Грузовой", else "Прочий".
NewTrain made_train(ConsistTrain made, const std::uint32_t number) {
    bool passenger = false, freight = false;
    for (const auto& car : made.cars) {
        passenger = passenger || car.definition->kind == formats::WagonKind::passenger;
        freight = freight || car.definition->kind == formats::WagonKind::freight;
    }
    NewTrain train;
    train.cars = std::move(made.cars);
    train.records = std::move(made.records);
    train.top_speed = made.top_speed;
    train.number = number;
    train.subtype = passenger ? 1 : freight ? 2 : 0;
    return train;
}

} // namespace

std::optional<NewTrain> train_at(ServerRoute& route, const std::string_view consist, const simulation::TrackLocation& front,
                                 const std::uint32_t number, std::string& error) {
    auto made = route.consist(consist, number);
    if (!made.ok()) {
        error = made.error.empty() ? "consist " + std::string(consist) + " has no cars" : made.error;
        return std::nullopt;
    }
    const auto* node = route.network().node(front.node);
    if (!node || node->kind != formats::TrackNodeKind::vector || front.offset < 0.0 || front.offset > node->length) {
        error = "no track at node " + std::to_string(front.node) + ", " + std::to_string(front.offset) + " m";
        return std::nullopt;
    }
    auto train = made_train(std::move(made), number);
    train.node = front.node;
    train.offset = front.offset;
    train.forward = front.forward;
    return train;
}

std::optional<NewTrain> train_on_path(ServerRoute& route, const std::string_view consist, const std::string_view path_name,
                                      const std::uint32_t number, std::string& error, const std::uint32_t slot) {
    const auto* path = route.path(path_name);
    if (!path) {
        error = "no path " + std::string(path_name) + " (see: paths)";
        return std::nullopt;
    }
    auto made = route.consist(consist, number);
    if (!made.ok()) {
        error = made.error.empty() ? "consist " + std::string(consist) + " has no cars" : made.error;
        return std::nullopt;
    }
    double length = 0.0;
    for (const auto& car : made.cars) length += car.definition->length;
    const auto front = route.path_front(*path, length, slot * (length + 30.0));
    if (!front) {
        error = "no track at the start of path " + path->file;
        return std::nullopt;
    }
    auto train = made_train(std::move(made), number);
    train.node = front->node();
    train.offset = front->offset();
    train.forward = front->forward();
    return train;
}

std::string place_cars(GameServer& server, ServerRoute& route, const net::ConsolePlace& place, std::uint32_t* placed) {
    std::vector<formats::ConsistEntry> entries;
    for (const auto& car : place.cars) {
        formats::ConsistEntry entry;
        entry.folder = net::to_utf8(car.folder);
        entry.name = net::to_utf8(car.name);
        entry.engine = car.engine;
        entry.flipped = car.flipped;
        entry.cold = car.cold;
        entry.place_drag = car.chocks;
        if (car.handbrake >= 0.0f) entry.handbrake = car.handbrake;
        entries.push_back(std::move(entry));
    }
    const auto number = place.number ? place.number : server.free_number();
    auto made = route.cars(entries, place.max_velocity, number);
    const auto name = net::to_utf8(place.consist);
    if (!made.ok()) return "error: " + name + ": " + made.error;
    std::string error;
    std::optional<NewTrain> train;
    if (!place.path.empty()) {
        const auto* path = route.path(net::to_utf8(place.path));
        if (!path) return "error: no path " + net::to_utf8(place.path);
        double length = 0.0;
        for (const auto& car : made.cars) length += car.definition->length;
        const auto front = route.path_front(*path, length);
        if (!front) return "error: no track at the start of path " + path->file;
        train = made_train(std::move(made), number);
        train->node = front->node();
        train->offset = front->offset();
        train->forward = front->forward();
    } else {
        const auto* node = route.network().node(place.node);
        if (!node || node->kind != formats::TrackNodeKind::vector || place.offset < 0.0 || place.offset > node->length) {
            return "error: no track at node " + std::to_string(place.node);
        }
        train = made_train(std::move(made), number);
        train->node = place.node;
        train->offset = place.offset;
        train->forward = place.forward;
    }
    train->name = place.name;
    train->kind = place.kind <= 3 ? place.kind : 0;
    train->state = place.state <= 2 ? place.state : 0;
    const auto id = server.add_train(std::move(*train), error);
    if (!id) return "error: " + name + ": " + error;
    if (placed) *placed = *id;
    if (!place.notes.empty()) server.set_names(*id, place.name, place.notes, error);
    return "train " + std::to_string(*id) + " (№" + std::to_string(number) + ", " + name + ", " + std::to_string(place.cars.size()) +
           " cars) stands there; give it to a player: assign " + std::to_string(*id) + " <nick>";
}

namespace {

std::filesystem::path placement_file(ServerRoute& route, const std::string& name) {
    return route.placements_folder() / std::filesystem::path(std::u8string(name.begin(), name.end()) + u8".placement");
}

} // namespace

namespace {

net::PlacementTrain placement_of(const StandingTrain& t) {
    net::PlacementTrain p{.number = t.number, .name = t.name, .notes = t.notes, .kind = t.kind, .subtype = t.subtype, .state = t.state,
                          .top_speed = t.top_speed, .node = t.node, .offset = t.offset, .forward = t.forward};
    // The records' flags as they go (flipped, cold, the chocks 0x100 / 0x200).
    for (const auto& car : t.cars) p.cars.push_back({.flags = car.flags & 0x303u, .handbrake = car.handbrake, .file = car.file});
    p.owner = net::to_utf8(t.owner);
    p.own_state = t.own_state;
    return p;
}

// A placement's train on the track again: its owner waited for, its state kept.
std::string place_back(GameServer& server, ServerRoute& route, const net::PlacementTrain& t, std::uint32_t* placed) {
    std::uint32_t id = 0;
    auto answer = place_cars(server, route, net::place_of(t), &id);
    if (id != 0) {
        if (!t.own_state.empty()) server.set_train_state(id, t.own_state);
        if (!t.owner.empty()) server.give_when_back(id, net::from_utf8(t.owner));
    }
    if (placed) *placed = id;
    return answer;
}

} // namespace

std::vector<net::PlacementTrain> standing_placement(const GameServer& server) {
    std::vector<net::PlacementTrain> out;
    for (const auto& t : server.standing_trains()) out.push_back(placement_of(t));
    return out;
}

std::uint8_t train_chocks(const GameServer& server, const std::uint32_t train) {
    for (const auto& t : server.standing_trains()) {
        if (t.id != train || t.cars.empty()) continue;
        return static_cast<std::uint8_t>(((t.cars.front().flags & 0x100u) ? 1u : 0u) | ((t.cars.back().flags & 0x200u) ? 2u : 0u));
    }
    return 0;
}

std::string replace_train(GameServer& server, ServerRoute& route, const std::uint32_t train, const std::optional<simulation::TrackLocation>& at,
                          const std::optional<std::uint8_t> chocks, std::uint32_t* placed) {
    for (const auto& t : server.trains()) {
        if (t.id == train && !t.owner.empty()) return "error: train " + std::to_string(train) + " is " + t.owner + "'s: take it back first";
    }
    const auto standing = server.standing_trains();
    const auto it = std::find_if(standing.begin(), standing.end(), [&](const StandingTrain& t) { return t.id == train; });
    if (it == standing.end()) return "error: no train " + std::to_string(train) + " on the track";
    const auto before = placement_of(*it);
    auto after = before;
    if (at) {
        after.node = at->node;
        after.offset = at->offset;
        after.forward = at->forward;
    }
    if (chocks && !after.cars.empty()) {
        for (auto& car : after.cars) car.flags &= ~0x300u;
        if (*chocks & 1u) after.cars.front().flags |= 0x100u;
        if (*chocks & 2u) after.cars.back().flags |= 0x200u;
    }
    std::string error;
    if (!server.remove_train(train, error)) return "error: " + error;
    std::uint32_t id = 0;
    const auto answer = place_back(server, route, after, &id);
    if (answer.rfind("error", 0) == 0) {
        std::uint32_t back = 0;
        (void)place_back(server, route, before, &back);
        if (placed) *placed = back;
        return answer + " (left where it was" + (back ? ": now train " + std::to_string(back) : std::string()) + ")";
    }
    if (placed) *placed = id;
    return "train " + std::to_string(train) + " put again: now train " + std::to_string(id);
}

std::optional<net::Placement> placement_file_read(ServerRoute& route, const std::string& name, std::string& error) {
    const auto text = placement_text(route, name);
    if (!text) {
        error = "no placement " + name;
        return std::nullopt;
    }
    auto placement = net::parse_placement(*text, error);
    if (!error.empty()) {
        error = name + ": " + error;
        return std::nullopt;
    }
    return placement;
}

std::string placement_file_write(ServerRoute& route, const std::string& name, const net::Placement& placement) {
    if (!net::placement_name_ok(name)) return "error: a placement's name: letters, digits, spaces";
    std::error_code error;
    std::filesystem::create_directories(route.placements_folder(), error);
    const auto path = placement_file(route, name);
    auto temporary = path;
    temporary += u8".new";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << net::write_placement(placement);
        if (!out) return "error: could not write " + path.string();
    }
    std::filesystem::rename(temporary, path, error);
    if (error) return "error: could not write " + path.string() + ": " + error.message();
    return {};
}

std::vector<net::PlacementTrain> placement_resolved(ServerRoute& route, const net::Placement& placement, const std::string& name,
                                                    std::vector<std::string>& problems) {
    return net::resolve_placement(placement, name, [&](const std::string& other) { return placement_text(route, other); }, problems);
}

std::string placement_include(ServerRoute& route, const std::string& name, const std::string& other, const bool add) {
    std::string error;
    auto placement = placement_file_read(route, name, error);
    if (!placement) return "error: " + error;
    auto& includes = placement->includes;
    const auto it = std::find(includes.begin(), includes.end(), other);
    if (add) {
        if (other == name) return "error: a placement cannot include itself";
        if (!placement_text(route, other)) return "error: no placement " + other;
        if (it != includes.end()) return "placement " + name + " includes " + other + " already";
        includes.push_back(other);
    } else {
        if (it == includes.end()) return "error: " + name + " does not include " + other;
        includes.erase(it);
    }
    if (auto written = placement_file_write(route, name, *placement); !written.empty()) return written;
    return "placement " + name + (add ? " includes " : " no more includes ") + other;
}

std::string save_placement(GameServer& server, ServerRoute& route, const std::string& name) {
    if (!net::placement_name_ok(name)) return "error: a placement's name: letters, digits, spaces";
    const auto trains = standing_placement(server);
    if (auto written = placement_file_write(route, name, net::Placement{.route = route.folder(), .trains = trains}); !written.empty()) return written;
    std::size_t cars = 0;
    for (const auto& t : trains) cars += t.cars.size();
    return "placement " + name + ": " + std::to_string(trains.size()) + " trains, " + std::to_string(cars) + " cars saved";
}

std::string autosave_placement(GameServer& server, ServerRoute& route) {
    if (server.standing_trains().empty()) return {};
    return save_placement(server, route, autosave_name);
}

void keep_previous_autosave(ServerRoute& route) {
    std::error_code error;
    const auto last = placement_file(route, autosave_name);
    if (!std::filesystem::exists(last, error)) return;
    std::filesystem::rename(last, placement_file(route, autosave_previous_name), error);
}

std::optional<std::string> placement_text(ServerRoute& route, const std::string& name) {
    if (!net::placement_name_ok(name)) return std::nullopt;
    std::ifstream in(placement_file(route, name), std::ios::binary);
    if (!in) return std::nullopt;
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::size_t clear_trains(GameServer& server) {
    std::string error;
    std::size_t removed = 0;
    for (const auto& t : server.trains()) removed += server.remove_train(t.id, error) ? 1 : 0;
    return removed;
}

std::string load_placement(GameServer& server, ServerRoute& route, const std::string& name, const bool add) {
    if (!net::placement_name_ok(name)) return "error: a placement's name: letters, digits, spaces";
    const auto text = placement_text(route, name);
    if (!text) return "error: no placement " + name;
    std::string error;
    const auto placement = net::parse_placement(*text, error);
    if (!error.empty()) return "error: " + name + ": " + error;
    if (!net::placement_route_fits(placement.route, route.folder())) return "error: " + name + " is a placement of route " + placement.route;
    std::vector<std::string> missing;
    const auto saved = placement_resolved(route, placement, name, missing);
    const auto removed = add ? 0 : clear_trains(server);
    std::size_t put = 0;
    std::string problems;
    for (const auto& m : missing) problems += "\n  " + m;
    for (const auto& t : saved) {
        const auto answer = place_back(server, route, t, nullptr);
        if (answer.rfind("error", 0) == 0) problems += "\n  №" + std::to_string(t.number) + (t.from.empty() ? "" : " (" + t.from + ")") + ": " + answer;
        else ++put;
    }
    std::string result = "placement " + name + ": " + std::to_string(put) + " of " + std::to_string(saved.size()) + " trains put";
    if (!add) result += " (" + std::to_string(removed) + " removed first)";
    return result + problems;
}

std::vector<std::string> placement_names(ServerRoute& route) {
    std::vector<std::string> names;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(route.placements_folder(), error)) {
        if (!entry.is_regular_file(error) || entry.path().extension() != ".placement") continue;
        const auto stem = entry.path().stem().u8string();
        names.emplace_back(stem.begin(), stem.end());
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> command_words(const std::string_view line) {
    std::vector<std::string> words;
    std::string word;
    bool quoted = false, any = false;
    for (const char c : line) {
        if (c == '"') {
            quoted = !quoted;
            any = true;
        } else if (!quoted && (c == ' ' || c == '\t' || c == '\r' || c == '\n')) {
            if (any) words.push_back(std::move(word));
            word.clear();
            any = false;
        } else {
            word.push_back(c);
            any = true;
        }
    }
    if (any) words.push_back(std::move(word));
    return words;
}

std::optional<double> parse_time_of_day(const std::string_view text) {
    std::uint32_t parts[3]{};
    std::size_t count = 0;
    std::size_t from = 0;
    while (count < 3) {
        const auto colon = text.find(':', from);
        const auto piece = text.substr(from, colon == std::string_view::npos ? std::string_view::npos : colon - from);
        const auto value = number_of(piece);
        if (!value) return std::nullopt;
        parts[count++] = *value;
        if (colon == std::string_view::npos) break;
        from = colon + 1;
    }
    if (count < 2 || parts[0] > 23 || parts[1] > 59 || parts[2] > 59) return std::nullopt;
    return parts[0] * 3600.0 + parts[1] * 60.0 + parts[2];
}

std::optional<std::uint8_t> parse_season(const std::string_view text) {
    const auto t = lower(text);
    if (t == "0" || t == "spring") return 0;
    if (t == "1" || t == "summer") return 1;
    if (t == "2" || t == "autumn") return 2;
    if (t == "3" || t == "winter") return 3;
    return std::nullopt;
}

std::optional<std::uint8_t> parse_weather(const std::string_view text) {
    const auto t = lower(text);
    if (t == "0" || t == "clear") return 0;
    if (t == "1" || t == "snow") return 1;
    if (t == "2" || t == "rain") return 2;
    if (t == "3" || t == "storm") return 3;
    return std::nullopt;
}

std::u16string game_description(const std::string_view route_id, const std::uint8_t season) {
    static const char16_t* const seasons[] = {u"Весна", u"Лето", u"Осень", u"Зима"};
    return u"Маршрут: " + net::from_utf8(route_id) + u"\r\nСезон: " + (season < 4 ? seasons[season] : u"Лето");
}

std::string run_command(GameServer& server, ServerRoute& route, const std::string_view line, bool& quit) {
    const auto words = command_words(line);
    if (words.empty()) return {};
    const auto command = lower(words[0]);
    const auto arg = [&](const std::size_t i) -> std::string_view { return i < words.size() ? std::string_view(words[i]) : std::string_view{}; };
    std::string error;
    const auto done = [&](const bool ok, const std::string& what) { return ok ? what : "error: " + error; };
    std::ostringstream out;

    if (command == "help" || command == "?") return help_text;
    if (command == "quit" || command == "exit") {
        quit = true;
        return "stopping";
    }
    if (command == "status") {
        std::size_t connected = 0, playing = 0;
        for (const auto& u : server.users()) {
            connected += u.connected ? 1 : 0;
            playing += u.playing ? 1 : 0;
        }
        out << "route " << route.folder() << " (" << route.route_id() << "), time " << time_text(server.time_of_day())
            << (server.paused() ? ", paused" : "") << (server.admits() ? "" : ", no new players") << "; players: " << connected
            << " connected, " << playing << " playing; trains: " << server.trains().size();
        return out.str();
    }
    if (command == "users") {
        const auto users = server.users();
        if (users.empty()) return "no players yet";
        for (const auto& u : users) {
            out << u.id << " " << u.nick << (u.console ? " (dispatcher)" : "") << (u.connected ? (u.playing ? " playing" : " in the menu") : " away")
                << " from " << u.address;
            if (!u.trains.empty()) {
                out << "; trains";
                for (const auto t : u.trains) out << ' ' << t;
            }
            if (u.helping) out << "; helps on train " << u.helping;
            if (u.asking) out << "; asks to help on train " << u.asking;
            if (u.openrts) out << "; openRTS";
            if (!u.where.empty()) out << "; " << u.where;
            out << '\n';
        }
        return out.str();
    }
    if (command == "trains") {
        const auto trains = server.trains();
        if (trains.empty()) return "no trains";
        for (const auto& t : trains) {
            out << t.id << " №" << t.number << ' ' << kind_name(t.kind) << ", " << t.cars << " cars";
            if (!t.owner.empty()) out << ", " << t.owner;
            out << (t.stands ? ", stands" : "") << (t.placed ? "" : ", off the server's track");
            if (t.route_junctions) out << ", route holds " << t.route_junctions << " junctions";
            if (!t.where.empty()) {
                char speed[32];
                std::snprintf(speed, sizeof speed, ", %.1f km/h", t.speed * 3.6);
                out << ", " << t.where << speed;
            }
            out << '\n';
        }
        return out.str();
    }
    if (command == "paths") {
        const auto filter = lower(arg(1));
        for (const auto& p : route.paths()) {
            if (!filter.empty() && lower(p.file).find(filter) == std::string::npos && lower(p.name).find(filter) == std::string::npos) continue;
            out << p.file << (p.name.empty() ? "" : "  (" + p.name + ")") << '\n';
        }
        return out.str().empty() ? "no paths" : out.str();
    }
    if (command == "consists") {
        const auto filter = lower(arg(1));
        for (const auto& name : route.consist_names()) {
            if (filter.empty() || lower(name).find(filter) != std::string::npos) out << name << '\n';
        }
        return out.str().empty() ? "no consists" : out.str();
    }
    if (command == "add") {
        if (words.size() < 3) return "usage: add \"<consist>\" <path> [number]";
        auto number = server.free_number();
        if (words.size() > 3) {
            const auto given = number_of(arg(3));
            if (!given) return "error: the number is a whole number";
            number = *given;
        }
        auto train = train_on_path(route, arg(1), arg(2), number, error);
        if (!train) return "error: " + error;
        const auto id = server.add_train(std::move(*train), error);
        return done(id.has_value(), id ? "train " + std::to_string(*id) + " (№" + std::to_string(number) + ") stands at the start of " + std::string(arg(2)) : "");
    }
    if (command == "place") {
        const auto node = number_of(arg(2));
        double offset = 0.0;
        const std::string offset_text(arg(3));
        char* end = nullptr;
        if (!offset_text.empty()) offset = std::strtod(offset_text.c_str(), &end);
        const auto way = lower(arg(4));
        if (words.size() < 5 || !node || !end || *end != 0 || (way != "forward" && way != "back")) {
            return "usage: place \"<consist>\" <node> <offset> <forward|back> [number]";
        }
        auto number = server.free_number();
        if (words.size() > 5) {
            const auto given = number_of(arg(5));
            if (!given) return "error: the number is a whole number";
            number = *given;
        }
        auto train = train_at(route, arg(1), {.node = *node, .offset = offset, .forward = way == "forward"}, number, error);
        if (!train) return "error: " + error;
        const auto id = server.add_train(std::move(*train), error);
        return done(id.has_value(), id ? "train " + std::to_string(*id) + " (№" + std::to_string(number) + ") placed at node " +
                                             std::to_string(*node) + "; give it to a player: assign " + std::to_string(*id) + " <nick>"
                                       : "");
    }
    if (command == "assign" || command == "unassign" || command == "remove") {
        const auto train = number_of(arg(1));
        if (!train) return "usage: " + command + " <train>" + (command == "assign" ? " <nick>" : "");
        if (command == "assign") {
            if (words.size() < 3) return "usage: assign <train> <nick>";
            return done(server.assign(*train, arg(2), error), "train " + std::to_string(*train) + " is " + std::string(arg(2)) + "'s");
        }
        if (command == "unassign") return done(server.unassign(*train, error), "train " + std::to_string(*train) + " is nobody's");
        return done(server.remove_train(*train, error), "train " + std::to_string(*train) + " removed");
    }
    // Placements: every train on the track saved, and put back.
    if (command == "placement") {
        const auto what = lower(arg(1));
        if (what == "save" && words.size() >= 3) return save_placement(server, route, std::string(arg(2)));
        if (what == "load" && words.size() >= 3) return load_placement(server, route, std::string(arg(2)), lower(arg(3)) == "add");
        // The list as "placements:" and a name a line (a console reads it).
        if (what == "list" || words.size() == 1) {
            out << "placements:\n";
            for (const auto& name : placement_names(route)) out << name << '\n';
            return out.str();
        }
        // A placement's file as it is (a console keeps it, puts its trains
        // it picks); none named: the trains on the track now.
        // With the trains of the placements it includes (`from` theirs),
        // its includes listed: what a console shows (it puts trains one by
        // one; a copy kept there is written without them).
        if (what == "get") {
            if (words.size() < 3) return net::write_placement(net::Placement{.route = route.folder(), .trains = standing_placement(server)});
            const auto name = std::string(arg(2));
            auto placement = placement_file_read(route, name, error);
            if (!placement) return "error: " + error;
            std::vector<std::string> problems;
            placement->trains = placement_resolved(route, *placement, name, problems);
            return net::write_placement(*placement);
        }
        if ((what == "include" || what == "exclude") && words.size() >= 4) {
            return placement_include(route, std::string(arg(2)), std::string(arg(3)), what == "include");
        }
        // A console's placement sent here in parts (placement_upload_commands),
        // written when the last part comes.
        if (what == "upload" && words.size() >= 5) {
            static std::map<std::string, std::vector<std::string>> parts_of;
            const auto name = std::string(arg(2));
            const auto part = number_of(arg(3));
            const auto parts = number_of(arg(4));
            if (!net::placement_name_ok(name)) return "error: a placement's name: letters, digits, spaces";
            if (!part || !parts || *part == 0 || *part > *parts || *parts > 4096) return "error: placement upload: a part " + std::string(arg(3)) + " of " + std::string(arg(4));
            auto& got = parts_of[name];
            if (*part == 1) got.assign(*parts, {});
            if (got.size() != *parts) return "error: placement upload " + name + ": its first part did not come";
            got[*part - 1] = words.size() > 5 ? words[5] : std::string();
            if (*part != *parts) return {};
            std::string joined;
            for (const auto& p : got) joined += p;
            parts_of.erase(name);
            const auto text = net::placement_from_line(joined);
            auto placement = net::parse_placement(text, error);
            if (!error.empty()) return "error: " + name + ": " + error;
            if (!net::placement_route_fits(placement.route, route.folder())) return "error: " + name + " is a placement of route " + placement.route;
            if (placement.route.empty()) placement.route = route.folder();
            if (auto written = placement_file_write(route, name, placement); !written.empty()) return written;
            return "placement " + name + " saved here: " + std::to_string(placement.trains.size()) + " trains, " +
                   std::to_string(placement.includes.size()) + " included";
        }
        if (what == "clear") return std::to_string(clear_trains(server)) + " trains removed";
        if (what == "delete" && words.size() >= 3) {
            const auto name = std::string(arg(2));
            if (!net::placement_name_ok(name)) return "error: a placement's name: letters, digits, spaces";
            std::error_code fs_error;
            return std::filesystem::remove(placement_file(route, name), fs_error) ? "placement " + name + " deleted" : "error: no placement " + name;
        }
        return "usage: placement save|load|delete|get <name> | placement load <name> add | placement include|exclude <name> <other> | placement get | list | clear";
    }
    // A standing train put again: elsewhere, or with chocks (2026-11-21).
    if (command == "move") {
        const auto train = number_of(arg(1));
        const auto node = number_of(arg(2));
        char* end = nullptr;
        const auto offset_text = std::string(arg(3));
        const double offset = std::strtod(offset_text.c_str(), &end);
        if (!train || !node || offset_text.empty() || end == offset_text.c_str() || words.size() < 5) return "usage: move <train> <node> <offset> <0|1>";
        return replace_train(server, route, *train, simulation::TrackLocation{*node, offset, arg(4) != "0"}, std::nullopt);
    }
    if (command == "chocks") {
        const auto train = number_of(arg(1));
        const auto which = lower(arg(2));
        const std::optional<std::uint8_t> mask = which == "none" ? std::optional<std::uint8_t>{0}
                                               : which == "head" ? std::optional<std::uint8_t>{1}
                                               : which == "tail" ? std::optional<std::uint8_t>{2}
                                               : which == "both" ? std::optional<std::uint8_t>{3}
                                                                 : std::nullopt;
        if (!train || !mask) return "usage: chocks <train> none|head|tail|both";
        return replace_train(server, route, *train, std::nullopt, mask);
    }
    // The train form: its number, its name and its notes (one line here;
    // the console's form takes several).
    if (command == "number") {
        const auto train = number_of(arg(1));
        const auto number = number_of(arg(2));
        if (!train || !number) return "usage: number <train> <number>";
        return done(server.set_number(*train, *number, error), "train " + std::to_string(*train) + " is №" + std::to_string(*number));
    }
    if (command == "name" || command == "notes") {
        const auto train = number_of(arg(1));
        if (!train) return "usage: " + command + " <train> [text]";
        std::string text;
        for (std::size_t i = 2; i < words.size(); ++i) text += (i > 2 ? " " : "") + words[i];
        auto [name, notes] = server.names(*train);
        (command == "name" ? name : notes) = net::from_utf8(text);
        return done(server.set_names(*train, std::move(name), std::move(notes), error),
                    "train " + std::to_string(*train) + "'s " + command + (text.empty() ? " cleared" : " set"));
    }
    if (command == "couple") {
        const auto a = number_of(arg(1));
        const auto b = number_of(arg(2));
        if (!a || !b) return "usage: couple <train> <train>";
        return done(server.couple(*a, *b, error), "train " + std::to_string(*a) + " took in train " + std::to_string(*b));
    }
    if (command == "cut") {
        const auto train = number_of(arg(1));
        const auto car = number_of(arg(2));
        if (!train || !car) return "usage: cut <train> <car>";
        return done(server.cut(*train, *car, error), "train " + std::to_string(*train) + " cut before car " + std::to_string(*car));
    }
    if (command == "say") {
        if (words.size() < 3) return "usage: say <nick|*> <text>";
        std::string text;
        for (std::size_t i = 2; i < words.size(); ++i) text += (i > 2 ? " " : "") + words[i];
        return done(server.message(arg(1), net::from_utf8(text), error), "sent");
    }
    // The weather while it runs: one that stands, or the dynamic one.
    if (command == "weather") {
        auto w = server.weather();
        static constexpr const char* names[] = {"clear", "snow", "rain", "thunderstorm"};
        if (words.size() < 2) {
            return w.dynamic ? "weather: dynamic, seed " + std::to_string(w.seed) + ", " + std::to_string(static_cast<int>(w.clock / 60.0)) + " min run"
                             : std::string("weather: ") + names[w.weather < 4 ? w.weather : 0];
        }
        if (lower(arg(1)) == "dynamic") {
            w.dynamic = 1;
            if (words.size() > 2) {
                if (const auto seed = number_of(arg(2))) w.seed = *seed;
            }
            server.set_weather(w);
            return "weather: dynamic, seed " + std::to_string(w.seed);
        }
        const auto kind = parse_weather(arg(1));
        if (!kind) return "usage: weather [clear|snow|rain|storm|dynamic [seed]]";
        w.dynamic = 0;
        w.weather = *kind;
        server.set_weather(w);
        return std::string("weather: ") + names[*kind];
    }
    if (command == "pause" || command == "resume") {
        server.set_paused(command == "pause");
        return command == "pause" ? "paused" : "running";
    }
    if (command == "time") {
        if (words.size() < 2) return time_text(server.time_of_day());
        const auto seconds = parse_time_of_day(arg(1));
        if (!seconds) return "usage: time HH:MM[:SS]";
        server.set_time(*seconds);
        return "time " + time_text(*seconds);
    }
    if (command == "speed") {
        if (words.size() < 2) return "clock x" + std::to_string(static_cast<int>(server.clock_speed()));
        const auto value = number_of(arg(1));
        if (!value || *value < 1 || *value > 3600) return "usage: speed 1..3600";
        server.set_clock(static_cast<float>(*value), server.season());
        return "clock x" + std::to_string(static_cast<int>(server.clock_speed()));
    }
    if (command == "season") {
        static constexpr const char* seasons[] = {"spring", "summer", "autumn", "winter"};
        if (words.size() < 2) return seasons[(std::min)(server.season(), std::uint8_t{3})];
        const auto wanted = lower(arg(1));
        for (std::uint8_t i = 0; i < 4; ++i) {
            if (wanted == seasons[i]) {
                server.set_clock(server.clock_speed(), i);
                return std::string("season ") + seasons[i];
            }
        }
        return "usage: season spring|summer|autumn|winter";
    }
    if (command == "route") {
        const auto train = number_of(arg(1));
        if (!train || words.size() < 3) return "usage: route <train> <signal> [signal...] | route <train> cancel";
        if (lower(arg(2)) == "cancel") return done(server.cancel_route(*train, error), "train " + std::to_string(*train) + "'s route freed");
        std::vector<std::vector<std::uint32_t>> stops;
        for (std::size_t i = 2; i < words.size(); ++i) {
            auto items = route.signal_items(words[i]);
            if (items.empty()) {
                // A TDB item number given as it is.
                if (const auto item = number_of(words[i])) items.push_back(*item);
            }
            if (items.empty()) return "error: no signal named " + words[i] + " (see: signals)";
            stops.push_back(std::move(items));
        }
        std::string result;
        const bool ok = server.set_route(*train, stops, result);
        return ok ? result : "error: " + result;
    }
    if (command == "stations") {
        const auto filter = lower(arg(1));
        for (const auto& [station, count] : route.station_routes().stations()) {
            if (!filter.empty() && lower(station).find(filter) == std::string::npos) continue;
            out << station << " (" << count << ")\n";
        }
        return out.str().empty() ? "no station routes" : out.str();
    }
    if (command == "next") {
        if (words.size() < 2) return "usage: next <train> [back]";
        const auto train = static_cast<std::uint32_t>(std::stoul(std::string(arg(1))));
        const bool back = words.size() > 2 && lower(arg(2)) == "back";
        const auto from = server.next_from(train, back);
        if (!from) return "error: train " + std::to_string(train) + " is not on the server's track";
        const auto& routes = route.station_routes();
        const auto choices = routes.available(route.network(), from->at, 60000.0, from->behind);
        for (const auto& c : choices) {
            const auto& r = routes.routes()[c.route];
            const auto last = c.reverse ? r.start : r.end;
            out << r.data->id << "  " << r.data->station << ": " << r.data->name << (c.reverse ? " (back: segment ... back)" : "");
            if (last) {
                const auto& point = routes.points()[*last];
                if (point.data->flags & point_flag::platform) out << " [П]";
                if (point.length > 0.0) out << ' ' << static_cast<int>(point.length) << " m";
                if (point.data->flags & point_flag::wrong_way) out << " (onto a wrong track)";
            }
            if (r.data->flags & route_flag::main_track) out << " (main track)";
            if (c.distance > 50.0) out << ", in " << static_cast<int>(c.distance) << " m";
            out << '\n';
        }
        out << (from->after ? "from the end of segment " + std::to_string(*from->after) : std::string("from the train"))
            << (back ? ", back" : "") << (from->rear_first ? " (rear first: segment ... rear)" : "") << '\n';
        return out.str();
    }
    if (command == "routes") {
        if (words.size() < 2) return "usage: routes <station> [text]";
        // The station: its name, else the only one containing the text.
        const auto wanted = lower(arg(1));
        std::vector<std::string> found;
        for (const auto& [station, _] : route.station_routes().stations()) {
            if (lower(station) == wanted) {
                found = {station};
                break;
            }
            if (lower(station).find(wanted) != std::string::npos) found.push_back(station);
        }
        if (found.empty()) return "error: no station " + std::string(arg(1)) + " (see: stations)";
        if (found.size() > 1) {
            std::string list;
            for (const auto& s : found) list += (list.empty() ? "" : ", ") + s;
            return "error: several stations: " + list;
        }
        const auto filter = lower(arg(2));
        const auto& routes = route.station_routes();
        for (const auto& r : routes.routes()) {
            if (r.data->station != found.front()) continue;
            if (!filter.empty() && lower(r.data->name).find(filter) == std::string::npos) continue;
            char flags[16];
            std::snprintf(flags, sizeof flags, "%x", r.data->flags);
            const bool forward = routes.path_of(r, false) != nullptr;
            const bool backward = routes.path_of(r, true) != nullptr;
            out << r.data->id << "  " << r.data->name << ": " << r.data->start << " -> " << r.data->end << " (flags " << flags << ")"
                << (forward ? "" : backward ? " (laid only from its end)" : " (its way is not laid)") << '\n';
        }
        return out.str().empty() ? "no routes" : out.str();
    }
    if (command == "segments") {
        static constexpr const char* states[] = {"waiting", "set", "set part of the way", "the train on it", "left"};
        for (const auto& s : server.segments()) {
            out << s.id << "  train " << s.train << ": " << s.route << " " << s.name << " (" << s.station << ")"
                << (s.reverse ? " from its end" : "") << (s.hold ? ", holds the next" : "") << (s.reversed ? ", rear first" : "")
                << ", priority " << s.priority << ": " << (s.state < 5 ? states[s.state] : "?") << ", " << s.thrown << " of "
                << s.junctions << " junctions set, " << static_cast<int>(s.length) << " m\n";
        }
        return out.str().empty() ? "no segments" : out.str();
    }
    if (command == "segment") {
        const std::string usage = "usage: segment <train> <route ID> [back] [hold] [rear] [priority N] | segment cancel <id>";
        if (lower(arg(1)) == "cancel") {
            const auto id = number_of(arg(2));
            if (!id) return usage;
            return done(server.cancel_segment(*id, error), "segment " + std::to_string(*id) + " cancelled");
        }
        const auto train = number_of(arg(1));
        const auto id = number_of(arg(2));
        if (!train || !id) return usage;
        GameServer::SegmentRequest request{.train = *train, .route = *id};
        for (std::size_t i = 3; i < words.size(); ++i) {
            const auto w = lower(words[i]);
            if (w == "back") request.reverse = true;
            else if (w == "hold") request.hold = true;
            else if (w == "rear") request.reversed = true;
            else if (w == "priority" && i + 1 < words.size() && number_of(words[i + 1])) request.priority = *number_of(words[++i]);
            else return usage;
        }
        const auto made = server.add_segment(request, error);
        return made ? "segment " + std::to_string(*made) + " queued (see: segments)" : "error: " + error;
    }
    if (command == "heads") {
        // Every head as the players get it (0x460C): its TDB item, aspect,
        // draw state, mode; what openrts-net-dump --heads prints of a snapshot.
        if (const auto* signals = route.signals()) {
            for (const auto& head : signals->heads()) {
                // The mode as 0x460C carries it: the head's `enabled`, its unit's mode.
                out << "head " << head.track_item << ' ' << static_cast<int>(head.state) << ' ' << static_cast<int>(head.draw_state) << ' '
                    << static_cast<int>(signals->dispatched() ? head.enabled : head.mode) << '\n';
            }
        }
        return out.str().empty() ? "no signal heads" : out.str();
    }
    if (command == "signals") {
        const auto filter = lower(arg(1));
        std::string last;
        std::size_t count = 0;
        const auto flush = [&] {
            if (!last.empty()) out << last << (count > 1 ? " (" + std::to_string(count) + ")" : std::string()) << '\n';
        };
        for (const auto& [name, item] : route.signal_names()) {
            if (!filter.empty() && lower(name).find(filter) == std::string::npos) continue;
            if (name == last) {
                ++count;
                continue;
            }
            flush();
            last = name;
            count = 1;
        }
        flush();
        return out.str().empty() ? "no signal names" : out.str();
    }
    if (command == "junction") {
        const auto node = number_of(arg(1));
        if (!node) return "usage: junction <node> [branch] [lock|unlock]";
        std::optional<std::uint32_t> branch;
        std::optional<bool> lock;
        for (std::size_t i = 2; i < words.size(); ++i) {
            const auto w = lower(words[i]);
            if (w == "lock") lock = true;
            else if (w == "unlock") lock = false;
            else if (const auto b = number_of(w)) branch = *b;
            else return "usage: junction <node> [branch] [lock|unlock]";
        }
        return done(server.set_junction(*node, branch, lock, error), "junction " + std::to_string(*node) + " set");
    }
    if (command == "signal") {
        const std::string usage = "usage: signal <item|name> [aspect|open|auto|close|default] | signal type <type> <manual|open|auto> | "
                                  "signal type <type> normal on|off | signal defaults | signal set-defaults | signal save";
        if (words.size() < 2) return usage;
        if (lower(arg(1)) == "defaults" && words.size() == 2) {
            return done(server.restore_signal_defaults(error), "every signal back to its defaults");
        }
        if (lower(arg(1)) == "set-defaults" && words.size() == 2) {
            return done(server.set_signal_defaults(error), "every signal's defaults are its mode and aspect now (signal save keeps them)");
        }
        if (lower(arg(1)) == "save" && words.size() == 2) {
            std::string result;
            const bool ok = route.save_signal_defaults(result);
            return ok ? result : "error: " + result;
        }
        if (lower(arg(1)) == "type") {
            if (words.size() < 4) return usage;
            const auto what = lower(arg(3));
            if (what == "normal") {
                const auto state = lower(arg(4));
                if (state != "on" && state != "off") return usage;
                std::size_t count = 0;
                const bool ok = server.set_signal_type_shown(arg(2), state == "on", count, error);
                return done(ok, "type " + std::string(arg(2)) + (state == "on" ? " shown as normal (" : " not shown as normal (") +
                                    std::to_string(count) + " signals)");
            }
            simulation::UnitMode mode = simulation::UnitMode::manual;
            if (what == "open") mode = simulation::UnitMode::open;
            else if (what == "auto" || what == "automatic") mode = simulation::UnitMode::automatic;
            else if (what != "manual" && what != "close") return usage;
            std::size_t count = 0;
            const bool ok = server.set_signal_type_mode(arg(2), mode, count, error);
            return done(ok, std::to_string(count) + " signals of type " + std::string(arg(2)) + ": " + what);
        }
        // The signal: a name only one signal has (names can be numbers, as
        // "1"), else a TDB item.
        std::uint32_t item = 0;
        const auto items = route.signal_items(arg(1));
        if (!items.empty()) {
            std::set<std::uint32_t> units;
            std::string list;
            for (const auto i : items) {
                const auto unit = route.signals() ? route.signals()->unit_of_item(i) : std::nullopt;
                if (unit && units.insert(*unit).second) list += (list.empty() ? "" : ", ") + std::to_string(i);
            }
            if (units.size() > 1) {
                return "error: " + std::to_string(units.size()) + " signals are named " + std::string(arg(1)) + ": items " + list;
            }
            item = items.front();
        } else if (const auto n = number_of(arg(1))) {
            item = *n;
        } else {
            return "error: no signal named " + std::string(arg(1)) + " (see: signals)";
        }
        if (words.size() < 3) return server.signal_report(item);
        const auto what = lower(arg(2));
        const auto action = [&](const GameServer::SignalAction a) { return done(server.control_signal(item, a, error), server.signal_report(item)); };
        if (what == "open") return action(GameServer::SignalAction::open);
        if (what == "auto" || what == "automatic") return action(GameServer::SignalAction::automatic);
        if (what == "close") return action(GameServer::SignalAction::close);
        if (what == "default") return action(GameServer::SignalAction::defaults);
        auto aspect = formats::signal_aspect_from_name(what);
        if (!aspect) {
            const auto n = number_of(what);
            if (!n || *n > 7) return "error: unknown aspect " + std::string(arg(2));
            aspect = static_cast<formats::SignalAspect>(*n);
        }
        return done(server.set_signal(item, aspect, error), server.signal_report(item));
    }
    if (command == "helper") {
        if (words.size() < 3) return "usage: helper <nick> <train|off>";
        if (lower(arg(2)) == "off") return done(server.set_helper(arg(1), std::nullopt, error), std::string(arg(1)) + " helps no more");
        const auto train = number_of(arg(2));
        if (!train) return "usage: helper <nick> <train|off>";
        return done(server.set_helper(arg(1), *train, error), std::string(arg(1)) + " helps on train " + std::to_string(*train));
    }
    if (command == "kick") {
        if (words.size() < 2) return "usage: kick <nick>";
        return done(server.kick(arg(1), error), std::string(arg(1)) + " dropped");
    }
    if (command == "admit") {
        const auto w = lower(arg(1));
        if (w != "on" && w != "off") return "usage: admit on|off";
        server.set_admit(w == "on");
        return w == "on" ? "new players are taken" : "no new players";
    }
    if (command == "fastpos") {
        const auto w = lower(arg(1));
        if (w != "on" && w != "off") return std::string("usage: fastpos on|off (now ") + (server.fast_positions() ? "on" : "off") + ")";
        server.set_fast_positions(w == "on");
        return w == "on" ? "fast positions on" : "fast positions off";
    }
    return "unknown command " + words[0] + " (help)";
}

} // namespace openrts::server
