#include <openrts/renderer/terrain_renderer.hpp>

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
#include <utility>

#include <openrts/formats/ace_file.hpp>
#include <openrts/renderer/webgpu_texture.hpp>

namespace openrts::renderer {
namespace {

using TileKey = std::pair<std::int32_t, std::int32_t>;

constexpr std::size_t max_concurrent_tile_jobs = 2;
constexpr std::uint64_t draw_uniform_stride = 256;

struct DrawUniform {
    glm::mat4 mvp;
};

struct FrameUniform {
    float light_diffuse[4];
    float light_ambient[4];
    float light_direction[4];
    float fog_colour[4];
    float fog_distance[4];
};

// Port of RTS terrain.vert/frag (additional light sources not yet fed).
constexpr char terrain_shader[] = R"(
struct Draw {
    mvp: mat4x4<f32>,
};

struct Frame {
    light_diffuse: vec4<f32>,
    light_ambient: vec4<f32>,
    light_direction: vec4<f32>,
    fog_colour: vec4<f32>,
    fog_distance: vec4<f32>,
};

@group(0) @binding(0) var base_texture: texture_2d<f32>;
@group(0) @binding(1) var micro_texture: texture_2d<f32>;
@group(0) @binding(2) var terrain_sampler: sampler;
@group(1) @binding(0) var<uniform> draw: Draw;
@group(1) @binding(1) var<uniform> frame: Frame;

struct VertexIn {
    @location(0) position: vec3<f32>,
    @location(1) normal: vec3<f32>,
    @location(2) uv: vec2<f32>,
};

struct VertexOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) colour: vec4<f32>,
    @location(2) fog: vec4<f32>,
};

@vertex
fn vs_main(input: VertexIn) -> VertexOut {
    var out: VertexOut;
    out.clip = draw.mvp * vec4<f32>(input.position, 1.0);
    out.uv = input.uv;

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

@fragment
fn fs_main(input: VertexOut) -> @location(0) vec4<f32> {
    let texel = textureSample(base_texture, terrain_sampler, input.uv)
        * textureSample(micro_texture, terrain_sampler, input.uv * 16.0) * 2.0;
    var color = vec4<f32>(0.0);
    if (texel.w > 0.0) {
        color = texel * input.colour;
        if (input.fog.w > 0.0) {
            color = vec4<f32>(((1.0 - input.fog.w) * color + input.fog.w * input.fog).xyz, color.w);
        }
    }
    return color;
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
        if (frame_layout) wgpuBindGroupLayoutRelease(frame_layout);
        if (material_layout) wgpuBindGroupLayoutRelease(material_layout);
    }

    static void release_tile(GpuTile& tile) {
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
                    {.width = decoded.image->width, .height = decoded.image->height, .pixels = &decoded.image->pixels},
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

[[nodiscard]] WGPURenderPipeline create_pipeline(const WGPUDevice device,
                                                 const WGPUPipelineLayout layout,
                                                 const WGPUShaderModule shader,
                                                 const WGPUTextureFormat target_format) {
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
    const WGPUFragmentState fragment{
        .module = shader,
        .entryPoint = wgsl_name("fs_main"),
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

    const WGPUShaderSourceWGSL source{
        .chain = {.sType = WGPUSType_ShaderSourceWGSL},
        .code = wgsl_name(terrain_shader),
    };
    const WGPUShaderModuleDescriptor shader_descriptor{.nextInChain = &source.chain};
    const auto shader = wgpuDeviceCreateShaderModule(device, &shader_descriptor);
    const std::array<WGPUBindGroupLayout, 2> layouts{impl_->material_layout, impl_->frame_layout};
    const WGPUPipelineLayoutDescriptor layout_descriptor{
        .bindGroupLayoutCount = layouts.size(),
        .bindGroupLayouts = layouts.data(),
    };
    const auto pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &layout_descriptor);
    impl_->pipeline = create_pipeline(device, pipeline_layout, shader, target_format);
    wgpuPipelineLayoutRelease(pipeline_layout);
    wgpuShaderModuleRelease(shader);
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

void TerrainRenderer::render(const WGPURenderPassEncoder pass,
                             const glm::mat4& view_projection,
                             const ModelLighting& lighting) {
    if (!ready() || impl_->tiles.empty()) return;
    auto& impl = *impl_;

    impl.ensure_draw_capacity(impl.tiles.size());
    if (!impl.frame_binding) return;

    constexpr double tile_size = formats::route_tile_size;
    std::vector<std::byte> uniforms(impl.tiles.size() * draw_uniform_stride);
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
        const DrawUniform uniform{.mvp = view_projection * model};
        std::memcpy(uniforms.data() + index * draw_uniform_stride, &uniform, sizeof(uniform));
        ++index;
    }

    FrameUniform frame{};
    copy_vec4(frame.light_diffuse, lighting.sun_diffuse);
    copy_vec4(frame.light_ambient, lighting.sun_ambient);
    copy_vec4(frame.light_direction, glm::vec4{lighting.sun_direction, 0.0f});
    copy_vec4(frame.fog_colour, lighting.fog_colour);
    frame.fog_distance[0] = lighting.fog_start;
    frame.fog_distance[1] = lighting.fog_end;

    const auto queue = wgpuDeviceGetQueue(impl.device);
    wgpuQueueWriteBuffer(queue, impl.draw_buffer, 0, uniforms.data(), uniforms.size());
    wgpuQueueWriteBuffer(queue, impl.frame_buffer, 0, &frame, sizeof(frame));
    if (queue) wgpuQueueRelease(queue);

    wgpuRenderPassEncoderSetPipeline(pass, impl.pipeline);
    index = 0;
    for (auto& [_, tile] : impl.tiles) {
        const auto offset = static_cast<std::uint32_t>(index++ * draw_uniform_stride);
        wgpuRenderPassEncoderSetBindGroup(pass, 1, impl.frame_binding, 1, &offset);
        wgpuRenderPassEncoderSetVertexBuffer(pass, 0, tile.vertices, 0, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderSetIndexBuffer(pass, tile.indices, WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
        for (const auto& patch : tile.patches) {
            const auto material = impl.material(patch);
            if (!material) continue;
            wgpuRenderPassEncoderSetBindGroup(pass, 0, material, 0, nullptr);
            wgpuRenderPassEncoderDrawIndexed(pass, patch.index_count, 1, patch.first_index, 0, 0);
        }
    }
}

std::size_t TerrainRenderer::loaded_tiles() const noexcept {
    return impl_ ? impl_->tiles.size() : 0;
}

std::size_t TerrainRenderer::pending_jobs() const noexcept {
    return impl_ ? impl_->tile_jobs.size() : 0;
}

} // namespace openrts::renderer
#endif
