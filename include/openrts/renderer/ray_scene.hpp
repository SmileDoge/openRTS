#pragma once

// Ray-traced sun shadows (docs/ray-tracing.md): with openRTS's wgpu-native
// (OPENRTS_HAS_RAY_QUERY) on a GPU with ray queries, the models' opaque
// primitives in a TLAS built every frame (ModelRenderer::prepare), bound in
// the scene effects' group (binding 14) and traced towards the sun from each
// lit pixel (common/ray_shadow). Without them all of this is idle and the
// shaders take a stub.

#include <cstdint>
#include <optional>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/mat4x4.hpp>
#include <webgpu/webgpu.h>

#ifdef OPENRTS_HAS_RAY_QUERY
#include <webgpu/wgpu.h>
#endif

namespace openrts::renderer {

// The device has ray queries (set as it is made, before any pipeline).
inline bool& ray_queries() {
    static bool on = false;
    return on;
}

// The setting this frame: shadows by rays (SceneEffectSettings::ray_shadows,
// taken by SceneEffects::begin_frame).
inline bool& ray_shadows_wanted() {
    static bool on = false;
    return on;
}

namespace detail {

// A cut-out's alpha at `size` a side for the rays, as 0 (light by) or 255
// (solid) against its cut-off (formats::cutoff_byte: a texel solid with its
// alpha over it). Each texel's share of its source texels solid, then as
// many texels solid as the source has (the shares highest first): a thin
// bar or twig of a large texture stays, small holes stay open, where the
// nearest texel dropped or filled them by chance.
[[nodiscard]] std::vector<std::uint8_t> coverage_mask(std::uint32_t width, std::uint32_t height, const std::uint8_t* rgba,
                                                      std::uint8_t cutoff, std::uint32_t size);

// The shadow shader piece to add with common/effects_main: the real one with
// ray queries, else the stub.
[[nodiscard]] const char* ray_shadow_code();

#ifdef OPENRTS_HAS_RAY_QUERY
// The frame's TLAS, one per program (the device's first use makes it).
class RayScene {
public:
    static constexpr std::uint32_t max_instances = 1u << 16;
    [[nodiscard]] static RayScene& get();
    [[nodiscard]] WGPUTlas tlas(WGPUDevice device);
    // An instance of `blas` placed by `transform` (world from the BLAS's own),
    // in the next build.
    // `custom`: its first entry in the cut-outs' table (0: every geometry
    // opaque, the table's first entry).
    void add(WGPUBlas blas, const glm::mat4& transform, std::uint32_t custom = 0);

    // The cut-outs (a texture's alpha read by the rays, effects/ray_shadow):
    // each cut-out texture's alpha as a layer of one array (mask_size a
    // side), each cut-out geometry's triangles' UVs in one buffer, and a
    // table of (its UVs' first, its layer | its cut-off << 16 | its kind <<
    // 24, its transmission's RGB 8 bits each, 0) by geometry (the cut-off
    // formats::cutoff_byte's: a texel casts with its alpha over it; kind 0
    // a mask, 1 transmission: the layer its alpha as it is), an instance's
    // geometries from its custom data on; opaque: layer opaque_layer.
    static constexpr std::uint32_t mask_size = 512, max_masks = 256;
    static constexpr std::uint32_t max_uvs = 4u << 20, max_entries = 1u << 18;
    static constexpr std::uint32_t opaque_layer = 0xFFFFFFFFu;
    // The texture `key`'s layer for the cut-off `cutoff` (made from its RGBA8
    // pixels the first time, coverage_mask: one layer a texture and cut-off);
    // none when the array is full.
    [[nodiscard]] std::optional<std::uint32_t> mask_layer(WGPUDevice device, const std::string& key, std::uint8_t cutoff,
                                                          std::uint32_t width, std::uint32_t height, const std::uint8_t* rgba);
    // The texture `key`'s alpha as it is (each mask texel its source
    // texels' mean), for a transmission's layer.
    [[nodiscard]] std::optional<std::uint32_t> alpha_layer(WGPUDevice device, const std::string& key, std::uint32_t width,
                                                           std::uint32_t height, const std::uint8_t* rgba);
    static constexpr std::size_t entry_words = 4;
    // `count` UVs (u, v pairs) in the buffer: the first's index; none when full.
    [[nodiscard]] std::optional<std::uint32_t> add_uvs(WGPUDevice device, const float* uv, std::size_t count);
    // Table entries (entry_words each) in a row: the first's index; none when full.
    [[nodiscard]] std::optional<std::uint32_t> add_entries(WGPUDevice device, const std::uint32_t* entries, std::size_t count);
    // A mask layer's pixels (mask_size a side, 0 or 255; coverage_mask), for
    // its opacity micromaps (opacity_micromap.hpp); empty for a transmission's.
    [[nodiscard]] std::vector<std::uint8_t> mask_pixels(std::uint32_t layer);
    // Whether the device makes opacity micromaps (the fork of wgpu-native;
    // OPENRTS_OMM=0: not used).
    [[nodiscard]] bool opacity_micromaps(WGPUDevice device);
    [[nodiscard]] WGPUTextureView masks() const noexcept { return mask_view_; }
    [[nodiscard]] WGPUBuffer uvs() const noexcept { return uv_buffer_; }
    [[nodiscard]] WGPUBuffer table() const noexcept { return table_buffer_; }
    // Builds the BLAS given (pending first-builds) and the TLAS of the
    // instances added since the last build, on its own command buffer.
    void build(WGPUDevice device, const WGPUBlasBuildEntry* blas, std::size_t blas_count);
    // A BLAS built now, alone (the terrain's tiles as they come).
    void build_blas(WGPUDevice device, const WGPUBlasBuildEntry& blas);
    // Called at each build, before the TLAS: what stays in the scene between
    // frames adds itself as it is then (the terrain's tiles: one unloaded
    // since a frame's add would leave a dropped BLAS in the build).
    void set_adder(const void* owner, std::function<void(RayScene&)> adder);
    void remove_adder(const void* owner);
    [[nodiscard]] std::uint32_t instance_count() const noexcept { return count_; }

private:
    WGPUTlas tlas_ = nullptr;
    WGPUTlasInstance* instances_ = nullptr;
    std::uint32_t count_ = 0;
    std::map<const void*, std::function<void(RayScene&)>> adders_;
    void ensure_cut_outs(WGPUDevice device);
    std::mutex mutex_; // the cut-outs: models load on several threads
    WGPUTexture mask_texture_ = nullptr;
    WGPUTextureView mask_view_ = nullptr;
    WGPUBuffer uv_buffer_ = nullptr, table_buffer_ = nullptr;
    std::unordered_map<std::string, std::uint32_t> mask_layers_;
    std::vector<std::vector<std::uint8_t>> mask_pixels_;
    int opacity_micromaps_ = -1; // -1 not asked yet
    std::uint32_t uv_count_ = 0, entry_count_ = 0;
};
#endif

} // namespace detail
} // namespace openrts::renderer
