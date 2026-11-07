#include <openrts/renderer/terrain_renderer.hpp>

#include <openrts/core/profile.hpp>

#include <algorithm>
#include <cmath>
#include <optional>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <openrts/formats/terrain.hpp>

namespace openrts::renderer {
namespace {

// Height lookup across a tile and its east, south and south-east neighbours;
// samples outside every loaded tile clamp to the nearest own sample.
class HeightField {
public:
    HeightField(const formats::TerrainTile& tile,
                const formats::TerrainTile* east,
                const formats::TerrainTile* south,
                const formats::TerrainTile* south_east)
        : tile_(tile), east_(east), south_(south), south_east_(south_east), last_(tile.sample_count - 1) {}

    [[nodiscard]] float at(std::int64_t row, std::int64_t column) const {
        const auto n = static_cast<std::int64_t>(tile_.sample_count);
        row = (std::max)(row, std::int64_t{0});
        column = (std::max)(column, std::int64_t{0});
        const bool beyond_south = row >= n;
        const bool beyond_east = column >= n;
        if (beyond_south && beyond_east && south_east_) return south_east_->height(0, 0);
        if (beyond_south && !beyond_east && south_) return south_->height(0, static_cast<std::uint32_t>(column));
        if (beyond_east && !beyond_south && east_) return east_->height(static_cast<std::uint32_t>(row), 0);
        return tile_.height(static_cast<std::uint32_t>((std::min)(row, n - 1)),
                            static_cast<std::uint32_t>((std::min)(column, n - 1)));
    }

private:
    const formats::TerrainTile& tile_;
    const formats::TerrainTile* east_;
    const formats::TerrainTile* south_;
    const formats::TerrainTile* south_east_;
    std::uint32_t last_;
};

[[nodiscard]] const formats::TerrainTile* if_ok(const formats::TerrainTile& tile) {
    return tile.ok() && tile.sample_count > 0 ? &tile : nullptr;
}

} // namespace

TerrainTileMesh build_terrain_tile_mesh(const core::Vfs& vfs,
                                        const core::VirtualPath& tiles,
                                        const std::int32_t tile_x,
                                        const std::int32_t tile_z) {
    TerrainTileMesh mesh{.tile_x = tile_x, .tile_z = tile_z};
    const auto tile = formats::load_terrain_tile(vfs, tiles, tile_x, tile_z);
    if (!tile.ok()) {
        mesh.error = tile.error;
        return mesh;
    }
    if (tile.patches_per_side == 0 || tile.sample_count % tile.patches_per_side != 0) {
        mesh.error = "Terrain patch grid does not divide the sample grid.";
        return mesh;
    }

    // Tile Z grows northwards, rows southwards: south is z - 1.
    const auto east = formats::load_terrain_tile(vfs, tiles, tile_x + 1, tile_z);
    const auto south = formats::load_terrain_tile(vfs, tiles, tile_x, tile_z - 1);
    const auto south_east = formats::load_terrain_tile(vfs, tiles, tile_x + 1, tile_z - 1);
    const HeightField heights(tile, if_ok(east), if_ok(south), if_ok(south_east));

    const auto size = tile.sample_size;
    const auto half_extent = static_cast<float>(tile.sample_count) * size * 0.5f;
    const auto samples_per_patch = tile.sample_count / tile.patches_per_side;

    const auto normal_at = [&](const std::int64_t row, const std::int64_t column) {
        // Columns grow east (+x), rows grow south (-z).
        const auto dh_dx = (heights.at(row, column + 1) - heights.at(row, column - 1)) / (2.0f * size);
        const auto dh_dz = (heights.at(row - 1, column) - heights.at(row + 1, column)) / (2.0f * size);
        return glm::normalize(glm::vec3{-dh_dx, 1.0f, -dh_dz});
    };

    for (std::uint32_t patch_row = 0; patch_row < tile.patches_per_side; ++patch_row) {
        for (std::uint32_t patch_column = 0; patch_column < tile.patches_per_side; ++patch_column) {
            const auto& patch = tile.patches[static_cast<std::size_t>(patch_row) * tile.patches_per_side + patch_column];
            const auto base = static_cast<std::uint32_t>(mesh.vertices.size());

            for (std::uint32_t i = 0; i <= samples_per_patch; ++i) {
                for (std::uint32_t j = 0; j <= samples_per_patch; ++j) {
                    const auto row = static_cast<std::int64_t>(patch_row * samples_per_patch + i);
                    const auto column = static_cast<std::int64_t>(patch_column * samples_per_patch + j);
                    const auto normal = normal_at(row, column);
                    mesh.vertices.push_back({
                        .position = {static_cast<float>(column) * size - half_extent, heights.at(row, column),
                                     half_extent - static_cast<float>(row) * size},
                        .normal = {normal.x, normal.y, normal.z},
                        // Patch-local sample coordinates (0..samples_per_patch).
                        .uv = {patch.u0 + patch.du_dx * static_cast<float>(j) + patch.du_dz * static_cast<float>(i),
                               patch.v0 + patch.dv_dx * static_cast<float>(j) + patch.dv_dz * static_cast<float>(i)},
                    });
                }
            }

            TerrainPatchMesh patch_mesh{.first_index = static_cast<std::uint32_t>(mesh.indices.size())};
            {
                glm::vec3 lo{mesh.vertices[base].position[0], mesh.vertices[base].position[1], mesh.vertices[base].position[2]};
                glm::vec3 hi = lo;
                for (auto v = base; v < mesh.vertices.size(); ++v) {
                    const glm::vec3 p{mesh.vertices[v].position[0], mesh.vertices[v].position[1], mesh.vertices[v].position[2]};
                    lo = glm::min(lo, p);
                    hi = glm::max(hi, p);
                }
                const auto centre = (lo + hi) * 0.5f;
                patch_mesh.center[0] = centre.x;
                patch_mesh.center[1] = centre.y;
                patch_mesh.center[2] = centre.z;
                patch_mesh.radius = glm::length(hi - centre);
            }
            const auto stride = samples_per_patch + 1;
            for (std::uint32_t i = 0; i < samples_per_patch; ++i) {
                for (std::uint32_t j = 0; j < samples_per_patch; ++j) {
                    const auto a = base + i * stride + j;
                    const auto b = a + 1;
                    const auto c = a + stride;
                    const auto d = c + 1;
                    // RTS (0x49D228) fans each 2x2 sample block around its
                    // odd-odd centre sample, so diagonals alternate in a
                    // checkerboard and always pass through that sample.
                    const auto row = patch_row * samples_per_patch + i;
                    const auto column = patch_column * samples_per_patch + j;
                    if (row % 2 == column % 2) {
                        mesh.indices.insert(mesh.indices.end(), {a, c, d, a, d, b});
                    } else {
                        mesh.indices.insert(mesh.indices.end(), {a, c, b, b, c, d});
                    }
                }
            }
            patch_mesh.index_count = static_cast<std::uint32_t>(mesh.indices.size()) - patch_mesh.first_index;
            if (patch.shader_index < tile.shaders.size()) {
                const auto& textures = tile.shaders[patch.shader_index].textures;
                if (!textures.empty()) patch_mesh.texture = textures[0];
                if (textures.size() > 1) patch_mesh.microtexture = textures[1];
            }
            mesh.patches.push_back(std::move(patch_mesh));
        }
    }

    // Water (rts1.exe 0x49B088): a quad over each patch flagged 0xC0, its
    // corners at the water height 0x4A2A5C takes from the tile's four
    // values (bug for bug: away from the first values, not towards the others).
    const auto water = tile.water_heights.value_or(std::array<float, 4>{});
    const auto water_height = [&](const float column, const float row) {
        const float near = column * (water[2] - water[3]) * 0.0625f + water[2];
        const float far = column * (water[0] - water[1]) * 0.0625f + water[0];
        return row * (near - far) * 0.0625f + near;
    };
    const float patch_size = half_extent * 2.0f / static_cast<float>(tile.patches_per_side);
    for (std::uint32_t patch_row = 0; patch_row < tile.patches_per_side; ++patch_row) {
        for (std::uint32_t patch_column = 0; patch_column < tile.patches_per_side; ++patch_column) {
            const auto& patch = tile.patches[static_cast<std::size_t>(patch_row) * tile.patches_per_side + patch_column];
            if ((patch.flags & 0xC0u) == 0) continue;
            const float west = static_cast<float>(patch_column) * patch_size - half_extent;
            const float north = half_extent - static_cast<float>(patch_row) * patch_size;
            const auto c = static_cast<float>(patch_column);
            const auto r = static_cast<float>(patch_row);
            const TerrainWaterVertex corners[4] = {
                {{west, water_height(c, r), north}, {west, north}, {0.0f, 0.0f}},
                {{west + patch_size, water_height(c + 1.0f, r), north}, {west, north}, {1.0f, 0.0f}},
                {{west + patch_size, water_height(c + 1.0f, r + 1.0f), north - patch_size}, {west, north}, {1.0f, 1.0f}},
                {{west, water_height(c, r + 1.0f), north - patch_size}, {west, north}, {0.0f, 1.0f}},
            };
            for (const int k : {0, 1, 2, 0, 2, 3}) mesh.water.push_back(corners[k]);
        }
    }
    return mesh;
}

std::optional<core::VirtualPath> resolve_terrain_texture(const core::Vfs& vfs,
                                                        const core::VirtualPath& route,
                                                        const std::string& texture,
                                                        const rts::Season season,
                                                        const rts::Weather weather) {
    const auto terrtex = route.resolve(std::string_view("TERRTEX"));
    if (!terrtex) return std::nullopt;
    const bool snow = weather == rts::Weather::snow;

    std::vector<const char*> folders;
    switch (season) {
    case rts::Season::spring:
        if (snow) folders = {"SPRINGSNOW", "SNOW"};
        folders.push_back("SPRING");
        break;
    case rts::Season::autumn:
        if (snow) folders = {"AUTUMNSNOW", "SNOW"};
        folders.push_back("AUTUMN");
        break;
    case rts::Season::winter:
        if (snow) folders = {"WINTERSNOW"};
        folders.push_back("SNOW");
        break;
    case rts::Season::summer:
        break;
    }
    for (const auto* folder : folders) {
        const auto candidate = terrtex->resolve(std::string(folder) + "/" + texture);
        if (candidate && vfs.exists(*candidate)) return candidate;
    }
    return terrtex->resolve(std::string_view(texture));
}

} // namespace openrts::renderer

#ifdef OPENRTS_HAS_WGPU
#include <openrts/renderer/depth.hpp>
#include <array>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <map>
#include <string>
#include <utility>

#include <openrts/formats/ace_file.hpp>
#include <openrts/renderer/webgpu_texture.hpp>

#include <openrts/renderer/frustum.hpp>
#include <openrts/renderer/scene_effects.hpp>

#include "rts_light_shapes.hpp"
#include "scene_effects_wgsl.hpp"

namespace openrts::renderer {
namespace {

using TileKey = std::pair<std::int32_t, std::int32_t>;

constexpr std::size_t max_concurrent_tile_jobs = 2;
constexpr std::uint64_t draw_uniform_stride = 256;

struct DrawUniform {
    glm::mat4 mvp;
    float offset[4]; // the tile's place relative to the route origin
    // Texture coordinates: uv = uv_rect.xy + uv * uv_rect.zw (terrain: 0 0 1 1);
    // water: uv_mode x 1 follows the camera (y, z: the eye; w: patch size).
    float uv_rect[4]{0.0f, 0.0f, 1.0f, 1.0f};
    float uv_mode[4]{};
};

// Headlights and searchlights on the ground: the model renderer's further
// lights in their rings (LightRings; RTS Shader.NumLights 10, the sun first).
constexpr std::size_t ring_lights = LightRings::count * LightRings::capacity;

struct FrameUniform {
    float light_diffuse[4];
    float light_ambient[4];
    float light_direction[4];
    float fog_colour[4];
    float fog_distance[4]; // x: start, y: end, w: physical falloff
    detail::GpuLight lights[ring_lights];
    std::int32_t ring_counts[12];
    float shading[4]; // x: night_ambient_scale()
};

// Port of RTS terrain.vert/frag, the additional light sources included.
constexpr char terrain_shader[] = R"(
struct Draw {
    mvp: mat4x4<f32>,
    offset: vec4<f32>,
    uv_rect: vec4<f32>,
    uv_mode: vec4<f32>,
};

struct Frame {
    light_diffuse: vec4<f32>,
    light_ambient: vec4<f32>,
    light_direction: vec4<f32>,
    fog_colour: vec4<f32>,
    fog_distance: vec4<f32>,
    lights: array<Light, 90>, // nine a ring
    ring_counts: array<vec4<i32>, 3>,
    shading: vec4<f32>, // x: the experimental path's night ambient factor
};

// The experimental path (openRTS's shading, as the models': linear light,
// lamps added) and the HDR frame (no soft clip, the tone pass does it).
override experimental: bool = false;
override hdr_output: bool = false;

@group(0) @binding(0) var base_texture: texture_2d<f32>;
@group(0) @binding(1) var micro_texture: texture_2d<f32>;
@group(0) @binding(2) var terrain_sampler: sampler;
@group(1) @binding(0) var<uniform> draw: Draw;
@group(1) @binding(1) var<uniform> frame: Frame;

struct VertexIn {
    @location(0) position: vec3<f32>,
    @location(1) normal: vec3<f32>,
    @location(2) uv: vec2<f32>,
    // The patch's ring of further lights, passed as the draw's first instance.
    @builtin(instance_index) ring: u32,
};

struct VertexOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) colour: vec4<f32>,
    @location(2) fog: vec4<f32>,
    @location(3) world: vec4<f32>,
    @location(4) normal: vec3<f32>,
    @location(5) @interpolate(flat) ring: u32,
};

fn ring_count(ring: u32) -> i32 {
    return min(frame.ring_counts[ring / 4u][ring % 4u], 9);
}

@vertex
fn vs_main(input: VertexIn) -> VertexOut {
    var out: VertexOut;
    out.ring = min(input.ring, 9u);
    out.clip = draw.mvp * vec4<f32>(input.position, 1.0);
    out.uv = draw.uv_rect.xy + input.uv * draw.uv_rect.zw;
    out.world = vec4<f32>(input.position + draw.offset.xyz, 1.0);
    out.normal = input.normal;

    let factor = clamp(-dot(input.normal, frame.light_direction.xyz) * 2.0, 0.0, 1.0);
    let diffuse = max(frame.light_diffuse * factor, vec4<f32>(0.0));
    let ambient = max(frame.light_ambient, vec4<f32>(0.0));
    out.colour = max(diffuse, ambient);

    let fog_start = frame.fog_distance.x;
    let fog_end = frame.fog_distance.y;
    if (fog_end <= 0.0) {
        out.fog = vec4<f32>(0.0);
    } else if (fog_end > fog_start) {
        let amount = clamp(frame.fog_colour.w * ((out.clip.z - fog_start) / (fog_end - fog_start)), 0.0, 1.0);
        out.fog = vec4<f32>(frame.fog_colour.xyz, amount);
    } else {
        out.fog = frame.fog_colour;
    }
    return out;
}

// Water layers (RTS water.vert): lit with the normal up; UVs per patch, or
// following the camera (world_water_layer_sky_reflection, 0x4A2144).
struct WaterIn {
    @location(0) position: vec3<f32>,
    @location(1) patch_corner: vec2<f32>,
    @location(2) corner: vec2<f32>,
    @builtin(instance_index) ring: u32,
};

@vertex
fn vs_water(input: WaterIn) -> VertexOut {
    var out: VertexOut;
    out.ring = min(input.ring, 9u);
    out.clip = draw.mvp * vec4<f32>(input.position, 1.0);
    if (draw.uv_mode.x > 0.5) {
        let size = draw.uv_mode.w;
        let start = vec2<f32>((input.patch_corner.x + draw.offset.x - draw.uv_mode.y) / size,
                              (draw.uv_mode.z - input.patch_corner.y - draw.offset.z) / size);
        out.uv = start + draw.uv_rect.xy + input.corner * draw.uv_rect.zw;
    } else {
        out.uv = draw.uv_rect.xy + input.corner * draw.uv_rect.zw;
    }
    out.world = vec4<f32>(input.position + draw.offset.xyz, 1.0);
    out.normal = vec3<f32>(0.0, 1.0, 0.0);
    let factor = clamp(-dot(out.normal, frame.light_direction.xyz) * 2.0, 0.0, 1.0);
    out.colour = max(max(frame.light_diffuse * factor, vec4<f32>(0.0)), max(frame.light_ambient, vec4<f32>(0.0)));
    let fog_start = frame.fog_distance.x;
    let fog_end = frame.fog_distance.y;
    if (fog_end <= 0.0) {
        out.fog = vec4<f32>(0.0);
    } else if (fog_end > fog_start) {
        let amount = clamp(frame.fog_colour.w * ((out.clip.z - fog_start) / (fog_end - fog_start)), 0.0, 1.0);
        out.fog = vec4<f32>(frame.fog_colour.xyz, amount);
    } else {
        out.fog = frame.fog_colour;
    }
    return out;
}

fn to_linear(c: vec3<f32>) -> vec3<f32> {
    return pow(max(c, vec3<f32>(0.0)), vec3<f32>(2.2));
}

fn to_display(c: vec3<f32>) -> vec3<f32> {
    return pow(max(c, vec3<f32>(0.0)), vec3<f32>(1.0 / 2.2));
}

// Linear up to the knee, then an exponential roll-off towards 1 (as the models').
fn shoulder(c: vec3<f32>) -> vec3<f32> {
    let knee = 0.6;
    let room = 1.0 - knee;
    let over = max(c - vec3<f32>(knee), vec3<f32>(0.0));
    let rolled = vec3<f32>(knee) + room * (vec3<f32>(1.0) - exp(-over / room));
    return select(c, rolled, c > vec3<f32>(knee));
}

// The experimental path, as the models' (model_renderer.cpp): sky ambient
// (less of it the stronger the sun, darker towards the ground), Lambert sun,
// the further lights added, all in linear light (each light's falloff taken
// in display space, as RTS's).
fn lamp_light(light: Light, world: vec4<f32>, n: vec3<f32>, physical: bool) -> vec3<f32> {
    if (physical) {
        let strength = physical_light(light, world, n);
        if (strength > 0.0) {
            return to_linear(light.diffuse.xyz) * strength;
        }
        return vec3<f32>(0.0);
    }
    let factor = light_factor(light, world, n);
    if (factor > 0.0) {
        return to_linear(light.diffuse.xyz * factor);
    }
    return vec3<f32>(0.0);
}

fn shade_experimental(input: VertexOut, texel: vec4<f32>) -> vec4<f32> {
    let base = to_linear(texel.xyz);
    let sun_colour = to_linear(frame.light_diffuse.xyz);
    let sky_colour = to_linear(frame.light_ambient.xyz);
    let n = normalize(input.normal);
    let to_sun = -normalize(frame.light_direction.xyz);
    let facing = max(dot(n, to_sun), 0.0);
    let sun_strength = clamp(dot(sun_colour, vec3<f32>(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
    // The sun's shadow and the ambient occlusion (SceneEffects).
    let shadow = effects_shadow(input.world.xyz, n, input.clip);
    let ambient = sky_colour * (1.0 - 0.65 * sun_strength) * mix(0.55, 1.0, n.y * 0.5 + 0.5)
        * mix(1.0, frame.shading.x, 1.0 - sun_strength) * effects_ao(input.clip);
    var lamps = vec3<f32>(0.0);
    let physical = frame.fog_distance.w > 0.5; // physical_light_falloff()
    if (effects.flags.x != 0u) {
        let cluster = effects_cluster(input.clip, input.world.xyz);
        let count = min(cluster_counts[cluster], effects.clusters.w);
        let first = cluster * effects.clusters.w;
        for (var i = 0u; i < count; i = i + 1u) {
            let index = cluster_lights[first + i];
            let light = lamp_light(scene_lights[index], input.world, n, physical);
            if (any(light > vec3<f32>(0.0))) {
                lamps = lamps + light * effects_lamp_shadow(index, input.world.xyz, n, input.clip);
            }
        }
    } else {
        let lights = ring_count(input.ring);
        for (var i = 0; i < lights; i = i + 1) {
            let ring_light = frame.lights[input.ring * 9u + u32(i)];
            let light = lamp_light(ring_light, input.world, n, physical);
            if (any(light > vec3<f32>(0.0))) {
                lamps = lamps + light * effects_lamp_shadow_of(ring_light, input.world.xyz, n, input.clip);
            }
        }
    }
    lamps = lamps * (1.0 - effects.lamps.x); // weaker by day
    var colour = base * (ambient + sun_colour * facing * shadow + lamps);
    if (effects.flags.z != 0u) {
        colour = effects_aerial(colour, input.world.xyz);
    }
    if (effects.flags.y == 2u) {
        return vec4<f32>(vec3<f32>(effects_ao(input.clip)), texel.w); // the occlusion itself (--effects show-ao)
    }
    var display = to_display(select(shoulder(colour), max(colour, vec3<f32>(0.0)), hdr_output));
    if (effects.flags.z == 0u && input.fog.w > 0.0) {
        display = mix(display, input.fog.xyz, input.fog.w);
    }
    return vec4<f32>(display, texel.w);
}

@fragment
fn fs_main(input: VertexOut) -> @location(0) vec4<f32> {
    let texel = textureSample(base_texture, terrain_sampler, input.uv)
        * textureSample(micro_texture, terrain_sampler, input.uv * 16.0) * 2.0;
    if (experimental) {
        if (texel.w <= 0.0) {
            return vec4<f32>(0.0);
        }
        return shade_experimental(input, texel);
    }
    var color = vec4<f32>(0.0);
    if (texel.w > 0.0) {
        // The further lights as RTS adds them to models: the strongest wins.
        var light_colour = vec4<f32>(0.0);
        let normal = normalize(input.normal);
        let lights = ring_count(input.ring);
        for (var i = 0; i < lights; i = i + 1) {
            let light = frame.lights[input.ring * 9u + u32(i)];
            let factor = light_factor(light, input.world, normal);
            if (factor < 0.0) { continue; }
            light_colour = max(light_colour, light.diffuse * factor);
        }
        color = texel * max(input.colour, light_colour);
        if (input.fog.w > 0.0) {
            color = vec4<f32>(((1.0 - input.fog.w) * color + input.fog.w * input.fog).xyz, color.w);
        }
    }
    return color;
}
)";

// Depth only (SceneEffects): each tile's whole mesh in one draw.
constexpr char terrain_depth_shader[] = R"(
struct Draw {
    mvp: mat4x4<f32>,
    offset: vec4<f32>,
};
@group(0) @binding(0) var<uniform> draw: Draw;

@vertex
fn vs_depth(@location(0) position: vec3<f32>) -> @builtin(position) vec4<f32> {
    return draw.mvp * vec4<f32>(position, 1.0);
}
)";

[[nodiscard]] WGPUStringView wgsl_name(const char* name) {
    return {.data = name, .length = WGPU_STRLEN};
}

[[nodiscard]] WGPUBuffer create_buffer(const WGPUDevice device,
                                       const WGPUBufferUsage usage,
                                       const std::uint64_t size,
                                       const void* data = nullptr) {
    const WGPUBufferDescriptor descriptor{.usage = usage | WGPUBufferUsage_CopyDst, .size = size};
    const auto buffer = wgpuDeviceCreateBuffer(device, &descriptor);
    if (!buffer || !data) return buffer;
    if (const auto queue = wgpuDeviceGetQueue(device)) {
        wgpuQueueWriteBuffer(queue, buffer, 0, data, size);
        wgpuQueueRelease(queue);
    }
    return buffer;
}

void copy_vec4(float (&target)[4], const glm::vec4& value) {
    target[0] = value.x;
    target[1] = value.y;
    target[2] = value.z;
    target[3] = value.w;
}

template <typename T>
[[nodiscard]] bool is_ready(const std::future<T>& task) {
    using namespace std::chrono_literals;
    return task.valid() && task.wait_for(0ms) == std::future_status::ready;
}

} // namespace

struct TerrainRenderer::Impl {
    struct GpuTile {
        WGPUBuffer vertices = nullptr;
        WGPUBuffer indices = nullptr;
        std::vector<TerrainPatchMesh> patches;
        WGPUBuffer water = nullptr;
        std::uint32_t water_vertices = 0;
    };

    // An environment water layer and where its animation is (0x4A2144):
    // flag 1 tiles, 2 scrolls, 4 stamps frames in turn.
    struct WaterLayer {
        std::optional<core::VirtualPath> texture;
        bool blended = false;
        bool sky_reflection = false;
        float height = 0.0f;
        std::uint32_t flags = 0;
        float tiles[2]{1.0f, 1.0f};
        float scroll_speed[2]{};
        std::vector<std::array<float, 4>> stamps;
        float frame_length = 0.0f;
        float scroll[2]{};
        float frame = 0.0f;
    };

    struct GpuTexture {
        std::future<formats::AceDecodeResult> decoding;
        WgpuTextureUploadResult texture;
        WGPUTextureView view = nullptr;
        bool failed = false;
    };

    WGPUDevice device = nullptr;
    WGPUBindGroupLayout material_layout = nullptr;
    WGPUBindGroupLayout frame_layout = nullptr;
    WGPURenderPipeline pipeline = nullptr;
    WGPURenderPipeline experimental_pipeline = nullptr;
    WGPURenderPipeline water_pipelines[2][2]{}; // [experimental][blended]
    std::vector<WaterLayer> water_layers;
    glm::vec3 eye{};
    ShadingPath shading = ShadingPath::rts_compatible;
    // Group 2 (SceneEffects), read by the experimental pipeline; a built-in
    // one with every effect off when none is given.
    WGPUBindGroupLayout effects_layout = nullptr;
    std::unique_ptr<SceneEffects> fallback_effects;
    // Depth only: a view-projection per slot and tile (dynamic offset).
    static constexpr std::uint32_t depth_slots = 14; // cascades, the focus, 8 lamps
    WGPUBindGroupLayout depth_layout = nullptr;
    WGPURenderPipeline shadow_pipeline = nullptr;
    WGPURenderPipeline camera_depth_pipeline = nullptr;
    WGPUBuffer depth_buffer = nullptr;
    std::uint64_t depth_capacity = 0; // tiles a slot holds
    WGPUBindGroup depth_binding = nullptr;
    WGPUSampler sampler = nullptr;
    WgpuTextureUploadResult black;  // failed base texture: RTS draws with texture 0
    WgpuTextureUploadResult grey;   // missing microtexture: neutral under the * 2 detail blend
    WGPUTextureView black_view = nullptr;
    WGPUTextureView grey_view = nullptr;

    WGPUBuffer frame_buffer = nullptr;
    WGPUBuffer draw_buffer = nullptr;
    std::uint64_t draw_capacity = 0;
    WGPUBindGroup frame_binding = nullptr;

    std::shared_ptr<const core::Vfs> vfs;
    core::VirtualPath route;
    formats::RoutePosition origin;
    int radius = 2;
    rts::Season season = rts::Season::summer;
    rts::Weather weather = rts::Weather::clear;

    std::map<TileKey, GpuTile> tiles;
    std::map<TileKey, std::future<TerrainTileMesh>> tile_jobs;
    std::map<TileKey, bool> failed_tiles;
    std::map<std::string, GpuTexture> textures; // by virtual path key
    std::map<std::pair<std::string, std::string>, WGPUBindGroup> materials;

    ~Impl() {
        for (auto& [_, job] : tile_jobs) {
            if (job.valid()) job.wait();
        }
        for (auto& [_, texture] : textures) {
            if (texture.decoding.valid()) texture.decoding.wait();
        }
        release_materials();
        for (auto& [_, tile] : tiles) release_tile(tile);
        for (auto& [_, texture] : textures) {
            if (texture.view) wgpuTextureViewRelease(texture.view);
        }
        if (frame_binding) wgpuBindGroupRelease(frame_binding);
        if (draw_buffer) wgpuBufferRelease(draw_buffer);
        if (frame_buffer) wgpuBufferRelease(frame_buffer);
        if (grey_view) wgpuTextureViewRelease(grey_view);
        if (black_view) wgpuTextureViewRelease(black_view);
        if (sampler) wgpuSamplerRelease(sampler);
        if (pipeline) wgpuRenderPipelineRelease(pipeline);
        if (experimental_pipeline) wgpuRenderPipelineRelease(experimental_pipeline);
        for (auto& pair : water_pipelines) {
            for (auto& water_pipeline : pair) {
                if (water_pipeline) wgpuRenderPipelineRelease(water_pipeline);
            }
        }
        fallback_effects.reset();
        if (depth_binding) wgpuBindGroupRelease(depth_binding);
        if (depth_buffer) wgpuBufferRelease(depth_buffer);
        if (shadow_pipeline) wgpuRenderPipelineRelease(shadow_pipeline);
        if (camera_depth_pipeline) wgpuRenderPipelineRelease(camera_depth_pipeline);
        if (depth_layout) wgpuBindGroupLayoutRelease(depth_layout);
        if (effects_layout) wgpuBindGroupLayoutRelease(effects_layout);
        if (frame_layout) wgpuBindGroupLayoutRelease(frame_layout);
        if (material_layout) wgpuBindGroupLayoutRelease(material_layout);
    }

    static void release_tile(GpuTile& tile) {
        if (tile.water) wgpuBufferRelease(tile.water);
        if (tile.indices) wgpuBufferRelease(tile.indices);
        if (tile.vertices) wgpuBufferRelease(tile.vertices);
        tile = {};
    }

    void release_materials() {
        for (auto& [_, binding] : materials) {
            if (binding) wgpuBindGroupRelease(binding);
        }
        materials.clear();
    }

    // Returns the texture view once decoded and uploaded; starts decoding on first use.
    [[nodiscard]] std::optional<WGPUTextureView> texture_view(const std::string& name, const WGPUTextureView fallback) {
        if (name.empty()) return fallback;
        const auto path = resolve_terrain_texture(*vfs, route, name, season, weather);
        if (!path) return fallback;
        return texture_view_at(path, fallback);
    }

    [[nodiscard]] std::optional<WGPUTextureView> texture_view_at(const std::optional<core::VirtualPath>& path, const WGPUTextureView fallback) {
        if (!path) return fallback;
        auto [it, inserted] = textures.try_emplace(path->key());
        auto& texture = it->second;
        if (inserted) {
            texture.decoding = std::async(std::launch::async, [vfs = vfs, path = *path] { return formats::decode_ace_rgba(*vfs, path); });
        }
        if (is_ready(texture.decoding)) {
            auto decoded = texture.decoding.get();
            if (decoded.ok()) {
                texture.texture = upload_rgba8_texture(
                    device,
                    {.width = decoded.image->width, .height = decoded.image->height, .pixels = &decoded.image->pixels,
                     .mips = &decoded.image->mips},
                    std::string(path->filename()));
                if (texture.texture.ok()) texture.view = wgpuTextureCreateView(texture.texture.texture.get(), nullptr);
            }
            if (!texture.view) {
                texture.failed = true;
                std::cout << "terrain texture '" << path->str() << "': "
                          << (decoded.ok() ? std::string("upload failed") : decoded.error) << '\n';
            }
        }
        if (texture.view) return texture.view;
        if (texture.failed) return fallback;
        return std::nullopt;
    }

    // A water layer's material: its texture and the neutral grey detail.
    [[nodiscard]] WGPUBindGroup water_material(const WaterLayer& layer) {
        const auto key = std::pair{std::string("water:") + (layer.texture ? layer.texture->key() : std::string()), std::string()};
        if (const auto it = materials.find(key); it != materials.end()) return it->second;
        const auto base = texture_view_at(layer.texture, black_view);
        if (!base) return nullptr;
        std::array<WGPUBindGroupEntry, 3> entries{};
        entries[0].binding = 0;
        entries[0].textureView = *base;
        entries[1].binding = 1;
        entries[1].textureView = grey_view;
        entries[2].binding = 2;
        entries[2].sampler = sampler;
        const WGPUBindGroupDescriptor descriptor{.layout = material_layout, .entryCount = entries.size(), .entries = entries.data()};
        const auto binding = wgpuDeviceCreateBindGroup(device, &descriptor);
        materials.emplace(key, binding);
        return binding;
    }

    [[nodiscard]] WGPUBindGroup material(const TerrainPatchMesh& patch) {
        const auto key = std::pair{patch.texture, patch.microtexture};
        if (const auto it = materials.find(key); it != materials.end()) return it->second;

        const auto base = texture_view(patch.texture, black_view);
        const auto micro = texture_view(patch.microtexture, grey_view);
        if (!base || !micro) return nullptr; // still decoding

        std::array<WGPUBindGroupEntry, 3> entries{};
        entries[0].binding = 0;
        entries[0].textureView = *base;
        entries[1].binding = 1;
        entries[1].textureView = *micro;
        entries[2].binding = 2;
        entries[2].sampler = sampler;
        const WGPUBindGroupDescriptor descriptor{
            .layout = material_layout,
            .entryCount = entries.size(),
            .entries = entries.data(),
        };
        const auto binding = wgpuDeviceCreateBindGroup(device, &descriptor);
        materials.emplace(key, binding);
        return binding;
    }

    void ensure_draw_capacity(const std::uint64_t count) {
        if (count <= draw_capacity && draw_buffer) return;
        auto capacity = (std::max)(draw_capacity, std::uint64_t{32});
        while (capacity < count) capacity *= 2;

        if (frame_binding) wgpuBindGroupRelease(frame_binding);
        if (draw_buffer) wgpuBufferRelease(draw_buffer);
        draw_buffer = create_buffer(device, WGPUBufferUsage_Uniform, capacity * draw_uniform_stride);
        draw_capacity = capacity;

        std::array<WGPUBindGroupEntry, 2> entries{};
        entries[0].binding = 0;
        entries[0].buffer = draw_buffer;
        entries[0].size = sizeof(DrawUniform);
        entries[1].binding = 1;
        entries[1].buffer = frame_buffer;
        entries[1].size = sizeof(FrameUniform);
        const WGPUBindGroupDescriptor descriptor{
            .layout = frame_layout,
            .entryCount = entries.size(),
            .entries = entries.data(),
        };
        frame_binding = wgpuDeviceCreateBindGroup(device, &descriptor);
    }

    void ensure_depth_capacity(const std::uint64_t tiles_count) {
        if (depth_buffer && tiles_count <= depth_capacity) return;
        auto capacity = (std::max)(depth_capacity, std::uint64_t{32});
        while (capacity < tiles_count) capacity *= 2;
        if (depth_binding) wgpuBindGroupRelease(depth_binding);
        if (depth_buffer) wgpuBufferRelease(depth_buffer);
        depth_buffer = create_buffer(device, WGPUBufferUsage_Uniform, capacity * depth_slots * draw_uniform_stride);
        depth_capacity = capacity;
        const WGPUBindGroupEntry entry{.binding = 0, .buffer = depth_buffer, .size = sizeof(DrawUniform)};
        const WGPUBindGroupDescriptor descriptor{.layout = depth_layout, .entryCount = 1, .entries = &entry};
        depth_binding = wgpuDeviceCreateBindGroup(device, &descriptor);
    }

    void upload(TerrainTileMesh&& mesh) {
        const TileKey key{mesh.tile_x, mesh.tile_z};
        if (!mesh.ok()) {
            failed_tiles[key] = true;
            return;
        }
        GpuTile tile;
        tile.vertices = create_buffer(device, WGPUBufferUsage_Vertex, mesh.vertices.size() * sizeof(TerrainVertex),
                                      mesh.vertices.data());
        tile.indices = create_buffer(device, WGPUBufferUsage_Index, mesh.indices.size() * sizeof(std::uint32_t),
                                     mesh.indices.data());
        tile.patches = std::move(mesh.patches);
        if (!mesh.water.empty()) {
            tile.water = create_buffer(device, WGPUBufferUsage_Vertex, mesh.water.size() * sizeof(TerrainWaterVertex), mesh.water.data());
            tile.water_vertices = static_cast<std::uint32_t>(mesh.water.size());
        }
        tiles[key] = std::move(tile);
    }
};

namespace {

[[nodiscard]] WGPUBindGroupLayout create_material_layout(const WGPUDevice device) {
    std::array<WGPUBindGroupLayoutEntry, 3> entries{};
    for (std::uint32_t i = 0; i < 2; ++i) {
        entries[i].binding = i;
        entries[i].visibility = WGPUShaderStage_Fragment;
        entries[i].texture.sampleType = WGPUTextureSampleType_Float;
        entries[i].texture.viewDimension = WGPUTextureViewDimension_2D;
    }
    entries[2].binding = 2;
    entries[2].visibility = WGPUShaderStage_Fragment;
    entries[2].sampler.type = WGPUSamplerBindingType_Filtering;
    const WGPUBindGroupLayoutDescriptor descriptor{.entryCount = entries.size(), .entries = entries.data()};
    return wgpuDeviceCreateBindGroupLayout(device, &descriptor);
}

[[nodiscard]] WGPUBindGroupLayout create_frame_layout(const WGPUDevice device) {
    std::array<WGPUBindGroupLayoutEntry, 2> entries{};
    entries[0].binding = 0;
    entries[0].visibility = WGPUShaderStage_Vertex;
    entries[0].buffer.type = WGPUBufferBindingType_Uniform;
    entries[0].buffer.hasDynamicOffset = true;
    entries[0].buffer.minBindingSize = sizeof(DrawUniform);
    entries[1].binding = 1;
    entries[1].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    entries[1].buffer.type = WGPUBufferBindingType_Uniform;
    entries[1].buffer.minBindingSize = sizeof(FrameUniform);
    const WGPUBindGroupLayoutDescriptor descriptor{.entryCount = entries.size(), .entries = entries.data()};
    return wgpuDeviceCreateBindGroupLayout(device, &descriptor);
}

[[nodiscard]] WGPURenderPipeline create_depth_pipeline(const WGPUDevice device, const WGPUPipelineLayout layout, const WGPUShaderModule shader,
                                                       const bool shadow) {
    const WGPUVertexAttribute attribute{.format = WGPUVertexFormat_Float32x3, .offset = offsetof(TerrainVertex, position), .shaderLocation = 0};
    const WGPUVertexBufferLayout vertex_layout{
        .stepMode = WGPUVertexStepMode_Vertex,
        .arrayStride = sizeof(TerrainVertex),
        .attributeCount = 1,
        .attributes = &attribute,
    };
    constexpr WGPUStencilFaceState keep{
        .compare = WGPUCompareFunction_Always,
        .failOp = WGPUStencilOperation_Keep,
        .depthFailOp = WGPUStencilOperation_Keep,
        .passOp = WGPUStencilOperation_Keep,
    };
    const WGPUDepthStencilState depth{
        .format = depth_format,
        .depthWriteEnabled = WGPUOptionalBool_True,
        .depthCompare = shadow ? WGPUCompareFunction_LessEqual : depth_nearer_or_equal,
        .stencilFront = keep,
        .stencilBack = keep,
        .stencilReadMask = ~0u,
        .stencilWriteMask = ~0u,
        .depthBias = shadow ? 2 : 0,
        .depthBiasSlopeScale = shadow ? 2.0f : 0.0f,
        .depthBiasClamp = 0.0f,
    };
    const WGPURenderPipelineDescriptor descriptor{
        .layout = layout,
        .vertex = {.module = shader, .entryPoint = wgsl_name("vs_depth"), .bufferCount = 1, .buffers = &vertex_layout},
        .primitive = {.topology = WGPUPrimitiveTopology_TriangleList},
        .depthStencil = &depth,
        .multisample = {.count = 1, .mask = ~0u, .alphaToCoverageEnabled = false},
        .fragment = nullptr,
    };
    return wgpuDeviceCreateRenderPipeline(device, &descriptor);
}

[[nodiscard]] WGPURenderPipeline create_pipeline(const WGPUDevice device,
                                                 const WGPUPipelineLayout layout,
                                                 const WGPUShaderModule shader,
                                                 const WGPUTextureFormat target_format, const bool experimental) {
    const std::array<WGPUVertexAttribute, 3> attributes{{
        {.format = WGPUVertexFormat_Float32x3, .offset = offsetof(TerrainVertex, position), .shaderLocation = 0},
        {.format = WGPUVertexFormat_Float32x3, .offset = offsetof(TerrainVertex, normal), .shaderLocation = 1},
        {.format = WGPUVertexFormat_Float32x2, .offset = offsetof(TerrainVertex, uv), .shaderLocation = 2},
    }};
    const WGPUVertexBufferLayout vertex_layout{
        .stepMode = WGPUVertexStepMode_Vertex,
        .arrayStride = sizeof(TerrainVertex),
        .attributeCount = attributes.size(),
        .attributes = attributes.data(),
    };
    constexpr WGPUStencilFaceState keep_stencil{
        .compare = WGPUCompareFunction_Always,
        .failOp = WGPUStencilOperation_Keep,
        .depthFailOp = WGPUStencilOperation_Keep,
        .passOp = WGPUStencilOperation_Keep,
    };
    // RTS terrain: depth test GL_LEQUAL with writes, blend and alpha test off.
    const WGPUDepthStencilState depth{
        .format = depth_format,
        .depthWriteEnabled = WGPUOptionalBool_True,
        .depthCompare = depth_nearer_or_equal,
        .stencilFront = keep_stencil,
        .stencilBack = keep_stencil,
        .stencilReadMask = ~0u,
        .stencilWriteMask = ~0u,
    };
    const WGPUColorTargetState target{.format = target_format, .writeMask = WGPUColorWriteMask_All};
    const std::array<WGPUConstantEntry, 2> constants{{
        {.key = wgsl_name("experimental"), .value = experimental ? 1.0 : 0.0},
        {.key = wgsl_name("hdr_output"), .value = scene_hdr() ? 1.0 : 0.0},
    }};
    const WGPUFragmentState fragment{
        .module = shader,
        .entryPoint = wgsl_name("fs_main"),
        .constantCount = constants.size(),
        .constants = constants.data(),
        .targetCount = 1,
        .targets = &target,
    };
    const WGPURenderPipelineDescriptor descriptor{
        .layout = layout,
        .vertex = {
            .module = shader,
            .entryPoint = wgsl_name("vs_main"),
            .bufferCount = 1,
            .buffers = &vertex_layout,
        },
        .primitive = {.topology = WGPUPrimitiveTopology_TriangleList},
        .depthStencil = &depth,
        .multisample = scene_multisample(),
        .fragment = &fragment,
    };
    return wgpuDeviceCreateRenderPipeline(device, &descriptor);
}

// A water layer (RTS 0x4A2144): depth test GL_LEQUAL with writes; the
// blend shaders laid over by alpha, the others drawn opaque.
[[nodiscard]] WGPURenderPipeline create_water_pipeline(const WGPUDevice device, const WGPUPipelineLayout layout, const WGPUShaderModule shader,
                                                       const WGPUTextureFormat target_format, const bool experimental, const bool blended) {
    const std::array<WGPUVertexAttribute, 3> attributes{{
        {.format = WGPUVertexFormat_Float32x3, .offset = offsetof(TerrainWaterVertex, position), .shaderLocation = 0},
        {.format = WGPUVertexFormat_Float32x2, .offset = offsetof(TerrainWaterVertex, patch), .shaderLocation = 1},
        {.format = WGPUVertexFormat_Float32x2, .offset = offsetof(TerrainWaterVertex, corner), .shaderLocation = 2},
    }};
    const WGPUVertexBufferLayout vertex_layout{
        .stepMode = WGPUVertexStepMode_Vertex,
        .arrayStride = sizeof(TerrainWaterVertex),
        .attributeCount = attributes.size(),
        .attributes = attributes.data(),
    };
    constexpr WGPUStencilFaceState keep_stencil{
        .compare = WGPUCompareFunction_Always,
        .failOp = WGPUStencilOperation_Keep,
        .depthFailOp = WGPUStencilOperation_Keep,
        .passOp = WGPUStencilOperation_Keep,
    };
    const WGPUDepthStencilState depth{
        .format = depth_format,
        .depthWriteEnabled = WGPUOptionalBool_True,
        .depthCompare = depth_nearer_or_equal,
        .stencilFront = keep_stencil,
        .stencilBack = keep_stencil,
        .stencilReadMask = ~0u,
        .stencilWriteMask = ~0u,
    };
    const WGPUBlendState over{
        .color = {.operation = WGPUBlendOperation_Add, .srcFactor = WGPUBlendFactor_SrcAlpha, .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha},
        .alpha = {.operation = WGPUBlendOperation_Add, .srcFactor = WGPUBlendFactor_One, .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha},
    };
    const WGPUColorTargetState target{.format = target_format, .blend = blended ? &over : nullptr, .writeMask = WGPUColorWriteMask_All};
    const std::array<WGPUConstantEntry, 2> constants{{
        {.key = wgsl_name("experimental"), .value = experimental ? 1.0 : 0.0},
        {.key = wgsl_name("hdr_output"), .value = scene_hdr() ? 1.0 : 0.0},
    }};
    const WGPUFragmentState fragment{
        .module = shader,
        .entryPoint = wgsl_name("fs_main"),
        .constantCount = constants.size(),
        .constants = constants.data(),
        .targetCount = 1,
        .targets = &target,
    };
    const WGPURenderPipelineDescriptor descriptor{
        .layout = layout,
        .vertex = {.module = shader, .entryPoint = wgsl_name("vs_water"), .bufferCount = 1, .buffers = &vertex_layout},
        .primitive = {.topology = WGPUPrimitiveTopology_TriangleList},
        .depthStencil = &depth,
        .multisample = scene_multisample(),
        .fragment = &fragment,
    };
    return wgpuDeviceCreateRenderPipeline(device, &descriptor);
}

} // namespace

TerrainRenderer::TerrainRenderer(const WGPUDevice device,
                                 const WGPUTextureFormat target_format,
                                 std::shared_ptr<const core::Vfs> vfs,
                                 core::VirtualPath route,
                                 const formats::RoutePosition origin,
                                 const int radius)
    : impl_(std::make_unique<Impl>()) {
    impl_->device = device;
    impl_->vfs = std::move(vfs);
    impl_->route = std::move(route);
    impl_->origin = origin;
    impl_->radius = (std::max)(radius, 0);
    impl_->material_layout = create_material_layout(device);
    impl_->frame_layout = create_frame_layout(device);
    impl_->frame_buffer = create_buffer(device, WGPUBufferUsage_Uniform, sizeof(FrameUniform));

    const WGPUSamplerDescriptor sampler{
        .addressModeU = WGPUAddressMode_Repeat,
        .addressModeV = WGPUAddressMode_Repeat,
        .addressModeW = WGPUAddressMode_Repeat,
        .magFilter = WGPUFilterMode_Linear,
        .minFilter = WGPUFilterMode_Linear,
        .mipmapFilter = WGPUMipmapFilterMode_Linear,
        .lodMaxClamp = 32.0f,
        .maxAnisotropy = max_anisotropy,
    };
    impl_->sampler = wgpuDeviceCreateSampler(device, &sampler);

    static const std::vector<std::uint8_t> black_pixel{0, 0, 0, 255};
    static const std::vector<std::uint8_t> grey_pixel{128, 128, 128, 255};
    impl_->black = upload_rgba8_texture(device, {.width = 1, .height = 1, .pixels = &black_pixel}, "terrain-black");
    impl_->grey = upload_rgba8_texture(device, {.width = 1, .height = 1, .pixels = &grey_pixel}, "terrain-grey");
    if (impl_->black.ok()) impl_->black_view = wgpuTextureCreateView(impl_->black.texture.get(), nullptr);
    if (impl_->grey.ok()) impl_->grey_view = wgpuTextureCreateView(impl_->grey.texture.get(), nullptr);

    // One module for both paths (the `experimental` override): both read
    // the effects (group 2), so both layouts have it.
    const auto shader_code = std::string(detail::light_shapes_wgsl) + detail::effects_struct_wgsl + detail::effects_main_wgsl + terrain_shader;
    const WGPUShaderSourceWGSL source{
        .chain = {.sType = WGPUSType_ShaderSourceWGSL},
        .code = {.data = shader_code.data(), .length = shader_code.size()},
    };
    const WGPUShaderModuleDescriptor shader_descriptor{.nextInChain = &source.chain};
    const auto shader = wgpuDeviceCreateShaderModule(device, &shader_descriptor);
    impl_->effects_layout = create_scene_effects_layout(device);
    const std::array<WGPUBindGroupLayout, 3> layouts{impl_->material_layout, impl_->frame_layout, impl_->effects_layout};
    const WGPUPipelineLayoutDescriptor layout_descriptor{
        .bindGroupLayoutCount = layouts.size(),
        .bindGroupLayouts = layouts.data(),
    };
    const auto pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &layout_descriptor);
    impl_->pipeline = create_pipeline(device, pipeline_layout, shader, target_format, false);
    impl_->experimental_pipeline = create_pipeline(device, pipeline_layout, shader, target_format, true);
    for (int experimental = 0; experimental < 2; ++experimental) {
        for (int blended = 0; blended < 2; ++blended) {
            impl_->water_pipelines[experimental][blended] =
                create_water_pipeline(device, pipeline_layout, shader, target_format, experimental != 0, blended != 0);
        }
    }
    wgpuPipelineLayoutRelease(pipeline_layout);
    wgpuShaderModuleRelease(shader);

    {
        WGPUBindGroupLayoutEntry entry{};
        entry.binding = 0;
        entry.visibility = WGPUShaderStage_Vertex;
        entry.buffer.type = WGPUBufferBindingType_Uniform;
        entry.buffer.hasDynamicOffset = true;
        entry.buffer.minBindingSize = sizeof(DrawUniform);
        const WGPUBindGroupLayoutDescriptor descriptor{.entryCount = 1, .entries = &entry};
        impl_->depth_layout = wgpuDeviceCreateBindGroupLayout(device, &descriptor);
        const WGPUPipelineLayoutDescriptor depth_layout_descriptor{.bindGroupLayoutCount = 1, .bindGroupLayouts = &impl_->depth_layout};
        const auto depth_pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &depth_layout_descriptor);
        const WGPUShaderSourceWGSL depth_source{.chain = {.sType = WGPUSType_ShaderSourceWGSL}, .code = wgsl_name(terrain_depth_shader)};
        const WGPUShaderModuleDescriptor depth_descriptor{.nextInChain = &depth_source.chain};
        const auto depth_shader = wgpuDeviceCreateShaderModule(device, &depth_descriptor);
        impl_->shadow_pipeline = create_depth_pipeline(device, depth_pipeline_layout, depth_shader, true);
        impl_->camera_depth_pipeline = create_depth_pipeline(device, depth_pipeline_layout, depth_shader, false);
        wgpuShaderModuleRelease(depth_shader);
        wgpuPipelineLayoutRelease(depth_pipeline_layout);
    }
}

TerrainRenderer::~TerrainRenderer() = default;
TerrainRenderer::TerrainRenderer(TerrainRenderer&&) noexcept = default;
TerrainRenderer& TerrainRenderer::operator=(TerrainRenderer&&) noexcept = default;

bool TerrainRenderer::ready() const noexcept {
    return impl_ && impl_->pipeline && impl_->sampler && impl_->frame_buffer && impl_->black_view && impl_->grey_view;
}

void TerrainRenderer::set_conditions(const rts::Season season, const rts::Weather weather) {
    if (impl_->season == season && impl_->weather == weather) return;
    impl_->season = season;
    impl_->weather = weather;
    // Texture paths depend on the conditions; decoded textures stay cached by path.
    impl_->release_materials();
}

void TerrainRenderer::set_shading_path(const ShadingPath path) noexcept {
    if (impl_) impl_->shading = path;
}

void TerrainRenderer::set_origin(const formats::RoutePosition& origin) {
    if (impl_) impl_->origin = origin;
}

void TerrainRenderer::update(const formats::RoutePosition& camera) {
    auto& impl = *impl_;
    const auto tiles = impl.route.resolve(std::string_view("TILES")).value_or(impl.route);

    // Collect finished tile meshes.
    for (auto it = impl.tile_jobs.begin(); it != impl.tile_jobs.end();) {
        if (!is_ready(it->second)) {
            ++it;
            continue;
        }
        impl.upload(it->second.get());
        it = impl.tile_jobs.erase(it);
    }

    // Wanted tiles, nearest first.
    std::vector<TileKey> wanted;
    for (int dz = -impl.radius; dz <= impl.radius; ++dz) {
        for (int dx = -impl.radius; dx <= impl.radius; ++dx) wanted.push_back({camera.tile_x + dx, camera.tile_z + dz});
    }
    std::sort(wanted.begin(), wanted.end(), [&](const TileKey& a, const TileKey& b) {
        const auto distance = [&](const TileKey& key) {
            return std::abs(key.first - camera.tile_x) + std::abs(key.second - camera.tile_z);
        };
        return distance(a) < distance(b);
    });
    for (const auto& key : wanted) {
        if (impl.tile_jobs.size() >= max_concurrent_tile_jobs) break;
        if (impl.tiles.contains(key) || impl.tile_jobs.contains(key) || impl.failed_tiles.contains(key)) continue;
        impl.tile_jobs.emplace(key, std::async(std::launch::async, [vfs = impl.vfs, tiles, key] {
                                   return build_terrain_tile_mesh(*vfs, tiles, key.first, key.second);
                               }));
    }

    // Drop tiles well outside the radius.
    const auto keep = impl.radius + 1;
    for (auto it = impl.tiles.begin(); it != impl.tiles.end();) {
        if (std::abs(it->first.first - camera.tile_x) > keep || std::abs(it->first.second - camera.tile_z) > keep) {
            Impl::release_tile(it->second);
            it = impl.tiles.erase(it);
        } else {
            ++it;
        }
    }
}

void TerrainRenderer::prepare_depth() {
    if (!ready() || impl_->tiles.empty()) return;
    impl_->ensure_depth_capacity(impl_->tiles.size());
}

void TerrainRenderer::render_depth(const WGPURenderPassEncoder pass, const std::uint32_t slot, const glm::mat4& view_projection,
                                   const DepthPassKind kind) {
    OPENRTS_ZONE("terrain: depth pass");
    if (!ready() || impl_->tiles.empty() || slot >= Impl::depth_slots) return;
    auto& impl = *impl_;
    // Depth passes may run on several threads: nothing shared is changed here
    // (prepare_depth() sized the buffer before them).
    if (!impl.depth_binding || impl.depth_capacity < impl.tiles.size()) return;
    constexpr double tile_size = formats::route_tile_size;
    std::vector<std::byte> uniforms(impl.tiles.size() * draw_uniform_stride);
    std::size_t index = 0;
    for (const auto& [key, _] : impl.tiles) {
        const auto offset = glm::vec3{
            static_cast<float>((key.first - impl.origin.tile_x) * tile_size - impl.origin.x),
            static_cast<float>(-impl.origin.y),
            static_cast<float>((key.second - impl.origin.tile_z) * tile_size - impl.origin.z),
        };
        glm::mat4 model{1.0f};
        model[3] = glm::vec4{offset, 1.0f};
        const DrawUniform uniform{.mvp = view_projection * model, .offset = {offset.x, offset.y, offset.z, 0.0f}};
        std::memcpy(uniforms.data() + index * draw_uniform_stride, &uniform, sizeof(uniform));
        ++index;
    }
    const auto base = static_cast<std::uint64_t>(slot) * impl.depth_capacity * draw_uniform_stride;
    const auto queue = wgpuDeviceGetQueue(impl.device);
    wgpuQueueWriteBuffer(queue, impl.depth_buffer, base, uniforms.data(), uniforms.size());
    if (queue) wgpuQueueRelease(queue);
    wgpuRenderPassEncoderSetPipeline(pass, kind == DepthPassKind::shadow ? impl.shadow_pipeline : impl.camera_depth_pipeline);
    const auto frustum = Frustum::from_view_projection(view_projection);
    index = 0;
    for (auto& [key, tile] : impl.tiles) {
        const auto offset = static_cast<std::uint32_t>(base + index++ * draw_uniform_stride);
        if (tile.patches.empty()) continue;
        const auto tile_offset = glm::vec3{
            static_cast<float>((key.first - impl.origin.tile_x) * tile_size - impl.origin.x),
            static_cast<float>(-impl.origin.y),
            static_cast<float>((key.second - impl.origin.tile_z) * tile_size - impl.origin.z),
        };
        bool bound = false;
        // Runs of contiguous patches in view, one draw each.
        std::uint32_t run_first = 0, run_count = 0;
        const auto flush = [&] {
            if (run_count == 0) return;
            if (!bound) {
                wgpuRenderPassEncoderSetBindGroup(pass, 0, impl.depth_binding, 1, &offset);
                wgpuRenderPassEncoderSetVertexBuffer(pass, 0, tile.vertices, 0, WGPU_WHOLE_SIZE);
                wgpuRenderPassEncoderSetIndexBuffer(pass, tile.indices, WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
                bound = true;
            }
            wgpuRenderPassEncoderDrawIndexed(pass, run_count, 1, run_first, 0, 0);
            run_count = 0;
        };
        for (const auto& patch : tile.patches) {
            const glm::vec3 centre = tile_offset + glm::vec3{patch.center[0], patch.center[1], patch.center[2]};
            if (!frustum.intersects_sphere(centre, patch.radius)) {
                flush();
                continue;
            }
            if (run_count > 0 && run_first + run_count == patch.first_index) {
                run_count += patch.index_count;
            } else {
                flush();
                run_first = patch.first_index;
                run_count = patch.index_count;
            }
        }
        flush();
    }
}

void TerrainRenderer::render(const WGPURenderPassEncoder pass,
                             const glm::mat4& view_projection,
                             const ModelLighting& lighting,
                             WGPUBindGroup effects) {
    OPENRTS_ZONE("terrain: main pass");
    if (!ready() || impl_->tiles.empty()) return;
    auto& impl = *impl_;
    if (!effects) {
        if (!impl.fallback_effects) impl.fallback_effects = std::make_unique<SceneEffects>(impl.device);
        effects = impl.fallback_effects->bind_group();
    }
    if (!effects) return;

    // A draw for each tile, then one for each tile with water and each layer.
    std::size_t water_tiles = 0;
    for (const auto& [_, tile] : impl.tiles) water_tiles += tile.water_vertices > 0 ? 1 : 0;
    const std::size_t draws = impl.tiles.size() + water_tiles * impl.water_layers.size();
    impl.ensure_draw_capacity(draws);
    if (!impl.frame_binding) return;

    constexpr double tile_size = formats::route_tile_size;
    std::vector<std::byte> uniforms(draws * draw_uniform_stride);
    std::size_t index = 0;
    for (const auto& [key, _] : impl.tiles) {
        // Tile-centred vertices, placed relative to the route origin.
        const auto offset = glm::vec3{
            static_cast<float>((key.first - impl.origin.tile_x) * tile_size - impl.origin.x),
            static_cast<float>(-impl.origin.y),
            static_cast<float>((key.second - impl.origin.tile_z) * tile_size - impl.origin.z),
        };
        glm::mat4 model{1.0f};
        model[3] = glm::vec4{offset, 1.0f};
        const DrawUniform uniform{.mvp = view_projection * model, .offset = {offset.x, offset.y, offset.z, 0.0f}};
        std::memcpy(uniforms.data() + index * draw_uniform_stride, &uniform, sizeof(uniform));
        ++index;
    }
    const std::size_t first_water_draw = index;
    for (const auto& [key, tile] : impl.tiles) {
        if (tile.water_vertices == 0) continue;
        for (const auto& layer : impl.water_layers) {
            // RTS 0x4A2144: the layer's height above the water, its UVs a
            // patch (tiles, a frame's stamp) moved by the scroll.
            const auto offset = glm::vec3{
                static_cast<float>((key.first - impl.origin.tile_x) * tile_size - impl.origin.x),
                static_cast<float>(-impl.origin.y) + layer.height,
                static_cast<float>((key.second - impl.origin.tile_z) * tile_size - impl.origin.z),
            };
            glm::mat4 model{1.0f};
            model[3] = glm::vec4{offset, 1.0f};
            DrawUniform uniform{.mvp = view_projection * model, .offset = {offset.x, offset.y, offset.z, 0.0f}};
            float rect[4]{0.0f, 0.0f, 1.0f, 1.0f};
            if ((layer.flags & 4u) != 0 && !layer.stamps.empty()) {
                const auto frame = (std::min)(static_cast<std::size_t>(layer.frame), layer.stamps.size() - 1);
                const auto& stamp = layer.stamps[frame];
                rect[0] = stamp[0];
                rect[1] = stamp[1];
                rect[2] = stamp[2] - stamp[0];
                rect[3] = stamp[3] - stamp[1];
            } else if ((layer.flags & 1u) != 0) {
                rect[2] = layer.tiles[0];
                rect[3] = layer.tiles[1];
            }
            if ((layer.flags & 2u) != 0) {
                rect[0] += layer.scroll[0];
                rect[1] += layer.scroll[1];
            }
            std::copy(std::begin(rect), std::end(rect), uniform.uv_rect);
            if (layer.sky_reflection) {
                uniform.uv_mode[0] = 1.0f;
                uniform.uv_mode[1] = impl.eye.x;
                uniform.uv_mode[2] = impl.eye.z;
                uniform.uv_mode[3] = static_cast<float>(tile_size / 16.0);
            }
            std::memcpy(uniforms.data() + index * draw_uniform_stride, &uniform, sizeof(uniform));
            ++index;
        }
    }

    FrameUniform frame{};
    copy_vec4(frame.light_diffuse, lighting.sun_diffuse);
    copy_vec4(frame.light_ambient, lighting.sun_ambient);
    copy_vec4(frame.light_direction, glm::vec4{lighting.sun_direction, 0.0f});
    copy_vec4(frame.fog_colour, lighting.fog_colour);
    frame.fog_distance[0] = lighting.fog_start;
    frame.fog_distance[1] = lighting.fog_end;
    frame.fog_distance[3] = physical_light_falloff() ? 1.0f : 0.0f;
    frame.shading[0] = night_ambient_scale();
    const auto& rings = lighting.light_rings;
    for (std::size_t k = 0; k < LightRings::count; ++k) {
        const auto n = (std::min)(rings.lights[k].size(), LightRings::capacity);
        frame.ring_counts[k] = static_cast<std::int32_t>(n);
        for (std::size_t i = 0; i < n; ++i) detail::pack_light(frame.lights[k * LightRings::capacity + i], rings.lights[k][i]);
    }

    const auto queue = wgpuDeviceGetQueue(impl.device);
    wgpuQueueWriteBuffer(queue, impl.draw_buffer, 0, uniforms.data(), uniforms.size());
    wgpuQueueWriteBuffer(queue, impl.frame_buffer, 0, &frame, sizeof(frame));
    if (queue) wgpuQueueRelease(queue);

    wgpuRenderPassEncoderSetPipeline(
        pass, impl.shading == ShadingPath::experimental && impl.experimental_pipeline ? impl.experimental_pipeline : impl.pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 2, effects, 0, nullptr);
    const auto frustum = Frustum::from_view_projection(view_projection);
    index = 0;
    for (auto& [key, tile] : impl.tiles) {
        const auto tile_offset = glm::vec3{
            static_cast<float>((key.first - impl.origin.tile_x) * tile_size - impl.origin.x),
            static_cast<float>(-impl.origin.y),
            static_cast<float>((key.second - impl.origin.tile_z) * tile_size - impl.origin.z),
        };
        const auto offset = static_cast<std::uint32_t>(index++ * draw_uniform_stride);
        wgpuRenderPassEncoderSetBindGroup(pass, 1, impl.frame_binding, 1, &offset);
        wgpuRenderPassEncoderSetVertexBuffer(pass, 0, tile.vertices, 0, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderSetIndexBuffer(pass, tile.indices, WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
        for (const auto& patch : tile.patches) {
            if (!frustum.intersects_sphere(tile_offset + glm::vec3{patch.center[0], patch.center[1], patch.center[2]}, patch.radius)) continue;
            const auto material = impl.material(patch);
            if (!material) continue;
            wgpuRenderPassEncoderSetBindGroup(pass, 0, material, 0, nullptr);
            // The patch's ring of further lights (RTS 0x4A2AD4) as the first instance.
            const auto ring = rings.ring_for(tile_offset + glm::vec3{patch.center[0], patch.center[1], patch.center[2]}, patch.radius);
            wgpuRenderPassEncoderDrawIndexed(pass, patch.index_count, 1, patch.first_index, 0, ring);
        }
    }

    // The water layers in order over the water patches (RTS: right after the terrain).
    if (impl.water_layers.empty() || water_tiles == 0) return;
    const int path = impl.shading == ShadingPath::experimental ? 1 : 0;
    index = first_water_draw;
    for (auto& [key, tile] : impl.tiles) {
        if (tile.water_vertices == 0) continue;
        const auto tile_offset = glm::vec3{
            static_cast<float>((key.first - impl.origin.tile_x) * tile_size - impl.origin.x),
            static_cast<float>(-impl.origin.y),
            static_cast<float>((key.second - impl.origin.tile_z) * tile_size - impl.origin.z),
        };
        const bool visible = frustum.intersects_sphere(tile_offset, static_cast<float>(tile_size * 0.75));
        const auto ring = rings.ring_for(tile_offset, static_cast<float>(tile_size * 0.75));
        for (const auto& layer : impl.water_layers) {
            const auto offset = static_cast<std::uint32_t>(index++ * draw_uniform_stride);
            if (!visible) continue;
            const auto material = impl.water_material(layer);
            const auto pipeline = impl.water_pipelines[path][layer.blended ? 1 : 0];
            if (!material || !pipeline) continue;
            wgpuRenderPassEncoderSetPipeline(pass, pipeline);
            wgpuRenderPassEncoderSetBindGroup(pass, 0, material, 0, nullptr);
            wgpuRenderPassEncoderSetBindGroup(pass, 1, impl.frame_binding, 1, &offset);
            wgpuRenderPassEncoderSetVertexBuffer(pass, 0, tile.water, 0, WGPU_WHOLE_SIZE);
            wgpuRenderPassEncoderDraw(pass, tile.water_vertices, 1, 0, ring);
        }
    }
}

void TerrainRenderer::set_water(const formats::EnvWater* water, std::vector<core::VirtualPath> texture_folders) {
    auto& impl = *impl_;
    impl.water_layers.clear();
    if (!water) return;
    for (const auto& source : water->layers) {
        Impl::WaterLayer layer;
        layer.height = source.height;
        layer.sky_reflection = source.sky_reflection;
        const auto& shader = source.shader;
        // Tex / TexDiff opaque; BlendATex(Diff) and AddATex(Diff) by alpha (0x4A2144).
        std::string name = shader.shader_name;
        for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        layer.blended = name.find("blend") != std::string::npos || name.find("add") != std::string::npos;
        for (const auto& folder : texture_folders) {
            if (shader.texture.empty()) break;
            if (auto file = folder.resolve(shader.texture); file && impl.vfs->exists(*file)) {
                layer.texture = *file;
                break;
            }
        }
        if (!shader.frames.empty()) {
            const auto& first = shader.frames.front();
            if (first.has_uv_tiles) {
                layer.flags |= 1u;
                layer.tiles[0] = first.uv_tiles[0];
                layer.tiles[1] = first.uv_tiles[1];
            }
            if (first.has_uv_scroll) {
                layer.flags |= 2u;
                layer.scroll_speed[0] = first.uv_scroll[0];
                layer.scroll_speed[1] = first.uv_scroll[1];
            }
            for (const auto& frame : shader.frames) {
                if (frame.has_uv_stamp) layer.stamps.push_back({frame.uv_stamp[0], frame.uv_stamp[1], frame.uv_stamp[2], frame.uv_stamp[3]});
            }
            if (!layer.stamps.empty()) layer.flags |= 4u;
        }
        layer.frame_length = shader.frame_length;
        impl.water_layers.push_back(std::move(layer));
    }
}

void TerrainRenderer::update_water(const float elapsed, const glm::vec3& eye) {
    auto& impl = *impl_;
    impl.eye = eye;
    for (auto& layer : impl.water_layers) {
        // 0x4A2144: the scroll wraps past 1, the frames over their count.
        if ((layer.flags & 2u) != 0) {
            for (int k = 0; k < 2; ++k) {
                layer.scroll[k] += layer.scroll_speed[k] * elapsed;
                if (std::fabs(layer.scroll[k]) > 1.0f) layer.scroll[k] = std::fmod(layer.scroll[k], 1.0f);
            }
        }
        if ((layer.flags & 4u) != 0 && layer.frame_length > 0.0f) {
            layer.frame += elapsed / layer.frame_length;
            const auto frames = static_cast<float>(layer.stamps.size());
            if (layer.frame >= frames) layer.frame = std::fmod(layer.frame, frames);
        }
    }
}

std::size_t TerrainRenderer::water_quads() const noexcept {
    if (!impl_) return 0;
    std::size_t quads = 0;
    for (const auto& [_, tile] : impl_->tiles) quads += tile.water_vertices / 6;
    return quads;
}

std::size_t TerrainRenderer::loaded_tiles() const noexcept {
    return impl_ ? impl_->tiles.size() : 0;
}

std::size_t TerrainRenderer::pending_jobs() const noexcept {
    return impl_ ? impl_->tile_jobs.size() : 0;
}

} // namespace openrts::renderer
#endif
