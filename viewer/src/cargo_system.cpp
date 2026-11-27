#include "cargo_system.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <utility>

#include <openrts/formats/route_world.hpp>
#include <openrts/simulation/train.hpp>

namespace openrts::viewer {
namespace {

constexpr double tile_size = formats::route_tile_size;

struct Vec3 {
    float x{}, y{}, z{};
};

[[nodiscard]] Vec3 rotate(const std::array<float, 4>& q, const Vec3& v) {
    const float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    const float tx = 2.0f * (qy * v.z - qz * v.y);
    const float ty = 2.0f * (qz * v.x - qx * v.z);
    const float tz = 2.0f * (qx * v.y - qy * v.x);
    return {v.x + qw * tx + qy * tz - qz * ty, v.y + qw * ty + qz * tx - qx * tz, v.z + qw * tz + qx * ty - qy * tx};
}

[[nodiscard]] Vec3 unrotate(const std::array<float, 4>& q, const Vec3& v) {
    const std::array<float, 4> inverse{-q[0], -q[1], -q[2], q[3]};
    return rotate(inverse, v);
}

[[nodiscard]] bool inside(const CargoSite& site, const std::array<float, 3>& point) {
    const Vec3 local = unrotate(site.rotation, {point[0] - site.at[0], point[1] - site.at[1], point[2] - site.at[2]});
    const auto& pickup = site.pickup;
    if (pickup.has_area) {
        const float tolerance = 0.05f;
        bool ok = true;
        for (int axis = 0; axis < 3; ++axis) {
            const float low = (std::min)(pickup.area[axis], pickup.area[axis + 3]) - tolerance;
            const float high = (std::max)(pickup.area[axis], pickup.area[axis + 3]) + tolerance;
            const float value = axis == 0 ? local.x : axis == 1 ? local.y : local.z;
            if (value < low || value > high) ok = false;
        }
        return ok;
    }
    if (!pickup.points.empty()) {
        const float half = (std::max)(0.25f, 0.0f) + (std::max)(0.0f, pickup.length) * 0.5f;
        for (const auto& p : pickup.points) {
            if (std::abs(local.x - p[0]) <= 1.5f && std::abs(local.y - p[1]) <= 3.0f && std::abs(local.z - p[2]) <= half) return true;
        }
        return false;
    }
    const float radius = (std::max)(2.0f, pickup.length);
    return local.x * local.x + local.z * local.z <= radius * radius;
}

} // namespace

const CargoSystem::Loaded CargoSystem::empty_{};

CargoSystem::CargoSystem(std::shared_ptr<const core::Vfs> vfs, core::VirtualPath route, const formats::RoutePosition origin)
    : vfs_(std::move(vfs)), route_(std::move(route)), origin_(origin) {
    loading_ = std::async(std::launch::async, [vfs = vfs_, route = route_, origin = origin_] {
        auto loaded = std::make_unique<Loaded>();
        loaded->catalog = formats::load_cargo_catalog(*vfs);
        if (!loaded->catalog.ok()) {
            loaded->error = loaded->catalog.error;
            return loaded;
        }
        const auto world = route.resolve(std::string_view("WORLD"));
        if (!world) return loaded;
        std::map<std::string, std::optional<formats::ShapeDescription>> shapes;
        for (const auto& entry : vfs->list(*world)) {
            if (entry.directory) continue;
            const auto file = std::string(entry.path.filename());
            if (file.empty() || (file.back() != 'w' && file.back() != 'W')) continue;
            const auto tile = formats::load_world_tile(*vfs, entry.path);
            for (const auto& object : tile.objects) {
                if (object.shape_file.empty()) continue;
                auto description_file = object.shape_file;
                description_file.replace_extension(".sd");
                const auto key = description_file.generic_string();
                auto found = shapes.find(key);
                if (found == shapes.end()) {
                    std::optional<formats::ShapeDescription> shape;
                    std::optional<core::VirtualPath> path = route.resolve(std::string_view(key));
                    if (!path || !vfs->exists(*path)) {
                        path.reset();
                        if (const auto folder = route.resolve(std::string_view("SHAPES"))) {
                            if (const auto candidate = folder->resolve(std::string_view(description_file.filename().generic_string()));
                                candidate && vfs->exists(*candidate)) {
                                path = candidate;
                            }
                        }
                    }
                    if (!path || !vfs->exists(*path)) {
                        path.reset();
                        if (const auto direct = core::VirtualPath::parse(key); direct && vfs->exists(*direct)) path = direct;
                    }
                    if (path && vfs->exists(*path)) {
                        std::string text_error;
                        const auto bytes = vfs->read(*path, &text_error);
                        bool pickup_shape = false;
                        if (bytes) {
                            pickup_shape = bytes->find("ESD_Pickup_Object") != std::string::npos;
                            if (!pickup_shape && bytes->size() >= 2 && static_cast<unsigned char>((*bytes)[0]) == 0xFF &&
                                static_cast<unsigned char>((*bytes)[1]) == 0xFE) {
                                std::string narrow;
                                narrow.reserve(bytes->size() / 2);
                                for (std::size_t k = 2; k + 1 < bytes->size(); k += 2) {
                                    if ((*bytes)[k + 1] == '\0') narrow.push_back((*bytes)[k]);
                                }
                                pickup_shape = narrow.find("ESD_Pickup_Object") != std::string::npos;
                            }
                        }
                        if (pickup_shape) {
                            auto result = formats::load_shape_description(*vfs, *path);
                            if (result.ok()) shape = std::move(result.value);
                        }
                    }
                    found = shapes.emplace(key, std::move(shape)).first;
                }
                if (!found->second || !found->second->pickup) continue;
                const auto& placement = object.position;
                CargoSite site;
                site.name = std::filesystem::path(key).stem().string();
                site.at = {static_cast<float>((placement.tile_x - origin.tile_x) * tile_size + placement.x - origin.x),
                           static_cast<float>(placement.y - origin.y),
                           static_cast<float>((placement.tile_z - origin.tile_z) * tile_size + placement.z - origin.z)};
                site.rotation = object.rotation;
                site.pickup = *found->second->pickup_object;
                site.stock = std::numeric_limits<double>::quiet_NaN();
                loaded->sites.push_back(std::move(site));
            }
        }
        return loaded;
    });
}

CargoSystem::~CargoSystem() {
    if (loading_.valid()) loading_.wait();
}

const std::string& CargoSystem::error() const noexcept { return loaded_ ? loaded_->error : empty_.error; }

std::size_t CargoSystem::sites() const noexcept { return loaded_ ? loaded_->sites.size() : 0; }

const formats::CargoCatalog& CargoSystem::catalog() const noexcept { return loaded_ ? loaded_->catalog : empty_.catalog; }

bool CargoSystem::poll() {
    if (loaded_) return true;
    if (!loading_.valid()) return false;
    if (loading_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
    loaded_ = loading_.get();
    return true;
}

const CargoSite* CargoSystem::site_at(const std::array<float, 3>& point, const float length) const {
    if (!loaded_) return nullptr;
    const CargoSite* best = nullptr;
    float best_distance = std::numeric_limits<float>::max();
    for (const auto& site : loaded_->sites) {
        if (site.pickup.types[0] == 13 || site.pickup.types[1] == 13) continue;
        if (!inside(site, point)) continue;
        const float distance = (std::max)(0.0f, (std::abs(point[0] - site.at[0]) + std::abs(point[2] - site.at[2])) - length);
        if (distance < best_distance) {
            best_distance = distance;
            best = &site;
        }
    }
    return best;
}

void CargoSystem::update(const float seconds, simulation::Train& train,
                         const std::span<const std::array<float, 3>> car_centres) {
    if (!(seconds > 0.0f)) return;
    if (!poll() || !loaded_ || !loaded_->catalog.ok()) return;
    if (std::abs(train.speed()) > 0.02) return;
    const auto& cars = train.cars();
    for (std::size_t i = 0; i < cars.size() && i < car_centres.size(); ++i) {
        const auto* definition = cars[i].definition;
        if (!definition || definition->file.empty()) continue;
        if (definition->kind == formats::WagonKind::engine) continue;
        const auto* wagon = loaded_->catalog.wagon_for(std::filesystem::path(definition->file).filename().string());
        if (!wagon) continue;
        for (auto& site : loaded_->sites) {
            if (!inside(site, car_centres[i])) continue;
            const auto* cargo = loaded_->catalog.cargo(site.pickup.cargo);
            if (!cargo || cargo->density <= 0.0f) continue;
            if (!formats::cargo_accepts(*wagon, *cargo)) continue;
            const float capacity = formats::cargo_capacity(*wagon, *cargo, definition->width, definition->height, definition->length);
            if (!(capacity > 0.0f)) continue;
            const bool bulk = site.pickup.types[0] == 13 || site.pickup.types[1] == 13;
            const bool unloading = bulk || site.pickup.capacity[1] < 0.0f;
            const float rate = std::abs(site.pickup.capacity[1]);
            if (!(rate > 0.0f)) continue;
            if (std::isnan(site.stock)) site.stock = unloading ? 0.0 : (std::max)(0.0f, site.pickup.capacity[0]);
            const double site_capacity = (std::max)(0.0f, site.pickup.capacity[0]);
            const double room = site_capacity > 0.0 ? (unloading ? site_capacity - site.stock : site.stock)
                                                    : std::numeric_limits<double>::infinity();
            if (!(room > 0.001)) continue;
            const double amount = (std::min)(static_cast<double>(rate) * cargo->density * seconds * (unloading ? 1.0 : 0.5), room);
            const double mass = cars[i].cargo_mass;
            if (!cars[i].cargo_id.empty() && cars[i].cargo_id != cargo->id) continue;
            const double room_car = unloading ? mass : static_cast<double>(capacity) - mass;
            const double moved = (std::min)(amount, room_car);
            if (!(moved > 0.0)) continue;
            const double next = unloading ? mass - moved : mass + moved;
            train.set_cargo(i, static_cast<float>((std::max)(0.0, next)), unloading && next <= 0.001 ? std::string{} : cargo->id,
                            capacity);
            site.stock += unloading ? moved : -moved;
            if (site_capacity > 0.0) site.stock = std::clamp(site.stock, 0.0, site_capacity);
            break;
        }
    }
}

} // namespace openrts::viewer
