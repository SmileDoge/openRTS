#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <glm/mat4x4.hpp>

#include <openrts/core/vfs.hpp>
#include <openrts/formats/environment.hpp>
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
    float center[3]{}; // bounding sphere in the tile's space
    float radius{};
    std::string texture;      // terrain_texslot 0
    std::string microtexture; // terrain_texslot 1 (DetailTerrain), empty otherwise
};

// A corner of a water patch (rts1.exe 0x49B088): its place in the tile, the
// patch's north-west corner (UVs of the camera-following layers) and which
// corner it is (0 / 1 east, 0 / 1 south).
struct TerrainWaterVertex {
    float position[3];
    float patch[2];
    float corner[2];
};

struct TerrainTileMesh {
    std::int32_t tile_x{};
    std::int32_t tile_z{};
    std::vector<TerrainVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<TerrainPatchMesh> patches;
    // The water patches (flag bits 0xC0), two triangles each, at the tile's
    // water heights (terrain_water_height_offset as 0x4A2A5C spreads them).
    std::vector<TerrainWaterVertex> water;
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
    // RTS's terrain shading, or the experimental path's (as the models': linear
    // light, lamps added; ModelRenderer::shading_path).
    void set_shading_path(ShadingPath path) noexcept;
    // Schedules tile and texture jobs and uploads finished ones.
    void update(const formats::RoutePosition& camera);
    // Moves the scene origin (tile offsets are computed per frame from it).
    void set_origin(const formats::RoutePosition& origin);
    // `effects`: SceneEffects::bind_group (nullptr: a built-in one, every effect off).
    void render(WGPURenderPassEncoder pass, const glm::mat4& view_projection, const ModelLighting& lighting,
                WGPUBindGroup effects = nullptr);
    // Depth only, for SceneEffects: every loaded tile from `view_projection`
    // in depth slot `slot` (< 12).
    void render_depth(WGPURenderPassEncoder pass, std::uint32_t slot, const glm::mat4& view_projection, DepthPassKind kind);
    // Sizes what the depth passes use; call before them (they may run on
    // several threads at once and change nothing shared).
    void prepare_depth();

    // The environment's water layers (nullptr: none), their textures from
    // `texture_folders` in order; drawn over the water patches after the
    // terrain, as RTS (0x4A2144) does.
    void set_water(const formats::EnvWater* water, std::vector<core::VirtualPath> texture_folders);
    // Moves the layers' scrolls and frames on, and where the eye is (the
    // layers that follow the camera).
    void update_water(float elapsed, const glm::vec3& eye);
    [[nodiscard]] std::size_t water_quads() const noexcept;

    [[nodiscard]] std::size_t loaded_tiles() const noexcept;
    [[nodiscard]] std::size_t pending_jobs() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
#endif

} // namespace openrts::renderer
