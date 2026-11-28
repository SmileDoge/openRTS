#include "cargo_view.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>

#include <imgui.h>

#include <openrts/renderer/cargo_shape.hpp>
#include <openrts/renderer/model_assets.hpp>

#include "train_view.hpp"
#include "ui_text.hpp"

namespace openrts::viewer {

namespace {

// RTS's world file name of a tile: w-004493+014580.w.
[[nodiscard]] std::string world_file_name(const std::int32_t tile_x, const std::int32_t tile_z) {
    char name[32];
    std::snprintf(name, sizeof name, "w%+07d%+07d.w", tile_x, tile_z);
    return name;
}

[[nodiscard]] bool bulk_type(std::uint16_t type) {
    return (type >= 1 && type <= 4) || type == 6 || type == 10;
}

[[nodiscard]] bool rack_type(std::uint16_t type) {
    return type == 5 || type == 7;
}

// The last key frame of a node's animation (its open, tipped pose).
[[nodiscard]] float last_frame(const formats::ShapeModel& rig, const std::int32_t matrix) {
    float last = 0.0f;
    for (const auto& animation : rig.animations) {
        for (const auto& node : animation.nodes) {
            if (node.matrix_index != matrix) continue;
            for (const auto& key : node.positions) last = (std::max)(last, static_cast<float>(key.frame));
            for (const auto& key : node.rotations) last = (std::max)(last, static_cast<float>(key.frame));
        }
    }
    return last;
}

[[nodiscard]] bool same_name(std::string_view a, std::string_view b) {
    return a.size() == b.size()
           && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
}

// pickup.sms's triggers (SOUND/pickup.sms): the "ready" one of a pickup
// (its "off" the next), and the loop it plays while it works (its stop the
// next): diesel 1 / 3, sand 11 / 13, coal and bulk 21 / 101, liquid 31 /
// 33, gas 35 / 37; a trestle's dump 251.
struct PickupSounds {
    std::uint32_t ready{}, loop{};
};
[[nodiscard]] PickupSounds pickup_sounds(const std::uint16_t type, const formats::CargoKind* cargo, const bool engine) {
    const bool gas = cargo && (cargo->loading & formats::cargo_gas);
    switch (type) {
    case 7: return engine ? PickupSounds{1, 3} : gas ? PickupSounds{35, 37} : PickupSounds{31, 33};
    case 10: return engine ? PickupSounds{11, 13} : PickupSounds{21, 101};
    case 1:
    case 2:
    case 3:
    case 4:
    case 6: return {21, 101};
    case 5:
    case 16: return gas ? PickupSounds{35, 37} : PickupSounds{31, 33};
    case 13: return {0, 251};
    default: return {};
    }
}

constexpr float pick_reach = 30.0f; // m from the eye, as far as Num3 reaches a pickup
constexpr float car_reach = 120.0f; // m: the cars a pickup looks at
constexpr float stream_radius = 0.35f;

} // namespace

// ---- the catalogue ----------------------------------------------------------

CargoCatalog::CargoCatalog(std::shared_ptr<const core::Vfs> vfs, const core::VirtualPath& route)
    : kinds(formats::load_cargo_kinds(*vfs)),
      descriptions(formats::load_cargo_descriptions(*vfs)),
      objects(formats::load_cargo_objects(*vfs, route)),
      vfs_(std::move(vfs)) {}

const formats::CargoObject* CargoCatalog::object(std::string_view id) const {
    for (const auto& o : objects) {
        if (o.id.size() == id.size()
            && std::equal(o.id.begin(), o.id.end(), id.begin(), [](char a, char b) { return std::tolower(a) == std::tolower(b); }))
            return &o;
    }
    return nullptr;
}

std::optional<core::VirtualPath> CargoCatalog::texture(const formats::CargoKind& kind) const {
    if (const auto found = textures_.find(kind.code); found != textures_.end()) return found->second;
    std::optional<core::VirtualPath> result;
    for (const char* tail : {"1.ace", ".ace", "1.png", ".png", "1.dds", ".dds"}) {
        if (const auto path = core::VirtualPath::parse("GLOBAL/TEXTURES/" + kind.texture + tail); path && vfs_->exists(*path)) {
            result = path;
            break;
        }
    }
    // None of its own (RTS ships coal, grain, gravel, sand, ore and slate):
    // a bulk cargo drawn with RTS's gravel rather than not at all.
    if (!result && (kind.loading & formats::cargo_pile)) {
        for (const char* fallback : {"GLOBAL/TEXTURES/RTSCargo_gravel1.ace", "GLOBAL/TEXTURES/RTSCargo_coal1.ace"}) {
            if (const auto path = core::VirtualPath::parse(fallback); path && vfs_->exists(*path)) {
                result = path;
                break;
            }
        }
    }
    textures_.emplace(kind.code, result);
    return result;
}

std::optional<core::VirtualPath> CargoCatalog::shape(const formats::CargoObject& object) const {
    if (object.shape.empty()) return std::nullopt;
    if (const auto found = shapes_.find(object.shape); found != shapes_.end()) return found->second;
    std::optional<core::VirtualPath> result;
    for (const char* base : {"TRAINS/OBJECTS/CARGO/", "TRAINS/OBJECTS/", "GLOBAL/SHAPES/"}) {
        if (const auto path = core::VirtualPath::parse(std::string(base) + object.shape); path && vfs_->exists(*path)) {
            result = path;
            break;
        }
    }
    shapes_.emplace(object.shape, result);
    return result;
}

std::string CargoCatalog::name(const formats::CargoKind& kind) const {
    if (kind.own) return ui_language() == UiLanguage::russian && !kind.russian_name.empty() ? kind.russian_name : kind.name;
    return tr(kind.name.c_str());
}

std::vector<CargoDraw> cargo_draws(const CargoCatalog& catalog, const simulation::CarCargo& cargo) {
    std::vector<CargoDraw> draws;
    const auto* description = cargo.description();
    if (!description) return draws;
    for (const auto& load : cargo.loads()) {
        if (load.object.empty()) {
            // A pile in a visible area; the bulk of a hopper or a tank (no
            // areas) stays out of sight, as in RTS.
            if (load.area < 1 || static_cast<std::size_t>(load.area) > description->areas.size() || load.volume <= 0.0f) continue;
            const auto& area = description->areas[static_cast<std::size_t>(load.area - 1)];
            if (area.invisible) continue;
            const auto* kind = catalog.kinds.find(load.cargo);
            const auto texture = kind ? catalog.texture(*kind) : std::nullopt;
            if (!texture) continue;
            const float width = area.box.high[0] - area.box.low[0];
            const float length = area.box.high[2] - area.box.low[2];
            const float height = area.box.high[1] - area.box.low[1];
            if (width <= 0.1f || length <= 0.1f) continue;
            const float pile = std::round(kind->pile * 10.0f) / 10.0f;
            const float fill = renderer::cargo_pile_fill(width, length, pile);
            // A little cargo: a heap in the middle; then it spreads to the
            // walls and rises (over the rim a little when full, heaped).
            float spread = 1.0f;
            float rise = load.volume / (fill * width * length);
            constexpr float low_heap = 0.5f;
            if (rise < low_heap) {
                spread = std::clamp(std::sqrt(load.volume / (fill * width * length * low_heap)), 0.25f, 1.0f);
                rise = load.volume / (fill * width * length * spread * spread);
            }
            rise = (std::min)(rise, height / (std::max)(fill, 0.3f));
            const auto name = renderer::cargo_pile_name(texture->filename(), width, length, pile);
            const auto path = texture->parent().resolve(name);
            if (!path) continue;
            const glm::vec3 centre{(area.box.low[0] + area.box.high[0]) * 0.5f, area.box.low[1], (area.box.low[2] + area.box.high[2]) * 0.5f};
            glm::mat4 local = glm::translate(glm::mat4{1.0f}, area.pivot ? centre + glm::vec3{(*area.pivot)[0], (*area.pivot)[1], (*area.pivot)[2]} : centre);
            local = glm::scale(local, glm::vec3{spread, rise, spread});
            draws.push_back({*path, local});
        } else {
            const auto* object = catalog.object(load.object);
            const auto shape = object ? catalog.shape(*object) : std::nullopt;
            if (!shape) continue;
            // The shape's origin under its Size box's centre (RTS's Size is in
            // the shape's space).
            glm::vec3 offset{0.0f};
            if (object->size) {
                offset = {(object->size->low[0] + object->size->high[0]) * 0.5f, object->size->low[1],
                          (object->size->low[2] + object->size->high[2]) * 0.5f};
            }
            // Those still on a crane's hook: the lowest of them hanging `lift`
            // over its place, the rest not there yet.
            const int placed = (std::max)(0, load.count - load.arriving);
            for (int k = 0; k < load.count && k <= placed; ++k) {
                glm::vec3 at{load.position[0], load.position[1] + static_cast<float>(k) * load.extent[1], load.position[2]};
                if (k == placed) {
                    if (load.arriving <= 0) break;
                    at.y += load.lift;
                }
                draws.push_back({*shape, glm::translate(glm::mat4{1.0f}, at - offset)});
            }
        }
    }
    return draws;
}

void cargo_node_adjustments(const simulation::CarCargo& cargo, const renderer::RenderModel& model,
                            std::vector<renderer::NodeAdjustment>& adjustments) {
    const auto* description = cargo.description();
    const auto* rig = model.rig();
    if (!description || !rig || description->animations.empty()) return;
    for (const auto& animation : description->animations) {
        if (!(animation.loading_unloading & cargo.opening_for)) continue;
        for (const auto& name : animation.nodes) {
            for (std::size_t m = 0; m < rig->matrices.size(); ++m) {
                if (!same_name(rig->matrices[m].name, name)) continue;
                const float last = animation.frame >= 0 ? static_cast<float>(animation.frame) : last_frame(*rig, static_cast<std::int32_t>(m));
                adjustments.push_back({.matrix = static_cast<std::int32_t>(m), .frame = cargo.opening * last});
            }
        }
    }
}

// ---- the pickups ------------------------------------------------------------

CargoView::CargoView(std::shared_ptr<const core::Vfs> vfs, core::VirtualPath route)
    : vfs_(vfs), route_(route), catalog_(vfs, route), bank_(vfs) {
    std::size_t own = 0;
    for (const auto& kind : catalog_.kinds.all()) own += kind.own ? 1 : 0;
    std::cout << "cargo: " << catalog_.kinds.all().size() << " kinds (" << own << " openRTS's), " << catalog_.descriptions.wagons.size()
              << " wagon types, " << catalog_.objects.size() << " cargo objects" << std::endl;
}

CargoView::~CargoView() {
    for (auto& [_, job] : loading_) {
        if (job.valid()) job.wait();
    }
    for (auto& job : model_loading_) {
        if (job.valid()) job.wait();
    }
}

const formats::WorldObjectPlacement* CargoView::world_object(const Key& key) {
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
        if (object.kind == formats::WorldObjectKind::pickup && object.uid == std::get<2>(key)) return &object;
    }
    return nullptr;
}

void CargoView::scan(const std::vector<renderer::StreamedInstance>& visible, renderer::ModelLibrary& library) {
    for (auto& [_, place] : pickups_) place.seen = false;
    for (const auto& instance : visible) {
        using Kind = formats::WorldObjectKind;
        if (instance.kind == Kind::signal || instance.kind == Kind::forest || instance.kind == Kind::transfer
            || instance.kind == Kind::car_spawner || instance.kind == Kind::level_crossing || instance.kind == Kind::speed_post)
            continue;
        const Key key{instance.world_tile_x, instance.world_tile_z, instance.uid};
        std::optional<formats::PickupDescription> description;
        // A world Pickup by its own fields (MSTS); any other object by its
        // .sd's ESD_Pickup_Object (RTS: "pickups not placed as Pickup").
        if (instance.kind == Kind::pickup) {
            if (const auto* object = world_object(key); object && object->pickup_type != 0) {
                formats::PickupDescription d;
                d.type = object->pickup_type;
                d.type_value = object->pickup_type_value;
                d.speed_min = object->pickup_speed_range[0];
                d.speed_max = object->pickup_speed_range[1];
                d.capacity = object->pickup_capacity[0];
                d.speed = object->pickup_capacity[1];
                d.from_world = true;
                description = std::move(d);
            }
        }
        if (!description) {
            auto* model = library.find(instance.source_path);
            if (!model || !model->pickup()) continue;
            description = *model->pickup();
        }
        auto& place = pickups_[key];
        if (!place.pickup) place.pickup = std::make_unique<simulation::Pickup>(std::move(*description), catalog_.kinds);
        place.transform = instance.transform;
        place.shape = instance.source_path.str();
        place.seen = true;
    }
}

void CargoView::say_state(Place& place, TrainView& trains) {
    const auto state = place.pickup->state();
    if (state == place.said_state) return;
    if (state == simulation::PickupState::done) trains.say(tr("Loading finished: the car is full"));
    if (state == simulation::PickupState::empty) trains.say(tr("The pickup is out of cargo"));
    place.said_state = state;
}

void CargoView::start_sound(Place& place, const std::uint32_t seed) {
    if (!sound_looked_up_) {
        sound_looked_up_ = true;
        // pickup.sms in the route's SOUND, else the global one.
        std::vector<core::VirtualPath> candidates;
        if (const auto folder = route_.resolve(std::string_view("SOUND"))) {
            if (auto file = folder->resolve(std::string_view("pickup.sms"))) candidates.push_back(*file);
        }
        if (const auto global = core::VirtualPath::parse("SOUND/pickup.sms")) candidates.push_back(*global);
        for (const auto& candidate : candidates) {
            if (!vfs_->exists(candidate)) continue;
            auto script = std::make_shared<formats::SoundScript>(formats::load_sound_script(*vfs_, candidate));
            if (!script->ok()) continue;
            sound_script_ = std::move(script);
            sound_path_ = candidate;
            break;
        }
    }
    if (!sound_script_ || place.sound) return;
    const auto* group = sound_script_->select_group(formats::SoundOwner::world);
    if (!group) return;
    auto lookup = [this](const std::string& name) -> std::shared_ptr<const audio::AudioClip> {
        const auto file = audio::find_sound_file(*vfs_, sound_path_, name);
        return file ? bank_.get(*file) : nullptr;
    };
    place.sound = std::make_unique<audio::SoundInstance>(sound_script_, *group, formats::SoundOwner::world, std::move(lookup), seed * 2654435761u);
}

void CargoView::stop_sound(audio::VoiceSink& sink) {
    for (auto& [_, place] : pickups_) {
        if (place.sound) place.sound->stop(sink);
        place.sound.reset();
    }
}

std::uint32_t CargoView::crane_trigger(const std::uint32_t loading, const bool unloading) {
    std::uint32_t code = 227; // a stack
    if (loading & formats::cargo_box) code = 221;
    else if (loading & formats::cargo_container) code = 223;
    else if (loading & formats::cargo_pile) code = 225;
    else if (loading & formats::cargo_stack) code = 227;
    else if (loading & formats::cargo_fridge) code = 229;
    return code + (unloading ? 1u : 0u);
}

void CargoView::update(float elapsed, const std::vector<renderer::StreamedInstance>& visible, renderer::ModelLibrary& library,
                       TrainView& trains, const glm::vec3& listener, audio::VoiceSink* sink, const float gain) {
    since_scan_ += elapsed;
    if (since_scan_ >= 1.0f || scanned_generation_ == ~0ull) {
        scan(visible, library);
        since_scan_ = 0.0f;
        scanned_generation_ = 0;
    }
    for (auto it = model_loading_.begin(); it != model_loading_.end();) {
        if (it->wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        auto asset = it->get();
        if (asset.ok() && !library.find(asset.source_path)) {
            library.add(std::move(asset));
        } else if (!asset.ok()) {
            std::cout << "cargo model '" << asset.source_path.str() << "': " << asset.error << std::endl;
        }
        it = model_loading_.erase(it);
    }

    streams_.clear();
    animate(elapsed, trains);
    for (auto& [key, place] : pickups_) {
        auto& pickup = *place.pickup;
        const glm::vec3 heard_at{place.transform[3]};
        // Its sound: "ready" / "off" when switched, the loop while it works.
        const auto sound_step = [&](const PickupSounds& sounds, const bool working) {
            const auto state = pickup.state();
            const bool on = state != simulation::PickupState::off;
            const bool was_on = place.sound_state != simulation::PickupState::off;
            if (sounds.ready && on != was_on) place.triggers.push_back(on ? sounds.ready : sounds.ready + 1);
            place.sound_state = state;
            const std::uint32_t loop = working && on ? sounds.loop : 0u;
            if (loop != (place.loop_stop ? place.loop_stop - 1 : 0u)) {
                if (place.loop_stop) place.triggers.push_back(place.loop_stop);
                if (loop) place.triggers.push_back(loop);
                place.loop_stop = loop ? loop + 1 : 0u;
            }
            if (!sink) return;
            const float hearing = glm::distance(listener, heard_at);
            if (hearing > 300.0f && place.triggers.empty()) {
                if (place.sound) {
                    place.sound->stop(*sink);
                    place.sound.reset();
                }
                return;
            }
            if (!place.sound && place.triggers.empty() && !place.loop_stop) return;
            start_sound(place, std::get<2>(key));
            if (!place.sound) return;
            audio::SoundInputs inputs{.camera = audio::SoundCamera::external, .position = heard_at};
            inputs.gain = gain;
            inputs[formats::SoundVariable::distance] = hearing;
            inputs[formats::SoundVariable::variable1] = place.fill;
            inputs.fades_with_distance = true;
            inputs.discrete = std::exchange(place.triggers, {});
            place.sound->update(elapsed, inputs, *sink);
        };
        if (pickup.state() == simulation::PickupState::off) {
            place.too_fast_said = place.unsuitable_said = place.window_shown = false;
            place.said_state = pickup.state();
            if (place.sound || place.sound_state != simulation::PickupState::off || !place.triggers.empty())
                sound_step(pickup_sounds(pickup.description().type, pickup.cargo(), false), false);
            continue;
        }
        const auto& d = pickup.description();
        const glm::vec3 origin{place.transform[3]};
        auto cars = trains.cargo_cars(origin, car_reach);
        std::vector<simulation::PickupCar> seen(cars.size());
        std::vector<glm::vec3> pour_from(cars.size(), origin);
        std::vector<glm::vec3> pour_local(cars.size(), glm::vec3{0.0f});
        std::vector<glm::vec3> points;
        for (const auto& p : d.points) points.push_back(glm::vec3{place.transform * glm::vec4{p[0], p[1], p[2], 1.0f}});
        if (points.empty()) points.push_back(origin);
        const glm::mat4 to_pickup = glm::inverse(place.transform);
        for (std::size_t i = 0; i < cars.size(); ++i) {
            const auto& car = cars[i];
            auto& pc = seen[i];
            pc.cargo = car.cargo;
            pc.speed = car.speed;
            pc.train = car.train;
            pc.index = car.car;
            pc.engine = car.engine;
            pc.diesel = car.diesel;
            const glm::mat4 to_car = glm::inverse(car.transform);
            const auto* description = car.cargo ? car.cargo->description() : nullptr;
            if (pickup.crane() && (d.area || d.type != 15)) {
                // A crane: the car's middle in its area, else near it.
                const glm::vec3 middle{car.transform * glm::vec4{0.0f, 1.5f, 0.0f, 1.0f}};
                if (d.area) {
                    const glm::vec3 in{to_pickup * glm::vec4{middle, 1.0f}};
                    pc.in_reach = in.x >= d.area->low[0] && in.x <= d.area->high[0] && in.z >= d.area->low[2] && in.z <= d.area->high[2]
                                  && in.y >= d.area->low[1] - 3.0f && in.y <= d.area->high[1] + 3.0f;
                } else {
                    pc.in_reach = glm::distance(middle, origin) <= (std::max)(d.length * 0.5f, 15.0f);
                }
                pc.area = 0;
                continue;
            }
            for (const auto& point : points) {
                const glm::vec3 local{to_car * glm::vec4{point, 1.0f}};
                const bool along_platform = d.length > 0.0f || d.type == 13 || d.type == 15 || d.type == 16 || d.type == 8;
                if (along_platform) {
                    // A platform or trestle: within its length of the point, the
                    // car beside or over it.
                    if (std::abs(local.x) <= 4.5f && std::abs(local.z) <= car.length * 0.5f + d.length * 0.5f + d.margin) {
                        pc.in_reach = true;
                        pc.area = 0;
                    }
                } else if (std::abs(local.x) <= 3.0f && description) {
                    // A loader over the car: its point over an area, else over an
                    // intake point (within its tolerance), else over the car.
                    std::optional<int> area;
                    if (!description->areas.empty()) {
                        area = car.cargo->area_at(local.z, 0.2f);
                    } else if (!description->intakes.empty()) {
                        for (const auto& intake : description->intakes) {
                            if (std::abs(local.z - intake.position[2]) <= (std::max)(intake.tolerance, 0.3f)) area = 0;
                        }
                    } else if (std::abs(local.z) <= car.length * 0.5f) {
                        area = 0;
                    }
                    if (area) {
                        pc.in_reach = true;
                        pc.area = area;
                    }
                } else if (std::abs(local.x) <= 3.0f && std::abs(local.z) <= car.length * 0.5f) {
                    pc.in_reach = true; // a car that takes nothing: said unsuitable
                    pc.area = 0;
                }
                if (pc.in_reach) {
                    pour_from[i] = point;
                    pour_local[i] = local;
                    break;
                }
            }
        }
        // OPENRTS_CARGO_DEBUG_SPEED (automated runs): the pickups' pace scaled.
        static const float pace = std::getenv("OPENRTS_CARGO_DEBUG_SPEED") ? static_cast<float>(std::atof(std::getenv("OPENRTS_CARGO_DEBUG_SPEED"))) : 1.0f;
        const auto step = pickup.step(elapsed * pace, seen);
        if (step.car >= 0) {
            const auto& car = cars[static_cast<std::size_t>(step.car)];
            if (car.cargo) place.fill = car.cargo->fill_percent();
            // Its hatches open while it is emptied (or filled, as its
            // LoadingAnimation says).
            if (car.cargo && step.volume != 0.0f && car.cargo->description() && !car.cargo->description()->animations.empty())
                openings_[{car.unit, car.car}] = step.volume < 0.0f ? 2u : 1u;
            // A trestle's dump: the cargo falling from under the car.
            if (step.volume < 0.0f && pickup.unloads() && car.cargo) {
                const auto* description = car.cargo->description();
                const formats::CargoKind* kind = nullptr;
                for (const auto& load : car.cargo->loads()) {
                    if (load.object.empty()) kind = catalog_.kinds.find(load.cargo);
                }
                if (!kind && !car.cargo->loads().empty()) kind = catalog_.kinds.find(car.cargo->loads().front().cargo);
                if (!kind) kind = catalog_.kinds.find("coal");
                const auto texture = kind ? catalog_.texture(*kind) : std::nullopt;
                const auto path = texture ? texture->parent().resolve(renderer::cargo_stream_name(texture->filename())) : std::nullopt;
                std::vector<glm::vec3> mouths;
                if (description) {
                    for (const auto& dump : description->dumps) mouths.push_back({dump.position[0], dump.position[1], dump.position[2]});
                    if (mouths.empty()) {
                        for (const auto& area : description->areas)
                            mouths.push_back({(area.box.low[0] + area.box.high[0]) * 0.5f, area.box.low[1], (area.box.low[2] + area.box.high[2]) * 0.5f});
                    }
                }
                if (mouths.empty()) mouths.push_back({0.0f, 1.0f, 0.0f});
                for (const auto& mouth : mouths) {
                    if (!path) break;
                    const glm::vec3 top{car.transform * glm::vec4{mouth, 1.0f}};
                    const glm::vec3 bottom{car.transform * glm::vec4{mouth.x, -0.6f, mouth.z, 1.0f}};
                    if (top.y <= bottom.y + 0.1f) continue;
                    stream_turn_ += elapsed * 9.0f;
                    glm::mat4 transform = glm::translate(glm::mat4{1.0f}, bottom);
                    transform = glm::rotate(transform, stream_turn_, glm::vec3{0.0f, 1.0f, 0.0f});
                    transform = glm::scale(transform, glm::vec3{stream_radius * 2.4f, top.y - bottom.y, stream_radius * 2.4f});
                    streams_.push_back({*path, transform});
                }
            }
        }
        {
            const bool engine = step.engine;
            const bool working = step.volume != 0.0f;
            sound_step(pickup_sounds(pickup.description().type, pickup.cargo(), engine), working);
        }
        // OPENRTS_CARGO_DEBUG: what a pickup switched on sees, each second.
        static const bool debug = std::getenv("OPENRTS_CARGO_DEBUG") != nullptr;
        if (debug && since_scan_ == 0.0f) {
            std::cout << "cargo debug: pickup " << std::get<2>(key) << " state " << static_cast<int>(pickup.state()) << ", " << cars.size()
                      << " cars, step car " << step.car << " volume " << step.volume << std::endl;
            for (std::size_t i = 0; i < cars.size(); ++i) {
                const glm::vec3 local{glm::inverse(cars[i].transform) * glm::vec4{points.front(), 1.0f}};
                std::cout << "  car " << cars[i].label << " at " << local.x << " " << local.y << " " << local.z << " speed " << cars[i].speed
                          << " reach " << seen[i].in_reach << " area " << (seen[i].area ? *seen[i].area : -9) << " desc "
                          << (cars[i].cargo && cars[i].cargo->description() ? cars[i].cargo->description()->name : "-") << std::endl;
            }
        }
        if (step.car >= 0 && step.volume != 0.0f) trains.cargo_changed(cars[static_cast<std::size_t>(step.car)].unit, cars[static_cast<std::size_t>(step.car)].car);
        // The activity window's line for it.
        if (step.car >= 0 && (step.volume != 0.0f || step.engine)) {
            const auto& c = cars[static_cast<std::size_t>(step.car)];
            Activity a;
            a.pickup = tr(formats::pickup_type_name(pickup.description().type));
            if (pickup.cargo()) a.cargo = catalog_.name(*pickup.cargo());
            else if (c.cargo && !c.cargo->loads().empty())
                if (const auto* kind = catalog_.kinds.find(c.cargo->loads().front().cargo)) a.cargo = catalog_.name(*kind);
            a.car = c.car + 1;
            a.loading = step.volume >= 0.0f;
            a.engine = step.engine;
            a.fill = c.cargo ? c.cargo->fill_percent() : 0.0f;
            a.mass = c.cargo ? c.cargo->load_mass() : 0.0f;
            a.stock = pickup.stock();
            bool found_line = false;
            for (auto& old : activity_) {
                if (old.pickup == a.pickup && old.car == a.car) {
                    old = a;
                    found_line = true;
                }
            }
            if (!found_line) activity_.push_back(a);
        }
        // The falling cargo: from over the car (RTS's disc in the air) down to
        // the pile.
        if (step.pouring && step.car >= 0 && pickup.cargo()) {
            const auto c = static_cast<std::size_t>(step.car);
            const auto& car = cars[c];
            const auto* description = car.cargo->description();
            float floor = 1.3f, top = 4.5f;
            const int area = seen[c].area.value_or(0);
            if (description && area >= 1 && static_cast<std::size_t>(area) <= description->areas.size()) {
                const auto& box = description->areas[static_cast<std::size_t>(area - 1)].box;
                const float room = car.cargo->room(area);
                const float full = room > 0.0f ? car.cargo->bulk_volume(area) / room : 0.0f;
                floor = box.low[1] + (box.high[1] - box.low[1]) * (std::min)(full * 1.2f, 1.1f);
                top = box.high[1] + 2.5f;
            } else if (description && !description->intakes.empty()) {
                floor = description->intakes.front().position[1] - 0.5f;
                top = description->intakes.front().position[1] + 2.5f;
            }
            const glm::vec3 bottom{car.transform * glm::vec4{pour_local[c].x, floor, pour_local[c].z, 1.0f}};
            const float above = (std::max)(glm::vec3{car.transform * glm::vec4{pour_local[c].x, top, pour_local[c].z, 1.0f}}.y, pour_from[c].y);
            if (const auto texture = catalog_.texture(*pickup.cargo()); texture && above > bottom.y + 0.2f) {
                if (const auto path = texture->parent().resolve(renderer::cargo_stream_name(texture->filename()))) {
                    // Turned a little each frame, its width wavering: the
                    // texture runs over it as falling lumps would.
                    stream_turn_ += elapsed * 9.0f;
                    const float width = stream_radius * 2.0f * (1.0f + 0.12f * std::sin(stream_turn_ * 2.3f));
                    glm::mat4 transform = glm::translate(glm::mat4{1.0f}, bottom);
                    transform = glm::rotate(transform, stream_turn_, glm::vec3{0.0f, 1.0f, 0.0f});
                    transform = glm::scale(transform, glm::vec3{width, above - bottom.y, width});
                    streams_.push_back({*path, transform});
                }
            }
        }
        if (step.too_fast && !place.too_fast_said) {
            trains.say(tr("Too fast for the pickup: stop the car under it"));
            place.too_fast_said = true;
        } else if (!step.too_fast) {
            place.too_fast_said = false;
        }
        if (step.unsuitable && !place.unsuitable_said) {
            trains.say(tr("This car does not take this cargo"));
            place.unsuitable_said = true;
        }
        say_state(place, trains);
        // A crane with a standing car: its window, once per activation.
        if (step.dialog && !place.window_shown && !window_) {
            Window window;
            window.pickup = key;
            for (std::size_t i = 0; i < cars.size(); ++i) {
                if (!seen[i].in_reach || cars[i].speed > 0.5f) continue;
                window.cars.emplace_back(cars[i].unit, cars[i].car);
                window.labels.push_back(cars[i].label);
                if (static_cast<int>(i) == step.car) window.car = static_cast<int>(window.cars.size() - 1);
            }
            if (!window.cars.empty()) {
                window_ = std::move(window);
                place.window_shown = true;
            }
        }
    }
}

void CargoView::animate(const float elapsed, TrainView& trains) {
    constexpr float hook_speed = 1.2f; // m/s a crane lowers and lifts
    const auto cable = [&](const glm::vec3& from, const float length) {
        // The crane's rope: a thin column of a dark cargo's texture.
        const auto* kind = catalog_.kinds.find("coal");
        const auto texture = kind ? catalog_.texture(*kind) : std::nullopt;
        const auto path = texture ? texture->parent().resolve(renderer::cargo_stream_name(texture->filename())) : std::nullopt;
        if (!path || length <= 0.0f) return;
        streams_.push_back({*path, glm::scale(glm::translate(glm::mat4{1.0f}, from), glm::vec3{0.05f, length, 0.05f})});
    };
    for (auto& [_, place] : pickups_) place.crane_target.reset();

    // LoadingAnimation: open while the car is worked on, else shutting.
    for (auto it = openings_.begin(); it != openings_.end();) {
        auto* cargo = trains.car_cargo(it->first.first, it->first.second);
        if (!cargo || !cargo->description()) {
            it = openings_.erase(it);
            continue;
        }
        float time = 2.0f;
        for (const auto& animation : cargo->description()->animations) time = (std::max)(animation.time, 0.1f);
        if (it->second) {
            cargo->opening_for = it->second;
            cargo->opening = (std::min)(1.0f, cargo->opening + elapsed / time);
            it->second = 0;
            ++it;
        } else {
            cargo->opening = (std::max)(0.0f, cargo->opening - elapsed / time);
            it = cargo->opening <= 0.0f ? openings_.erase(it) : std::next(it);
        }
    }

    // A crane lowering objects onto a car, one at a time.
    for (auto it = lowering_.begin(); it != lowering_.end();) {
        auto* cargo = trains.car_cargo(it->unit, it->car);
        const auto body = trains.car_body(it->unit, it->car);
        if (!cargo || !body) {
            if (cargo) {
                for (auto& load : cargo->loads_seen()) load.arriving = 0, load.lift = 0.0f;
            }
            it = lowering_.erase(it);
            continue;
        }
        simulation::CarLoad* hanging = nullptr;
        for (auto& load : cargo->loads_seen()) {
            if (load.arriving > 0) {
                hanging = &load;
                break;
            }
        }
        if (!hanging) {
            it = lowering_.erase(it);
            continue;
        }
        if (hanging->lift <= 0.0f) hanging->lift = it->drop;
        hanging->lift = (std::max)(0.0f, hanging->lift - hook_speed * elapsed);
        const int level = (std::max)(0, hanging->count - hanging->arriving);
        const glm::vec3 slot{hanging->position[0], hanging->position[1] + static_cast<float>(level) * hanging->extent[1], hanging->position[2]};
        const glm::vec3 top{*body * glm::vec4{slot.x, slot.y + hanging->lift + hanging->extent[1], slot.z, 1.0f}};
        cable(top, it->drop - hanging->lift + 3.0f);
        const auto place = pickups_.find(it->pickup);
        if (place != pickups_.end()) {
            place->second.crane_target = glm::vec3{*body * glm::vec4{slot, 1.0f}};
            place->second.crane_hook = hanging->lift + hanging->extent[1];
        }
        if (hanging->lift <= 0.0f) {
            // Set down: the next on the hook, the crane's sound of it.
            --hanging->arriving;
            hanging->lift = hanging->arriving > 0 ? it->drop : 0.0f;
            if (place != pickups_.end()) {
                const auto* object = catalog_.object(hanging->object);
                place->second.triggers.push_back(crane_trigger(object ? object->loading : formats::cargo_stack, false));
            }
        }
        ++it;
    }

    // Objects lifted off cars: rising on the rope, then gone.
    for (auto it = lifted_.begin(); it != lifted_.end();) {
        const auto body = trains.car_body(it->unit, it->car);
        it->rise += hook_speed * elapsed;
        const auto place = pickups_.find(it->pickup);
        if (!body || it->rise >= it->top) {
            if (place != pickups_.end()) place->second.triggers.push_back(crane_trigger(it->loading, true));
            it = lifted_.erase(it);
            continue;
        }
        const glm::mat4 world = *body * glm::translate(glm::mat4{1.0f}, glm::vec3{0.0f, it->rise, 0.0f}) * it->draw.local;
        streams_.push_back({it->draw.path, world});
        const glm::vec3 base{world * glm::vec4{0.0f, 0.0f, 0.0f, 1.0f}};
        cable(base + glm::vec3{0.0f, it->height, 0.0f}, it->top - it->rise + 3.0f);
        if (place != pickups_.end()) {
            place->second.crane_target = base - glm::vec3{0.0f, it->rise, 0.0f};
            place->second.crane_hook = it->rise + it->height;
        }
        ++it;
    }
}

std::optional<std::vector<renderer::NodeAdjustment>> CargoView::adjustments(const renderer::StreamedInstance& instance,
                                                                           renderer::ModelLibrary& library) const {
    if (lowering_.empty() && lifted_.empty()) return std::nullopt;
    const auto found = pickups_.find(Key{instance.world_tile_x, instance.world_tile_z, instance.uid});
    if (found == pickups_.end() || !found->second.crane_target) return std::nullopt;
    const auto& place = found->second;
    const auto& nodes = place.pickup->description().crane_nodes;
    if (!nodes) return std::nullopt;
    const auto* model = library.find(instance.source_path);
    const auto* rig = model ? model->rig() : nullptr;
    if (!rig) return std::nullopt;
    // PickupCraneShapeNodes in the crane's own space: its bridge runs along
    // PickupCraneOriented's axis, its cabin across it, its hook goes down
    // from HookInitialHeight to the load (each node from its rest).
    const glm::vec3 target{glm::inverse(place.transform) * glm::vec4{*place.crane_target, 1.0f}};
    glm::vec3 along_axis{0.0f, 0.0f, 1.0f}, across_axis{1.0f, 0.0f, 0.0f};
    switch (place.pickup->description().crane_oriented) {
    case 1: along_axis = {1.0f, 0.0f, 0.0f}, across_axis = {0.0f, 0.0f, 1.0f}; break;
    case 2: along_axis = {0.0f, 0.0f, -1.0f}, across_axis = {-1.0f, 0.0f, 0.0f}; break;
    case 3: along_axis = {-1.0f, 0.0f, 0.0f}, across_axis = {0.0f, 0.0f, -1.0f}; break;
    default: break;
    }
    const float along = glm::dot(target, along_axis);
    const float across = glm::dot(target, across_axis);
    const float drop = target.y + place.crane_hook - nodes->hook_height;
    std::vector<renderer::NodeAdjustment> out;
    for (std::size_t m = 0; m < rig->matrices.size(); ++m) {
        const auto& name = rig->matrices[m].name;
        glm::vec3 move{0.0f};
        if (!nodes->bridge.empty() && same_name(name, nodes->bridge)) move = along_axis * along;
        else if (!nodes->cabin.empty() && same_name(name, nodes->cabin)) move = across_axis * across;
        else if (!nodes->hook.empty() && same_name(name, nodes->hook)) move = glm::vec3{0.0f, drop, 0.0f};
        else continue;
        out.push_back({.matrix = static_cast<std::int32_t>(m), .local = glm::translate(glm::mat4{1.0f}, move)});
    }
    if (out.empty()) return std::nullopt;
    return out;
}

std::optional<float> CargoView::pick(const glm::vec3& eye, const glm::mat4& view_projection, glm::vec2 aim) {
    picked_.reset();
    std::optional<float> best;
    // OPENRTS_CARGO_DEBUG (automated runs): the screen's middle, not the
    // cursor wherever it was left.
    if (std::getenv("OPENRTS_CARGO_DEBUG")) aim = glm::vec2{0.0f};
    for (const auto& [key, place] : pickups_) {
        if (!place.seen) continue;
        std::vector<glm::vec3> spots{glm::vec3{place.transform[3]}};
        for (const auto& p : place.pickup->description().points) spots.push_back(glm::vec3{place.transform * glm::vec4{p[0], p[1], p[2], 1.0f}});
        for (const auto& p : spots) {
            for (const float lift : {0.0f, 2.0f}) {
                const glm::vec3 at = p + glm::vec3{0.0f, lift, 0.0f};
                const float d = glm::distance(at, eye);
                if (d > pick_reach) continue;
                const glm::vec4 clip = view_projection * glm::vec4{at, 1.0f};
                if (!(clip.w > 0.0f && std::abs(clip.x) <= clip.w && std::abs(clip.y) <= clip.w)) continue;
                const float score = glm::distance(glm::vec2{clip.x, clip.y} / clip.w, aim) + 0.01f * d;
                static const bool debug = std::getenv("OPENRTS_CARGO_DEBUG") != nullptr;
                if (debug) std::cout << "cargo debug: pick " << std::get<2>(key) << " lift " << lift << " distance " << d << " score " << score << std::endl;
                if (!best || score < *best) {
                    best = score;
                    picked_ = key;
                }
            }
        }
    }
    return best;
}

void CargoView::use(bool unload, TrainView& trains) {
    if (!picked_) return;
    const auto found = pickups_.find(*picked_);
    if (found == pickups_.end()) return;
    auto& place = found->second;
    auto& pickup = *place.pickup;
    const char* said = pickup.toggle(unload);
    // OPENRTS_CARGO_DEBUG=active (automated runs): one Num3 switches it on.
    if (const char* debug = std::getenv("OPENRTS_CARGO_DEBUG"); debug && (std::string_view(debug) == "active" || std::string_view(debug) == "load")
                                                                  && pickup.state() == simulation::PickupState::ready)
        said = pickup.toggle(false);
    place.window_shown = false;
    place.too_fast_said = place.unsuitable_said = false;
    place.said_state = pickup.state();
    if (window_ && window_->pickup == found->first && pickup.state() == simulation::PickupState::off) window_.reset();
    std::cout << "cargo: Num3 on pickup " << std::get<2>(found->first) << ": " << said << std::endl;
    std::string text = tr(said);
    text += " (";
    text += tr(formats::pickup_type_name(pickup.description().type));
    if (const auto* kind = pickup.cargo()) text += ": " + catalog_.name(*kind);
    text += ")";
    trains.say(std::move(text));
}

void CargoView::submit(renderer::ModelRenderer& renderer, renderer::ModelLibrary& library) {
    for (const auto& stream : streams_) {
        if (auto* model = library.find(stream.path)) {
            renderer.submit(*model, stream.transform);
            continue;
        }
        if (std::find(stream_models_asked_.begin(), stream_models_asked_.end(), stream.path) != stream_models_asked_.end()) continue;
        stream_models_asked_.push_back(stream.path);
        model_loading_.push_back(std::async(std::launch::async, [vfs = vfs_, path = stream.path] {
            return renderer::load_model_asset(*vfs, path, {});
        }));
    }
}

std::string CargoView::report() const {
    std::size_t on = 0;
    for (const auto& [_, place] : pickups_) on += place.pickup->state() != simulation::PickupState::off ? 1 : 0;
    return "pickups " + std::to_string(pickups_.size()) + " (" + std::to_string(on) + " on), streams " + std::to_string(streams_.size());
}

// ---- the crane's window -------------------------------------------------------

std::vector<CargoView::Offer> CargoView::offers(const Place& place) const {
    std::vector<Offer> result;
    const auto& d = place.pickup->description();
    const auto bits = formats::pickup_loading(d.type);
    const auto label = [](Offer& offer, std::string name) {
        offer.label = std::move(name) + (offer.mass_maximum > offer.mass ? trf(" [%.0f-%.0f t]", offer.mass * 0.001f, offer.mass_maximum * 0.001f)
                                                                         : trf(" [%.1f t]", offer.mass * 0.001f));
    };
    const auto bulk = [&](const formats::CargoKind& kind, float mass) {
        Offer offer;
        offer.kind = &kind;
        offer.mass = mass;
        offer.mass_maximum = 100000.0f;
        label(offer, catalog_.name(kind));
        result.push_back(std::move(offer));
    };
    if (!d.table.empty()) {
        for (const auto& item : d.table) {
            if (item.object.empty()) {
                if (const auto* kind = catalog_.kinds.find(item.cargo)) bulk(*kind, item.mass > 0.0f ? item.mass : 10000.0f);
                continue;
            }
            const auto* object = catalog_.object(item.object);
            if (!object) continue;
            Offer offer;
            offer.object = object;
            offer.kind = catalog_.kinds.find(object->cargo.empty() ? item.cargo : object->cargo);
            offer.mass = item.mass >= 0.0f ? item.mass : object->mass;
            offer.mass_maximum = (std::max)(object->mass_maximum, offer.mass);
            offer.count = item.count;
            label(offer, object->name);
            result.push_back(std::move(offer));
        }
        return result;
    }
    // No table: every object and bulk kind of its loading types (RTS's one
    // list for all: a flat car may take a covered car's load).
    for (const auto& object : catalog_.objects) {
        if (!(object.loading & bits)) continue;
        Offer offer;
        offer.object = &object;
        offer.kind = catalog_.kinds.find(object.cargo);
        offer.mass = object.mass;
        offer.mass_maximum = (std::max)(object.mass_maximum, object.mass);
        label(offer, object.name);
        result.push_back(std::move(offer));
    }
    if (bits & formats::cargo_pile) {
        for (const auto& kind : catalog_.kinds.all()) {
            if (kind.loading & formats::cargo_pile) bulk(kind, 10000.0f);
        }
    }
    return result;
}

void CargoView::close_window() {
    if (!window_) return;
    // As RTS after its dialog: the pickup ready again.
    if (const auto found = pickups_.find(window_->pickup); found != pickups_.end()) found->second.pickup->set_state(simulation::PickupState::ready);
    window_.reset();
}

void CargoView::load_selected(Place& place, TrainView& trains) {
    auto& w = *window_;
    const auto all = offers(place);
    if (w.item < 0 || static_cast<std::size_t>(w.item) >= all.size()) return;
    const auto& [unit, car] = w.cars[static_cast<std::size_t>(w.car)];
    auto* cargo = trains.car_cargo(unit, car);
    if (!cargo) return;
    const auto& offer = all[static_cast<std::size_t>(w.item)];
    const float mass = std::clamp(w.weight_t * 1000.0f, 1.0f, (std::max)(offer.mass_maximum, offer.mass));
    if (offer.object) {
        const bool many = (offer.object->loading & (formats::cargo_stack | formats::cargo_box)) != 0;
        const int count = many ? (std::max)(1, w.amount) : 1;
        std::string why;
        std::vector<int> before;
        for (const auto& load : cargo->loads()) before.push_back(load.count);
        const int placed = cargo->place_objects(*offer.object, offer.kind ? offer.kind->code : 0, mass, count, w.area,
                                                static_cast<simulation::CargoPlacement>(w.placement), w.centred, &why);
        if (placed > 0) {
            // On the crane's hook first: lowered onto the car one by one.
            auto& loads = cargo->loads_seen();
            for (std::size_t i = 0; i < loads.size(); ++i) {
                const int added = loads[i].count - (i < before.size() ? before[i] : 0);
                if (added > 0) loads[i].arriving += added;
            }
            const bool known = std::any_of(lowering_.begin(), lowering_.end(), [&](const Lowering& l) { return l.unit == unit && l.car == car; });
            if (!known) lowering_.push_back({.pickup = window_->pickup, .unit = unit, .car = car, .drop = 6.0f});
            trains.cargo_changed(unit, car);
            place.pickup->take(mass * static_cast<float>(placed));
            trains.say(trf("Loaded: %s x%d", offer.object->name.c_str(), placed));
        }
        if (!why.empty()) trains.say(tr(why.c_str()));
    } else if (offer.kind && offer.kind->density > 0.0f) {
        const float moved = cargo->move_bulk(*offer.kind, mass / offer.kind->density, w.area);
        if (moved > 0.0f) {
            trains.cargo_changed(unit, car);
            place.pickup->take(moved * offer.kind->density);
            trains.say(trf("Loaded: %s, %.1f t", catalog_.name(*offer.kind).c_str(), moved * offer.kind->density * 0.001f));
            place.triggers.push_back(crane_trigger(formats::cargo_pile, false));
        } else {
            trains.say(tr("Wagon maximum weight limit reached"));
        }
    }
}

void CargoView::unload_selected(Place& place, TrainView& trains, const std::optional<std::size_t> index) {
    auto& w = *window_;
    const auto& [unit, car] = w.cars[static_cast<std::size_t>(w.car)];
    auto* cargo = trains.car_cargo(unit, car);
    if (!cargo) return;
    // What goes up on the crane's rope (objects; bulk is grabbed away).
    const auto lift = [&](const simulation::CarLoad& load) {
        if (load.object.empty()) {
            place.triggers.push_back(crane_trigger(formats::cargo_pile, true));
            return;
        }
        simulation::CarCargo one(cargo->description(), {});
        auto shown = load;
        shown.arriving = 0;
        shown.lift = 0.0f;
        one.restore({shown});
        const auto* object = catalog_.object(load.object);
        for (const auto& draw : cargo_draws(catalog_, one)) {
            lifted_.push_back({.pickup = w.pickup,
                               .unit = unit,
                               .car = car,
                               .draw = draw,
                               .rise = 0.0f,
                               .top = 7.0f,
                               .height = load.extent[1],
                               .loading = object ? object->loading : formats::cargo_stack});
        }
    };
    if (index) {
        if (*index >= cargo->loads().size()) return;
        lift(cargo->loads()[*index]);
        cargo->unload(*index);
    } else {
        for (const auto& load : cargo->loads()) {
            if (w.area == 0 || load.area == w.area) lift(load);
        }
        cargo->unload_all(w.area);
    }
    trains.cargo_changed(unit, car);
    w.loaded = -1;
}

void CargoView::draw_activity() {
    const float dt = ImGui::GetIO().DeltaTime;
    for (auto& a : activity_) a.age += dt;
    std::erase_if(activity_, [](const Activity& a) { return a.age > 3.0f; });
    if (activity_.empty()) return;
    const auto& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 20.0f, 120.0f), ImGuiCond_FirstUseEver, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.8f);
    if (ImGui::Begin((std::string(tr("Cargo loading")) + "###cargo_activity").c_str(), nullptr,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoFocusOnAppearing)) {
        for (const auto& a : activity_) {
            ImGui::TextUnformatted(trf("Pickup type: %s    Current cargo: %s", a.pickup.c_str(), a.cargo.empty() ? "-" : a.cargo.c_str()).c_str());
            if (a.engine) {
                ImGui::TextUnformatted(trf("Car %zu: the locomotive's supplies", a.car).c_str());
            } else {
                ImGui::TextUnformatted(trf(a.loading ? "Car %zu: loading, %.0f %%, %.1f t" : "Car %zu: unloading, %.0f %%, %.1f t", a.car, a.fill,
                                           a.mass * 0.001f)
                                           .c_str());
                ImGui::ProgressBar(a.fill / 100.0f, ImVec2(260.0f, 0.0f));
            }
            if (a.stock >= 0.0f) ImGui::TextUnformatted(trf("Stock: %.1f t", a.stock * 0.001f).c_str());
            if (a.age > 0.5f) ImGui::TextDisabled("%s", tr("(stopped)"));
            ImGui::Separator();
        }
    }
    ImGui::End();
}

void CargoView::draw_ui(TrainView& trains) {
    draw_activity();
    if (!window_) return;
    const auto found = pickups_.find(window_->pickup);
    if (found == pickups_.end() || window_->cars.empty()) {
        window_.reset();
        return;
    }
    auto& place = found->second;
    auto& w = *window_;
    w.car = std::clamp(w.car, 0, static_cast<int>(w.cars.size()) - 1);
    const auto& [unit, car] = w.cars[static_cast<std::size_t>(w.car)];
    auto* cargo = trains.car_cargo(unit, car);
    if (!cargo) {
        window_.reset();
        return;
    }
    const auto* description = cargo->description();
    const auto& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(900.0f, 0.0f), ImGuiCond_Appearing);
    bool open = true;
    const std::string title = std::string(tr("Load cargo")) + " — " + tr(formats::pickup_type_name(place.pickup->description().type)) + "###cargo_window";
    if (!ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        if (!open) close_window();
        return;
    }
    const auto all = offers(place);
    // OPENRTS_CARGO_DEBUG=load (automated runs): the first offer loaded at once.
    // (OPENRTS_CARGO_DEBUG_DELAY: that many seconds after the window opened.)
    if (w.opened < 0.0) w.opened = ImGui::GetTime();
    const char* delay = std::getenv("OPENRTS_CARGO_DEBUG_DELAY");
    if (const char* debug = std::getenv("OPENRTS_CARGO_DEBUG"); debug && std::string_view(debug) == "load" && w.item < 0 && !all.empty()
        && ImGui::GetTime() - w.opened >= (delay ? std::atof(delay) : 0.0)) {
        w.item = 0;
        w.weight_t = all.front().mass * 0.001f;
        load_selected(place, trains);
    }
    if (ImGui::BeginTable("##cargo_columns", 3, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        // The car, its area, how the load is placed.
        ImGui::TextUnformatted(tr("Wagon"));
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##cargo_wagon", w.labels[static_cast<std::size_t>(w.car)].c_str())) {
            for (int i = 0; i < static_cast<int>(w.labels.size()); ++i) {
                if (ImGui::Selectable(w.labels[static_cast<std::size_t>(i)].c_str(), i == w.car)) {
                    w.car = i;
                    w.area = 0;
                    w.loaded = -1;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::TextUnformatted(tr("Area"));
        if (ImGui::BeginListBox("##cargo_area", ImVec2(-1.0f, 110.0f))) {
            const auto areas = description ? description->areas.size() : 0;
            if (ImGui::Selectable(areas ? tr("All areas") : tr("The whole car"), w.area == 0)) w.area = 0;
            for (std::size_t a = 0; a < areas; ++a) {
                const auto& box = description->areas[a].box;
                const auto text = trf("Area %d: %.1f x %.1f x %.1f m", static_cast<int>(a + 1), box.high[0] - box.low[0], box.high[1] - box.low[1],
                                      box.high[2] - box.low[2]);
                if (ImGui::Selectable(text.c_str(), w.area == static_cast<int>(a + 1))) w.area = static_cast<int>(a + 1);
            }
            ImGui::EndListBox();
        }
        ImGui::TextUnformatted(tr("Placement"));
        const char* placements[] = {tr("Placement: center"), tr("Placement: front"), tr("Placement: back"), tr("Placement: manual (one item)")};
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::Combo("##cargo_placement", &w.placement, placements, 4);
        ImGui::Checkbox(tr("Centred across the car"), &w.centred);
        ImGui::Spacing();
        ImGui::TextUnformatted(trf("Loaded: %.0f %%, %.1f t", cargo->fill_percent(), cargo->load_mass() * 0.001f).c_str());
        if (cargo->mass_limit() > 0.0f) ImGui::TextUnformatted(trf("Limit: %.1f t", cargo->mass_limit() * 0.001f).c_str());

        ImGui::TableNextColumn();
        // The cargo offered.
        ImGui::TextUnformatted(tr("Cargo type"));
        const char* filters[] = {tr("Cargo: all"), tr("Cargo: bulk"), tr("Cargo: liquid"), tr("Cargo: containers"), tr("Cargo: stacks"),
                                 tr("Cargo: boxes"), tr("Cargo: refrigerated")};
        constexpr std::uint32_t filter_bits[] = {0,
                                                 formats::cargo_pile,
                                                 formats::cargo_liquid,
                                                 formats::cargo_container,
                                                 formats::cargo_stack,
                                                 formats::cargo_box,
                                                 formats::cargo_fridge};
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::Combo("##cargo_filter", &w.filter, filters, 7);
        if (ImGui::BeginListBox("##cargo_items", ImVec2(-1.0f, 200.0f))) {
            for (int i = 0; i < static_cast<int>(all.size()); ++i) {
                const auto& offer = all[static_cast<std::size_t>(i)];
                const auto loading = offer.object ? offer.object->loading : (offer.kind ? offer.kind->loading : 0u);
                const auto bits = filter_bits[std::clamp(w.filter, 0, 6)];
                if (bits && !(loading & bits)) continue;
                ImGui::PushID(i);
                if (ImGui::Selectable(offer.label.c_str(), i == w.item)) {
                    w.item = i;
                    w.weight_t = offer.mass * 0.001f;
                }
                ImGui::PopID();
            }
            ImGui::EndListBox();
        }
        if (all.empty()) ImGui::TextWrapped("%s", tr("This pickup offers no cargo (no CargoTable, no cargo objects of its loading types)."));
        const bool chosen = w.item >= 0 && static_cast<std::size_t>(w.item) < all.size();
        const auto* offer = chosen ? &all[static_cast<std::size_t>(w.item)] : nullptr;
        ImGui::BeginDisabled(!offer || offer->mass_maximum <= offer->mass);
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputFloat(tr("Weight, t"), &w.weight_t, 1.0f, 5.0f, "%.1f");
        ImGui::EndDisabled();
        const bool many = offer && offer->object && (offer->object->loading & (formats::cargo_stack | formats::cargo_box));
        ImGui::BeginDisabled(!many);
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputInt(tr("Amount"), &w.amount);
        w.amount = std::clamp(w.amount, 1, 10000);
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!offer);
        if (ImGui::Button(tr("Load"), ImVec2(-1.0f, 0.0f))) load_selected(place, trains);
        ImGui::EndDisabled();

        ImGui::TableNextColumn();
        // What the car has.
        ImGui::TextUnformatted(tr("Loaded cargo"));
        if (ImGui::BeginListBox("##cargo_loaded", ImVec2(-1.0f, 260.0f))) {
            const auto& loads = cargo->loads();
            for (int i = 0; i < static_cast<int>(loads.size()); ++i) {
                const auto& load = loads[static_cast<std::size_t>(i)];
                if (w.area != 0 && load.area != w.area) continue;
                std::string name;
                if (!load.object.empty()) {
                    const auto* object = catalog_.object(load.object);
                    name = object ? object->name : load.object;
                } else if (const auto* kind = catalog_.kinds.find(load.cargo)) {
                    name = catalog_.name(*kind);
                }
                const auto line = load.count > 1 ? trf("%s X%d [%.1f t]", name.c_str(), load.count, load.mass * 0.001f)
                                                 : trf("%s [%.1f t]", name.c_str(), load.mass * 0.001f);
                ImGui::PushID(1000 + i);
                if (ImGui::Selectable(line.c_str(), i == w.loaded)) w.loaded = i;
                ImGui::PopID();
            }
            ImGui::EndListBox();
        }
        ImGui::BeginDisabled(w.loaded < 0);
        if (ImGui::Button(tr("Unload"), ImVec2(-1.0f, 0.0f))) unload_selected(place, trains, static_cast<std::size_t>(w.loaded));
        ImGui::EndDisabled();
        ImGui::BeginDisabled(cargo->empty());
        if (ImGui::Button(tr("Unload all"), ImVec2(-1.0f, 0.0f))) unload_selected(place, trains, std::nullopt);
        ImGui::EndDisabled();
        ImGui::EndTable();
    }
    ImGui::Separator();
    if (ImGui::Button(tr("Close"))) open = false;
    ImGui::End();
    if (!open) close_window();
}

} // namespace openrts::viewer
