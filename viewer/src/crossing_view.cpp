#include "crossing_view.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <iostream>

#include <glm/geometric.hpp>

#include <openrts/formats/sound.hpp>
#include <openrts/simulation/track_traveller.hpp>

namespace openrts::viewer {
namespace {

[[nodiscard]] bool starts_with_ignoring_case(const std::string& name, const std::string_view prefix) {
    if (name.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(name[i])) != prefix[i]) return false;
    }
    return true;
}

// The last key frame of matrix `m`'s tracks in the shape's first animation.
[[nodiscard]] float last_frame(const formats::ShapeModel& rig, const std::int32_t m) {
    float last = 0.0f;
    if (rig.animations.empty()) return last;
    for (const auto& node : rig.animations.front().nodes) {
        if (node.matrix_index != m) continue;
        for (const auto& key : node.positions) last = (std::max)(last, static_cast<float>(key.frame));
        for (const auto& key : node.rotations) last = (std::max)(last, static_cast<float>(key.frame));
    }
    return last;
}

// RTS's world file name of a tile: w-004493+014580.w.
[[nodiscard]] std::string world_file_name(const std::int32_t tile_x, const std::int32_t tile_z) {
    char name[32];
    std::snprintf(name, sizeof name, "w%+07d%+07d.w", tile_x, tile_z);
    return name;
}

} // namespace

CrossingView::CrossingView(std::shared_ptr<const core::Vfs> vfs, core::VirtualPath route, const simulation::TrackNetwork* network)
    : vfs_(std::move(vfs)), route_(std::move(route)), network_(network), bank_(vfs_) {}

CrossingView::~CrossingView() {
    for (auto& [_, job] : loading_) {
        if (job.valid()) job.wait();
    }
}

const formats::WorldObjectPlacement* CrossingView::world_object(const Key& key) {
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
        if (object.kind == formats::WorldObjectKind::level_crossing && object.uid == std::get<2>(key)) return &object;
    }
    return nullptr;
}

// rts1.exe 0x508520 for every train: the crossing items ahead of its front
// (2000 m) while it moves towards them, those behind its rear while it
// backs, and those under it, each with the distance and the time to them.
void CrossingView::mark_trains(const std::vector<const simulation::Train*>& trains) {
    marks_.clear();
    if (!network_) return;
    const auto is_crossing = [](const formats::TrackItem& item) { return item.kind == formats::TrackItemKind::level_crossing; };
    const auto mark = [&](const std::uint32_t item, const float time, const float distance) {
        auto [it, inserted] = marks_.try_emplace(item, Mark{time, distance});
        if (inserted) return;
        if (distance < it->second.distance) it->second = Mark{time, distance};
    };
    for (const auto* train : trains) {
        if (!train || !train->placed() || train->cars().empty()) continue;
        const float speed = train->cars().front().velocity; // towards the train front
        if (const auto front = train->front_end()) {
            const auto ahead = simulation::look_ahead(*network_, front->node(), front->offset(), front->forward(), 2000.0, is_crossing);
            for (const auto& item : ahead.items) {
                if (speed <= 0.0f) break;
                mark(item.item, static_cast<float>(item.distance) / speed, static_cast<float>(item.distance));
            }
        }
        if (const auto rear = train->rear_end()) {
            const auto behind = simulation::look_ahead(*network_, rear->node(), rear->offset(), !rear->forward(), 2000.0, is_crossing);
            for (const auto& item : behind.items) {
                if (speed >= 0.0f) break;
                mark(item.item, static_cast<float>(item.distance) / -speed, static_cast<float>(item.distance));
            }
            const auto under = simulation::look_ahead(*network_, rear->node(), rear->offset(), rear->forward(), train->length(), is_crossing);
            for (const auto& item : under.items) mark(item.item, 0.0f, -1.0f);
        }
    }
}

void CrossingView::start_sound(Crossing& crossing) {
    if (!bell_looked_up_) {
        bell_looked_up_ = true;
        // crossing.sms in the route's SOUND, else the global one.
        std::vector<core::VirtualPath> candidates;
        if (const auto folder = route_.resolve(std::string_view("SOUND"))) {
            if (auto file = folder->resolve(std::string_view("crossing.sms"))) candidates.push_back(*file);
        }
        if (const auto global = core::VirtualPath::parse("SOUND/crossing.sms")) candidates.push_back(*global);
        for (const auto& candidate : candidates) {
            if (!vfs_->exists(candidate)) continue;
            auto script = std::make_shared<formats::SoundScript>(formats::load_sound_script(*vfs_, candidate));
            if (!script->ok()) continue;
            bell_ = std::move(script);
            bell_path_ = candidate;
            break;
        }
    }
    if (!bell_ || crossing.sound) return;
    const auto* group = bell_->select_group(formats::SoundOwner::world);
    if (!group) return;
    auto lookup = [this](const std::string& name) -> std::shared_ptr<const audio::AudioClip> {
        const auto file = audio::find_sound_file(*vfs_, bell_path_, name);
        return file ? bank_.get(*file) : nullptr;
    };
    crossing.sound = std::make_unique<audio::SoundInstance>(bell_, *group, formats::SoundOwner::world, std::move(lookup),
                                                            static_cast<std::uint32_t>(crossing.object.uid * 2654435761u));
}

void CrossingView::update(const float elapsed, const std::vector<renderer::StreamedInstance>& visible,
                          const std::vector<const simulation::Train*>& trains, const glm::vec3& listener, audio::VoiceSink* sink) {
    for (auto& [_, crossing] : crossings_) crossing.seen = false;
    for (const auto& instance : visible) {
        if (instance.kind != formats::WorldObjectKind::level_crossing) continue;
        const Key key{instance.world_tile_x, instance.world_tile_z, instance.uid};
        auto found = crossings_.find(key);
        if (found == crossings_.end()) {
            const auto* object = world_object(key);
            if (!object) continue;
            found = crossings_.emplace(key, Crossing{.object = *object}).first;
        }
        found->second.seen = true;
        found->second.position = glm::vec3{instance.transform[3]};
    }
    // Crossings out of reach go quiet and are forgotten.
    for (auto it = crossings_.begin(); it != crossings_.end();) {
        if (it->second.seen) {
            ++it;
            continue;
        }
        if (it->second.sound && sink) it->second.sound->stop(*sink);
        it = crossings_.erase(it);
    }

    mark_trains(trains);
    road_states_.clear();
    const auto& items = network_ ? network_->database().items : std::vector<formats::TrackItem>{};
    for (auto& [_, crossing] : crossings_) {
        const auto& object = crossing.object;
        // 0x422C18: closed when a train arrives within LevelCrParameters'
        // time or stands closer than its distance; warned when it arrives
        // within LevelCrTiming's first time.
        bool closed = false;
        bool warned = false;
        for (const auto id : object.rail_items) {
            if (id >= items.size() || items[id].kind != formats::TrackItemKind::level_crossing) continue;
            const auto mark = marks_.find(id);
            if (mark == marks_.end() || mark->second.distance == 0.0f) continue;
            if (mark->second.time < object.crossing_parameters[0]) closed = true;
            if (mark->second.distance < object.crossing_parameters[1]) closed = true;
            if (mark->second.time < object.crossing_timing[0]) warned = true;
        }
        const bool was = crossing.closed || crossing.warned;
        const bool now = closed || warned;
        if (now && !was) crossing.triggers.push_back(3);
        if (!now && was) crossing.triggers.push_back(4);
        crossing.closed = closed;
        crossing.warned = warned;
        for (const auto road : object.road_items) road_states_[road] = static_cast<std::uint8_t>((closed ? 1 : 0) + (warned ? 1 : 0));

        // The gates take LevelCrTiming's third value to swing; the lights
        // swing to and fro in 2 s while warned (0x420E7C, PROBABLE times).
        const float gate_time = (std::max)(object.crossing_timing[2], 0.1f);
        const float target = closed ? 1.0f : 0.0f;
        const float step = elapsed / gate_time;
        crossing.gates = crossing.gates < target ? (std::min)(target, crossing.gates + step) : (std::max)(target, crossing.gates - step);
        const float light_step = elapsed / 2.0f;
        if (warned) {
            if (crossing.lights_rising) {
                crossing.lights = (std::min)(1.0f, crossing.lights + light_step);
                if (crossing.lights >= 1.0f) crossing.lights_rising = false;
            } else {
                crossing.lights = (std::max)(0.0f, crossing.lights - light_step);
                if (crossing.lights <= 0.0f) crossing.lights_rising = true;
            }
        } else {
            crossing.lights = (std::max)(0.0f, crossing.lights - light_step);
            crossing.lights_rising = true;
        }

        // The bell: crossing.sms at the crossing, trigger 3 closing, 4 opening.
        if (!sink) continue;
        const float hearing = glm::distance(listener, crossing.position);
        if (hearing > 1500.0f) {
            if (crossing.sound) {
                crossing.sound->stop(*sink);
                crossing.sound.reset();
            }
            crossing.triggers.clear();
            continue;
        }
        start_sound(crossing);
        if (!crossing.sound) continue;
        audio::SoundInputs inputs{.camera = audio::SoundCamera::external, .position = crossing.position};
        inputs[formats::SoundVariable::distance] = hearing;
        inputs.discrete = crossing.triggers;
        crossing.sound->update(elapsed, inputs, *sink);
        crossing.triggers.clear();
    }
}

void CrossingView::stop_sound(audio::VoiceSink& sink) {
    for (auto& [_, crossing] : crossings_) {
        if (crossing.sound) crossing.sound->stop(sink);
        crossing.sound.reset();
    }
}

std::optional<std::vector<renderer::NodeAdjustment>> CrossingView::adjustments(const renderer::StreamedInstance& instance,
                                                                                renderer::ModelLibrary& library) {
    if (instance.kind != formats::WorldObjectKind::level_crossing) return std::nullopt;
    const auto found = crossings_.find(Key{instance.world_tile_x, instance.world_tile_z, instance.uid});
    if (found == crossings_.end()) return std::nullopt;
    const auto* model = library.find(instance.source_path);
    const auto* rig = model ? model->rig() : nullptr;
    if (!rig) return std::nullopt;
    std::vector<renderer::NodeAdjustment> nodes;
    for (std::size_t m = 0; m < rig->matrices.size(); ++m) {
        const auto& name = rig->matrices[m].name;
        // 0x420E7C: nodes named gate* are the gates, signal* the lights.
        const bool gate = starts_with_ignoring_case(name, "gate");
        const bool light = !gate && starts_with_ignoring_case(name, "signal");
        if (!gate && !light) continue;
        const float last = last_frame(*rig, static_cast<std::int32_t>(m));
        nodes.push_back({.matrix = static_cast<std::int32_t>(m), .frame = (gate ? found->second.gates : found->second.lights) * last});
    }
    return nodes;
}

std::uint8_t CrossingView::road_state(const std::uint32_t road_item) const {
    const auto found = road_states_.find(road_item);
    return found == road_states_.end() ? 0 : found->second;
}

std::size_t CrossingView::closed() const noexcept {
    return static_cast<std::size_t>(std::count_if(crossings_.begin(), crossings_.end(), [](const auto& entry) { return entry.second.closed; }));
}

} // namespace openrts::viewer
