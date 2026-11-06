// openrts-path-check <installation root> <route> <consist> [path...] [--seconds N] [--together] [--following M]
// openrts-path-check <installation root> <route> x --replan      (plan every path from its ends, compare)
// openrts-path-check <installation root> <route> x --junction N  (a junction, its tracks and their signals)
//
// Drives a consist with the test driver along route paths (docs/paths.md):
// the path resolved on the network, its junctions reserved by the
// dispatcher, signals evaluated with the train's reserved way, speed limits
// from the speed posts. Each path runs on its own; with --together all the
// given paths run at once (their trains meet at shared junctions). Reports
// how far each train got, its stops, speed against the limit, signals
// passed at danger and what it waits for at the end.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <openrts/core/installation.hpp>
#include <openrts/core/vfs.hpp>
#include <openrts/formats/rolling_stock.hpp>
#include <openrts/formats/route_database.hpp>
#include <openrts/formats/route_world.hpp>
#include <openrts/formats/signal_config.hpp>
#include <openrts/formats/signal_script.hpp>
#include <openrts/formats/train_path.hpp>
#include <openrts/simulation/auto_driver.hpp>
#include <openrts/simulation/dispatcher.hpp>
#include <openrts/simulation/signals.hpp>
#include <openrts/simulation/speed_limits.hpp>
#include <openrts/simulation/train.hpp>
#include <openrts/simulation/train_route.hpp>

namespace {

using namespace openrts;

struct Runner {
    std::string path_name;
    std::vector<std::unique_ptr<formats::WagonDefinition>> stock;
    std::unique_ptr<simulation::Train> train;
    std::unique_ptr<simulation::AutoDriver> driver;
    bool freight = true;
    float wait_left = -1.0f;
    double travelled = 0.0;
    double over_limit = 0.0; // worst speed above the limit, m/s
    double top_speed = 0.0;
    std::size_t stops = 0;
    bool moving = false;
    std::size_t passed_at_danger = 0;
    std::vector<std::string> history; // the next signal over the last steps
    std::string failure;
};

std::string load_train(const core::Vfs& vfs, const std::string& consist_name, Runner& runner) {
    const auto consist = formats::load_consist(vfs, consist_name);
    if (!consist.ok()) return "consist: " + consist.error;
    std::vector<simulation::CarSetup> setups;
    bool passenger = false;
    for (const auto& entry : consist.entries) {
        auto wagon = std::make_unique<formats::WagonDefinition>(formats::load_wagon(vfs, entry.folder, entry.name, entry.engine));
        if (!wagon->ok()) return entry.name + ": " + wagon->error;
        passenger = passenger || wagon->kind == formats::WagonKind::passenger;
        setups.push_back({.definition = wagon.get(), .flipped = entry.flipped,
                          .handbrake = entry.handbrake ? *entry.handbrake / 100.0f : 0.0f});
        runner.stock.push_back(std::move(wagon));
    }
    runner.freight = !passenger;
    runner.train = std::make_unique<simulation::Train>(std::move(setups));
    return {};
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: openrts-path-check <installation root> <route> <consist> [path...] [--seconds N] [--together] [--following M]\n");
        return 2;
    }
    const std::filesystem::path root = argv[1];
    const std::string route_name = argv[2];
    const std::string consist = argv[3];
    std::vector<std::string> wanted;
    double seconds = 1800.0;
    bool together = false;
    double following = 0.0;
    for (int i = 4; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--seconds" && i + 1 < argc) {
            seconds = std::atof(argv[++i]);
        } else if (arg == "--together") {
            together = true;
        } else if (arg == "--following" && i + 1 < argc) {
            following = std::atof(argv[++i]);
        } else if (arg == "--junction" && i + 1 < argc) {
            ++i;
        } else if (arg == "--replan") {
        } else {
            wanted.push_back(arg);
        }
    }
    core::Vfs vfs;
    core::mount_installation(vfs, root, core::load_installation_config(root));
    const auto route = *core::VirtualPath::parse("ROUTES/" + route_name);
    std::string error;
    auto network = simulation::TrackNetwork::load(vfs, route, &error);
    if (!network) {
        std::printf("network: %s\n", error.c_str());
        return 1;
    }
    // Signals as the viewer loads them.
    auto config = std::make_shared<formats::SignalConfig>(formats::load_signal_config(vfs, route));
    auto scripts = std::make_shared<formats::SignalScriptFile>(formats::load_signal_scripts(vfs, route, *config));
    std::vector<formats::WorldObjectPlacement> placements;
    if (const auto world = route.resolve(std::string_view("WORLD"))) {
        for (const auto& entry : vfs.list(*world)) {
            if (entry.directory) continue;
            const auto tile = formats::load_world_tile(vfs, entry.path);
            for (const auto& object : tile.objects) {
                if (object.kind == formats::WorldObjectKind::signal) placements.push_back(object);
            }
        }
    }
    simulation::SignalSystem signals(*network, config, scripts, std::move(placements));
    simulation::Dispatcher dispatcher(*network);
    signals.set_authority([&](const simulation::Train& train) { return dispatcher.authority_ahead(train.id()); });
    const auto definition = formats::load_route_definition(vfs, route);
    simulation::SpeedLimits limits(*network, definition.ok() ? definition.speed_limit : 0.0f);
    std::printf("%s: %zu signal heads, %zu limit posts, route limit %.0f km/h\n", route_name.c_str(), signals.heads().size(),
                limits.posts().size(), limits.route_limit() * 3.6f);
    {
        // Signal names of the route database against the TDB signal items,
        // as RTS matches them (0x4903DC: same tile, x/y/z within 0.2 m).
        const auto database = formats::load_route_database(vfs, route);
        std::size_t named = 0, exact = 0;
        for (const auto& unit : database.signal_units) named += unit.name.empty() ? 0 : 1;
        const auto all = formats::match_signal_units(network->database(), database);
        const auto strict = formats::match_signal_units(network->database(), database, 0.0);
        exact = strict.size();
        const std::size_t matched = all.size();
        {
            // How far the moved ones are from their unit.
            std::vector<double> moved;
            for (const auto& [item, u] : all) {
                if (strict.contains(item)) continue;
                const auto& at = network->database().items[item];
                const auto& p = database.signal_units[u].position;
                const double dx = (at.tile_x - p.tile_x) * 2048.0 + at.x - p.x, dz = (at.tile_z - p.tile_z) * 2048.0 + at.z - p.z;
                moved.push_back(std::sqrt(dx * dx + dz * dz + (at.y - p.y) * (at.y - p.y)));
            }
            std::sort(moved.begin(), moved.end());
            if (!moved.empty()) {
                std::printf("  moved signals: %zu, distance median %.1f m, 90%% %.1f m, max %.1f m\n", moved.size(), moved[moved.size() / 2],
                            moved[moved.size() * 9 / 10], moved.back());
            }
        }
        std::printf("  route database: %zu signal units, %zu named, %zu at their TDB signal as RTS matches, %zu with the "
                    "nearest within 30 m\n",
                    database.signal_units.size(), named, exact, matched);
    }

    for (int i = 4; i + 1 < argc; ++i) {
        if (std::string(argv[i]) != "--junction") continue;
        const auto n = static_cast<std::uint32_t>(std::atoi(argv[i + 1]));
        const auto* node = network->node(n);
        const auto* state = network->junction(n);
        if (!node || !state) {
            std::printf("no junction %u\n", n);
            return 1;
        }
        std::printf("junction %u: %zu branches, MainRoute %u, selected %u\n", n, node->branch_count(), state->main_route, state->selected);
        const auto& items = network->database().items;
        for (std::size_t k = 0; k < node->pins.size(); ++k) {
            const auto& pin = node->pins[k];
            const auto* track = network->node(pin.node);
            std::string signs;
            if (track) {
                for (const auto& entry : track->items) {
                    if (entry.item < items.size() && items[entry.item].kind == formats::TrackItemKind::signal) {
                        signs += items[entry.item].signal_direction == 0 ? " along@" : " against@";
                        signs += std::to_string(static_cast<int>(entry.offset));
                    }
                }
            }
            std::printf("  %s pin -> node %u (%s), %.0f m, signals:%s\n", k == 0 ? "trunk " : "branch", pin.node,
                        pin.direction == 1 ? "its start" : "its end", track ? track->length : 0.0, signs.c_str());
        }
        return 0;
    }
    auto paths = formats::load_train_paths(vfs, route);
    std::erase_if(paths, [](const formats::TrainPath& path) { return !path.ok(); });
    // Paths by file name, or by number (#1 is the first) where the console
    // cannot pass the name.
    for (std::size_t i = 0; i < paths.size(); ++i) paths[i].id = "#" + std::to_string(i + 1);
    std::erase_if(paths, [&](const formats::TrainPath& path) {
        return !wanted.empty() && std::find(wanted.begin(), wanted.end(), path.file) == wanted.end()
            && std::find(wanted.begin(), wanted.end(), path.id) == wanted.end();
    });
    if (paths.empty()) {
        std::printf("no paths\n");
        return 1;
    }
    // --replan: each path planned again from its start and end only (as
    // Track control plans a new path) and compared with the .pat, junction
    // by junction: where the planned way leaves the author's.
    if (std::find(argv + 4, argv + argc, std::string("--replan")) != argv + argc) {
        std::size_t agree_total = 0, pat_total = 0;
        for (const auto& path : paths) {
            auto plan = simulation::plan_from_path(*network, path, &error);
            if (!plan) continue;
            const auto original = simulation::build_route(*network, *plan);
            simulation::PathPlan bare = *plan;
            bare.points = {plan->points.back()};
            bare.points.front().through.clear();
            bare.points.front().branches.clear();
            const auto planned = simulation::build_route(*network, bare);
            if (!original.ok() || !planned.ok()) {
                std::printf("  %s: %s\n", path.id.c_str(), planned.error.c_str());
                continue;
            }
            const auto& a = original.legs.front().junctions;
            const auto& b = planned.legs.front().junctions;
            {
                std::multiset<std::tuple<std::uint32_t, std::uint32_t, bool>> pat;
                for (const auto& j : a) pat.insert({j.junction, j.branch, j.facing});
                for (const auto& j : b) {
                    if (const auto it = pat.find({j.junction, j.branch, j.facing}); it != pat.end()) {
                        ++agree_total;
                        pat.erase(it);
                    }
                }
                pat_total += a.size();
            }
            std::size_t same = 0;
            while (same < a.size() && same < b.size() && a[same].junction == b[same].junction && a[same].branch == b[same].branch) ++same;
            std::printf("  %s: %.0f m by the .pat, %.0f m planned; same for %zu of %zu junctions", path.id.c_str(),
                        original.legs.front().length(), planned.legs.front().length(), same, a.size());
            if (same < a.size() && same < b.size()) {
                std::printf("; first difference: .pat junction %u branch %u, planned junction %u branch %u at %.0f m", a[same].junction,
                            a[same].branch, b[same].junction, b[same].branch, b[same].distance);
            }
            std::printf("\n");
        }
        std::printf("  junction passes as the authors chose them: %zu of %zu\n", agree_total, pat_total);
        return 0;
    }

    const auto run = [&](std::vector<const formats::TrainPath*> group) {
        std::vector<std::unique_ptr<Runner>> runners;
        for (const auto* path : group) {
            // With --following a second train runs the same path, starting
            // `following` metres ahead of the first.
            for (int copy = 0; copy < (following > 0.0 ? 2 : 1); ++copy) {
                auto runner = std::make_unique<Runner>();
                runner->path_name = path->id + (copy == 1 ? " ahead" : "") + " " + path->file;
                if (auto e = load_train(vfs, consist, *runner); !e.empty()) {
                    std::printf("  %s: %s\n", path->file.c_str(), e.c_str());
                    continue;
                }
                auto plan = simulation::plan_from_path(*network, *path, &error);
                if (!plan) {
                    std::printf("  %s: %s\n", path->file.c_str(), error.c_str());
                    continue;
                }
                auto built = simulation::build_route(*network, *plan);
                if (!built.ok()) {
                    std::printf("  %s: %s\n", path->file.c_str(), built.error.c_str());
                    continue;
                }
                // The rear at the path start, the front a train length along it.
                const auto& leg = built.legs.front();
                const double ahead = copy == 1 ? following : 0.0;
                const auto front = leg.at((std::min)(runner->train->length() + ahead, leg.length()));
                if (!front || !runner->train->place(*network, front->node, front->offset, front->forward, &error)) {
                    std::printf("  %s: cannot place: %s\n", path->file.c_str(), error.c_str());
                    continue;
                }
                runner->driver = std::make_unique<simulation::AutoDriver>(simulation::attach_keyboard_driver(*runner->train));
                dispatcher.assign(runner->train->id(), std::move(*plan), std::move(built));
                runners.push_back(std::move(runner));
            }
        }
        std::vector<const simulation::Train*> trains;
        std::vector<simulation::Train*> moving;
        for (const auto& r : runners) {
            trains.push_back(r->train.get());
            moving.push_back(r->train.get());
        }
        dispatcher.update(trains);
        signals.update(trains, simulation::train_step);
        std::size_t contacts = 0;
        const int steps = static_cast<int>(seconds / simulation::train_step);
        for (int step = 0; step < steps; ++step) {
            bool all_done = true;
            for (auto& r : runners) {
                auto& train = *r->train;
                const auto* progress = dispatcher.progress(train.id());
                const auto orders = simulation::path_orders(train, dispatcher, &signals, limits, simulation::category_from_cars(r->freight), r->wait_left);
                if (progress && progress->on_route && !progress->finished && !train.derailed()) {
                    all_done = false;
                    const double speed = std::abs(train.speed());
                    r->top_speed = (std::max)(r->top_speed, speed);
                    const auto track = limits.evaluate(train, simulation::category_from_cars(r->freight));
                    if (track.current > 0.0f) r->over_limit = (std::max)(r->over_limit, speed - track.current);
                }
                r->driver->step(simulation::train_step, train, orders);
                {
                    const auto next = signals.ahead(train);
                    char line[160];
                    std::snprintf(line, sizeof line, "next signal %u at %.1f m: %s; speed %.1f km/h, stop in %.0f m, target %.0f km/h",
                                  next.object, next.distance, next.valid ? formats::to_string(next.aspect) : "-",
                                  std::abs(train.speed()) * 3.6, orders.stop_distance, r->driver->target_speed() * 3.6);
                    r->history.push_back(line);
                    if (r->history.size() > 12) r->history.erase(r->history.begin());
                }
            }
            contacts += simulation::resolve_contacts(moving).size();
            for (auto& r : runners) {
                const auto before = r->train->front_end();
                r->train->step();
                const double speed = std::abs(r->train->speed());
                r->travelled += speed * simulation::train_step;
                if (r->moving && speed < 0.05) ++r->stops;
                r->moving = speed > 0.3 || (r->moving && speed >= 0.05);
                (void)before;
                for (const auto& event : r->train->take_events()) {
                    if (event.kind != simulation::TrainEventKind::derailment) continue;
                    const auto& car = r->train->cars()[event.car];
                    const auto* p = dispatcher.progress(r->train->id());
                    std::printf("    derailment: car %u, %.0f km/h, curve radius %.0f m, kind %d, at %.0f m of the leg; m v^2/R %.0f kN vs DerailRailForce %.0f kN\n",
                                event.car + 1, std::abs(car.velocity) * 3.6, car.radius, static_cast<int>(car.derail), p ? p->head : -1.0,
                                car.radius > 0 ? car.definition->mass * car.velocity * car.velocity / car.radius / 1000.0 : 0.0,
                                car.definition->derail_rail_force / 1000.0);
                }
            }
            dispatcher.update(trains);
            signals.update(trains, simulation::train_step);
            for (const auto& pass : signals.take_passes()) {
                if (!pass.at_danger()) continue;
                for (auto& r : runners) {
                    if (r->train->id() != pass.train) continue;
                    ++r->passed_at_danger;
                    const auto* p = dispatcher.progress(pass.train);
                    const auto& object = signals.objects()[pass.object];
                    std::printf("    %s passed signal object %u (UiD %u) at %s, %.0f km/h, at %.0f m, way reserved to %.0f m\n",
                                r->path_name.c_str(), pass.object, object.placement.uid, formats::to_string(pass.aspect),
                                std::abs(r->train->speed()) * 3.6, p ? p->head : -1.0, p ? p->authority : -1.0);
                    for (const auto h : object.heads) {
                        const auto& head = signals.heads()[h];
                        std::printf("      head %u %s: now %s, block %d, route %d\n", h, head.type ? head.type->name.c_str() : "?",
                                    formats::to_string(head.aspect()), signals.block_state(h), signals.route_set(h));
                    }
                    for (const auto& line : r->history) std::printf("      %s\n", line.c_str());
                    // The train's own extent against its front.
                    if (const auto rear = r->train->rear_end(); rear && r->train->front_end()) {
                        auto walker = *rear;
                        walker.move(r->train->length());
                        const auto front = r->train->front_end()->pose();
                        std::printf("      rear + length lands %.2f m from the front\n", simulation::distance(walker.pose(), front));
                    }
                }
            }
            if (all_done) break;
        }
        for (const auto& r : runners) {
            const auto* progress = dispatcher.progress(r->train->id());
            const auto* route_now = dispatcher.route(r->train->id());
            std::string where = "-";
            if (progress && route_now) {
                char text[200];
                std::snprintf(text, sizeof text, "leg %zu/%zu at %.0f of %.0f m%s", progress->leg + 1, route_now->legs.size(),
                              progress->head, route_now->legs[progress->leg].length(), progress->finished ? ", ARRIVED" : "");
                where = text;
                if (!progress->finished && progress->blocked_at) {
                    where += ", waits at junction " + std::to_string(*progress->blocked_at);
                }
            }
            if (const auto km = limits.ordinate(*r->train)) std::printf("    at km %.3f\n", *km);
            std::printf("  %-14s %s; ran %.0f m, top %.0f km/h, worst over the limit %+.1f km/h, %zu stops, %zu signals passed at danger%s\n",
                        r->path_name.c_str(), where.c_str(), r->travelled, r->top_speed * 3.6, r->over_limit * 3.6, r->stops,
                        r->passed_at_danger, r->train->derailed() ? ", DERAILED" : "");
            dispatcher.clear(r->train->id());
            r->train->vacate();
        }
        if (contacts > 0) std::printf("  trains touched %zu times\n", contacts);
    };

    if (together) {
        std::vector<const formats::TrainPath*> group;
        for (const auto& path : paths) group.push_back(&path);
        run(group);
    } else {
        for (const auto& path : paths) run({&path});
    }
    return 0;
}
