// openrts-net-dump: prints an RTS startup snapshot (RTS/multdata, the file
// the client saves at its login) package by package and as a world, and
// with --route checks it against the route: every junction, signal head,
// train position and car file (docs/net.md).
//
//   openrts-net-dump <snapshot> [--packages] [--root <game folder> --route <name>]
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <openrts/core/installation.hpp>
#include <openrts/core/vfs.hpp>
#include <openrts/formats/route_world.hpp>
#include <openrts/formats/track_database.hpp>
#include <openrts/net/packages.hpp>
#include <openrts/net/snapshot.hpp>
#include <openrts/net/wire.hpp>
#include <openrts/net/world.hpp>
#include <openrts/simulation/track_network.hpp>

namespace {

using namespace openrts;

const char* kind_name(const std::uint16_t kind) {
    switch (kind) {
    case 0: return "STATIC";
    case 1: return "PLAYER";
    case 2: return "TRAFFIC";
    case 3: return "LOOSE";
    default: return "?";
    }
}

const char* category_name(const std::uint16_t subtype) {
    switch (subtype) {
    case 0: return "Прочий";
    case 1: return "Пассажирский";
    case 2: return "Грузовой";
    default: return "?";
    }
}

const char* state_name(const std::uint8_t state) {
    switch (state) {
    case 0: return "Холодный";
    case 1: return "Включен";
    case 2: return "Движется";
    default: return "?";
    }
}

std::string clock(const float seconds) {
    const auto total = static_cast<long>(std::lround(seconds));
    char text[32];
    std::snprintf(text, sizeof text, "%02ld:%02ld:%02ld", total / 3600 % 24, total / 60 % 60, total % 60);
    return text;
}

void print_packages(const net::ByteView data, const std::uint16_t parent, const int depth) {
    const auto list = net::split_frames(data);
    std::map<std::uint16_t, std::size_t> repeats;
    for (const auto& f : list.frames) {
        const auto name = net::package_name(f.id, parent);
        const bool container = f.id == net::id(net::Package::junctions) || f.id == net::id(net::Package::signals);
        if (++repeats[f.id] <= 3 || depth == 0) {
            std::printf("%*s0x%04X %-20s %zu bytes\n", depth * 2, "", f.id, name.empty() ? "?" : std::string(name).c_str(),
                        f.body.size() + 6);
        } else if (repeats[f.id] == 4) {
            std::printf("%*s...\n", depth * 2, "");
        }
        if (container) print_packages(f.body, f.id, depth + 1);
    }
    if (!list.error.empty()) std::printf("%*s%s\n", depth * 2, "", list.error.c_str());
}

void print_world(const net::Snapshot& s) {
    std::printf("time: %s\n", s.time ? clock(*s.time).c_str() : "none");
    const auto locked = std::count_if(s.junctions.begin(), s.junctions.end(), [](const auto& j) { return j.lock != 0; });
    std::printf("junctions: %zu (%td locked)\n", s.junctions.size(), locked);
    std::map<int, std::size_t> aspects;
    for (const auto& h : s.signals) ++aspects[static_cast<int>(h.state)];
    std::printf("signal heads: %zu; by aspect:", s.signals.size());
    for (const auto& [aspect, count] : aspects) std::printf(" %d:%zu", aspect, count);
    std::printf("\n");
    for (const auto& x : s.substations) {
        std::printf("substation %u: %.0f V%s (flags %u)\n", x.id, x.voltage, (x.flags & 1u) ? ", switched off" : "", x.flags);
    }
    for (const auto& p : s.phases) {
        std::printf("substation %u phases:", p.id);
        for (const auto v : p.voltages) std::printf(" %.0f", v);
        std::printf("%s\n", p.voltages.empty() ? " none" : "");
    }
    for (const auto& x : s.sections) std::printf("section %u: source %d\n", x.section, x.source);
    for (const auto& x : s.pieces) {
        std::printf("piece supply: node %u piece %u source %d keep %u\n", x.node, x.piece, x.source, x.keep);
    }
    for (const auto& t : s.trains) {
        std::printf("train %u \"%s\": %s, %s, %s, top %.0f km/h, %zu cars%s%s\n", t.id, net::train_name(t).c_str(),
                    kind_name(t.kind), category_name(t.subtype), state_name(t.state), t.top_speed * 3.6f, t.cars.size(),
                    t.stands ? ", stands" : "", t.mine() ? ", mine" : "");
        if (t.pos) {
            std::printf("  rear end: node %u offset %.2f %s, %.2f m/s; front %.2f m ahead%s\n", t.pos->node, t.pos->offset,
                        t.pos->forward() ? "along" : "against", t.pos->speed, net::front_distance(t),
                        t.forced ? " (forced)" : "");
        }
        for (const auto& car : t.cars) {
            std::printf("  %s uid %u%s%s\n", net::to_utf8(car.file).c_str(), car.uid, car.flipped() ? " flipped" : "",
                        (car.flags & 0x300u) ? " chocked" : "");
        }
        if (t.names && !(*t.names)[1].empty()) std::printf("  notes: %s\n", net::to_utf8((*t.names)[1]).c_str());
        if (t.brakes) {
            std::printf("  brakes: pipe %.2f, main %.2f, auxiliary %.2f, cylinders %.2f\n", (*t.brakes)[0], (*t.brakes)[1],
                        (*t.brakes)[2], (*t.brakes)[3]);
        }
        if (t.route_left) std::printf("  route left: %.1f m\n", t.route_left->first);
        if (!t.cargo.empty()) std::printf("  cargo: %zu\n", t.cargo.size());
        if (t.derail) std::printf("  derailed cars\n");
    }
    for (const auto& p : s.users) std::printf("user %u \"%s\": train %u\n", p.user, p.name_utf8().c_str(), p.train);
    for (const auto& p : s.players) {
        std::printf("player %u \"%s\": train %u, at %.1f %.1f %.1f\n", p.user, p.name_utf8().c_str(), p.train,
                    p.position[0], p.position[1], p.position[2]);
    }
    std::map<std::uint16_t, std::size_t> other;
    for (const auto& raw : s.other) ++other[raw.id];
    for (const auto& [id, count] : other) {
        const auto name = net::package_name(id);
        std::printf("other: 0x%04X %s x%zu\n", id, name.empty() ? "?" : std::string(name).c_str(), count);
    }
    std::printf("%s\n", s.complete ? "ends with 0x4608" : "no 0x4608: cut short");
}

// Checks against the route. Returns the number of problems.
int check_route(const net::Snapshot& s, const std::filesystem::path& root, const std::string& route_name) {
    core::Vfs vfs;
    core::mount_installation(vfs, root, core::load_installation_config(root));
    vfs.build_index();
    const auto route = core::VirtualPath::parse("ROUTES/" + route_name);
    if (!route) {
        std::printf("bad route name %s\n", route_name.c_str());
        return 1;
    }
    std::string error;
    auto network = simulation::TrackNetwork::load(vfs, *route, &error);
    if (!network) {
        std::printf("route %s: %s\n", route_name.c_str(), error.c_str());
        return 1;
    }
    int problems = 0;
    const auto report = [&](const std::vector<std::string>& list, const char* what, const std::size_t applied) {
        std::printf("route: %zu %s fit", applied, what);
        if (!list.empty()) std::printf(", %zu do not", list.size());
        std::printf("\n");
        for (std::size_t i = 0; i < list.size() && i < 10; ++i) std::printf("  %s\n", list[i].c_str());
        problems += static_cast<int>(list.size());
    };

    const auto junctions = net::apply_junctions(*network, s);
    report(junctions.problems, "junctions", junctions.applied);

    // Signal heads: a TDB signal item that a world signal shows.
    const auto& tdb = network->database();
    std::set<std::uint32_t> shown;
    if (const auto world = route->resolve(std::string_view("WORLD"))) {
        for (const auto& entry : vfs.list(*world)) {
            if (entry.directory) continue;
            const auto file = std::string(entry.path.filename());
            if (file.empty() || (file.back() != 'w' && file.back() != 'W')) continue;
            for (const auto& object : formats::load_world_tile(vfs, entry.path).objects) {
                if (object.kind != formats::WorldObjectKind::signal) continue;
                for (const auto& unit : object.signal_units) shown.insert(unit.track_item);
            }
        }
    }
    std::vector<std::string> heads;
    std::size_t heads_ok = 0;
    for (const auto& h : s.signals) {
        if (h.item >= tdb.items.size() || tdb.items[h.item].kind != formats::TrackItemKind::signal) {
            heads.push_back("item " + std::to_string(h.item) + " is no signal item");
        } else if (!shown.contains(h.item)) {
            heads.push_back("item " + std::to_string(h.item) + " has no world signal");
        } else {
            ++heads_ok;
        }
    }
    report(heads, "signal heads", heads_ok);

    std::vector<std::string> trains;
    std::size_t trains_ok = 0;
    for (const auto& t : s.trains) {
        std::string why;
        if (const auto front = net::train_front(*network, t, &why)) {
            std::printf("route: train %u front at node %u offset %.2f %s\n", t.id, front->node, front->offset,
                        front->forward ? "along" : "against");
            ++trains_ok;
        } else {
            trains.push_back("train " + std::to_string(t.id) + ": " + why);
        }
    }
    report(trains, "train positions", trains_ok);

    std::vector<std::string> cars;
    std::size_t cars_ok = 0;
    for (const auto& t : s.trains) {
        for (const auto& car : t.cars) {
            const auto file = net::car_file(car);
            const auto path = core::VirtualPath::parse("TRAINS/TRAINSET/" + file.folder + "/" + file.name +
                                                       (file.engine ? ".eng" : ".wag"));
            if (path && vfs.exists(*path)) {
                ++cars_ok;
            } else {
                cars.push_back(net::to_utf8(car.file) + " is not installed");
            }
        }
    }
    report(cars, "car files", cars_ok);

    std::vector<std::string> pieces;
    std::size_t pieces_ok = 0;
    for (const auto& p : s.pieces) {
        const auto* node = network->node(p.node);
        if (node && node->kind == formats::TrackNodeKind::vector) {
            ++pieces_ok;
        } else {
            pieces.push_back("piece supply node " + std::to_string(p.node) + " is no vector node");
        }
    }
    report(pieces, "piece supplies", pieces_ok);
    return problems;
}

} // namespace

int main(const int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: openrts-net-dump <snapshot> [--packages] [--root <game folder> --route <name>]\n");
        return 2;
    }
    const std::filesystem::path file = argv[1];
    bool packages = false;
    std::filesystem::path root;
    std::string route;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--packages") {
            packages = true;
        } else if (arg == "--root" && i + 1 < argc) {
            root = argv[++i];
        } else if (arg == "--route" && i + 1 < argc) {
            route = argv[++i];
        } else {
            std::fprintf(stderr, "unknown option %s\n", arg.c_str());
            return 2;
        }
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot read %s\n", file.string().c_str());
        return 2;
    }
    const net::Bytes bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::printf("%s: %zu bytes\n", file.string().c_str(), bytes.size());
    if (packages) print_packages(bytes, 0, 0);
    const auto read = net::read_snapshot(bytes);
    for (const auto& problem : read.problems) std::printf("problem: %s\n", problem.c_str());
    print_world(read.snapshot);
    const bool same = net::write_snapshot(read.snapshot) == bytes;
    std::printf("written back: %s\n", same ? "the same bytes" : "DIFFERENT bytes");
    int problems = static_cast<int>(read.problems.size()) + (same ? 0 : 1);
    if (!route.empty()) {
        if (root.empty()) root = file.parent_path().parent_path();
        problems += check_route(read.snapshot, root, route);
    }
    return problems == 0 ? 0 : 1;
}
