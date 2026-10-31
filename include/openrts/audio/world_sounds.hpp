#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/vec3.hpp>

#include <openrts/audio/sound_bank.hpp>
#include <openrts/audio/sound_instance.hpp>
#include <openrts/core/vfs.hpp>
#include <openrts/formats/route_world.hpp>
#include <openrts/formats/sound.hpp>

namespace openrts::audio {

struct WorldSoundStats {
    std::size_t tiles{};
    std::size_t sources{};
    std::size_t active{};
    std::size_t voices{};
    std::size_t scripts{};
    std::size_t clips{};
};

// Point sound sources of a route's .ws files (stations, factories, ...),
// loaded for the tiles around the listener. Scripts come from the route's
// SOUND folder, then the global SOUND folder.
class WorldSounds {
public:
    WorldSounds(std::shared_ptr<const core::Vfs> vfs, core::VirtualPath route, formats::RoutePosition origin,
                int tile_radius = 1);
    ~WorldSounds();
    WorldSounds(const WorldSounds&) = delete;
    WorldSounds& operator=(const WorldSounds&) = delete;

    // `listener` in scene coordinates (relative to the origin).
    void update(float dt, const glm::vec3& listener, SoundCamera camera, VoiceSink& sink);
    void stop(VoiceSink& sink);
    [[nodiscard]] WorldSoundStats stats() const;

private:
    struct Source {
        std::uint32_t uid{};
        glm::vec3 position{};
        std::unique_ptr<SoundInstance> instance;
    };
    using TileKey = std::pair<std::int32_t, std::int32_t>;

    void load_tile(const TileKey& key);
    [[nodiscard]] std::shared_ptr<const formats::SoundScript> script(const std::string& file,
                                                                      core::VirtualPath& location);

    std::shared_ptr<const core::Vfs> vfs_;
    core::VirtualPath route_;
    formats::RoutePosition origin_;
    int radius_;
    std::map<TileKey, core::VirtualPath> files_;
    std::map<TileKey, std::vector<Source>> tiles_;
    std::unordered_map<std::string, std::pair<std::shared_ptr<const formats::SoundScript>, core::VirtualPath>> scripts_;
    SoundBank bank_;
    std::set<std::string> missing_; // sound files already reported missing
};

} // namespace openrts::audio
