#include <openrts/server/server_route.hpp>

#include <algorithm>
#include <cmath>
#include <set>

#include <openrts/core/installation.hpp>
#include <openrts/core/virtual_path.hpp>
#include <openrts/formats/electrification.hpp>
#include <openrts/formats/route_database.hpp>
#include <openrts/formats/route_world.hpp>
#include <openrts/formats/signal_config.hpp>
#include <openrts/formats/signal_script.hpp>
#include <openrts/net/wire.hpp>

namespace openrts::server {

namespace {

[[nodiscard]] std::string lower(std::string_view text) { return core::fold_case(text); }

const formats::TrackDatabase& database_of(const simulation::TrackNetwork& network) { return network.database(); }

// Every world Signal of the route (the shapes' heads come with them).
std::vector<formats::WorldObjectPlacement> world_signals(const core::Vfs& vfs, const core::VirtualPath& route) {
    std::vector<formats::WorldObjectPlacement> signals;
    const auto world = route.resolve(std::string_view("WORLD"));
    if (!world) return signals;
    for (const auto& entry : vfs.list(*world)) {
        if (entry.directory) continue;
        const auto file = std::string(entry.path.filename());
        if (file.size() < 2 || (file.back() != 'w' && file.back() != 'W')) continue;
        const auto tile = formats::load_world_tile(vfs, entry.path);
        for (const auto& object : tile.objects) {
            if (object.kind == formats::WorldObjectKind::signal) signals.push_back(object);
        }
    }
    return signals;
}

} // namespace

ServerRoute::~ServerRoute() = default;

std::unique_ptr<ServerRoute> ServerRoute::load(const std::filesystem::path& root, const std::string& route, std::string& error) {
    std::unique_ptr<ServerRoute> result(new ServerRoute());
    auto& r = *result;
    core::mount_installation(r.vfs_, root, core::load_installation_config(root));
    r.vfs_.build_index();
    // The route folder as written, found case-blind.
    std::string folder = route;
    if (const auto routes = core::VirtualPath::parse("ROUTES")) {
        for (const auto& entry : r.vfs_.list(*routes)) {
            if (entry.directory && lower(entry.path.filename()) == lower(route)) folder = std::string(entry.path.filename());
        }
    }
    const auto path = core::VirtualPath::parse("ROUTES/" + folder);
    if (!path || !r.vfs_.is_directory(*path)) {
        error = "no route " + route + " under " + root.string() + "/ROUTES";
        return nullptr;
    }
    r.route_ = *path;
    r.folder_ = folder;

    const auto definition = formats::load_route_definition(r.vfs_, r.route_);
    r.route_id_ = definition.route_id.empty() ? folder : definition.route_id;

    std::string network_error;
    r.network_ = simulation::TrackNetwork::load(r.vfs_, r.route_, &network_error);
    if (!r.network_) {
        error = "track database: " + network_error;
        return nullptr;
    }
    const auto& database = r.network_->database();
    r.counts_.nodes = database.nodes.empty() ? 0u : static_cast<std::uint32_t>(database.nodes.size() - 1); // [0] unused
    r.counts_.items = static_cast<std::uint32_t>(database.items.size());
    const auto electrification = formats::load_route_electrification(r.vfs_, r.route_);
    if (electrification.has_types) r.counts_.substations = static_cast<std::uint32_t>(electrification.types.substations.size());
    if (electrification.has_database) r.counts_.sections = static_cast<std::uint32_t>(electrification.database.sections.size());
    if (!electrification.error.empty()) r.notes_.push_back("electrification: " + electrification.error);

    // The signals with their scripts; without them the server still runs
    // (the heads keep what the clients show by themselves).
    const auto config = formats::load_signal_config(r.vfs_, r.route_);
    if (config.ok()) {
        auto scripts = formats::load_signal_scripts(r.vfs_, r.route_, config);
        for (const auto& script : scripts.scripts) {
            if (!script.ok()) r.notes_.push_back("signal script " + script.name + " did not compile");
        }
        r.signal_config_ = std::make_shared<formats::SignalConfig>(config);
        r.signal_scripts_ = std::make_shared<formats::SignalScriptFile>(std::move(scripts));
        r.signals_ = std::make_unique<simulation::SignalSystem>(*r.network_, r.signal_config_, r.signal_scripts_,
                                                                world_signals(r.vfs_, r.route_));
    } else {
        r.notes_.push_back("sigcfg: " + config.error + "; the server runs without signals");
    }

    for (auto& p : formats::load_train_paths(r.vfs_, r.route_)) {
        if (p.ok()) r.paths_.push_back(std::move(p));
    }

    // The signals' names from the route database, on their TDB items as
    // the game shows them (Ctrl+F7: RTS's match, else the nearest item).
    const auto route_database = formats::load_route_database(r.vfs_, r.route_);
    for (const auto& [item, unit] : formats::match_signal_units(database_of(*r.network_), route_database)) {
        const auto& name = route_database.signal_units[unit].name;
        if (!name.empty()) r.signal_names_.emplace_back(name, item);
    }
    std::sort(r.signal_names_.begin(), r.signal_names_.end(), [](const auto& a, const auto& b) {
        const auto la = lower(a.first), lb = lower(b.first);
        return la != lb ? la < lb : a.second < b.second;
    });
    return result;
}

std::vector<std::uint32_t> ServerRoute::signal_items(const std::string_view name) const {
    std::vector<std::uint32_t> items;
    const auto wanted = lower(name);
    for (const auto& [n, item] : signal_names_) {
        if (lower(n) == wanted) items.push_back(item);
    }
    return items;
}

const formats::TrainPath* ServerRoute::path(const std::string_view name) const {
    const auto wanted = lower(name);
    for (const auto& p : paths_) {
        if (lower(p.file) == wanted || lower(p.id) == wanted) return &p;
    }
    return nullptr;
}

std::vector<std::string> ServerRoute::consist_names() const {
    std::set<std::string> names;
    for (const char* folder : {"TRAINS/CONSISTS_RTS", "TRAINS/CONSISTS"}) {
        const auto path = core::VirtualPath::parse(folder);
        if (!path) continue;
        for (const auto& entry : vfs_.list(*path)) {
            if (entry.directory || lower(entry.path.extension()) != ".con") continue;
            auto name = std::string(entry.path.filename());
            name.resize(name.size() - 4);
            names.insert(name);
        }
    }
    return {names.begin(), names.end()};
}

ConsistTrain ServerRoute::consist(const std::string_view name, const std::uint32_t number) {
    const auto consist = formats::load_consist(vfs_, name);
    if (!consist.ok()) {
        ConsistTrain out;
        out.error = "consist " + std::string(name) + ": " + consist.error;
        return out;
    }
    auto out = cars(consist.entries, consist.max_velocity, number);
    if (out.cars.empty() && out.error.empty()) out.error = "consist " + std::string(name) + " has no cars";
    return out;
}

ConsistTrain ServerRoute::cars(const std::vector<formats::ConsistEntry>& entries, const float max_velocity, const std::uint32_t number) {
    ConsistTrain out;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        const auto file = entry.folder + "\\" + entry.name + (entry.engine ? ".eng" : ".wag");
        auto& definition = wagons_[lower(file)];
        if (!definition) {
            definition = std::make_unique<formats::WagonDefinition>(formats::load_wagon(vfs_, entry.folder, entry.name, entry.engine));
        }
        if (!definition->ok()) {
            out.error = file + ": " + definition->error;
            out.cars.clear();
            return out;
        }
        const float handbrake = entry.handbrake ? std::clamp(*entry.handbrake / 100.0f, 0.0f, 1.0f) : 0.0f;
        out.cars.push_back({.definition = definition.get(), .flipped = entry.flipped, .handbrake = handbrake});

        // The record as the RTS server writes a car (0x460F in 0x4607):
        // kind 0 wagon, 1 electric, 2 diesel; flags 1 flipped, 2 cold.
        net::TrainCar car;
        if (definition->engine) {
            car.kind = definition->engine->kind == formats::EngineKind::electric ? 1 : definition->engine->kind == formats::EngineKind::diesel ? 2 : 0;
        }
        car.file = net::from_utf8(file);
        car.uid = (number << 18) | static_cast<std::uint32_t>(i);
        car.flags = (entry.flipped ? 1u : 0u) | (entry.engine && entry.cold ? 2u : 0u);
        car.mass = definition->mass;
        car.width = definition->width;
        car.length = definition->length;
        car.height = definition->height;
        car.handbrake = handbrake;
        car.max_handbrake = definition->brakes.max_handbrake_force;
        out.records.push_back(std::move(car));
        if (out.top_speed <= 0.0f && definition->engine && definition->engine->max_velocity > 0.0f) out.top_speed = definition->engine->max_velocity;
    }
    if (max_velocity > 0.0f) out.top_speed = max_velocity;
    if (out.cars.empty() && out.error.empty()) out.error = "no cars";
    return out;
}

std::optional<simulation::TrackTraveller> ServerRoute::path_front(const formats::TrainPath& path, const double length, const double back) {
    if (path.nodes.empty()) return std::nullopt;
    const auto& first = path.nodes.front();
    const auto start = path.points[first.point].position;
    const auto location = network_->locate(
        simulation::TrackPoint{.tile_x = start.tile_x, .tile_z = start.tile_z, .x = start.x, .y = start.y, .z = start.z}, 20.0);
    if (!location) return std::nullopt;
    // The first leg: towards the next node's point (metres, across tiles).
    double dx = 0.0, dz = 0.0;
    if (first.next != formats::PathNode::invalid && first.next < path.nodes.size()) {
        const auto next = path.points[path.nodes[first.next].point].position;
        dx = (next.tile_x - start.tile_x) * 2048.0 + (next.x - start.x);
        dz = (next.tile_z - start.tile_z) * 2048.0 + (next.z - start.z);
    }
    simulation::TrackTraveller rear(*network_, location->node, location->offset, true);
    {
        auto ahead = rear;
        const auto here = rear.pose();
        ahead.move(1.0);
        const auto there = ahead.pose();
        const double ax = (there.tile_x - here.tile_x) * 2048.0 + (there.x - here.x);
        const double az = (there.tile_z - here.tile_z) * 2048.0 + (there.z - here.z);
        if (ax * dx + az * dz < 0.0) rear = simulation::TrackTraveller(*network_, location->node, location->offset, false);
    }
    if (back > 0.0) {
        simulation::TrackTraveller behind(*network_, rear.node(), rear.offset(), !rear.forward());
        behind.move(back);
        rear = simulation::TrackTraveller(*network_, behind.node(), behind.offset(), !behind.forward());
    }
    auto front = rear;
    front.move(length);
    return front;
}

} // namespace openrts::server
