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
    // Collects completed worker jobs and schedules model/texture jobs.
    void poll();
    [[nodiscard]] std::vector<StreamedModelAsset> take_ready_models();
    [[nodiscard]] std::vector<StreamedInstance> visible_instances() const;
    [[nodiscard]] RouteStreamerStats stats() const;
    // Folders searched first for model textures (the route's TEXTURES).
    [[nodiscard]] const std::vector<core::VirtualPath>& texture_directories() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openrts::renderer
