// openrts-track-check: loads the track network of installed routes and
// reports what docs/track.md asks for: parse results, strict topology checks,
// traversal on cyclic networks, and measured (not enforced) geometry joints.
//
//   openrts-track-check <installation root> [route...]

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <numbers>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <openrts/core/installation.hpp>
#include <openrts/core/vfs.hpp>
#include <openrts/formats/route_world.hpp>
#include <openrts/formats/shape_binary.hpp>
#include <openrts/formats/shape_file.hpp>
#include <openrts/simulation/track_network.hpp>
#include <openrts/simulation/track_traveller.hpp>
#include <openrts/formats/train_path.hpp>
#include <openrts/simulation/train_route.hpp>

namespace {

namespace fs = std::filesystem;
using namespace openrts;
using simulation::TrackNetwork;

int failures = 0;

// An openRTS bug: semantics broken on data that is fine.
void fail(const std::string& route, const std::string& what) {
    ++failures;
    std::cout << "  FAIL " << route << ": " << what << '\n';
}

// A defect of the route data (reported; RTS copes with it too).
void data_issue(const std::string& what) {
    std::cout << "  data: " << what << '\n';
}

// Every pin is answered by a pin of the linked node pointing back. Pins to
// missing nodes are dangling links in the data (treated as track ends).
std::size_t unanswered_pins(const TrackNetwork& network, std::size_t& dangling) {
    std::size_t missing = 0;
    dangling = 0;
    for (std::uint32_t i = 1; i < network.node_count(); ++i) {
        const auto* node = network.node(i);
        if (!node) continue;
        for (const auto& pin : node->pins) {
            const auto* other = network.node(pin.node);
            if (!other) {
                ++dangling;
            } else if (std::none_of(other->pins.begin(), other->pins.end(), [&](const auto& back) { return back.node == i; })) {
                ++missing;
            }
        }
    }
    return missing;
}

// Strict: a single pass through each vector node from one boundary to the
// other visits each of its sections exactly once, in order, and covers the
// node length.
std::size_t bad_single_passes(TrackNetwork& network) {
    std::size_t bad = 0;
    for (std::uint32_t i = 1; i < network.node_count(); ++i) {
        const auto* node = network.node(i);
        if (!node || node->kind != formats::TrackNodeKind::vector || node->length <= 0.0) continue;
        for (const bool forward : {true, false}) {
            simulation::TrackTraveller traveller(network, i, forward ? 0.0 : node->length, forward);
            std::vector<std::size_t> visited{traveller.section()};
            double moved = 0.0;
            // Step through the node in small increments without leaving it.
            const double step = 0.25;
            while (moved + step < node->length) {
                traveller.move(step);
                moved += step;
                if (traveller.section() != visited.back()) visited.push_back(traveller.section());
            }
            const auto count = node->sections.size();
            std::size_t zero_length = 0;
            for (const auto& section : node->sections) zero_length += section.length < step ? 1 : 0;
            bool ok = traveller.node() == i;
            if (zero_length == 0) {
                ok = ok && visited.size() == count;
                for (std::size_t k = 0; ok && k < visited.size(); ++k) {
                    ok = visited[k] == (forward ? k : count - 1 - k);
                }
            }
            if (!ok) ++bad;
        }
    }
    return bad;
}

// Connected components of the undirected node graph.
std::pair<std::size_t, std::size_t> components(const TrackNetwork& network) {
    std::vector<std::uint32_t> parent(network.node_count());
    for (std::uint32_t i = 0; i < parent.size(); ++i) parent[i] = i;
    const auto find = [&](std::uint32_t x) {
        while (parent[x] != x) x = parent[x] = parent[parent[x]];
        return x;
    };
    for (std::uint32_t i = 1; i < network.node_count(); ++i) {
        const auto* node = network.node(i);
        if (!node) continue;
        for (const auto& pin : node->pins) {
            if (network.node(pin.node)) parent[find(i)] = find(pin.node);
        }
    }
    std::map<std::uint32_t, std::size_t> sizes;
    for (std::uint32_t i = 1; i < network.node_count(); ++i) {
        if (network.node(i)) ++sizes[find(i)];
    }
    std::size_t largest = 0;
    for (const auto& [_, size] : sizes) largest = (std::max)(largest, size);
    return {sizes.size(), largest};
}

// Directed exploration of (node, direction) states through every branch,
// started from every vector node; must terminate on cyclic graphs.
std::size_t explore_states(const TrackNetwork& network, std::size_t& revisits) {
    std::set<std::pair<std::uint32_t, bool>> visited;
    std::vector<std::pair<std::uint32_t, bool>> stack;
    for (std::uint32_t i = 1; i < network.node_count(); ++i) {
        const auto* node = network.node(i);
        if (node && node->kind == formats::TrackNodeKind::vector) {
            stack.emplace_back(i, true);
            stack.emplace_back(i, false);
        }
    }
    revisits = 0;
    while (!stack.empty()) {
        const auto state = stack.back();
        stack.pop_back();
        if (!visited.insert(state).second) {
            ++revisits;
            continue;
        }
        const auto* node = network.node(state.first);
        if (node->pins.size() < 2) continue;
        std::vector<formats::TrackPin> frontier{node->pins[state.second ? 1 : 0]};
        std::set<std::uint32_t> junctions_seen;
        while (!frontier.empty()) {
            const auto current = frontier.back();
            frontier.pop_back();
            const auto* target = network.node(current.node);
            if (!target) continue;
            if (target->kind == formats::TrackNodeKind::vector) {
                stack.emplace_back(target->index, current.direction == 1);
            } else if (target->kind == formats::TrackNodeKind::junction && junctions_seen.insert(target->index).second) {
                if (current.direction == 1) {
                    for (std::size_t k = 1; k < target->pins.size(); ++k) frontier.push_back(target->pins[k]);
                } else {
                    frontier.push_back(target->pins[0]);
                }
            }
        }
    }
    return visited.size();
}

struct WorldTrackObjects {
    std::map<std::tuple<std::int32_t, std::int32_t, std::uint32_t>, formats::RoutePosition> objects;
};

WorldTrackObjects load_track_objects(const core::Vfs& vfs, const core::VirtualPath& route) {
    WorldTrackObjects result;
    const auto world = route.resolve(std::string_view("WORLD"));
    if (!world) return result;
    for (const auto& entry : vfs.list(*world)) {
        if (entry.directory || core::fold_case(entry.path.extension()) != ".w") continue;
        const auto tile = formats::load_world_tile(vfs, entry.path);
        if (!tile.ok()) continue;
        for (const auto& object : tile.objects) {
            if (object.kind != formats::WorldObjectKind::track) continue;
            result.objects[{tile.tile_x, tile.tile_z, object.uid}] = object.position;
        }
    }
    return result;
}

// Measured: the blade animation of every switch shape the route uses —
// frame count, key frames, and whether the last key returns to the first
// (then the thrown position is an inner key, not the last one).
void survey_switch_animations(const core::Vfs& vfs, const TrackNetwork& network) {
    std::set<std::string> files;
    for (std::uint32_t i = 1; i < network.node_count(); ++i) {
        const auto* node = network.node(i);
        if (node && node->kind == formats::TrackNodeKind::junction && node->shape) files.insert(node->shape->file_name);
    }
    std::map<std::string, std::size_t> patterns;
    std::size_t missing = 0;
    for (const auto& file : files) {
        const auto path = core::VirtualPath::parse("GLOBAL/SHAPES/" + file);
        const auto body = path ? formats::load_binary_shape_body(vfs, *path) : formats::ShapeInflateResult{};
        if (!body.ok()) {
            ++missing;
            continue;
        }
        const auto model = formats::parse_binary_shape_model(*body.body);
        std::string pattern = "no animation";
        if (!model.animations.empty()) {
            const auto& animation = model.animations.front();
            std::size_t keys = 0;
            bool returns = false;
            std::size_t unbound = 0;
            for (const auto& node : animation.nodes) {
                keys = (std::max)(keys, node.rotations.size() + node.positions.size());
                unbound += node.matrix_index < 0 ? 1 : 0;
                if (node.rotations.size() >= 3) {
                    const auto& a = node.rotations.front().value;
                    const auto& b = node.rotations.back().value;
                    returns = returns || (std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]) < 1e-4f);
                }
            }
            pattern = std::to_string(animation.frame_count) + " frames, " + std::to_string(animation.nodes.size())
                + " animated nodes" + (returns ? ", last key = first" : "") + (unbound ? ", unbound nodes" : "");
        }
        ++patterns[pattern];
    }
    std::cout << "  switch shapes: " << files.size() << " (" << missing << " not loadable)\n";
    for (const auto& [pattern, count] : patterns) std::cout << "    " << count << " x " << pattern << '\n';
}

// Every .pat of the route resolved as a plan and a route (docs/paths.md).
void check_paths(const core::Vfs& vfs, const core::VirtualPath& route, const TrackNetwork& network) {
    const auto paths = formats::load_train_paths(vfs, route);
    std::size_t total = 0, resolved = 0, legs = 0, junctions = 0;
    std::vector<std::string> problems;
    for (const auto& path : paths) {
        if (!path.ok()) continue;
        ++total;
        std::string error;
        const auto plan = simulation::plan_from_path(network, path, &error);
        if (!plan) {
            problems.push_back(path.file + ": " + error);
            continue;
        }
        const auto built = simulation::build_route(network, *plan);
        if (!built.ok()) {
            problems.push_back(path.file + ": " + built.error);
            continue;
        }
        ++resolved;
        legs += built.legs.size();
        for (const auto& leg : built.legs) junctions += leg.junctions.size();
    }
    std::printf("  paths: %zu of %zu resolve (%zu legs, %zu junction passes)\n", resolved, total, legs, junctions);
    for (std::size_t i = 0; i < problems.size() && i < 8; ++i) std::printf("    %s\n", problems[i].c_str());
}

// Which way speed posts face (docs/paths.md). Four readings of the angle are
// tested: the sign's facing as a heading (angle, or angle + pi), or as a
// mathematical angle in the ground plane (pi/2 - angle, or that + pi; the
// last is the one simulation/speed_limits.cpp uses). A post is for travel
// against its facing. Two tests: a warning board comes before the limit
// post of the speed it shows, and a post that ends a restriction comes after
// a limit post for the same direction.
void check_speed_posts(const TrackNetwork& network) {
    std::string summary;
    std::size_t total_posts = 0;
    for (int variant = 0; variant < 4; ++variant) {
    struct Placed {
        std::uint32_t node{};
        double offset{};
        formats::SpeedPost post;
        bool along = true; // by the convention
    };
    std::map<std::uint32_t, Placed> posts;
    const auto& items = network.database().items;
    for (std::uint32_t n = 1; n < network.node_count(); ++n) {
        const auto* node = network.node(n);
        if (!node || node->kind != formats::TrackNodeKind::vector) continue;
        for (const auto& entry : node->items) {
            if (entry.item >= items.size()) continue;
            const auto post = formats::speed_post(items[entry.item]);
            const auto where = network.point(n, entry.offset);
            if (!post || !where) continue;
            double facing = variant < 2 ? post->angle : std::numbers::pi / 2.0 - post->angle;
            if (variant % 2 == 1) facing += std::numbers::pi;
            posts[entry.item] = {n, entry.offset, *post, std::cos(facing - where->heading) < 0.0};
        }
    }
    if (posts.empty()) return;
    const auto speed_posts = [&](const formats::TrackItem& item) { return posts.contains(item.id); };
    std::size_t warning_agree = 0, warning_disagree = 0, resume_agree = 0, resume_disagree = 0;
    for (const auto& [item, placed] : posts) {
        if (placed.post.warning && placed.post.number > 0.0f) {
            // The nearest limit post of that speed either way along the track:
            // trains going that way see the board first.
            double nearest[2] = {1e9, 1e9};
            for (const bool forward : {true, false}) {
                const auto ahead = simulation::look_ahead(network, placed.node, placed.offset, forward, 3000.0, speed_posts);
                for (const auto& found : ahead.items) {
                    const auto& other = posts.at(found.item);
                    if (!other.post.limit || other.post.resume) continue;
                    if (std::abs(other.post.speed * 3.6f - placed.post.number) > 1.0f) continue;
                    nearest[forward ? 0 : 1] = found.distance;
                    break;
                }
            }
            if (nearest[0] < 1e8 || nearest[1] < 1e8) {
                const bool towards = nearest[0] <= nearest[1];
                (placed.along == towards ? warning_agree : warning_disagree)++;
            }
        }
        if (placed.post.limit && placed.post.resume) {
            // The nearest limit post behind, for travel the way this post is for.
            const auto behind = simulation::look_ahead(network, placed.node, placed.offset, !placed.along, 5000.0, speed_posts);
            for (const auto& found : behind.items) {
                const auto& other = posts.at(found.item);
                if (!other.post.limit || other.post.resume) continue;
                // A limit for the other direction often stands at the same
                // place (where that direction's restriction begins).
                if (found.distance < 50.0) continue;
                // Travel on that node towards the resume post is the walk reversed.
                const bool other_for_us = other.along == !found.facing_forward;
                (other_for_us ? resume_agree : resume_disagree)++;
                break;
            }
        }
    }
    static const char* names[] = {"angle", "angle+pi", "pi/2-angle", "3pi/2-angle (used)"};
    char line[160];
    std::snprintf(line, sizeof line, "%s%s %zu/%zu, %zu/%zu", summary.empty() ? "" : "; ", names[variant], warning_agree,
                  warning_agree + warning_disagree, resume_agree, resume_agree + resume_disagree);
    summary += line;
    total_posts = posts.size();
    }
    std::printf("  speed posts: %zu; agreeing (warning boards, restriction ends) by reading: %s\n", total_posts,
                summary.c_str());
}

void check_route(const core::Vfs& vfs, const std::string& name) {
    const auto route = *core::VirtualPath::parse("ROUTES/" + name);
    const auto started = std::chrono::steady_clock::now();
    std::string error;
    auto network = TrackNetwork::load(vfs, route, &error);
    if (!network) {
        std::cout << name << ": not loaded: " << error << '\n';
        return;
    }
    const auto& db = network->database();
    std::map<formats::TrackNodeKind, std::size_t> kinds;
    for (std::uint32_t i = 1; i < network->node_count(); ++i) {
        if (const auto* node = network->node(i)) ++kinds[node->kind];
    }
    std::map<formats::TrackItemKind, std::size_t> items;
    for (const auto& item : db.items) ++items[item.kind];
    const auto load_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

    std::cout << name << ": " << kinds[formats::TrackNodeKind::vector] << " vector, "
              << kinds[formats::TrackNodeKind::junction] << " junction, " << kinds[formats::TrackNodeKind::end]
              << " end nodes; " << db.items.size() << " items (";
    bool first = true;
    for (const auto& [kind, count] : items) {
        std::cout << (first ? "" : ", ") << count << ' ' << formats::to_string(kind);
        first = false;
    }
    std::printf("); %zu track sections, %zu track shapes; %.0f ms\n", network->sections().sections.size(),
                network->sections().shapes.size(), load_ms);

    // Data defects are reported; strict checks fail only on openRTS bugs.
    for (const auto& issue : network->issues()) {
        data_issue("node " + std::to_string(issue.node) + ": " + issue.message);
    }
    std::size_t dangling = 0;
    if (const auto missing = unanswered_pins(*network, dangling); missing > 0) {
        data_issue(std::to_string(missing) + " pins not answered by the linked node");
    }
    if (dangling > 0) data_issue(std::to_string(dangling) + " dangling pins (treated as track ends)");
    if (const auto bad = bad_single_passes(*network); bad > 0) {
        fail(name, std::to_string(bad) + " vector node passes do not visit each section once");
    }

    const auto [component_count, largest] = components(*network);
    std::size_t revisits = 0;
    const auto states = explore_states(*network, revisits);
    std::cout << "  graph: " << component_count << " components (largest " << largest << " nodes); " << states
              << " (node, direction) states explored, " << revisits << " revisits (cycles) handled\n";

    // Random rides: throw junctions at random, drive far in both directions;
    // every ride must terminate.
    std::mt19937 random(12345);
    std::vector<std::uint32_t> vectors;
    for (std::uint32_t i = 1; i < network->node_count(); ++i) {
        const auto* node = network->node(i);
        if (node && node->kind == formats::TrackNodeKind::vector && node->length > 0.0) vectors.push_back(i);
    }
    double total = 0.0;
    std::size_t ends = 0;
    std::size_t junction_passes = 0;
    for (int ride = 0; ride < 200 && !vectors.empty(); ++ride) {
        const auto start = vectors[random() % vectors.size()];
        simulation::TrackTraveller traveller(*network, start, network->node(start)->length / 2, random() % 2 == 0);
        for (int leg = 0; leg < 40; ++leg) {
            for (std::uint32_t j = 1; j < network->node_count(); j += 97) {
                if (const auto* node = network->node(j); node && node->kind == formats::TrackNodeKind::junction) {
                    network->throw_junction(j, static_cast<std::uint32_t>(random() % node->branch_count()));
                }
            }
            const auto result = traveller.move(500.0);
            total += result.moved;
            junction_passes += result.junctions.size();
            if (!result.ok()) {
                ++ends;
                traveller.reverse();
            }
        }
    }
    std::printf("  rides: 200 x 20 km requested, %.0f km driven, %zu junction passes, %zu end stops\n", total / 1000.0,
                junction_passes, ends);

    // Look-ahead from every vector node in both directions must terminate.
    std::size_t loop_stops = 0;
    std::size_t signal_hits = 0;
    for (const auto node : vectors) {
        for (const bool forward : {true, false}) {
            const auto ahead = simulation::look_ahead(
                *network, node, forward ? 0.0 : network->node(node)->length, forward, 20000.0,
                [](const formats::TrackItem& item) { return item.kind == formats::TrackItemKind::signal; }, 1);
            loop_stops += ahead.stop == simulation::LookAheadStop::loop ? 1 : 0;
            signal_hits += ahead.items.empty() ? 0 : 1;
        }
    }
    std::cout << "  look-ahead (20 km, next signal): " << signal_hits << " found, " << loop_stops
              << " ended by the loop guard\n";

    survey_switch_animations(vfs, *network);

    // Measured: geometry joints.
    const auto joints = network->measure_joints(5);
    std::printf("  joints: %zu, median %.4f m, p95 %.4f m, max %.3f m\n", joints.count, joints.median, joints.p95,
                joints.max);
    for (const auto& joint : joints.worst) {
        std::printf("    %s node %u%s%u: %.3f m\n", joint.between_nodes ? "pin" : "section", joint.node,
                    joint.between_nodes ? " -> " : " #", joint.between_nodes ? joint.other : static_cast<std::uint32_t>(joint.section),
                    joint.gap);
    }
    // Gross-error guard (an openRTS bug, not data gaps): most joints apart.
    if (joints.count > 0 && joints.median > 0.05) fail(name, "median joint gap above 5 cm");

    // Measured: sections against the TrackObj they belong to (the shape
    // origin is at one end of one of its sections).
    const auto world = load_track_objects(vfs, route);
    std::vector<double> distances;
    std::map<std::tuple<std::int32_t, std::int32_t, std::uint32_t>, double> nearest;
    for (std::uint32_t i = 1; i < network->node_count(); ++i) {
        const auto* node = network->node(i);
        if (!node || node->kind != formats::TrackNodeKind::vector) continue;
        for (const auto& section : node->sections) {
            const auto& p = *section.placement;
            const std::tuple key{p.world_tile_x, p.world_tile_z, p.world_uid};
            const auto object = world.objects.find(key);
            if (object == world.objects.end()) continue;
            const auto& o = object->second;
            const simulation::TrackPoint origin{.tile_x = o.tile_x, .tile_z = o.tile_z, .x = o.x, .y = o.y, .z = o.z};
            double d = simulation::distance(origin, simulation::section_start(p));
            if (section.definition) {
                d = (std::min)(d, simulation::distance(origin, simulation::evaluate_section(p, *section.definition, section.length)));
            }
            auto [it, inserted] = nearest.try_emplace(key, d);
            if (!inserted) it->second = (std::min)(it->second, d);
        }
    }
    for (const auto& [_, d] : nearest) distances.push_back(d);
    std::sort(distances.begin(), distances.end());
    if (!distances.empty()) {
        std::printf("  TrackObj agreement: %zu objects, nearest section end median %.4f m, p95 %.3f m, max %.2f m\n",
                    distances.size(), distances[distances.size() / 2], distances[distances.size() * 95 / 100],
                    distances.back());
    }
    check_paths(vfs, route, *network);
    check_speed_posts(*network);
}

} // namespace

int main(const int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "usage: openrts-track-check <installation root> [route...]\n";
        return 64;
    }
    const fs::path root = argv[1];
    core::Vfs vfs;
    core::mount_installation(vfs, root, core::load_installation_config(root));

    std::vector<std::string> routes(argv + 2, argv + argc);
    if (routes.empty()) {
        for (const auto& entry : vfs.list(*core::VirtualPath::parse("ROUTES"))) {
            if (entry.directory) routes.emplace_back(entry.path.filename());
        }
    }
    for (const auto& route : routes) check_route(vfs, route);
    std::cout << (failures == 0 ? "track check: all strict checks passed\n" : "track check: FAILURES\n");
    return failures == 0 ? 0 : 1;
}
