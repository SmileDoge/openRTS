// openRTS's КЛУБ's electronic map (simulation/electronic_map) on Kuban's
// route database: its station tracks placed on the track; a train coming up
// to Албаши from Староминская with a track entered gets the map's target and
// distance, a number no station has and 99 "без ЭК"; driven through the
// station as the switches lie, a track entered that it does not take leaves
// the ЭК at the other track's signal and comes back near a station with it.
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>
#include <iostream>
#include <set>
#include <string>

#include <openrts/core/installation.hpp>
#include <openrts/core/vfs.hpp>
#include <openrts/formats/route_database.hpp>
#include <openrts/formats/signal_config.hpp>
#include <openrts/simulation/electronic_map.hpp>
#include <openrts/simulation/milepost.hpp>
#include <openrts/simulation/track_traveller.hpp>

using namespace openrts;

namespace {

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

simulation::TrackPoint at(const formats::RoutePosition& p) { return {.tile_x = p.tile_x, .tile_z = p.tile_z, .x = p.x, .y = p.y, .z = p.z}; }

const formats::RoutePoint* point(const formats::RouteDatabase& database, const std::string& station, const std::string& name) {
    for (const auto& p : database.points)
        if (p.station == station && p.name == name) return &p;
    return nullptr;
}

struct Drive {
    std::uint32_t number{};
    simulation::EkState state;
    bool matched_at_start = false;
    double on_at = -1.0;   // m driven when the ЭК came first (with a target)
    double off_at = -1.0;  // and when "без ЭК" came after it
    double back_at = -1.0; // and when the ЭК came back
    std::size_t targets{};
};

} // namespace

// A look at one case (not a check): OPENRTS_EK_PROBE="<route>|<station>|<track
// name>|<number>" drives from the middle of that station track both ways
// with the number entered and prints what the map gives.
int probe(const std::filesystem::path& root, std::string spec) {
    // "@<file>": the spec in a UTF-8 file (the environment's is the code page's).
    if (!spec.empty() && spec[0] == '@') {
        std::ifstream in(std::filesystem::path(spec.substr(1)), std::ios::binary);
        std::getline(in, spec);
        while (!spec.empty() && (spec.back() == '\r' || spec.back() == '\n')) spec.pop_back();
    }
    std::vector<std::string> part;
    for (std::size_t from = 0;;) {
        const auto bar = spec.find('|', from);
        part.push_back(spec.substr(from, bar == std::string::npos ? std::string::npos : bar - from));
        if (bar == std::string::npos) break;
        from = bar + 1;
    }
    if (part.size() < 4) return 2;
    core::Vfs vfs;
    core::mount_installation(vfs, root, core::load_installation_config(root));
    vfs.build_index();
    const auto route = core::VirtualPath::parse("ROUTES/" + part[0]);
    std::string error;
    auto network = route ? simulation::TrackNetwork::load(vfs, *route, &error) : nullptr;
    if (!network) return 2;
    const auto database = formats::load_route_database(vfs, *route);
    const auto config = formats::load_signal_config(vfs, *route);
    const auto probe_units = formats::rts_signal_units(network->database(), database);
    std::vector<simulation::MapSignal> signals;
    std::set<std::uint32_t> normal;
    const auto& db = network->database();
    for (std::uint32_t n = 0; n < network->node_count(); ++n) {
        const auto* node = network->node(n);
        if (!node || node->kind != formats::TrackNodeKind::vector) continue;
        for (const auto& entry : node->items) {
            const auto& item = db.items[entry.item];
            if (item.kind != formats::TrackItemKind::signal) continue;
            const auto* type = config.find_type(item.name);
            if (!type || type->function != formats::SignalFunction::normal) continue;
            if (entry.item < probe_units.size() && probe_units[entry.item] >= 0 && static_cast<std::size_t>(probe_units[entry.item]) < database.signal_units.size() && simulation::shunting_signal_name(database.signal_units[static_cast<std::size_t>(probe_units[entry.item])].name)) continue;
            signals.push_back({.object = entry.item, .node = n, .offset = entry.offset, .forward = item.signal_direction == 0});
            normal.insert(entry.item);
        }
    }
    const simulation::ElectronicMap map(*network, database, signals);
    const auto number = static_cast<std::uint32_t>(std::stoul(part[3]));
    const auto item_unit = formats::rts_signal_units(db, database);
    const auto label = [&](const std::uint32_t object) {
        const auto* track = map.track_of(object);
        std::string name;
        if (object < item_unit.size() && item_unit[object] >= 0 && static_cast<std::size_t>(item_unit[object]) < database.signal_units.size())
            name = " " + database.signal_units[static_cast<std::size_t>(item_unit[object])].name;
        return std::to_string(object) + name + (track ? " (" + track->station + " " + track->name + ")" : std::string());
    };
    if (part[2] == "?") {
        for (const auto& track : map.tracks())
            if (track.station.find(part[1]) != std::string::npos) std::cout << track.station << " | " << track.name << " | " << track.number << "\n";
        return 0;
    }
    for (const auto& track : map.tracks()) {
        if (track.station != part[1] || track.name != part[2] || track.spans.empty()) continue;
        const auto& span = track.spans[track.spans.size() / 2];
        for (const bool way : {true, false}) {
            std::cout << "== " << track.station << " " << track.name << (way ? " forward" : " back") << ", " << number << " entered\n";
            simulation::TrackTraveller t(*network, span.node, 0.5 * (span.from + span.to), way);
            simulation::EkState state;
            std::uint32_t last = ~0u;
            bool was = false;
            double moved = 0.0;
            double prev_turn = -1.0;
            float prev_speed = 0.0f;
            for (int step = 0; step < 2400; ++step) {
                const auto ahead = simulation::look_ahead(*network, t.node(), t.offset(), t.forward(), 5.0,
                                                          [](const formats::TrackItem& item) { return item.kind == formats::TrackItemKind::signal; });
                std::vector<std::uint32_t> passed;
                for (const auto& item : ahead.items) {
                    const bool facing = (db.items[item.item].signal_direction == 0) == item.facing_forward;
                    if (facing && normal.contains(item.item) && item.distance > 0.0) passed.push_back(item.item);
                }
                const auto target = map.update(state, *network, t.node(), t.offset(), t.forward(), number, passed);
                for (const auto p : passed) std::cout << "  passed " << label(p) << " at " << static_cast<int>(moved) << "\n";
                // The map's way's switches off the straight (the КЛУБ's 40 km/h): printed as passed.
                if (target.matched) {
                    const auto w = map.way(*network, t.node(), t.offset(), t.forward(), number);
                    const double first = w.turns.empty() ? -1.0 : w.turns.front().distance;
                    if (prev_turn >= 0.0 && prev_turn <= 5.0 && (first < 0.0 || first > prev_turn))
                        std::cout << "  switch off the straight at " << static_cast<int>(moved + prev_turn) << ", " << static_cast<int>(prev_speed * 3.6f + 0.5f)
                                  << " km/h" << std::endl;
                    prev_turn = first;
                    prev_speed = w.turns.empty() ? 0.0f : w.turns.front().speed;
                }
                // A fifth part "from-to": every step there.
                if (part.size() > 4) {
                    const auto dash = part[4].find('-');
                    const double from = std::stod(part[4].substr(0, dash)), to = std::stod(part[4].substr(dash + 1));
                    if (moved >= from && moved <= to)
                        std::cout << "    " << static_cast<int>(moved) << ": node " << t.node() << " at " << static_cast<int>(t.offset())
                                  << (t.forward() ? " fwd" : " back") << ", "
                                  << (target.has_target ? label(target.object) + " in " + std::to_string(static_cast<int>(target.distance)) : std::string("-"))
                                  << (target.matched ? "" : " без ЭК") << '\n';
                }
                if (target.matched != was || (target.has_target && target.object != last)) {
                    std::cout << "  at " << static_cast<int>(moved) << ": "
                              << (!target.matched ? std::string("без ЭК")
                                  : target.has_target ? "target " + label(target.object) + " in " + std::to_string(static_cast<int>(target.distance)) + " m"
                                                      : std::string("no target"))
                              << " (line " << map.line_number(t.node()) << ")\n";
                    was = target.matched;
                    last = target.has_target ? target.object : ~0u;
                }
                if (!t.move(5.0).ok()) break;
                moved += 5.0;
            }
        }
    }
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "electronic_map_test: no RTS folder given, skipped\n";
        return 0;
    }
    {
        std::string spec;
#ifdef _WIN32
        char* value = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&value, &length, "OPENRTS_EK_PROBE") == 0 && value) spec = value;
        std::free(value);
#else
        if (const char* value = std::getenv("OPENRTS_EK_PROBE")) spec = value;
#endif
        if (!spec.empty()) return probe(argv[1], spec);
    }
    const std::filesystem::path root = argv[1];
    core::Vfs vfs;
    core::mount_installation(vfs, root, core::load_installation_config(root));
    vfs.build_index();
    const auto route = core::VirtualPath::parse("ROUTES/Kuban_RTS");
    std::string error;
    auto network = route ? simulation::TrackNetwork::load(vfs, *route, &error) : nullptr;
    if (!network) {
        std::cout << "electronic_map_test: no Kuban_RTS (" << error << "), skipped\n";
        return 0;
    }
    const auto database = formats::load_route_database(vfs, *route);
    const auto config = formats::load_signal_config(vfs, *route);
    // The train signals from the TDB: a NORMAL type, the way it faces.
    std::vector<simulation::MapSignal> signals;
    std::set<std::uint32_t> normal;
    const auto& db = network->database();
    for (std::uint32_t n = 0; n < network->node_count(); ++n) {
        const auto* node = network->node(n);
        if (!node || node->kind != formats::TrackNodeKind::vector) continue;
        for (const auto& entry : node->items) {
            const auto& item = db.items[entry.item];
            if (item.kind != formats::TrackItemKind::signal) continue;
            const auto* type = config.find_type(item.name);
            if (!type || type->function != formats::SignalFunction::normal) continue;
            signals.push_back({.object = entry.item, .node = n, .offset = entry.offset, .forward = item.signal_direction == 0});
            normal.insert(entry.item);
        }
    }
    const simulation::ElectronicMap map(*network, database, signals);
    std::size_t station_tracks = 0;
    for (const auto& p : database.points) station_tracks += p.type == 2 && simulation::ElectronicMap::track_number(p.name) ? 1 : 0;
    std::cout << "map: " << map.tracks().size() << " of " << station_tracks << " station tracks placed, " << signals.size()
              << " train signals\n";
    check(map.tracks().size() + 2 >= station_tracks, "the station tracks placed on the track");
    // A switch's speed by its shape (GLOBAL tsection.dat): a 3° B point 40,
    // 4.7° (482 m) 40, 5° (655 m) 80, 3.0° (1239 m) 120, a slip 25.
    {
        const auto& lib = network->sections();
        const auto kmh = [&](const std::uint32_t shape) {
            return static_cast<int>(simulation::switch_speed(lib.shape(shape), lib) * 3.6f + 0.5f);
        };
        check(kmh(38046) == 40 && kmh(21994) == 40 && kmh(70) == 80 && kmh(21500) == 120 && kmh(22022) == 25,
              "switch speeds by shape: " + std::to_string(kmh(38046)) + " " + std::to_string(kmh(21994)) + " " + std::to_string(kmh(70)) + " "
                  + std::to_string(kmh(21500)) + " " + std::to_string(kmh(22022)));
    }
    check(simulation::ElectronicMap::track_number("1БП") == 1 && simulation::ElectronicMap::track_number("21") == 21
              && simulation::ElectronicMap::track_number("Н Албаши") == 0 && simulation::ElectronicMap::track_number("99") == 0,
          "the track numbers of the names");

    // Albashi's entry from Староминская, the way into the station: the one
    // its track 1 is ahead on.
    const auto* entry = point(database, "Албаши", "Н Староминская-Тимашевская");
    check(entry != nullptr, "Албаши's entry in the database");
    if (!entry) return 1;
    const auto located = network->locate(at(entry->point1.position), 10.0, 2.0);
    check(located.has_value(), "the entry on the track");
    if (!located) return 1;
    bool inwards = true;
    {
        simulation::EkState probe;
        const auto one = map.update(probe, *network, located->node, located->offset, true, 1, {});
        simulation::EkState other;
        const auto two = map.update(other, *network, located->node, located->offset, false, 1, {});
        inwards = one.has_target && (!two.has_target || one.distance < two.distance);
    }
    // 2 km out on the line.
    simulation::TrackTraveller train(*network, located->node, located->offset, inwards);
    train.reverse();
    train.move(2000.0);
    train.reverse();

    // A number no station has, and 99: "без ЭК".
    {
        simulation::EkState state;
        check(!map.update(state, *network, train.node(), train.offset(), train.forward(), 77, {}).matched, "77: no station has it");
        simulation::EkState s99;
        check(!map.update(s99, *network, train.node(), train.offset(), train.forward(), 99, {}).matched, "99: без ЭК");
    }

    // Driven through Albashi as the switches lie (MainRoute), with each
    // station track's number entered.
    std::set<std::uint32_t> albashi, derevyankovka;
    for (const auto& t : map.tracks()) {
        if (t.station == "Албаши") albashi.insert(t.number);
        if (t.station == "Деревянковка") derevyankovka.insert(t.number);
    }
    check(albashi.size() >= 2, "Albashi's tracks on the map");
    for (const auto number : albashi) {
        simulation::TrackTraveller t = train;
        Drive drive{.number = number};
        std::uint32_t last_target = ~0u;
        double last_distance = 0.0;
        int stuck = 0;    // times the same target's distance stood
        int standing = 0; // steps running it has not fallen
        double moved = 0.0;
        bool was_matched = true;
        std::string log;
        for (int step = 0; step < 6000 && moved < 26000.0; ++step) {
            // The signals the front passes this step, as the switches lie.
            constexpr double stride = 5.0;
            const auto ahead = simulation::look_ahead(*network, t.node(), t.offset(), t.forward(), stride,
                                                      [](const formats::TrackItem& item) { return item.kind == formats::TrackItemKind::signal; });
            std::vector<std::uint32_t> passed;
            for (const auto& item : ahead.items) {
                const bool facing = (db.items[item.item].signal_direction == 0) == item.facing_forward;
                if (facing && normal.contains(item.item) && item.distance > 0.0) passed.push_back(item.item);
            }
            const auto target = map.update(drive.state, *network, t.node(), t.offset(), t.forward(), number, passed);
            if (step == 0) drive.matched_at_start = target.matched && target.has_target;
            if (drive.on_at < 0.0 && target.matched && target.has_target) drive.on_at = moved;
            for (const auto p : passed) {
                const auto* track = map.track_of(p);
                log += "  passed " + std::to_string(p) + (track ? " (" + track->station + " " + track->name + ")" : std::string()) + " at "
                       + std::to_string(static_cast<int>(moved)) + "\n";
            }
            // Standing: 3 steps (15 m) running without a metre less.
            if (target.matched && target.has_target && target.object == last_target && target.distance > last_distance - 1.0) {
                if (++standing == 3 && stuck++ < 3)
                    log += "  stuck at " + std::to_string(static_cast<int>(target.distance)) + " m at " + std::to_string(static_cast<int>(moved)) + "\n";
            } else {
                standing = 0;
            }
            if (target.has_target) last_distance = target.distance;
            if (target.has_target && target.object != last_target) {
                ++drive.targets;
                const auto* track = map.track_of(target.object);
                log += "  target " + std::to_string(target.object) + (track ? " (" + track->station + " " + track->name + ")" : std::string())
                       + " in " + std::to_string(static_cast<int>(target.distance)) + " m" + (target.matched ? "" : " без ЭК") + "\n";
                last_target = target.object;
            }
            if (!target.matched && was_matched) {
                log += "  без ЭК from " + std::to_string(static_cast<int>(moved)) + " m\n";
                if (drive.off_at < 0.0 && drive.on_at >= 0.0) drive.off_at = moved;
            } else if (target.matched && !was_matched) {
                log += "  ЭК again from " + std::to_string(static_cast<int>(moved)) + " m\n";
                if (drive.back_at < 0.0 && drive.off_at >= 0.0) drive.back_at = moved;
            }
            was_matched = target.matched;
            if (!t.move(stride).ok()) break;
            moved += stride;
        }
        std::cout << "track " << number << ": " << stuck << " steps stuck\n" << log;
        const std::string name = "track " + std::to_string(number);
        // The distance counts down as the train runs, past a nearer signal
        // of the entered track too (the testers' 2026-11-21: it stood at 2 m).
        check(stuck == 0, name + ": the distance never stands while the train runs");
        // Track 1 (MainRoute) and 2 (its line track abreast) at once; a side
        // track only in front of the signal its turn lies beyond, Albashi's
        // entry here (2 km out the block signal at 570 m before it: the
        // testers' 2026-11-21).
        if (number <= 2) {
            check(drive.matched_at_start, name + ": the ЭК with a target on the line before the station");
        } else {
            check(!drive.matched_at_start && drive.on_at >= 500.0 && drive.on_at < 2000.0,
                  name + ": \"без ЭК\" up to the block signal, the ЭК in front of the entry");
        }
        check(drive.targets >= 2, name + ": the targets change along the way");
        if (number == 1) {
            // The main line: the train keeps to it, the ЭК all the way.
            check(drive.off_at < 0.0, name + ": the ЭК all the way (the train on track 1)");
        } else if (number == 2) {
            // The other main track: out of Albashi by track 1 the ЭК goes on
            // along the line's track 2 (the testers' 2026-11-21).
            check(drive.off_at < 0.0 || drive.off_at > 24000.0, name + ": the ЭК on along the line's track 2 out of Albashi by track 1");
        } else {
            // Another track: the train goes on along track 1; its station's
            // exits passed on (4100 m), "без ЭК" past the first block
            // signal (7050 m; the testers' 2026-11-21).
            check(drive.off_at > 6000.0 && drive.off_at < 8000.0, name + ": \"без ЭК\" past the first block signal out of Albashi");
            // Back before Деревянковка's entry where it has the track, else not.
            if (derevyankovka.contains(number)) check(drive.back_at > 9000.0, name + ": back before Деревянковка, which has it");
            else check(drive.back_at < 0.0, name + ": not back (Деревянковка has none)");
        }
    }
    // Standing on Albashi's track 1 with another of its tracks entered (the
    // testers' case, 2026-11-20: the train at its platform, the switches set
    // for it): that track's signal, as the real КЛУБ-У's map by its
    // coordinate, not track 1's (nor the next station's track of the number).
    for (const auto& one : map.tracks()) {
        if (one.station != "Албаши" || one.name != "1" || one.spans.empty()) continue;
        const auto& span = one.spans[one.spans.size() / 2];
        const double middle = 0.5 * (span.from + span.to);
        for (const auto number : albashi) {
            if (number == 1) continue;
            int targets = 0;
            for (const bool way : {true, false}) {
                simulation::EkState state;
                const auto target = map.update(state, *network, span.node, middle, way, number, {});
                const auto* track = target.has_target ? map.track_of(target.object) : nullptr;
                std::cout << "on Албаши 1, track " << number << (way ? " forward: " : " back: ")
                          << (!target.matched ? std::string("без ЭК")
                              : track ? track->station + " " + track->name + " in " + std::to_string(static_cast<int>(target.distance)) + " m"
                                      : std::string("signal ") + std::to_string(target.object))
                          << "\n";
                if (!target.has_target) continue;
                ++targets;
                check(target.matched && track && track->station == "Албаши" && track->number == number,
                      "on Албаши 1, track " + std::to_string(number) + ": its signal");
            }
            check(targets > 0, "on Албаши 1, track " + std::to_string(number) + ": a target");
        }
        break;
    }
    // On the line (the testers' 2026-11-21): 2 km out before Albashi the
    // train's line track has its station track's number; the other line
    // track's number entered gives that track's next train signal abreast
    // (its back to the train), and on along it as the train runs: driven
    // 4 km away from Albashi, each target on that track ahead of the front.
    {
        std::unordered_map<std::uint32_t, simulation::MapSignal> placed;
        for (const auto& s : signals) placed[s.object] = s;
        const auto own = map.line_number(train.node());
        std::cout << "on the line 2 km out: line track " << own << "\n";
        check(own != 0, "the line before Albashi has its track's number");
        simulation::TrackTraveller t = train;
        t.reverse();
        const std::uint32_t other = own == 1 ? 2 : 1;
        simulation::EkState state;
        std::uint32_t last_target = ~0u;
        double last_distance = 0.0;
        int targets = 0, on_other = 0, backs = 0, nearer = 0, steps = 0, matched = 0;
        for (double moved = 0.0; moved < 4000.0; moved += 10.0) {
            const auto target = map.update(state, *network, t.node(), t.offset(), t.forward(), other, {});
            ++steps;
            if (target.matched && target.has_target) {
                ++matched;
                if (target.object != last_target) {
                    ++targets;
                    const auto it = placed.find(target.object);
                    const bool its = it != placed.end() && map.line_number(it->second.node) == other;
                    on_other += its ? 1 : 0;
                    std::cout << "  target " << target.object << (its ? " on line " + std::to_string(other) : std::string(" elsewhere"))
                              << " in " << static_cast<int>(target.distance) << " m at +" << static_cast<int>(moved) << "\n";
                    // Its back to the train: it faces the other way along its node
                    // than the train runs abreast of it.
                    if (it != placed.end()) {
                        const auto a = network->point(t.node(), t.offset());
                        const auto b = network->point(it->second.node, it->second.offset);
                        if (a && b) {
                            const double train_heading = a->heading + (t.forward() ? 0.0 : 3.14159265358979323846);
                            const double faces = b->heading + (it->second.forward ? 0.0 : 3.14159265358979323846);
                            const bool back = std::cos(faces - train_heading) < 0.0;
                            backs += back ? 1 : 0;
                            std::cout << "    " << (back ? "its back to the train" : "facing the train") << ", " << db.items[target.object].name
                                      << " on node " << it->second.node << "\n";
                        }
                    }
                } else if (target.distance < last_distance) {
                    ++nearer;
                }
                last_target = target.object;
                last_distance = target.distance;
            }
            if (!t.move(10.0).ok()) break;
        }
        std::cout << "line " << other << " entered on line " << own << ": " << matched << " of " << steps << " steps matched, " << targets
                  << " targets, " << on_other << " on line " << other << ", " << backs << " with their backs\n";
        check(matched == steps, "the other line track entered: the ЭК all the way");
        check(targets >= 2 && on_other == targets, "its targets are the other line track's signals, one after another");
        check(backs == targets, "their backs to the train (the other way's block signals)");
        check(nearer > steps / 2, "each counts down as the train runs");
    }
    // The coordinate (Engine::CurrentMilepost, simulation/milepost.hpp):
    // driven 3 km on from 2 km out before Albashi, every 500 m it moves by
    // about 0.5 km (the kilometre posts' numbers, one way or the other).
    {
        simulation::TrackTraveller t = train;
        simulation::MilepostState state;
        float last = -1.0f;
        int steps = 0, moved_right = 0;
        for (int k = 0; k <= 6; ++k) {
            simulation::update_milepost(state, *network, t.node(), t.offset(), t.forward());
            const float km = simulation::car_milepost(state, 0.0);
            std::cout << "coordinate at +" << k * 500 << " m: " << km << (state.decreasing ? " (falling)" : "") << "\n";
            if (last >= 0.0f && km >= 0.0f) {
                ++steps;
                if (std::abs(std::abs(km - last) - 0.5f) < 0.1f) ++moved_right; // the posts are not 1000 m apart everywhere
            }
            last = km;
            if (!t.move(500.0).ok()) break;
        }
        check(steps >= 4 && moved_right == steps, "the coordinate moves 0.5 km in 500 m");
    }
    if (failures == 0) std::cout << "electronic_map_test: ok\n";
    return failures == 0 ? 0 : 1;
}
