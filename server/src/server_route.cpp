#include <openrts/server/server_route.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
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

std::unique_ptr<ServerRoute> ServerRoute::load(const std::filesystem::path& root, const std::string& route, std::string& error,
                                               const std::filesystem::path& data) {
    std::unique_ptr<ServerRoute> result(new ServerRoute());
    auto& r = *result;
    r.data_ = data;
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

    // The route database: the one the server saved, else the installation's.
    r.database_file_ = formats::route_database_file(r.vfs_, r.route_);
    r.database_name_ = r.database_file_ ? std::string(r.database_file_->filename()) : r.route_id_ + ".rtsdb";
    {
        std::error_code exists_error;
        const auto saved = r.saved_database_path();
        if (!r.data_.empty() && std::filesystem::is_regular_file(saved, exists_error)) {
            std::ifstream input(saved, std::ios::binary);
            const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            r.route_database_ = formats::parse_route_database(bytes);
            r.notes_.push_back("the route database saved by the server: " + saved.string());
        } else {
            r.route_database_ = formats::load_route_database(r.vfs_, r.route_);
        }
        if (!r.route_database_.ok()) r.notes_.push_back("route database: " + r.route_database_.error);
    }
    const auto& route_database = r.route_database_;
    r.station_routes_ = StationRoutes::build(*r.network_, route_database);
    if (!route_database.routes.empty()) r.notes_.push_back("station routes: " + r.station_routes_.summary());
    // The signals' names from the route database, on their TDB items as
    // the game shows them (Ctrl+F7: RTS's match, else the nearest item).
    for (const auto& [item, unit] : formats::match_signal_units(database_of(*r.network_), route_database)) {
        const auto& name = route_database.signal_units[unit].name;
        if (!name.empty()) r.signal_names_.emplace_back(name, item);
    }
    std::sort(r.signal_names_.begin(), r.signal_names_.end(), [](const auto& a, const auto& b) {
        const auto la = lower(a.first), lb = lower(b.first);
        return la != lb ? la < lb : a.second < b.second;
    });

    // The signals as rts_server.exe runs them (docs/reverse/dispatcher.md,
    // "Signal modes on the server"): its signal units (objects), every one
    // manual and run once with no train (0x402F80), then each with a
    // SignalUnit in the route database takes its defaults (0x449C68,
    // 0x41F39C): UnitFlags bit 2 open, else bit 4 automatic block, else
    // manual at its DefaultAspect. A database unit is found by its
    // position, as the game finds it (0x4903DC).
    if (r.signals_) {
        auto& signals = *r.signals_;
        signals.set_dispatched(true);
        for (const auto& type : route_database.shown_as_normal) signals.set_shown_as_normal(type, true);
        // 0x449C68: the file's SignalUnits in order, each to the unit of its
        // own index when that unit has as many items and stands at its
        // Position (0x41F418: the first head's tile, x, y, z within 0.2),
        // else to the first unit standing there; a later one taking a unit
        // replaces the earlier.
        r.unit_records_.assign(signals.unit_count(), -1);
        {
            std::vector<std::vector<simulation::TrackPoint>> places(signals.unit_count());
            for (std::uint32_t u = 0; u < signals.unit_count(); ++u) places[u] = signals.unit_places(u);
            const auto at = [&](const std::uint32_t u, const formats::RouteSignalUnit& record) {
                for (const auto& p : places[u]) {
                    if (p.tile_x == record.position.tile_x && p.tile_z == record.position.tile_z && std::abs(p.x - record.position.x) <= 0.2 &&
                        std::abs(p.y - record.position.y) <= 0.2 && std::abs(p.z - record.position.z) <= 0.2)
                        return true;
                }
                return false;
            };
            const auto count = (std::min)(route_database.signal_units.size(), static_cast<std::size_t>(signals.unit_count()));
            for (std::size_t i = 0; i < count; ++i) {
                const auto& record = route_database.signal_units[i];
                std::optional<std::uint32_t> unit;
                const auto own = static_cast<std::uint32_t>(i);
                if (signals.unit_items(own).size() == record.items && at(own, record)) unit = own;
                for (std::uint32_t u = 0; !unit && u < signals.unit_count(); ++u) {
                    if (u != own && at(u, record)) unit = u;
                }
                if (unit) r.unit_records_[*unit] = static_cast<std::int32_t>(i);
            }
        }
        std::size_t by_mode[3]{};
        std::size_t without = 0;
        for (std::uint32_t u = 0; u < signals.unit_count(); ++u) {
            const auto unit = r.unit_records_[u];
            if (unit < 0) {
                ++without;
                continue;
            }
            const auto& record = route_database.signal_units[static_cast<std::size_t>(unit)];
            simulation::UnitDefaults value;
            value.mode = (record.flags & 2u)   ? simulation::UnitMode::open
                         : (record.flags & 4u) ? simulation::UnitMode::automatic
                                             : simulation::UnitMode::manual;
            if (const auto aspect = formats::signal_aspect_from_name(record.default_aspect)) value.aspect = *aspect;
            signals.set_defaults(u, value);
            signals.restore_defaults(u);
            ++by_mode[static_cast<int>(value.mode)];
        }
        r.signal_modes_ = std::to_string(signals.unit_count()) + " signal units: " + std::to_string(by_mode[2]) + " automatic block, " +
                          std::to_string(by_mode[1]) + " open, " + std::to_string(by_mode[0]) + " manual from the route database (" +
                          std::to_string(route_database.signal_units.size()) + " SignalUnits); " + std::to_string(without) +
                          " without one (manual, at what their script gives with no train)";
    }
    return result;
}

std::filesystem::path ServerRoute::saved_database_path() const {
    return data_ / "server" / std::filesystem::path(net::from_utf8(folder_)) / std::filesystem::path(net::from_utf8(database_name_));
}

std::filesystem::path ServerRoute::placements_folder() const {
    return data_ / "server" / std::filesystem::path(net::from_utf8(folder_)) / "placements";
}

bool ServerRoute::save_signal_defaults(std::string& result) {
    if (!signals_ || !signals_->dispatched()) {
        result = "the route's signals did not load";
        return false;
    }
    if (data_.empty()) {
        result = "the server has no data folder (--data)";
        return false;
    }
    const auto& signals = *signals_;
    // The names as the server shows them (signal_names_), by item.
    std::map<std::uint32_t, std::string> name_of;
    for (const auto& [name, item] : signal_names_) name_of.emplace(item, name);
    formats::RouteDatabase out;
    out.shown_as_normal = signals.shown_as_normal_types();
    for (std::uint32_t u = 0; u < signals.unit_count(); ++u) {
        formats::RouteSignalUnit unit;
        const auto& unit_items = signals.unit_items(u);
        // Its Position as rts_server.exe writes it: its first head's place
        // as it keeps it (unit_places: moved 20 m for some), the one read
        // when there was one.
        const auto record_of = u < unit_records_.size() ? unit_records_[u] : -1;
        if (record_of >= 0) {
            unit.position = route_database_.signal_units[static_cast<std::size_t>(record_of)].position;
        } else if (const auto places = signals.unit_places(u); !places.empty()) {
            const auto& p = places.front();
            unit.position = {.tile_x = p.tile_x, .tile_z = p.tile_z, .x = p.x, .y = p.y, .z = p.z};
        }
        unit.items = static_cast<std::uint32_t>(unit_items.size());
        for (const auto item : unit_items) {
            if (const auto it = name_of.find(item); it != name_of.end()) {
                unit.name = it->second;
                break;
            }
        }
        const auto record = u < unit_records_.size() ? unit_records_[u] : -1;
        if (record >= 0) unit.flags = route_database_.signal_units[static_cast<std::size_t>(record)].flags & ~6u;
        unit.default_aspect = "STOP";
        if (const auto value = signals.defaults(u)) {
            if (value->mode == simulation::UnitMode::open) unit.flags |= 2u;
            if (value->mode == simulation::UnitMode::automatic) unit.flags |= 4u;
            unit.default_aspect = formats::to_string(value->aspect);
        }
        out.signal_units.push_back(std::move(unit));
    }
    // The file to edit: the saved one, else the installation's.
    std::string original;
    std::error_code fs_error;
    const auto path = saved_database_path();
    if (std::filesystem::is_regular_file(path, fs_error)) {
        std::ifstream input(path, std::ios::binary);
        original.assign((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    } else if (database_file_) {
        if (const auto bytes = vfs_.read(*database_file_)) original = *bytes;
    }
    std::string error;
    const auto bytes = formats::write_route_database(original, out, error);
    if (bytes.empty()) {
        result = "the route database could not be written: " + error;
        return false;
    }
    std::filesystem::create_directories(path.parent_path(), fs_error);
    if (std::filesystem::is_regular_file(path, fs_error)) {
        auto backup = path;
        backup += ".bak";
        std::filesystem::remove(backup, fs_error);
        std::filesystem::rename(path, backup, fs_error);
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        result = "could not write " + path.string();
        return false;
    }
    result = std::to_string(out.signal_units.size()) + " signal units and " + std::to_string(out.shown_as_normal.size()) +
             " types shown as normal saved to " + path.string();
    return true;
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
            if (!stock_rules_) {
                std::string messages;
                stock_rules_ = simulation::StockRules::load(vfs_, messages);
                if (!messages.empty()) std::fprintf(stderr, "%s", messages.c_str());
            }
            definition = std::make_unique<formats::WagonDefinition>(
                formats::load_wagon(vfs_, entry.folder, entry.name, entry.engine, stock_rules_->hook()));
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
        // RTS's PlaceDrag as the record's 0x100 / 0x200 (docs/reverse/multiplayer-protocol.md).
        car.flags = (entry.flipped ? 1u : 0u) | (entry.engine && entry.cold ? 2u : 0u) | (static_cast<std::uint32_t>(entry.place_drag & 3u) << 8);
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
