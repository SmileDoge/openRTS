#pragma once

// Opacity micromaps (VK_EXT_opacity_micromap, D3D12's DXR 1.2; the fork of
// wgpu-native's wgpuDeviceCreateOpacityMicromap): a cut-out's triangles cut
// into 4^level micro-triangles, each opaque, transparent or unknown by its
// ray mask (ray_scene.hpp, coverage_mask), so the ray hardware takes the
// opaque and skips the transparent without the shader; only the unknown
// (a texture's edge inside the micro-triangle) go to effects/ray_shadow's
// candidate loop.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace openrts::renderer {

// A micromap's micro-triangle states (2 bits each).
enum class MicroState : std::uint8_t { transparent = 0, opaque = 1, unknown_transparent = 2, unknown_opaque = 3 };

// The special indices of a triangle all one state.
inline constexpr std::int32_t micromap_fully_transparent = -1;
inline constexpr std::int32_t micromap_fully_opaque = -2;

// A geometry's micromaps: `count` 4-state micromaps of `level` in `data`
// (((4^level * 2 + 7) / 8) bytes each), and per triangle its micromap or a
// special index.
struct BakedMicromaps {
    std::uint32_t level = 0;
    std::uint32_t count = 0;
    std::vector<std::uint8_t> data;
    std::vector<std::int32_t> indices;
};

// The micro-triangle `index` of `level` (the bird-curve order of the Vulkan
// specification's "micromap indexing"): its corners' barycentrics (u, v; the
// point v0 (1 - u - v) + v1 u + v2 v).
[[nodiscard]] std::array<std::array<float, 2>, 3> micro_triangle(std::uint32_t index, std::uint32_t level);

// `triangles` triangles' UVs (u, v of each corner, 6 floats a triangle) over
// a mask `size` a side (0 open, 255 solid; wrapped as the texture is): a
// micro-triangle opaque or transparent where every mask texel it covers is,
// unknown where both are under it (the shader reads the mask there); one
// spanning too many texels (a UV tiled far) is unknown. Equal micromaps are
// shared; a triangle all one state takes a special index.
[[nodiscard]] BakedMicromaps bake_opacity_micromaps(const float* uvs, std::size_t triangles, const std::uint8_t* mask,
                                                    std::uint32_t size, std::uint32_t level);

// A bake on the micromaps' worker thread: `done` once `result` is there.
struct MicromapJob {
    std::atomic<bool> done{false};
    BakedMicromaps result;
};

// bake_opacity_micromaps on a thread of its own (the frame does not wait on
// a model's thousands of triangles): the job, done some frames later.
[[nodiscard]] std::shared_ptr<MicromapJob> bake_opacity_micromaps_async(std::vector<float> uvs, std::vector<std::uint8_t> mask,
                                                                        std::uint32_t size, std::uint32_t level);

} // namespace openrts::renderer
