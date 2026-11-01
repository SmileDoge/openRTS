#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <glm/mat4x4.hpp>

#include <openrts/core/vfs.hpp>
#include <openrts/formats/route_world.hpp>
#include <openrts/renderer/model_renderer.hpp>
#include <openrts/renderer/rts_sky.hpp>

#ifdef OPENRTS_HAS_WGPU
#include <webgpu/webgpu.h>
#endif

namespace openrts::renderer {

// CPU mesh of one terrain tile, positioned relative to the tile centre.
struct TerrainVertex {
    float position[3];
    float normal[3];
    float uv[2];
};

struct TerrainPatchMesh {
    std::uint32_t first_index{};
    std::uint32_t index_count{};
    std::string texture;      // terrain_texslot 0
    std::string microtexture; // terrain_texslot 1 (DetailTerrain), empty otherwise
};

struct TerrainTileMesh {
    std::int32_t tile_x{};
    std::int32_t tile_z{};
    std::vector<TerrainVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<TerrainPatchMesh> patches;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// Builds the full-detail mesh of a tile. Edge rows and columns take their
// heights from the east, south and south-east neighbours so tiles join.
[[nodiscard]] TerrainTileMesh build_terrain_tile_mesh(const core::Vfs& vfs,
                                                      const core::VirtualPath& tiles,
                                                      std::int32_t tile_x,
                                                      std::int32_t tile_z);

// Terrain texture lookup of RTS (0x422648): seasonal TERRTEX sub-folders with
// fallbacks, then TERRTEX itself.
[[nodiscard]] std::optional<core::VirtualPath> resolve_terrain_texture(const core::Vfs& vfs,
                                                                      const core::VirtualPath& route,
                                                                      const std::string& texture,
                                                                      rts::Season season,
                                                                      rts::Weather weather);

#ifdef OPENRTS_HAS_WGPU
// Streams and draws the terrain tiles around the camera.
class TerrainRenderer {
public:
    TerrainRenderer(WGPUDevice device,
                    WGPUTextureFormat target_format,
                    std::shared_ptr<const core::Vfs> vfs,
                    core::VirtualPath route,
                    formats::RoutePosition origin,
                    int radius = 2);
    ~TerrainRenderer();
    TerrainRenderer(const TerrainRenderer&) = delete;
    TerrainRenderer& operator=(const TerrainRenderer&) = delete;
    TerrainRenderer(TerrainRenderer&&) noexcept;
    TerrainRenderer& operator=(TerrainRenderer&&) noexcept;

    [[nodiscard]] bool ready() const noexcept;
    // Selects the seasonal texture variant; textures are re-resolved.
    void set_conditions(rts::Season season, rts::Weather weather);
    // Schedules tile and texture jobs and uploads finished ones.
    void update(const formats::RoutePosition& camera);
    // Moves the scene origin (tile offsets are computed per frame from it).
    void set_origin(const formats::RoutePosition& origin);
    void render(WGPURenderPassEncoder pass, const glm::mat4& view_projection, const ModelLighting& lighting);

    [[nodiscard]] std::size_t loaded_tiles() const noexcept;
    [[nodiscard]] std::size_t pending_jobs() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
#endif

} // namespace openrts::renderer
