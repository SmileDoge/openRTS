#include <openrts/audio/world_sounds.hpp>

#include <cmath>
#include <iostream>

#include <glm/geometric.hpp>

namespace openrts::audio {
namespace {

} // namespace

WorldSounds::WorldSounds(std::shared_ptr<const core::Vfs> vfs, core::VirtualPath route,
                         const formats::RoutePosition origin, const int tile_radius)
    : vfs_(vfs), route_(std::move(route)), origin_(origin), radius_(tile_radius), bank_(std::move(vfs)) {
    if (const auto world = route_.resolve(std::string_view("WORLD"))) {
        for (const auto& entry : vfs_->list(*world)) {
            if (entry.directory || core::fold_case(entry.path.extension()) != ".ws") continue;
            if (const auto tile = formats::parse_world_tile_name(entry.path.filename())) files_.emplace(*tile, entry.path);
        }
    }
}

WorldSounds::~WorldSounds() = default;

std::shared_ptr<const formats::SoundScript> WorldSounds::script(const std::string& file, core::VirtualPath& location) {
    const auto key = core::fold_case(file);
    if (const auto it = scripts_.find(key); it != scripts_.end()) {
        location = it->second.second;
        return it->second.first;
    }
    std::shared_ptr<const formats::SoundScript> result;
    std::vector<core::VirtualPath> candidates;
    if (const auto route_sound = route_.resolve(std::string_view("SOUND"))) {
        if (const auto path = route_sound->resolve(file)) candidates.push_back(*path);
    }
    if (const auto path = core::VirtualPath::parse("SOUND")->resolve(file)) candidates.push_back(*path);
    for (const auto& candidate : candidates) {
        const auto* path = &candidate;
        if (!vfs_->exists(*path)) continue;
        auto parsed = std::make_shared<formats::SoundScript>(formats::load_sound_script(*vfs_, *path));
        if (!parsed->ok()) {
            std::cout << "sound script '" << path->str() << "': " << parsed->error << '\n';
            break;
        }
        location = *path;
        result = std::move(parsed);
        break;
    }
    if (!result) std::cout << "sound script '" << file << "' not found\n";
    scripts_.emplace(key, std::pair{result, location});
    return result;
}

void WorldSounds::load_tile(const TileKey& key) {
    auto& sources = tiles_[key];
    const auto file = files_.find(key);
    if (file == files_.end()) return;
    const auto tile = formats::load_world_sounds(*vfs_, file->second);
    if (!tile.ok()) {
        std::cout << "world sounds '" << file->second.str() << "': " << tile.error << '\n';
        return;
    }
    for (const auto& source : tile.sources) {
        core::VirtualPath location;
        auto parsed = script(source.file, location);
        if (!parsed) continue;
        const auto* group = parsed->select_group(formats::SoundOwner::world);
        if (!group) continue;
        const auto& position = source.position;
        const glm::vec3 local{
            static_cast<float>((position.tile_x - origin_.tile_x) * formats::route_tile_size + position.x - origin_.x),
            static_cast<float>(position.y - origin_.y),
            static_cast<float>((position.tile_z - origin_.tile_z) * formats::route_tile_size + position.z - origin_.z),
        };
        auto lookup = [this, location](const std::string& name) -> std::shared_ptr<const AudioClip> {
            const auto file = find_sound_file(*vfs_, location, name);
            if (file) return bank_.get(*file);
            if (missing_.insert(core::fold_case(location.parent().str() + "/" + name)).second) {
                std::cout << "sound '" << name << "' of '" << location.str() << "' not found" << std::endl;
            }
            return nullptr;
        };
        sources.push_back({
            .uid = source.uid,
            .position = local,
            .instance = std::make_unique<SoundInstance>(parsed, *group, formats::SoundOwner::world, std::move(lookup),
                                                        source.uid * 2654435761u),
        });
    }
}

void WorldSounds::set_origin(const formats::RoutePosition& origin) {
    const glm::vec3 shift{
        static_cast<float>((origin.tile_x - origin_.tile_x) * formats::route_tile_size + origin.x - origin_.x),
        static_cast<float>(origin.y - origin_.y),
        static_cast<float>((origin.tile_z - origin_.tile_z) * formats::route_tile_size + origin.z - origin_.z),
    };
    origin_ = origin;
    for (auto& [_, sources] : tiles_) {
        for (auto& source : sources) source.position -= shift;
    }
}

void WorldSounds::update(const float dt, const glm::vec3& listener, const SoundCamera camera, VoiceSink& sink) {
    // Tiles around the listener.
    const auto tile_x = origin_.tile_x + static_cast<std::int32_t>(std::floor((listener.x + origin_.x) / formats::route_tile_size + 0.5));
    const auto tile_z = origin_.tile_z + static_cast<std::int32_t>(std::floor((listener.z + origin_.z) / formats::route_tile_size + 0.5));
    for (auto it = tiles_.begin(); it != tiles_.end();) {
        if (std::abs(it->first.first - tile_x) > radius_ || std::abs(it->first.second - tile_z) > radius_) {
            for (auto& source : it->second) source.instance->stop(sink);
            it = tiles_.erase(it);
        } else {
            ++it;
        }
    }
    for (int dz = -radius_; dz <= radius_; ++dz) {
        for (int dx = -radius_; dx <= radius_; ++dx) {
            const TileKey key{tile_x + dx, tile_z + dz};
            if (!tiles_.contains(key)) load_tile(key);
        }
    }

    for (auto& [_, sources] : tiles_) {
        for (auto& source : sources) {
            SoundInputs inputs{.camera = camera, .position = source.position};
            inputs[formats::SoundVariable::distance] = glm::distance(listener, source.position);
            source.instance->update(dt, inputs, sink);
        }
    }
}

void WorldSounds::stop(VoiceSink& sink) {
    for (auto& [_, sources] : tiles_) {
        for (auto& source : sources) source.instance->stop(sink);
    }
}

WorldSoundStats WorldSounds::stats() const {
    WorldSoundStats result{.tiles = tiles_.size(), .scripts = scripts_.size(), .clips = bank_.size()};
    for (const auto& [_, sources] : tiles_) {
        result.sources += sources.size();
        for (const auto& source : sources) {
            result.active += source.instance->active() ? 1 : 0;
            result.voices += source.instance->voices();
        }
    }
    return result;
}

} // namespace openrts::audio
