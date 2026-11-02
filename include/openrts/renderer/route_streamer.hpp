#pragma once

#include <memory>
#include <vector>

#include <openrts/core/vfs.hpp>
#include <openrts/formats/route_world.hpp>
#include <openrts/renderer/model_assets.hpp>
#include <openrts/renderer/model_renderer.hpp>

namespace openrts::renderer {

struct StreamedInstance {
    core::VirtualPath source_path;
    formats::RoutePosition position;
    ModelTransform transform{1.0f};
    // World object identity (world file tile + UiD), e.g. to find the TDB
    // junction a switch shape belongs to.
    formats::WorldObjectKind kind = formats::WorldObjectKind::static_object;
    std::int32_t world_tile_x{};
    std::int32_t world_tile_z{};
    std::uint32_t uid{};
    std::uint32_t collide_flags{}; // WorldObjectPlacement::collide_flags
    std::uint32_t static_flags{};  // WorldObjectPlacement::static_flags (its RTS draw pass)
};

struct RouteStreamerStats {
    std::size_t requested_tiles{};
    std::size_t loaded_tiles{};
    std::size_t pending_model_assets{};
    std::size_t ready_model_assets{};
    std::size_t visible_instances{};
};

// Streams the world tiles of a route (`ROUTES/<name>` in the content VFS)
// around the camera and loads the models they place on worker threads.
class RouteStreamer {
public:
    RouteStreamer(std::shared_ptr<const core::Vfs> vfs, core::VirtualPath route, formats::RoutePosition origin,
                  int radius = 1);
    ~RouteStreamer();
    RouteStreamer(const RouteStreamer&) = delete;
    RouteStreamer& operator=(const RouteStreamer&) = delete;
    RouteStreamer(RouteStreamer&&) noexcept;
    RouteStreamer& operator=(RouteStreamer&&) noexcept;

    // Schedules missing tiles in a square around camera. This call is cheap and
    // never waits for file parsing or ACE decoding.
    void update(const formats::RoutePosition& camera);
    // Moves the scene origin: every instance's translation is computed again
    // from its route position (double precision), so float positions stay
    // precise near the camera on long routes.
    void set_origin(const formats::RoutePosition& origin);
    // Collects completed worker jobs and schedules model/texture jobs.
    void poll();
    [[nodiscard]] std::vector<StreamedModelAsset> take_ready_models();
    // Instances of the loaded tiles around the camera. The list is rebuilt
    // only when that set of tiles changes; the reference stays valid until
    // the next update() or poll().
    [[nodiscard]] const std::vector<StreamedInstance>& visible_instances() const;
    // Grows each time visible_instances() is built again (another set of tiles).
    [[nodiscard]] std::uint64_t visible_generation() const noexcept;
    [[nodiscard]] RouteStreamerStats stats() const;
    // Night, season and weather for the models it loads from now on
    // (ESD_Alternative_Texture; ModelLibrary::set_texture_variant reloads the rest).
    void set_texture_variant(const TextureVariant& variant);
    // Folders searched first for model textures (the route's TEXTURES).
    [[nodiscard]] const std::vector<core::VirtualPath>& texture_directories() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openrts::renderer
