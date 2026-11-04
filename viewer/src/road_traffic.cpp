#include "road_traffic.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>

#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>

#include <openrts/formats/track_database.hpp>
#include <openrts/formats/track_sections.hpp>

#include "crossing_view.hpp"

namespace openrts::viewer {
namespace {

[[nodiscard]] std::string world_file_name(const std::int32_t tile_x, const std::int32_t tile_z) {
    char name[32];
    std::snprintf(name, sizeof name, "w%+07d%+07d.w", tile_x, tile_z);
    return name;
}

// 0x49396C: how fast a car takes a curve of this radius (0: no limit).
[[nodiscard]] float curve_limit(const float radius) {
    if (radius <= 0.0f) return 0.0f;
    if (radius <= 50.0f) return 7.0f;
    if (radius <= 150.0f) return 10.0f;
    if (radius <= 250.0f) return 12.0f;
    if (radius <= 500.0f) return 20.0f;
    return 0.0f;
}

} // namespace

RoadTraffic::RoadTraffic(std::shared_ptr<const core::Vfs> vfs, core::VirtualPath route, const formats::RoutePosition origin)
    : vfs_(std::move(vfs)), route_(std::move(route)), origin_(origin) {
    types_ = formats::load_car_spawn_list(*vfs_, route_);
    if (types_.types.empty()) return;
    // The road database: <route>.rdb (RTS 0x492BE4), on the route's track sections.
    const std::string name(route_.filename());
    std::optional<std::string> bytes;
    if (const auto file = route_.resolve(name + ".rdb")) bytes = vfs_->read(*file);
    if (!bytes) {
        for (const auto& entry : vfs_->list(route_)) {
            if (entry.directory) continue;
            std::string file(entry.path.filename());
            for (auto& c : file) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (file.size() > 4 && file.ends_with(".rdb")) {
                bytes = vfs_->read(entry.path);
                break;
            }
        }
    }
    if (!bytes) return;
    auto database = formats::parse_track_database(*bytes);
    auto sections = formats::load_track_sections(*vfs_, route_);
    if (!database.ok() || !sections.ok()) {
        std::cout << "road traffic: " << (database.ok() ? sections.error : database.error) << std::endl;
        return;
    }
    network_ = std::make_unique<simulation::TrackNetwork>(std::move(database), std::move(sections));
    models_.resize(types_.types.size());
    for (std::size_t i = 0; i < types_.types.size(); ++i) {
        // The route's SHAPES, else GLOBAL\SHAPES.
        const auto& shape = types_.types[i].shape;
        for (const auto& folder : {route_.resolve(std::string_view("SHAPES")), core::VirtualPath::parse("GLOBAL/SHAPES")}) {
            if (!folder) continue;
            if (auto file = folder->resolve(shape); file && vfs_->exists(*file)) {
                models_[i].shape = *file;
                break;
            }
        }
    }
}

RoadTraffic::~RoadTraffic() {
    for (auto& [_, job] : loading_) {
        if (job.valid()) job.wait();
    }
    for (auto& model : models_) {
        if (model.loading.valid()) model.loading.wait();
    }
}

const formats::WorldObjectPlacement* RoadTraffic::world_object(const Key& key) {
    const TileKey tile{std::get<0>(key), std::get<1>(key)};
    auto found = tiles_.find(tile);
    if (found == tiles_.end()) {
        auto job = loading_.find(tile);
        if (job == loading_.end()) {
            const auto file = route_.resolve(std::string_view("WORLD")).value_or(route_).resolve(world_file_name(tile.first, tile.second));
            if (!file) return nullptr;
            loading_.emplace(tile, std::async(std::launch::async, [vfs = vfs_, path = *file] { return formats::load_world_tile(*vfs, path); }));
            return nullptr;
        }
        if (job->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return nullptr;
        found = tiles_.emplace(tile, job->second.get()).first;
        loading_.erase(job);
    }
    for (const auto& object : found->second.objects) {
        if (object.kind == formats::WorldObjectKind::car_spawner && object.uid == std::get<2>(key)) return &object;
    }
    return nullptr;
}

// The spawner's road: from its first road item towards its second, the
// curves and the level crossings along it.
void RoadTraffic::prepare(Spawner& spawner) {
    spawner.failed = true;
    const auto& roads = spawner.object.road_items;
    if (roads.size() < 2 || !network_) return;
    const std::uint32_t first = roads[0], last = roads[1];
    std::optional<std::pair<std::uint32_t, double>> place;
    for (std::uint32_t n = 1; n < network_->node_count() && !place; ++n) {
        const auto* node = network_->node(n);
        if (!node) continue;
        for (const auto& item : node->items) {
            if (item.item == first) {
                place = std::pair{n, item.offset};
                break;
            }
        }
    }
    if (!place) return;
    const auto is_end = [last](const formats::TrackItem& item) { return item.id == last; };
    for (const bool forward : {true, false}) {
        const auto found = simulation::look_ahead(*network_, place->first, place->second, forward, 8000.0, is_end, 1);
        if (found.items.empty()) continue;
        spawner.start.emplace(*network_, place->first, place->second, forward);
        spawner.end = found.items.front().distance;
        break;
    }
    if (!spawner.start || spawner.end <= 1.0) return;

    // Curves: the section under each 2 m of the road.
    simulation::TrackTraveller walker = *spawner.start;
    Curve current{};
    for (double along = 0.0; along < spawner.end; along += 2.0) {
        float limit = 0.0f;
        if (const auto* node = network_->node(walker.node())) {
            const auto section = walker.section();
            if (section < node->sections.size() && node->sections[section].definition) {
                const auto* definition = node->sections[section].definition;
                if (definition->curved) limit = curve_limit(definition->radius);
            }
        }
        if (limit != current.limit) {
            if (current.limit > 0.0f) {
                current.end = along;
                spawner.curves.push_back(current);
            }
            current = Curve{.start = along, .end = along, .limit = limit};
        }
        if (!walker.move(2.0).ok()) break;
    }
    if (current.limit > 0.0f) {
        current.end = spawner.end;
        spawner.curves.push_back(current);
    }
    const auto is_crossing = [](const formats::TrackItem& item) { return item.kind == formats::TrackItemKind::level_crossing; };
    const auto crossings = simulation::look_ahead(*network_, spawner.start->node(), spawner.start->offset(), spawner.start->forward(),
                                                  spawner.end, is_crossing);
    for (const auto& item : crossings.items) spawner.crossings.push_back({.at = item.distance, .item = item.item});
    spawner.sent.assign(types_.types.size(), 0);
    spawner.failed = false;
}

void RoadTraffic::request_model(const std::size_t type, renderer::ModelLibrary& library) {
    auto& model = models_[type];
    if (!model.shape) return;
    if (!model.requested && !library.find(*model.shape)) {
        model.requested = true;
        model.loading = std::async(std::launch::async, [vfs = vfs_, path = *model.shape] { return renderer::load_model_asset(*vfs, path, {}); });
    }
    if (model.loading.valid() && model.loading.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto asset = model.loading.get();
        if (asset.ok()) {
            library.add(std::move(asset));
        } else {
            std::cout << "car shape '" << model.shape->str() << "': " << asset.error << '\n';
        }
    }
}

float RoadTraffic::car_length(const std::size_t type, renderer::ModelLibrary& library) {
    // RTS (0x493FB8): the model's bounding diameter once it is there.
    if (const auto& shape = models_[type].shape) {
        if (const auto* model = library.find(*shape)) return (std::max)(model->bounds().radius * 2.0f, 1.0f);
    }
    return types_.types[type].distance;
}

// 0x494B28: a car, unless the last one sent is still near the start; the type
// by CarSpread, the rarely sent first.
void RoadTraffic::send(Spawner& spawner, renderer::ModelLibrary& library) {
    const auto& types = types_.types;
    std::size_t type = 0;
    {
        std::vector<double> weights(types.size());
        for (std::size_t i = 0; i < types.size(); ++i) weights[i] = types[i].spread / (1.0 + spawner.sent[i]);
        std::discrete_distribution<std::size_t> pick(weights.begin(), weights.end());
        type = pick(random_);
    }
    if (!spawner.cars.empty()) {
        const auto& newest = spawner.cars.back();
        const double behind = newest.along - newest.length;
        if (behind < 2.0) return;
        if (newest.along < types[newest.type].distance + types[type].distance) return;
    }
    Car car;
    car.type = type;
    car.target = (std::min)(spawner.object.car_average_speed, types[type].max_velocity);
    car.speed = car.target;
    car.length = car_length(type, library);
    car.along = car.length;
    spawner.cars.push_back(car);
    ++spawner.sent[type];
    spawner.timer = 0.0f;
}

// 0x49396C and 0x493FB8: each car keeps behind the one ahead, slows for
// curves and stops before a closed crossing, then moves on.
void RoadTraffic::move(Spawner& spawner, const float elapsed, const CrossingView* crossings) {
    const auto& types = types_.types;
    for (std::size_t k = 0; k < spawner.cars.size(); ++k) {
        auto& car = spawner.cars[k];
        float target = car.target;
        float free = 100.0f;
        float strongest = 0.0f; // the hardest braking asked so far
        if (k > 0) {
            const auto& ahead = spawner.cars[k - 1];
            const float gap = static_cast<float>(ahead.along - ahead.length - car.along) - 2.0f;
            if (gap < free) {
                target = ahead.speed;
                free = gap;
                strongest = (car.speed - target) / (std::max)(gap, 0.001f);
            }
        }
        if (crossings) {
            for (const auto& crossing : spawner.crossings) {
                if (crossings->road_state(crossing.item) == 0) continue;
                const float gap = static_cast<float>(crossing.at - car.along) - 10.0f;
                if (gap > -2.0f && gap < free) {
                    const float braking = car.speed / (gap + 2.01f);
                    if (braking > strongest) {
                        target = 0.0f;
                        free = gap;
                        strongest = braking;
                    }
                }
            }
        }
        for (const auto& curve : spawner.curves) {
            if (car.along >= curve.start && car.along <= curve.end) {
                target = (std::min)(target, curve.limit);
                continue;
            }
            const float distance = static_cast<float>(curve.start - car.along);
            if (distance < 0.01f || distance > free) continue;
            if ((car.speed - curve.limit) / distance > strongest) {
                target = curve.limit;
                free = distance;
                strongest = (car.speed - curve.limit) / distance;
            }
        }
        const auto& type = types[car.type];
        if (target > car.speed) {
            car.speed = (std::min)(target, car.speed + type.acceleration * elapsed);
        } else if (target < car.speed) {
            if (free > 0.0f) {
                const float time = (car.speed - target) / type.deceleration;
                const float stopping = car.speed * time - type.deceleration * time * time * 0.5f;
                if (stopping >= free) car.speed = (std::max)(target, car.speed - type.deceleration * elapsed);
            } else {
                car.speed = target;
            }
        }
        car.along += static_cast<double>(car.speed) * elapsed;
        car.wheels = std::fmod(car.wheels + car.speed * elapsed * 10.0f, 20.0f);
    }
    // Past the end item: gone.
    std::erase_if(spawner.cars, [&](const Car& car) { return car.along - car.length > spawner.end; });
}

void RoadTraffic::update(const float elapsed, const std::vector<renderer::StreamedInstance>& visible, const CrossingView* crossings,
                         renderer::ModelLibrary& library) {
    if (!ready()) return;
    for (auto& [_, spawner] : spawners_) spawner.seen = false;
    for (const auto& instance : visible) {
        if (instance.kind != formats::WorldObjectKind::car_spawner) continue;
        const Key key{instance.world_tile_x, instance.world_tile_z, instance.uid};
        auto found = spawners_.find(key);
        if (found == spawners_.end()) {
            const auto* object = world_object(key);
            if (!object) continue;
            found = spawners_.emplace(key, Spawner{.object = *object}).first;
            prepare(found->second);
        }
        found->second.seen = true;
    }
    std::erase_if(spawners_, [](const auto& entry) { return !entry.second.seen; });
    for (std::size_t type = 0; type < models_.size(); ++type) {
        if (models_[type].requested || models_[type].loading.valid()) request_model(type, library);
    }
    for (auto& [_, spawner] : spawners_) {
        if (spawner.failed) continue;
        // 0x493FB8: the next car after CarFrequency x 0.6 .. 0.96.
        spawner.timer += elapsed;
        if (spawner.timer > spawner.interval) {
            spawner.interval = (0.6f + static_cast<float>(random_() % 10u) * 0.04f) * spawner.object.car_frequency;
            send(spawner, library);
        }
        for (const auto& car : spawner.cars) request_model(car.type, library);
        move(spawner, elapsed, crossings);
    }
}

std::optional<glm::mat4> RoadTraffic::place(const Spawner& spawner, const Car& car) const {
    if (!spawner.start) return std::nullopt;
    auto front = *spawner.start;
    if (!front.move(car.along).ok()) return std::nullopt;
    auto rear = *spawner.start;
    rear.move((std::max)(car.along - car.length, 0.0));
    const auto local = [this](const simulation::TrackPoint& point) {
        return glm::vec3{
            static_cast<float>((point.tile_x - origin_.tile_x) * simulation::tile_size + point.x - origin_.x),
            static_cast<float>(point.y - origin_.y),
            static_cast<float>((point.tile_z - origin_.tile_z) * simulation::tile_size + point.z - origin_.z),
        };
    };
    const auto a = local(front.pose());
    const auto b = local(rear.pose());
    glm::vec3 axis = a - b;
    if (glm::length(axis) < 0.01f) {
        const auto heading = front.pose().heading;
        axis = glm::vec3{std::sin(heading), 0.0f, std::cos(heading)};
    }
    const float heading = std::atan2(axis.x, axis.z);
    const float up = std::atan2(axis.y, std::hypot(axis.x, axis.z));
    glm::mat4 transform = glm::translate(glm::mat4{1.0f}, (a + b) * 0.5f);
    transform = glm::rotate(transform, heading, glm::vec3{0.0f, 1.0f, 0.0f});
    transform = glm::rotate(transform, -up, glm::vec3{1.0f, 0.0f, 0.0f});
    return transform;
}

void RoadTraffic::submit(renderer::ModelRenderer& renderer, renderer::ModelLibrary& library, const renderer::Frustum* frustum,
                         const glm::vec3& eye, const float draw_distance) {
    std::vector<renderer::NodeAdjustment> wheels;
    for (const auto& [_, spawner] : spawners_) {
        for (const auto& car : spawner.cars) {
            const auto& shape = models_[car.type].shape;
            auto* model = shape ? library.find(*shape) : nullptr;
            if (!model) continue;
            const auto transform = place(spawner, car);
            if (!transform) continue;
            const auto bounds = model->bounds();
            const glm::vec3 centre{*transform * glm::vec4{bounds.center, 1.0f}};
            if (draw_distance > 0.0f && glm::distance(centre, eye) > draw_distance + bounds.radius) continue;
            if (frustum && !frustum->intersects_sphere(centre, bounds.radius)) continue;
            // The wheels: every animated node at the car's wheel frame (0x493FB8, a 20-frame turn).
            wheels.clear();
            if (const auto* rig = model->rig(); rig && !rig->animations.empty()) {
                for (const auto& node : rig->animations.front().nodes) {
                    if (node.matrix_index <= 0) continue;
                    float last = 0.0f;
                    for (const auto& key : node.rotations) last = (std::max)(last, static_cast<float>(key.frame));
                    if (last > 0.0f) wheels.push_back({.matrix = node.matrix_index, .frame = std::fmod(car.wheels, last)});
                }
            }
            if (wheels.empty()) {
                renderer.submit(*model, *transform);
            } else {
                renderer.submit(*model, *transform, wheels);
            }
        }
    }
}

std::size_t RoadTraffic::cars() const noexcept {
    std::size_t total = 0;
    for (const auto& [_, spawner] : spawners_) total += spawner.cars.size();
    return total;
}

} // namespace openrts::viewer
