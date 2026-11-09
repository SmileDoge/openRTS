#include <openrts/server/console.hpp>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstdio>
#include <sstream>

#include <openrts/core/virtual_path.hpp>
#include <openrts/formats/signal_config.hpp>
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
    "  say <nick|*> <text>            a message to a player or everyone\n"
    "  pause | resume                 stop or start the game\n"
    "  time [HH:MM[:SS]]              show or set the time of day\n"
    "  route <train> <signal> [signal...]  set and hold the junctions from the train to a signal\n"
    "                                 (its name, \"литера\", from the route database), through the others in turn\n"
    "  route <train> cancel           free the route's junctions\n"
    "  signals [text]                 the signals' names and how many of each\n"
    "  junction <node> [branch] [lock|unlock]  throw or lock a junction (TDB node)\n"
    "  signal <item> <aspect|auto>    hold a signal's heads (TDB item) at an aspect, or give them back\n"
    "                                 aspects: stop, stop_and_proceed, restricting, approach_1..3, clear_1, clear_2\n"
    "  kick <nick>                    drop a player\n"
    "  admit on|off                   take new players or not\n"
    "  quit                           stop the server\n";

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

std::string place_cars(GameServer& server, ServerRoute& route, const net::ConsolePlace& place) {
    std::vector<formats::ConsistEntry> entries;
    for (const auto& car : place.cars) {
        formats::ConsistEntry entry;
        entry.folder = net::to_utf8(car.folder);
        entry.name = net::to_utf8(car.name);
        entry.engine = car.engine;
        entry.flipped = car.flipped;
        entry.cold = car.cold;
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
    const auto id = server.add_train(std::move(*train), error);
    if (!id) return "error: " + name + ": " + error;
    return "train " + std::to_string(*id) + " (№" + std::to_string(number) + ", " + name + ", " + std::to_string(place.cars.size()) +
           " cars) stands there; give it to a player: assign " + std::to_string(*id) + " <nick>";
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
    if (command == "say") {
        if (words.size() < 3) return "usage: say <nick|*> <text>";
        std::string text;
        for (std::size_t i = 2; i < words.size(); ++i) text += (i > 2 ? " " : "") + words[i];
        return done(server.message(arg(1), net::from_utf8(text), error), "sent");
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
        const auto item = number_of(arg(1));
        if (!item || words.size() < 3) return "usage: signal <item> <aspect|auto>";
        std::optional<formats::SignalAspect> aspect;
        if (lower(arg(2)) != "auto") {
            aspect = formats::signal_aspect_from_name(lower(arg(2)));
            if (!aspect) {
                const auto n = number_of(arg(2));
                if (!n || *n > 7) return "error: unknown aspect " + std::string(arg(2));
                aspect = static_cast<formats::SignalAspect>(*n);
            }
        }
        return done(server.set_signal(*item, aspect, error),
                    "signal " + std::to_string(*item) + (aspect ? std::string(" held at ") + formats::to_string(*aspect) : " runs its script"));
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
    return "unknown command " + words[0] + " (help)";
}

} // namespace openrts::server
