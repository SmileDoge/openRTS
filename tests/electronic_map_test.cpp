// openRTS's КЛУБ's electronic map (simulation/electronic_map) on Kuban's
// route database: its station tracks placed on the track; a train coming up
// to Албаши from Староминская with a track entered gets the map's target and
// distance, a number no station has and 99 "без ЭК"; driven through the
// station as the switches lie, a track entered that it does not take leaves
// the ЭК at the other track's signal and comes back near a station with it.
#include <cmath>
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
    double off_at = -1.0;  // m driven when "без ЭК" came first
    double back_at = -1.0; // and when the ЭК came back
    std::size_t targets{};
};

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "electronic_map_test: no RTS folder given, skipped\n";
        return 0;
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
            for (const auto p : passed) {
                const auto* track = map.track_of(p);
                log += "  passed " + std::to_string(p) + (track ? " (" + track->station + " " + track->name + ")" : std::string()) + " at "
                       + std::to_string(static_cast<int>(moved)) + "\n";
            }
            if (target.has_target && target.object != last_target) {
                ++drive.targets;
                const auto* track = map.track_of(target.object);
                log += "  target " + std::to_string(target.object) + (track ? " (" + track->station + " " + track->name + ")" : std::string())
                       + " in " + std::to_string(static_cast<int>(target.distance)) + " m" + (target.matched ? "" : " без ЭК") + "\n";
                last_target = target.object;
            }
            if (!target.matched && was_matched) {
                log += "  без ЭК from " + std::to_string(static_cast<int>(moved)) + " m\n";
                if (drive.off_at < 0.0) drive.off_at = moved;
            } else if (target.matched && !was_matched) {
                log += "  ЭК again from " + std::to_string(static_cast<int>(moved)) + " m\n";
                if (drive.back_at < 0.0) drive.back_at = moved;
            }
            was_matched = target.matched;
            if (!t.move(stride).ok()) break;
            moved += stride;
        }
        std::cout << "track " << number << ":\n" << log;
        const std::string name = "track " + std::to_string(number);
        check(drive.matched_at_start, name + ": the ЭК with a target on the line before the station");
        check(drive.targets >= 2, name + ": the targets change along the way");
        if (number == 1) {
            // The main line: the train keeps to it, the ЭК all the way.
            check(drive.off_at < 0.0, name + ": the ЭК all the way (the train on track 1)");
        } else {
            // Another track: the train goes on along track 1, whose signal
            // leaves the ЭК; not back in Albashi.
            check(drive.off_at > 2000.0 && drive.off_at < 6000.0, name + ": \"без ЭК\" at Albashi's track 1 signal");
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
