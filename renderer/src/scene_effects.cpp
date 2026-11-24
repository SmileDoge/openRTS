#include <openrts/renderer/scene_effects.hpp>
#include <openrts/renderer/gpu_pass_timer.hpp>

#include <openrts/core/profile.hpp>

#ifdef OPENRTS_HAS_WGPU
#include <algorithm>
#include <chrono>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <glm/common.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

#include <openrts/renderer/depth.hpp>

#include <webgpu/wgpu.h> // wgpuDevicePoll (the cache's check)

#include "rts_light_shapes.hpp"
#include "shader_code.hpp"

namespace openrts::renderer {
namespace {

constexpr std::uint32_t cluster_x = 16;
constexpr std::uint32_t cluster_y = 9;
constexpr std::uint32_t cluster_z = 24;
constexpr std::uint32_t cluster_capacity = 64; // lights a cluster holds at most
constexpr std::uint32_t cluster_count = cluster_x * cluster_y * cluster_z;
constexpr WGPUTextureFormat shadow_format = WGPUTextureFormat_Depth32Float;
constexpr WGPUTextureFormat ao_format = WGPUTextureFormat_R32Float;

// Laid out as `Effects` in shaders/common/effects_struct.wgsl.
struct EffectsUniform {
    glm::mat4 cascades[SceneEffects::max_cascades];
    glm::vec4 splits{0.0f};
    glm::vec4 shadow{0.0f};
    glm::vec4 eye{0.0f};
    glm::vec4 forward{0.0f};
    std::uint32_t clusters[4]{};
    glm::vec4 cluster_params{0.0f};
    glm::vec4 screen{0.0f};
    std::uint32_t flags[4]{};
    glm::vec4 sun{0.0f};
    glm::vec4 aerial{0.0f};
    glm::vec4 haze{0.0f};
    glm::vec4 sun_colour{0.0f};
    glm::vec4 texel{0.0f};
    glm::mat4 view{1.0f};
    glm::mat4 inv_projection{1.0f};
    glm::vec4 ao{0.0f};
    glm::vec4 horizon{0.0f};
    glm::mat4 focus{1.0f};
    glm::vec4 focus_params{0.0f};
    glm::vec4 lamps{0.0f};
    glm::mat4 lamp_shadows[SceneEffects::max_lamp_shadows]{};
    std::uint32_t lamp_shadow_ids[SceneEffects::max_lamp_shadows]{}; // the lamp's index in the light list + 1
    float lamp_shadow_texel[SceneEffects::max_lamp_shadows]{};        // a texel's size a metre from the lamp (or in metres, parallel)
    glm::vec4 lamp_shadow_params{0.0f}; // x: lamps, y: map size, z: filter radius (texels)
    // Each one's to_light translation: finds the lamp among the RTS light
    // rings when the clustered list is off.
    glm::vec4 lamp_shadow_keys[SceneEffects::max_lamp_shadows]{};
    // The sphere the lamps' shadows cover in flight (xyz its middle, w its
    // radius; 0: none): they fade out towards its edge.
    glm::vec4 lamp_focus{0.0f};
    // openRTS's sky (FogRing): xy the sun's way along the ground (0: off);
    // the horizon at each fog_ring_angle, linear.
    glm::vec4 fog_sun{0.0f};
    glm::vec4 fog_ring[FogRing::count]{};
    // openRTS's clouds' shadow (CloudShadow): where its map lies, the sun.
    glm::vec4 cloud_area{0.0f};
    glm::vec4 cloud_sun{0.0f};
    glm::vec4 cloud_edge{0.0f, 1.0f, 1.0f, 0.0f}; // xy: the edge band (smoothstep), z: CloudShadow::low_sun
    // The sky's light (SkyLightView): x 1 on, y the prefiltered cube's last
    // mip, z the sky's openness round the eye.
    glm::vec4 sky_light{0.0f};
    // Its tuning (SceneEffectSettings): x the diffuse part, y the
    // reflections, z the reflections indoors.
    glm::vec4 sky_tune{1.0f, 1.0f, 0.35f, 0.0f};
    // x: the material debug view (SceneEffectSettings::material_view); y:
    // the height maps' relief out to this many metres (0: off), z: its steps at most.
    glm::vec4 views{0.0f};
};
static_assert(sizeof(EffectsUniform) % 16 == 0);

// How strongly a lamp lights `world` (RTS lights.glsl, the shape, cone and
// attenuation; no surface): 0 outside it.
[[nodiscard]] float lamp_strength_at(const ModelExtraLight& light, const glm::vec3& world) {
    const glm::vec4 p = light.to_light * glm::vec4{world, 1.0f};
    const int kind = static_cast<int>(light.kind) & 31;
    const float reach = light.size.z * (light.attenuation.y + light.attenuation.z);
    float d = 0.0f;
    if (kind == 1) {
        if (!(p.z > 0.0f && p.z <= reach)) return 0.0f;
        d = (std::sqrt(p.x * p.x + p.y * p.y) - light.size.w) / (p.z * light.size.y);
    } else if (kind == 4) {
        if (!(p.z > 0.0f && p.z <= reach)) return 0.0f;
        const float x = p.x / light.size.x, y = p.y / light.size.y;
        d = std::sqrt(x * x + y * y) / p.z;
    } else if (kind == 2) {
        if (std::abs(p.z) > reach) return 0.0f;
        const float x = p.x * light.size.z / light.size.x, y = p.y * light.size.z / light.size.y;
        d = std::sqrt(x * x + y * y + p.z * p.z) / light.size.z;
    } else if (kind == 3) {
        if (!(p.z > 0.0f && p.z <= reach)) return 0.0f;
        d = (std::max)(std::abs(p.x) / light.size.x, std::abs(p.y) / light.size.y);
    } else {
        return 0.0f;
    }
    if (!std::isfinite(d)) return 0.0f;
    const float spot = light.spot_exponent / 128.0f;
    if (spot < 1.0f) {
        if (spot != 0.0f) {
            d = d < 1.0f - spot ? 0.0f : (d - 1.0f + spot) / std::abs(spot);
        } else if (d < 1.0f) {
            d = 0.0f;
        }
    }
    if (d > 1.0f) return 0.0f;
    float factor = std::clamp(1.0f - d, 0.0f, 1.0f);
    const int attenuation = static_cast<int>(light.attenuation.x);
    if (p.z > light.size.z * light.attenuation.y && light.size.z > 0.0f) {
        const float depth = p.z / light.size.z - light.attenuation.y;
        if (attenuation == 2 && light.attenuation.z > 0.0f) {
            factor *= 1.0f - depth / light.attenuation.z;
        } else if (attenuation == 1 && light.attenuation.z > 0.0f) {
            factor *= 1.0f - depth * depth / (light.attenuation.z * light.attenuation.z);
        }
    }
    return std::isfinite(factor) ? (std::max)(factor, 0.0f) : 0.0f;
}

// The least radius of the shadow filter, in metres: about the sun's
// penumbra a metre behind a window frame (the sun is half a degree wide),
// so the focus map's fine texels do not draw a razor edge.
constexpr float min_penumbra = 0.004f;

[[nodiscard]] WGPUStringView name(const char* text) {
    return {.data = text, .length = WGPU_STRLEN};
}

[[nodiscard]] WGPUBuffer make_buffer(const WGPUDevice device, const WGPUBufferUsage usage, const std::uint64_t size) {
    const WGPUBufferDescriptor descriptor{.usage = usage | WGPUBufferUsage_CopyDst, .size = size};
    return wgpuDeviceCreateBuffer(device, &descriptor);
}

[[nodiscard]] WGPUShaderModule make_shader(const WGPUDevice device, const std::string& code) {
    const WGPUShaderSourceWGSL source{.chain = {.sType = WGPUSType_ShaderSourceWGSL}, .code = {.data = code.data(), .length = code.size()}};
    const WGPUShaderModuleDescriptor descriptor{.nextInChain = &source.chain};
    return wgpuDeviceCreateShaderModule(device, &descriptor);
}

[[nodiscard]] WGPUComputePipeline make_compute(const WGPUDevice device, const WGPUBindGroupLayout layout, const std::string& code,
                                               const char* entry, const std::vector<WGPUConstantEntry>& constants = {}) {
    const auto shader = make_shader(device, code);
    const WGPUPipelineLayoutDescriptor layout_descriptor{.bindGroupLayoutCount = 1, .bindGroupLayouts = &layout};
    const auto pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &layout_descriptor);
    const WGPUComputePipelineDescriptor descriptor{
        .layout = pipeline_layout,
        .compute = {.module = shader, .entryPoint = name(entry), .constantCount = constants.size(), .constants = constants.data()}};
    const auto pipeline = wgpuDeviceCreateComputePipeline(device, &descriptor);
    wgpuPipelineLayoutRelease(pipeline_layout);
    wgpuShaderModuleRelease(shader);
    return pipeline;
}

[[nodiscard]] WGPURenderPipeline make_full_pipeline(const WGPUDevice device, const WGPUPipelineLayout layout, const std::string& code,
                                                    const char* fragment_entry) {
    const auto shader = make_shader(device, code);
    const WGPUDepthStencilState depth{
        .format = WGPUTextureFormat_Depth32Float,
        .depthWriteEnabled = WGPUOptionalBool_True,
        .depthCompare = WGPUCompareFunction_Always,
        .stencilFront = {.compare = WGPUCompareFunction_Always},
        .stencilBack = {.compare = WGPUCompareFunction_Always},
    };
    const WGPUFragmentState fragment{.module = shader, .entryPoint = name(fragment_entry), .targetCount = 0, .targets = nullptr};
    const WGPURenderPipelineDescriptor descriptor{
        .layout = layout,
        .vertex = {.module = shader, .entryPoint = name("vs_full")},
        .primitive = {.topology = WGPUPrimitiveTopology_TriangleList},
        .depthStencil = &depth,
        .multisample = {.count = 1, .mask = ~0u},
        .fragment = &fragment,
    };
    const auto pipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor);
    wgpuShaderModuleRelease(shader);
    return pipeline;
}

struct Texture {
    WGPUTexture texture = nullptr;
    WGPUTextureView view = nullptr;
    std::uint32_t width = 0, height = 0, layers = 0;

    void release() {
        if (view) wgpuTextureViewRelease(view);
        if (texture) {
            wgpuTextureDestroy(texture);
            wgpuTextureRelease(texture);
        }
        *this = {};
    }
};

[[nodiscard]] Texture make_texture(const WGPUDevice device, const WGPUTextureFormat format, const WGPUTextureUsage usage,
                                   const std::uint32_t width, const std::uint32_t height, const std::uint32_t layers,
                                   const WGPUTextureViewDimension dimension) {
    Texture result{.width = width, .height = height, .layers = layers};
    const WGPUTextureDescriptor descriptor{
        .usage = usage,
        .dimension = WGPUTextureDimension_2D,
        .size = {.width = width, .height = height, .depthOrArrayLayers = layers},
        .format = format,
        .mipLevelCount = 1,
        .sampleCount = 1,
    };
    result.texture = wgpuDeviceCreateTexture(device, &descriptor);
    const WGPUTextureViewDescriptor view{
        .format = format,
        .dimension = dimension,
        .baseMipLevel = 0,
        .mipLevelCount = 1,
        .baseArrayLayer = 0,
        .arrayLayerCount = layers,
        .aspect = WGPUTextureAspect_All,
    };
    if (result.texture) result.view = wgpuTextureCreateView(result.texture, &view);
    return result;
}

[[nodiscard]] glm::vec3 linear(const glm::vec4& display) {
    return glm::pow(glm::max(glm::vec3(display), glm::vec3(0.0f)), glm::vec3(2.2f));
}

// A light's own reach as a sphere around the lamp (lights.glsl's shapes:
// reach Size.z x (Attenuation.y + .z) along its axis, spreading by Size.x,
// Size.y), for clustering.
[[nodiscard]] std::pair<glm::vec3, float> light_sphere(const ModelExtraLight& light) {
    const glm::vec3 position{glm::inverse(light.to_light)[3]};
    const float reach = (std::max)(light.size.z * (light.attenuation.y + light.attenuation.z), 0.0f);
    const int kind = static_cast<int>(light.kind) & 31;
    float radius = reach;
    if (kind == 4 || kind == 1) { // cone / spotlight: spread per metre along the axis
        const float spread = (std::max)(std::abs(light.size.x), std::abs(light.size.y));
        radius = reach * std::sqrt(1.0f + spread * spread) + light.size.w;
    } else if (kind == 2) { // sphere: half sizes Size.x, Size.y, reach along z
        radius = (std::max)({reach, std::abs(light.size.x), std::abs(light.size.y)});
    } else if (kind == 3) { // box
        radius = std::sqrt(reach * reach + light.size.x * light.size.x + light.size.y * light.size.y);
    }
    return {position, radius};
}

} // namespace

// A cone light's cone for the clusters (kinds 1 and 4, one lamp): its apex,
// axis (unit), reach from the apex and the tangent of its half angle, as
// physical_light and light_factor bound it (d <= 1, 0 < z <= reach). A
// spotlight's source radius (Size.w) moves the apex back to where its sides
// meet. None for a light array (its lamps along a line) or a cone wider than
// some 84 degrees each way.
std::optional<ExtraLightCone> extra_light_cone(const ModelExtraLight& light) {
    const int kind = static_cast<int>(light.kind) & 31;
    if ((kind != 1 && kind != 4) || light.array_count >= 2) return std::nullopt;
    const float reach = (std::max)(light.size.z * (light.attenuation.y + light.attenuation.z), 0.0f);
    const float tangent = kind == 1 ? std::abs(light.size.y) : (std::max)(std::abs(light.size.x), std::abs(light.size.y));
    if (reach <= 0.0f || tangent > 10.0f) return std::nullopt;
    const glm::mat4 to_world = glm::inverse(light.to_light);
    const glm::vec3 along{to_world * glm::vec4{0.0f, 0.0f, 1.0f, 0.0f}};
    if (glm::length(along) < 1e-6f) return std::nullopt;
    const glm::vec3 axis = glm::normalize(along);
    glm::vec3 apex{to_world[3]};
    float back = 0.0f;
    if ((kind == 1 || kind == 4) && light.size.w > 0.0f) {
        if (tangent < 1e-4f) return std::nullopt; // a cylinder
        back = light.size.w / tangent;
    }
    apex -= axis * back;
    return ExtraLightCone{apex, axis, reach + back, tangent};
}


WGPUBindGroupLayout create_scene_effects_layout(const WGPUDevice device) {
    std::array<WGPUBindGroupLayoutEntry, 14> entries{};
    entries[0].binding = 0;
    entries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    entries[0].buffer.type = WGPUBufferBindingType_Uniform;
    entries[0].buffer.minBindingSize = sizeof(EffectsUniform);
    entries[1].binding = 1;
    entries[1].visibility = WGPUShaderStage_Fragment;
    entries[1].texture.sampleType = WGPUTextureSampleType_Depth;
    entries[1].texture.viewDimension = WGPUTextureViewDimension_2DArray;
    entries[2].binding = 2;
    entries[2].visibility = WGPUShaderStage_Fragment;
    entries[2].sampler.type = WGPUSamplerBindingType_Comparison;
    entries[3].binding = 3;
    entries[3].visibility = WGPUShaderStage_Fragment;
    entries[3].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
    entries[3].texture.viewDimension = WGPUTextureViewDimension_2D;
    for (std::uint32_t i = 4; i < 7; ++i) {
        entries[i].binding = i;
        entries[i].visibility = WGPUShaderStage_Fragment;
        entries[i].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
    }
    entries[4].buffer.minBindingSize = sizeof(detail::GpuLight);
    entries[5].buffer.minBindingSize = sizeof(std::uint32_t);
    entries[6].buffer.minBindingSize = sizeof(std::uint32_t);
    entries[7].binding = 7;
    entries[7].visibility = WGPUShaderStage_Fragment;
    entries[7].texture.sampleType = WGPUTextureSampleType_Depth;
    entries[7].texture.viewDimension = WGPUTextureViewDimension_2DArray;
    entries[8].binding = 8;
    entries[8].visibility = WGPUShaderStage_Fragment;
    entries[8].texture.sampleType = WGPUTextureSampleType_Float;
    entries[8].texture.viewDimension = WGPUTextureViewDimension_2D;
    entries[9].binding = 9;
    entries[9].visibility = WGPUShaderStage_Fragment;
    entries[9].sampler.type = WGPUSamplerBindingType_Filtering;
    // The sky's light (SkyLight): the prefiltered cube, the split sum's
    // table, their sampler, the SH9.
    entries[10].binding = 10;
    entries[10].visibility = WGPUShaderStage_Fragment;
    entries[10].texture.sampleType = WGPUTextureSampleType_Float;
    entries[10].texture.viewDimension = WGPUTextureViewDimension_Cube;
    entries[11].binding = 11;
    entries[11].visibility = WGPUShaderStage_Fragment;
    entries[11].texture.sampleType = WGPUTextureSampleType_Float;
    entries[11].texture.viewDimension = WGPUTextureViewDimension_2D;
    entries[12].binding = 12;
    entries[12].visibility = WGPUShaderStage_Fragment;
    entries[12].sampler.type = WGPUSamplerBindingType_Filtering;
    entries[13].binding = 13;
    entries[13].visibility = WGPUShaderStage_Fragment;
    entries[13].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
    entries[13].buffer.minBindingSize = sky_light_buffer_size;
    const WGPUBindGroupLayoutDescriptor descriptor{.entryCount = entries.size(), .entries = entries.data()};
    return wgpuDeviceCreateBindGroupLayout(device, &descriptor);
}

struct SceneEffects::Impl {
    WGPUDevice device = nullptr;
    SceneEffectSettings settings;

    WGPUBindGroupLayout main_layout = nullptr;
    WGPUBindGroupLayout gtao_layout = nullptr;
    WGPUBindGroupLayout blur_layout = nullptr;
    WGPUBindGroupLayout cluster_layout = nullptr;
    WGPUComputePipeline gtao = nullptr;
    WGPUComputePipeline blur = nullptr;
    // The view depth's levels for the occlusion (prefilter_wgsl).
    WGPUBindGroupLayout prefilter_layout = nullptr;
    WGPUComputePipeline prefilter = nullptr;
    // The low quality's (SceneEffectSettings::occlusion_low); without them high.
    WGPUComputePipeline gtao_low = nullptr;
    WGPUComputePipeline blur_low = nullptr;
    WGPUComputePipeline cluster = nullptr;

    WGPUBuffer uniform = nullptr;
    WGPUBuffer lights = nullptr;
    WGPUBuffer spheres = nullptr;
    WGPUBuffer counts = nullptr;
    WGPUBuffer indices = nullptr;
    WGPUSampler compare_sampler = nullptr;
    // openRTS's clouds' shadow: NativeSky's map (not ours), else a texel of 1.
    Texture cloud_stand_in;
    WGPUSampler cloud_sampler = nullptr;
    WGPUTextureView cloud_view = nullptr;
    // The sky's light: SkyLight's (not ours), else stand-ins of nothing.
    Texture sky_cube_stand_in;  // 1 x 1, six layers
    Texture sky_brdf_stand_in;  // 1 x 1
    WGPUBuffer sky_buffer_stand_in = nullptr;
    WGPUSampler sky_sampler = nullptr;
    SkyLightView sky;

    Texture shadow;          // the cascades, then the focus map (or a 1 x 1 stand-in)
    std::array<WGPUTextureView, max_cascades + 1> shadow_layers{};
    Texture lamp_shadow;     // a layer a lamp throwing shadows (or a 1 x 1 stand-in)
    std::array<WGPUTextureView, max_lamp_shadows> lamp_layers{};
    Texture depth;           // the camera's pre-pass
    // Its view depth (prefilter_wgsl): the frame's size and three smaller
    // levels; `view` all of them, `view_levels` each.
    static constexpr std::uint32_t view_depth_levels = 4;
    Texture view_depth;
    std::array<WGPUTextureView, view_depth_levels> view_levels{};
    std::uint32_t view_level_count = 0;
    Texture ao_raw;          // the frame's size, or half of it (ao_half)
    Texture ao;              // blurred, read by the main pass (or a 1 x 1 stand-in)
    bool ao_is_real = false;
    bool ao_half = false;    // ao_raw at half the size: the low quality's pipelines

    WGPUBindGroup main_group = nullptr;
    WGPUBindGroup gtao_group = nullptr;
    WGPUBindGroup blur_group = nullptr;
    WGPUBindGroup cluster_group = nullptr;
    WGPUBindGroup prefilter_group = nullptr;
    bool groups_stale = true;

    EffectsUniform frame{};
    SceneEffectView view{};
    bool active = false;
    std::uint32_t cascade_count = 0;
    std::uint32_t light_count = 0;
    std::array<glm::mat4, max_cascades + 1> cascade_matrices{};
    // The focus map: the layer after the cascades, drawn every frame.
    static constexpr std::uint32_t focus_slot = max_cascades + 1;
    bool focus_active = false;
    std::uint32_t focus_layer = 0;
    float daylight = 0.0f;
    // The lamps throwing shadows: their maps in depth slots lamp_slot on.
    static constexpr std::uint32_t lamp_slot = max_cascades + 2;
    std::vector<ModelExtraLight> packed_lights; // as the light list holds them
    bool lamp_shadows_wanted = false;
    bool camera_depth_wanted = false; // the pre-pass: the occlusion's, the bloom's or the light shafts'
    std::uint32_t lamp_shadow_count = 0;
    std::uint32_t lamps_lighting = 0;
    float lamp_radius = 0.0f;
    std::array<glm::mat4, max_lamp_shadows> lamp_matrices{};
    std::array<std::uint32_t, max_lamp_shadows> lamp_owners{}; // each shadow's lamp's owner
    std::vector<glm::vec3> lamp_shadow_kept; // last frame's lamps, by position
    // What each lamp's map holds (2026-11-17): the lamp, the sphere and the
    // matrix it was drawn with. A map is drawn every other frame (the slots
    // alternating), at once for a lamp new in its slot, a lamp or sphere
    // moved at all, or a new array; between, the shaders take the matrix it
    // was drawn with: half the lamps' passes while they stand (8 on Ultra:
    // about 1 ms of the GPU and 2.6 ms of the workers a frame at a night
    // station). Until 2026-11-20 a lamp or sphere moved under a metre (a
    // twentieth of the sphere) kept it too: in a moving cab (its lamps, its
    // sphere) every other frame showed the shadows a frame behind, the
    // cab's moved by the frame's travel (0.8 m at 60 km/h and 20 fps), and
    // they flickered, the more the lower the fps (testers).
    struct LampKept {
        glm::vec3 position{};
        glm::vec3 centre{};
        glm::mat4 matrix{1.0f};
        float texel = 0.0f;
        std::uint32_t owner = 0;
        int kind = 0;
        std::uint64_t generation = 0;
        bool valid = false;
    };
    std::array<LampKept, max_lamp_shadows> lamp_kept{};
    // How much of each slot's shadow shows (2026-11-21): a lamp that takes a
    // slot fades in, one that loses it fades out before the slot goes to
    // another (lamp_fade_time); until then a shadow came and went at once as
    // the picks changed with the view (testers: a train's shadow gone as the
    // camera moved a hair).
    std::array<float, max_lamp_shadows> lamp_weight{};
    std::optional<std::chrono::steady_clock::time_point> lamp_clock;
    static constexpr float lamp_fade_time = 0.3f; // s
    std::array<bool, max_lamp_shadows> lamp_redraw{};
    std::uint64_t lamp_generation = 1; // grows when the lamps' array is made again
    std::uint64_t lamp_frame = 0;
    void fit_focus(const std::optional<ShadowFocus>& focus);
    void choose_lamp_shadows(const std::optional<ShadowFocus>& focus, bool fade);
    [[nodiscard]] std::uint32_t lamp_map_size() const noexcept { return (std::max)(1024u, map_size() / 2); }
    void record_depth(WGPUCommandEncoder encoder, WGPUTextureView target, const glm::mat4& matrix, std::uint32_t slot,
                      const DepthDraw& draw, GpuPass timed) const;
    // What each cascade's map holds: drawn for this sphere and sun, this
    // frame; redraw[] says which are drawn this frame.
    struct CascadeCache {
        glm::vec3 centre{};
        float radius = 0.0f;
        glm::vec3 to_sun{};
        float texel = 0.0f;
        std::uint64_t generation = 0;
        bool valid = false;
    };
    std::array<CascadeCache, max_cascades> cached{};
    std::array<bool, max_cascades> redraw{};
    std::uint64_t frame_index = 0;
    std::uint64_t shadow_generation = 1; // grows when the shadow array is made again

    // The sun's shadow cache (2026-11-17, SceneEffectSettings::cache_far_cascades):
    // what does not move (the terrain, the world's objects) kept in a layer
    // of its own a cascade, toroidally: a global texel (of the light's plane,
    // in the cascade's texels) at its place modulo the size. As the window
    // moves only the strips it enters are drawn (viewport and scissor, each
    // piece of a strip in a depth slot of its own); each frame the working
    // map is the layer read at the window's offset, then the moving casters
    // (ModelRenderer::set_moving) over it. Drawn whole again when the sun
    // turns, the depth's anchor no longer holds the slice, the window jumps a
    // map or more, the map is new, and every 30 frames or so (objects that
    // came in meanwhile, signal arms; place_still). Until then the cascades were drawn
    // whole every frame (the far two every other frame): the trees' alpha
    // test over and over (Ultra: 1.9 ms of the GPU in a 2ES6 cab by day).
    static constexpr std::uint32_t still_slot = lamp_slot + max_lamp_shadows;
    static constexpr std::uint32_t still_pieces = 8; // a cascade's pieces a frame at most
    static constexpr std::uint32_t check_slot = still_slot + max_cascades * still_pieces;
    static_assert(check_slot + max_cascades == ModelRenderer::depth_slots, "the renderers' depth slots");
    struct StillCache {
        bool valid = false;
        std::uint64_t generation = 0;
        glm::dvec3 s{}, u{}, f{};                   // the light's basis it is drawn in
        float radius = 0.0f;
        std::int64_t col = 0, row = 0;             // the window's corner, global texels
        std::int64_t phase_col = 0, phase_row = 0; // the global texel at the layer's (0, 0)
        double z0 = 0.0, zr = 1.0;                 // its depth: (dot(f, p) - z0) / zr
        std::uint64_t drawn = 0;                   // the frame it was last drawn whole
    };
    struct StillPiece {
        glm::mat4 matrix{1.0f};
        std::uint32_t x = 0, y = 0, width = 0, height = 0; // in the layer
    };
    std::array<StillCache, max_cascades> still{};
    std::array<std::vector<StillPiece>, max_cascades> still_now{}; // this frame's, drawn into the cache
    std::uint64_t still_generation = 1;
    bool still_on = false;   // this frame's working maps come from the cache
    std::uint32_t still_full = 0;
    std::uint64_t still_texels = 0;
    std::array<std::uint32_t, max_cascades * 4> unwrap_at{};
    Texture shadow_still;
    std::array<WGPUTextureView, max_cascades> still_layers{};
    WGPUBindGroupLayout unwrap_layout = nullptr;
    WGPURenderPipeline unwrap = nullptr;
    WGPURenderPipeline fill_far = nullptr;
    WGPUBuffer unwrap_params = nullptr;
    WGPUBindGroup unwrap_group = nullptr;
    // Maps kept while nothing in them changed (set_moving_signature): the
    // cascades' working maps, then the cab's map.
    struct MapKept {
        bool valid = false;
        std::uint64_t shadow_generation = 0, still_generation = 0, signature = 0, drawn = 0;
        glm::mat4 matrix{1.0f};
    };
    std::array<MapKept, max_cascades + 1> map_kept{};
    std::array<bool, max_cascades + 1> map_skip{};
    // --shadow-cache-check: the cascades drawn whole, compared.
    bool check_on = false;
    Texture whole;
    std::array<WGPUTextureView, max_cascades> whole_layers{};
    WGPUBindGroupLayout check_layout = nullptr;
    WGPUComputePipeline check = nullptr;
    WGPUBuffer check_counts = nullptr;
    std::uint64_t check_frames = 0;

    void release_still() {
        for (auto& layer : still_layers) {
            if (layer) wgpuTextureViewRelease(layer);
            layer = nullptr;
        }
        shadow_still.release();
        if (unwrap_group) wgpuBindGroupRelease(unwrap_group);
        unwrap_group = nullptr;
        ++still_generation;
    }
    void release_whole() {
        for (auto& layer : whole_layers) {
            if (layer) wgpuTextureViewRelease(layer);
            layer = nullptr;
        }
        whole.release();
    }
    void make_layers(const Texture& texture, std::array<WGPUTextureView, max_cascades>& layers) const {
        for (std::uint32_t i = 0; i < texture.layers && i < max_cascades && texture.texture; ++i) {
            const WGPUTextureViewDescriptor layer{
                .format = shadow_format,
                .dimension = WGPUTextureViewDimension_2D,
                .baseMipLevel = 0,
                .mipLevelCount = 1,
                .baseArrayLayer = i,
                .arrayLayerCount = 1,
                .aspect = WGPUTextureAspect_DepthOnly,
            };
            layers[i] = wgpuTextureCreateView(texture.texture, &layer);
        }
    }
    // The cache's layers, one a cascade at their size while it is wanted
    // (64 MB a cascade at 4096, 16 at 2048), else none; the check's likewise.
    void ensure_still(const bool wanted) {
        const std::uint32_t size = map_size();
        const std::uint32_t layers = cascades_wanted();
        if (!wanted || !unwrap || !fill_far || !unwrap_params) {
            if (shadow_still.texture) release_still();
            if (whole.texture) release_whole();
            return;
        }
        if (!shadow_still.texture || shadow_still.width != size || shadow_still.layers != layers) {
            release_still();
            shadow_still = make_texture(device, shadow_format, WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding, size, size,
                                        layers, WGPUTextureViewDimension_2DArray);
            make_layers(shadow_still, still_layers);
            if (shadow_still.view) {
                std::array<WGPUBindGroupEntry, 2> entries{};
                entries[0] = {.binding = 0, .buffer = unwrap_params, .size = sizeof(unwrap_at)};
                entries[1] = {.binding = 1, .textureView = shadow_still.view};
                const WGPUBindGroupDescriptor descriptor{.layout = unwrap_layout, .entryCount = entries.size(), .entries = entries.data()};
                unwrap_group = wgpuDeviceCreateBindGroup(device, &descriptor);
            }
        }
        if (check_on && check && (!whole.texture || whole.width != size || whole.layers != layers)) {
            release_whole();
            whole = make_texture(device, shadow_format, WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding, size, size,
                                 layers, WGPUTextureViewDimension_2DArray);
            make_layers(whole, whole_layers);
        } else if (!check_on && whole.texture) {
            release_whole();
        }
    }
    void place_still(std::uint32_t i, const glm::vec3& centre, float radius, float split, const glm::vec3& to_sun, std::uint32_t size);
    void record_still(WGPUCommandEncoder encoder, std::uint32_t i, const DepthDraw& draw) const;
    void record_working(WGPUCommandEncoder encoder, std::uint32_t i, const DepthDraw& draw) const;
    // record_parallel(): a command buffer a task, in submission order.
    std::vector<WGPUCommandBuffer> parallel_buffers;
    std::vector<float> parallel_ms; // by parallel_buffers' index: the pass's recording time

    void record_shadow(WGPUCommandEncoder encoder, std::uint32_t layer, std::uint32_t slot, const DepthDraw& draw) const;
    void record_camera(WGPUCommandEncoder encoder, const DepthDraw& draw) const;

    ~Impl() {
        release_groups();
        for (auto& layer : shadow_layers) {
            if (layer) wgpuTextureViewRelease(layer);
        }
        shadow.release();
        release_still();
        release_whole();
        for (const auto pipeline : {unwrap, fill_far}) {
            if (pipeline) wgpuRenderPipelineRelease(pipeline);
        }
        if (check) wgpuComputePipelineRelease(check);
        for (const auto layout : {unwrap_layout, check_layout}) {
            if (layout) wgpuBindGroupLayoutRelease(layout);
        }
        for (const auto buffer : {unwrap_params, check_counts}) {
            if (buffer) wgpuBufferRelease(buffer);
        }
        for (auto& layer : lamp_layers) {
            if (layer) wgpuTextureViewRelease(layer);
        }
        lamp_shadow.release();
        depth.release();
        release_view_depth();
        ao_raw.release();
        ao.release();
        cloud_stand_in.release();
        if (cloud_sampler) wgpuSamplerRelease(cloud_sampler);
        sky_cube_stand_in.release();
        sky_brdf_stand_in.release();
        if (sky_sampler) wgpuSamplerRelease(sky_sampler);
        for (const auto buffer : {uniform, lights, spheres, counts, indices, sky_buffer_stand_in}) {
            if (buffer) wgpuBufferRelease(buffer);
        }
        if (compare_sampler) wgpuSamplerRelease(compare_sampler);
        for (const auto pipeline : {gtao, blur, gtao_low, blur_low, cluster, prefilter}) {
            if (pipeline) wgpuComputePipelineRelease(pipeline);
        }
        for (const auto layout : {main_layout, gtao_layout, blur_layout, cluster_layout, prefilter_layout}) {
            if (layout) wgpuBindGroupLayoutRelease(layout);
        }
    }

    void release_groups() {
        for (auto* group : {&main_group, &gtao_group, &blur_group, &cluster_group, &prefilter_group}) {
            if (*group) wgpuBindGroupRelease(*group);
            *group = nullptr;
        }
        groups_stale = true;
    }

    void release_view_depth() {
        for (auto& level : view_levels) {
            if (level) wgpuTextureViewRelease(level);
            level = nullptr;
        }
        view_level_count = 0;
        view_depth.release();
    }

    // The view depth at the frame's size with its levels.
    void make_view_depth(const std::uint32_t width, const std::uint32_t height) {
        release_view_depth();
        // Four levels, each at least a texel.
        const std::uint32_t levels = view_depth_levels;
        if (width < 8 || height < 8) return;
        const WGPUTextureDescriptor descriptor{
            .usage = WGPUTextureUsage_StorageBinding | WGPUTextureUsage_TextureBinding,
            .dimension = WGPUTextureDimension_2D,
            .size = {.width = width, .height = height, .depthOrArrayLayers = 1},
            .format = ao_format,
            .mipLevelCount = levels,
            .sampleCount = 1,
        };
        view_depth.texture = wgpuDeviceCreateTexture(device, &descriptor);
        if (!view_depth.texture) return;
        view_depth.width = width;
        view_depth.height = height;
        view_depth.layers = 1;
        const WGPUTextureViewDescriptor all{
            .format = ao_format,
            .dimension = WGPUTextureViewDimension_2D,
            .baseMipLevel = 0,
            .mipLevelCount = levels,
            .baseArrayLayer = 0,
            .arrayLayerCount = 1,
            .aspect = WGPUTextureAspect_All,
        };
        view_depth.view = wgpuTextureCreateView(view_depth.texture, &all);
        for (std::uint32_t level = 0; level < levels; ++level) {
            auto one = all;
            one.baseMipLevel = level;
            one.mipLevelCount = 1;
            view_levels[level] = wgpuTextureCreateView(view_depth.texture, &one);
        }
        view_level_count = levels;
    }

    // The shadow array: a layer a cascade (and one for the focus) at the
    // chosen size while shadows are on, else 1 x 1.
    void ensure_shadow(const bool wanted) {
        const std::uint32_t size = wanted ? map_size() : 1;
        const std::uint32_t layers = wanted ? cascades_wanted() + (settings.cab_shadows ? 1u : 0u) : 1;
        if (shadow.texture && shadow.width == size && shadow.layers == layers) return;
        for (auto& layer : shadow_layers) {
            if (layer) wgpuTextureViewRelease(layer);
            layer = nullptr;
        }
        shadow.release();
        ++shadow_generation;
        shadow = make_texture(device, shadow_format, WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding, size, size,
                              layers, WGPUTextureViewDimension_2DArray);
        if (wanted && shadow.texture) {
            for (std::uint32_t i = 0; i < layers; ++i) {
                const WGPUTextureViewDescriptor layer{
                    .format = shadow_format,
                    .dimension = WGPUTextureViewDimension_2D,
                    .baseMipLevel = 0,
                    .mipLevelCount = 1,
                    .baseArrayLayer = i,
                    .arrayLayerCount = 1,
                    .aspect = WGPUTextureAspect_DepthOnly,
                };
                shadow_layers[i] = wgpuTextureCreateView(shadow.texture, &layer);
            }
        }
        release_groups();
    }

    // The lamps' shadow array: max_lamp_shadows layers while lamps throw
    // shadows, else 1 x 1.
    void ensure_lamp_shadow(const bool wanted) {
        const std::uint32_t size = wanted ? lamp_map_size() : 1;
        const std::uint32_t layers = wanted ? max_lamp_shadows : 1;
        if (lamp_shadow.texture && lamp_shadow.width == size && lamp_shadow.layers == layers) return;
        for (auto& layer : lamp_layers) {
            if (layer) wgpuTextureViewRelease(layer);
            layer = nullptr;
        }
        lamp_shadow.release();
        lamp_shadow = make_texture(device, shadow_format, WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding, size, size,
                                   layers, WGPUTextureViewDimension_2DArray);
        ++lamp_generation;
        if (wanted && lamp_shadow.texture) {
            for (std::uint32_t i = 0; i < layers; ++i) {
                const WGPUTextureViewDescriptor layer{
                    .format = shadow_format,
                    .dimension = WGPUTextureViewDimension_2D,
                    .baseMipLevel = 0,
                    .mipLevelCount = 1,
                    .baseArrayLayer = i,
                    .arrayLayerCount = 1,
                    .aspect = WGPUTextureAspect_DepthOnly,
                };
                lamp_layers[i] = wgpuTextureCreateView(lamp_shadow.texture, &layer);
            }
        }
        release_groups();
    }

    // The pre-pass depth and the occlusion at the frame's size while AO is
    // on; else a 1 x 1 occlusion of 1.
    void ensure_ao(const bool wanted, const std::uint32_t width, const std::uint32_t height, bool half = false) {
        half = half && gtao_low && blur_low;
        if (!wanted) {
            if (ao.texture && !ao_is_real) return;
            depth.release();
            release_view_depth();
            ao_raw.release();
            ao.release();
            ao = make_texture(device, ao_format, WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst, 1, 1, 1,
                              WGPUTextureViewDimension_2D);
            const float one = 1.0f;
            if (ao.texture) {
                const auto queue = wgpuDeviceGetQueue(device);
                const WGPUTexelCopyTextureInfo target{.texture = ao.texture};
                const WGPUTexelCopyBufferLayout layout{.bytesPerRow = 4, .rowsPerImage = 1};
                const WGPUExtent3D extent{.width = 1, .height = 1, .depthOrArrayLayers = 1};
                wgpuQueueWriteTexture(queue, &target, &one, sizeof(one), &layout, &extent);
                wgpuQueueRelease(queue);
            }
            ao_is_real = false;
            release_groups();
            return;
        }
        if (ao_is_real && ao.width == width && ao.height == height && ao_half == half) return;
        depth.release();
        ao_raw.release();
        ao.release();
        depth = make_texture(device, depth_format, WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding, width, height, 1,
                             WGPUTextureViewDimension_2D);
        make_view_depth(width, height);
        const auto usage = WGPUTextureUsage_StorageBinding | WGPUTextureUsage_TextureBinding;
        ao_half = half;
        ao_raw = make_texture(device, ao_format, usage, half ? (width + 1) / 2 : width, half ? (height + 1) / 2 : height, 1,
                              WGPUTextureViewDimension_2D);
        ao = make_texture(device, ao_format, usage, width, height, 1, WGPUTextureViewDimension_2D);
        ao_is_real = true;
        release_groups();
    }

    void ensure_groups() {
        if (!groups_stale) return;
        release_groups();
        {
            std::array<WGPUBindGroupEntry, 14> entries{};
            entries[0] = {.binding = 0, .buffer = uniform, .size = sizeof(EffectsUniform)};
            entries[1] = {.binding = 1, .textureView = shadow.view};
            entries[8] = {.binding = 8, .textureView = cloud_view ? cloud_view : cloud_stand_in.view};
            entries[9] = {.binding = 9, .sampler = cloud_sampler};
            const bool sky_on = sky.on();
            entries[10] = {.binding = 10, .textureView = sky_on ? sky.prefiltered : sky_cube_stand_in.view};
            entries[11] = {.binding = 11, .textureView = sky_on ? sky.brdf : sky_brdf_stand_in.view};
            entries[12] = {.binding = 12, .sampler = sky_sampler};
            entries[13] = {.binding = 13, .buffer = sky_on ? sky.irradiance : sky_buffer_stand_in, .size = sky_light_buffer_size};
            entries[7] = {.binding = 7, .textureView = lamp_shadow.view};
            entries[2] = {.binding = 2, .sampler = compare_sampler};
            entries[3] = {.binding = 3, .textureView = ao.view};
            entries[4] = {.binding = 4, .buffer = lights, .size = sizeof(detail::GpuLight) * max_lights};
            entries[5] = {.binding = 5, .buffer = counts, .size = sizeof(std::uint32_t) * cluster_count};
            entries[6] = {.binding = 6, .buffer = indices, .size = sizeof(std::uint32_t) * cluster_count * cluster_capacity};
            const WGPUBindGroupDescriptor descriptor{.layout = main_layout, .entryCount = entries.size(), .entries = entries.data()};
            main_group = wgpuDeviceCreateBindGroup(device, &descriptor);
        }
        if (ao_is_real && depth.view && view_depth.view && view_level_count == view_depth_levels && ao_raw.view && ao.view) {
            {
                std::array<WGPUBindGroupEntry, 6> entries{};
                entries[0] = {.binding = 0, .buffer = uniform, .size = sizeof(EffectsUniform)};
                entries[1] = {.binding = 1, .textureView = depth.view};
                for (std::uint32_t level = 0; level < view_depth_levels; ++level) {
                    entries[2 + level] = {.binding = 2 + level, .textureView = view_levels[level]};
                }
                const WGPUBindGroupDescriptor descriptor{.layout = prefilter_layout, .entryCount = entries.size(), .entries = entries.data()};
                prefilter_group = wgpuDeviceCreateBindGroup(device, &descriptor);
            }
            std::array<WGPUBindGroupEntry, 3> entries{};
            entries[0] = {.binding = 0, .buffer = uniform, .size = sizeof(EffectsUniform)};
            entries[1] = {.binding = 1, .textureView = view_depth.view};
            entries[2] = {.binding = 2, .textureView = ao_raw.view};
            const WGPUBindGroupDescriptor descriptor{.layout = gtao_layout, .entryCount = entries.size(), .entries = entries.data()};
            gtao_group = wgpuDeviceCreateBindGroup(device, &descriptor);
            std::array<WGPUBindGroupEntry, 4> blur_entries{};
            blur_entries[0] = {.binding = 0, .buffer = uniform, .size = sizeof(EffectsUniform)};
            blur_entries[1] = {.binding = 1, .textureView = view_depth.view};
            blur_entries[2] = {.binding = 2, .textureView = ao_raw.view};
            blur_entries[3] = {.binding = 3, .textureView = ao.view};
            const WGPUBindGroupDescriptor blur_descriptor{.layout = blur_layout, .entryCount = blur_entries.size(), .entries = blur_entries.data()};
            blur_group = wgpuDeviceCreateBindGroup(device, &blur_descriptor);
        }
        {
            std::array<WGPUBindGroupEntry, 4> entries{};
            entries[0] = {.binding = 0, .buffer = uniform, .size = sizeof(EffectsUniform)};
            entries[1] = {.binding = 1, .buffer = spheres, .size = sizeof(glm::vec4) * 3 * max_lights};
            entries[2] = {.binding = 2, .buffer = counts, .size = sizeof(std::uint32_t) * cluster_count};
            entries[3] = {.binding = 3, .buffer = indices, .size = sizeof(std::uint32_t) * cluster_count * cluster_capacity};
            const WGPUBindGroupDescriptor descriptor{.layout = cluster_layout, .entryCount = entries.size(), .entries = entries.data()};
            cluster_group = wgpuDeviceCreateBindGroup(device, &descriptor);
        }
        groups_stale = false;
    }

    [[nodiscard]] std::uint32_t map_size() const noexcept {
        const auto size = settings.shadow_map_size;
        return size <= 1024 ? 1024u : size >= 4096 ? 4096u : 2048u;
    }
    [[nodiscard]] std::uint32_t cascades_wanted() const noexcept {
        return std::clamp(settings.shadow_cascades, 2u, max_cascades);
    }

    // Stable cascades: each slice of the view (practical split, lambda 0.8)
    // in a sphere, seen orthographically from the sun, its origin snapped to
    // whole texels; 300 m more towards the sun for casters out of view.
    void compute_cascades(const glm::vec3& to_sun) {
        const float near = (std::max)(view.near_plane, 0.05f);
        const float far = (std::min)(settings.shadow_distance, view.far_plane);
        const auto forward = glm::normalize(glm::vec3(glm::inverse(view.view) * glm::vec4{0.0f, 0.0f, -1.0f, 0.0f}));
        const auto right = glm::normalize(glm::vec3(glm::inverse(view.view) * glm::vec4{1.0f, 0.0f, 0.0f, 0.0f}));
        const auto up = glm::normalize(glm::vec3(glm::inverse(view.view) * glm::vec4{0.0f, 1.0f, 0.0f, 0.0f}));
        const float tan_y = std::tan(view.vertical_fov * 0.5f);
        const float tan_x = tan_y * view.aspect;
        cascade_count = cascades_wanted();
        const auto size = map_size();
        // The far cascades kept between frames: the last two of 3 or 4, the
        // last of 2; drawn with a tenth more room so a moving camera stays
        // inside them a while.
        const std::uint32_t first_cached = settings.cache_far_cascades ? (cascade_count >= 3 ? cascade_count - 2 : cascade_count - 1) : cascade_count;
        ++frame_index;
        still_on = settings.cache_far_cascades && shadow_still.texture && unwrap_group && unwrap && fill_far;
        map_skip.fill(false);
        still_full = 0;
        still_texels = 0;
        for (auto& pieces : still_now) pieces.clear();
        float previous = near;
        for (std::uint32_t i = 0; i < cascade_count; ++i) {
            const float t = static_cast<float>(i + 1) / static_cast<float>(cascade_count);
            const float split = 0.8f * near * std::pow(far / near, t) + 0.2f * (near + (far - near) * t);
            // The slice's bounding sphere: its corners around the centre on the axis.
            const float mid = (previous + split) * 0.5f;
            const glm::vec3 centre = view.eye + forward * mid;
            float radius = 0.0f;
            for (const float d : {previous, split}) {
                for (const float sx : {-1.0f, 1.0f}) {
                    for (const float sy : {-1.0f, 1.0f}) {
                        const glm::vec3 corner = view.eye + forward * d + right * (sx * d * tan_x) + up * (sy * d * tan_y);
                        radius = (std::max)(radius, glm::distance(corner, centre));
                    }
                }
            }
            if (still_on) {
                place_still(i, centre, std::ceil(radius * 4.0f) / 4.0f, split, to_sun, size);
                previous = split;
                continue;
            }
            const bool cacheable = i >= first_cached;
            if (cacheable) radius *= 1.1f;
            radius = std::ceil(radius * 4.0f) / 4.0f;
            // A kept cascade is drawn again on its turn (every other frame,
            // the two far ones alternating), or at once when its map no
            // longer covers this frame's slice, the sun moved or the map is new.
            auto& kept = cached[i];
            if (cacheable && kept.valid && kept.generation == shadow_generation) {
                const bool covered = glm::distance(kept.centre, centre) + radius / 1.1f <= kept.radius;
                const bool same_sun = glm::dot(kept.to_sun, to_sun) > 0.99995f;
                const bool turn = (frame_index + (i - first_cached)) % 2 == 0;
                if (covered && same_sun && !turn) {
                    redraw[i] = false;
                    frame.cascades[i] = cascade_matrices[i];
                    frame.splits[static_cast<int>(i)] = split;
                    frame.texel[static_cast<int>(i)] = kept.texel;
                    previous = split;
                    continue;
                }
            }
            redraw[i] = true;
            constexpr float behind = 300.0f;
            const glm::vec3 light_up = std::abs(to_sun.y) > 0.99f ? glm::vec3{0.0f, 0.0f, 1.0f} : glm::vec3{0.0f, 1.0f, 0.0f};
            const glm::mat4 light_view = glm::lookAtRH(centre + to_sun * (radius + behind), centre, light_up);
            glm::mat4 projection = glm::orthoRH_ZO(-radius, radius, -radius, radius, 0.0f, 2.0f * radius + behind);
            glm::mat4 matrix = projection * light_view;
            // Snap the world's origin to a whole texel.
            const float half_size = static_cast<float>(size) * 0.5f;
            const glm::vec4 origin = matrix * glm::vec4{0.0f, 0.0f, 0.0f, 1.0f};
            const glm::vec2 texels{origin.x * half_size, origin.y * half_size};
            const glm::vec2 offset = (glm::round(texels) - texels) / half_size;
            projection[3][0] += offset.x;
            projection[3][1] += offset.y;
            matrix = projection * light_view;
            cascade_matrices[i] = matrix;
            frame.cascades[i] = matrix;
            frame.splits[static_cast<int>(i)] = split;
            frame.texel[static_cast<int>(i)] = 2.0f * radius / static_cast<float>(size);
            kept = {.centre = centre, .radius = radius, .to_sun = to_sun, .texel = frame.texel[static_cast<int>(i)],
                    .generation = shadow_generation, .valid = true};
            previous = split;
        }
        if (still_on) {
            const auto queue = wgpuDeviceGetQueue(device);
            wgpuQueueWriteBuffer(queue, unwrap_params, 0, unwrap_at.data(), sizeof(unwrap_at));
            wgpuQueueRelease(queue);
        }
    }
};

SceneEffects::SceneEffects(const WGPUDevice device) : impl_(std::make_unique<Impl>()) {
    auto& impl = *impl_;
    impl.device = device;
    impl.main_layout = create_scene_effects_layout(device);
    {
        std::array<WGPUBindGroupLayoutEntry, 3> entries{};
        entries[0].binding = 0;
        entries[0].visibility = WGPUShaderStage_Compute;
        entries[0].buffer.type = WGPUBufferBindingType_Uniform;
        entries[0].buffer.minBindingSize = sizeof(EffectsUniform);
        entries[1].binding = 1;
        entries[1].visibility = WGPUShaderStage_Compute;
        entries[1].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
        entries[1].texture.viewDimension = WGPUTextureViewDimension_2D;
        entries[2].binding = 2;
        entries[2].visibility = WGPUShaderStage_Compute;
        entries[2].storageTexture = {.access = WGPUStorageTextureAccess_WriteOnly, .format = ao_format, .viewDimension = WGPUTextureViewDimension_2D};
        const WGPUBindGroupLayoutDescriptor descriptor{.entryCount = entries.size(), .entries = entries.data()};
        impl.gtao_layout = wgpuDeviceCreateBindGroupLayout(device, &descriptor);
    }
    {
        // The prefilter: the camera's depth into the view depth's four levels.
        std::array<WGPUBindGroupLayoutEntry, 6> entries{};
        entries[0].binding = 0;
        entries[0].visibility = WGPUShaderStage_Compute;
        entries[0].buffer.type = WGPUBufferBindingType_Uniform;
        entries[0].buffer.minBindingSize = sizeof(EffectsUniform);
        entries[1].binding = 1;
        entries[1].visibility = WGPUShaderStage_Compute;
        entries[1].texture.sampleType = WGPUTextureSampleType_Depth;
        entries[1].texture.viewDimension = WGPUTextureViewDimension_2D;
        for (const std::uint32_t b : {2u, 3u, 4u, 5u}) {
            entries[b].binding = b;
            entries[b].visibility = WGPUShaderStage_Compute;
            entries[b].storageTexture = {.access = WGPUStorageTextureAccess_WriteOnly, .format = ao_format, .viewDimension = WGPUTextureViewDimension_2D};
        }
        const WGPUBindGroupLayoutDescriptor descriptor{.entryCount = entries.size(), .entries = entries.data()};
        impl.prefilter_layout = wgpuDeviceCreateBindGroupLayout(device, &descriptor);
    }
    {
        std::array<WGPUBindGroupLayoutEntry, 4> entries{};
        entries[0].binding = 0;
        entries[0].visibility = WGPUShaderStage_Compute;
        entries[0].buffer.type = WGPUBufferBindingType_Uniform;
        entries[0].buffer.minBindingSize = sizeof(EffectsUniform);
        entries[1].binding = 1;
        entries[1].visibility = WGPUShaderStage_Compute;
        entries[1].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
        entries[1].texture.viewDimension = WGPUTextureViewDimension_2D;
        entries[2].binding = 2;
        entries[2].visibility = WGPUShaderStage_Compute;
        entries[2].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
        entries[2].texture.viewDimension = WGPUTextureViewDimension_2D;
        entries[3].binding = 3;
        entries[3].visibility = WGPUShaderStage_Compute;
        entries[3].storageTexture = {.access = WGPUStorageTextureAccess_WriteOnly, .format = ao_format, .viewDimension = WGPUTextureViewDimension_2D};
        const WGPUBindGroupLayoutDescriptor descriptor{.entryCount = entries.size(), .entries = entries.data()};
        impl.blur_layout = wgpuDeviceCreateBindGroupLayout(device, &descriptor);
    }
    {
        std::array<WGPUBindGroupLayoutEntry, 4> entries{};
        entries[0].binding = 0;
        entries[0].visibility = WGPUShaderStage_Compute;
        entries[0].buffer.type = WGPUBufferBindingType_Uniform;
        entries[0].buffer.minBindingSize = sizeof(EffectsUniform);
        entries[1].binding = 1;
        entries[1].visibility = WGPUShaderStage_Compute;
        entries[1].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
        entries[2].binding = 2;
        entries[2].visibility = WGPUShaderStage_Compute;
        entries[2].buffer.type = WGPUBufferBindingType_Storage;
        entries[3].binding = 3;
        entries[3].visibility = WGPUShaderStage_Compute;
        entries[3].buffer.type = WGPUBufferBindingType_Storage;
        const WGPUBindGroupLayoutDescriptor descriptor{.entryCount = entries.size(), .entries = entries.data()};
        impl.cluster_layout = wgpuDeviceCreateBindGroupLayout(device, &descriptor);
    }
    const std::string header = detail::shader_code("common/effects_struct");
    impl.gtao = make_compute(device, impl.gtao_layout, header + detail::shader_code("effects/gtao"), "gtao");
    impl.blur = make_compute(device, impl.blur_layout, header + detail::shader_code("effects/gtao_blur"), "blur");
    impl.gtao_low = make_compute(device, impl.gtao_layout, header + detail::shader_code("effects/gtao"), "gtao",
                                 {{.key = name("slices"), .value = 1.0},
                                  {.key = name("steps"), .value = 4.0},
                                  {.key = name("scale"), .value = 2.0},
                                  {.key = name("max_radius"), .value = 64.0}});
    impl.blur_low = make_compute(device, impl.blur_layout, header + detail::shader_code("effects/gtao_blur"), "blur",
                                 {{.key = name("scale"), .value = 2.0}, {.key = name("reach"), .value = 1.0}});
    impl.cluster = make_compute(device, impl.cluster_layout, header + detail::shader_code("effects/light_clusters"), "assign");
    impl.prefilter = make_compute(device, impl.prefilter_layout, header + detail::shader_code("effects/prefilter"), "prefilter");

    {
        // The sun's shadow cache: the unwrap (its offsets, the cache's
        // layers) and the far plane's fill (nothing bound).
        std::array<WGPUBindGroupLayoutEntry, 2> entries{};
        entries[0].binding = 0;
        entries[0].visibility = WGPUShaderStage_Fragment;
        entries[0].buffer.type = WGPUBufferBindingType_Uniform;
        entries[0].buffer.minBindingSize = sizeof(impl.unwrap_at);
        entries[1].binding = 1;
        entries[1].visibility = WGPUShaderStage_Fragment;
        entries[1].texture.sampleType = WGPUTextureSampleType_Depth;
        entries[1].texture.viewDimension = WGPUTextureViewDimension_2DArray;
        const WGPUBindGroupLayoutDescriptor descriptor{.entryCount = entries.size(), .entries = entries.data()};
        impl.unwrap_layout = wgpuDeviceCreateBindGroupLayout(device, &descriptor);
        const WGPUPipelineLayoutDescriptor with{.bindGroupLayoutCount = 1, .bindGroupLayouts = &impl.unwrap_layout};
        const auto unwrap_layout = wgpuDeviceCreatePipelineLayout(device, &with);
        impl.unwrap = make_full_pipeline(device, unwrap_layout, detail::shader_code("effects/shadow_cache_still"), "fs_unwrap");
        wgpuPipelineLayoutRelease(unwrap_layout);
        const WGPUPipelineLayoutDescriptor none{.bindGroupLayoutCount = 0, .bindGroupLayouts = nullptr};
        const auto far_layout = wgpuDeviceCreatePipelineLayout(device, &none);
        impl.fill_far = make_full_pipeline(device, far_layout, detail::shader_code("effects/shadow_cache_far"), "fs_far");
        wgpuPipelineLayoutRelease(far_layout);
        impl.unwrap_params = make_buffer(device, WGPUBufferUsage_Uniform, sizeof(impl.unwrap_at));
    }
    {
        // --shadow-cache-check: the working maps against the cascades drawn whole.
        std::array<WGPUBindGroupLayoutEntry, 3> entries{};
        for (std::uint32_t b = 0; b < 2; ++b) {
            entries[b].binding = b;
            entries[b].visibility = WGPUShaderStage_Compute;
            entries[b].texture.sampleType = WGPUTextureSampleType_Depth;
            entries[b].texture.viewDimension = WGPUTextureViewDimension_2DArray;
        }
        entries[2].binding = 2;
        entries[2].visibility = WGPUShaderStage_Compute;
        entries[2].buffer.type = WGPUBufferBindingType_Storage;
        const WGPUBindGroupLayoutDescriptor descriptor{.entryCount = entries.size(), .entries = entries.data()};
        impl.check_layout = wgpuDeviceCreateBindGroupLayout(device, &descriptor);
        impl.check = make_compute(device, impl.check_layout, detail::shader_code("effects/shadow_cache_check"), "compare");
        impl.check_counts = make_buffer(device, WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc, 8 * sizeof(std::uint32_t));
    }

    impl.uniform = make_buffer(device, WGPUBufferUsage_Uniform, sizeof(EffectsUniform));
    impl.lights = make_buffer(device, WGPUBufferUsage_Storage, sizeof(detail::GpuLight) * max_lights);
    impl.spheres = make_buffer(device, WGPUBufferUsage_Storage, sizeof(glm::vec4) * 3 * max_lights);
    impl.counts = make_buffer(device, WGPUBufferUsage_Storage, sizeof(std::uint32_t) * cluster_count);
    impl.indices = make_buffer(device, WGPUBufferUsage_Storage, sizeof(std::uint32_t) * cluster_count * cluster_capacity);
    const WGPUSamplerDescriptor sampler{
        .addressModeU = WGPUAddressMode_ClampToEdge,
        .addressModeV = WGPUAddressMode_ClampToEdge,
        .addressModeW = WGPUAddressMode_ClampToEdge,
        .magFilter = WGPUFilterMode_Linear,
        .minFilter = WGPUFilterMode_Linear,
        .mipmapFilter = WGPUMipmapFilterMode_Nearest,
        .lodMaxClamp = 32.0f,
        .compare = WGPUCompareFunction_LessEqual,
        .maxAnisotropy = 1,
    };
    impl.compare_sampler = wgpuDeviceCreateSampler(device, &sampler);
    {
        const WGPUSamplerDescriptor clouds{
            .addressModeU = WGPUAddressMode_ClampToEdge,
            .addressModeV = WGPUAddressMode_ClampToEdge,
            .addressModeW = WGPUAddressMode_ClampToEdge,
            .magFilter = WGPUFilterMode_Linear,
            .minFilter = WGPUFilterMode_Linear,
            .mipmapFilter = WGPUMipmapFilterMode_Nearest,
            .lodMaxClamp = 1.0f,
            .maxAnisotropy = 1,
        };
        impl.cloud_sampler = wgpuDeviceCreateSampler(device, &clouds);
        impl.cloud_stand_in = make_texture(device, WGPUTextureFormat_R8Unorm, WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst, 1, 1,
                                           1, WGPUTextureViewDimension_2D);
        if (impl.cloud_stand_in.texture) {
            const std::uint8_t lit = 255;
            const auto queue = wgpuDeviceGetQueue(device);
            const WGPUTexelCopyTextureInfo target{.texture = impl.cloud_stand_in.texture};
            const WGPUTexelCopyBufferLayout layout{.bytesPerRow = 1, .rowsPerImage = 1};
            const WGPUExtent3D extent{.width = 1, .height = 1, .depthOrArrayLayers = 1};
            wgpuQueueWriteTexture(queue, &target, &lit, sizeof(lit), &layout, &extent);
            wgpuQueueRelease(queue);
        }
    }
    {
        // The sky's light until SkyLight gives its own: a black cube, a
        // table of nothing, no SH (the shaders then keep the old sky light).
        impl.sky_cube_stand_in = make_texture(device, WGPUTextureFormat_RGBA16Float, WGPUTextureUsage_TextureBinding, 1, 1, 6,
                                              WGPUTextureViewDimension_Cube);
        impl.sky_brdf_stand_in = make_texture(device, WGPUTextureFormat_RGBA16Float, WGPUTextureUsage_TextureBinding, 1, 1, 1,
                                              WGPUTextureViewDimension_2D);
        impl.sky_buffer_stand_in = make_buffer(device, WGPUBufferUsage_Storage, sky_light_buffer_size);
        const WGPUSamplerDescriptor sky{
            .addressModeU = WGPUAddressMode_ClampToEdge,
            .addressModeV = WGPUAddressMode_ClampToEdge,
            .addressModeW = WGPUAddressMode_ClampToEdge,
            .magFilter = WGPUFilterMode_Linear,
            .minFilter = WGPUFilterMode_Linear,
            .mipmapFilter = WGPUMipmapFilterMode_Linear,
            .lodMaxClamp = 32.0f,
            .maxAnisotropy = 1,
        };
        impl.sky_sampler = wgpuDeviceCreateSampler(device, &sky);
    }
    impl.ensure_shadow(false);
    impl.ensure_lamp_shadow(false);
    impl.ensure_ao(false, 1, 1);
    impl.ensure_groups();
}

SceneEffects::~SceneEffects() = default;

bool SceneEffects::ready() const noexcept {
    const auto& impl = *impl_;
    return impl.main_layout && impl.gtao && impl.blur && impl.cluster && impl.uniform && impl.lights && impl.compare_sampler
        && impl.main_group;
}

void SceneEffects::set_settings(const SceneEffectSettings& settings) {
    impl_->settings = settings;
}

const SceneEffectSettings& SceneEffects::settings() const noexcept {
    return impl_->settings;
}

void SceneEffects::begin_frame(const SceneEffectView& view, const ModelLighting& lighting, const bool active, const CloudShadow& clouds,
                               const SkyLightView& sky) {
    OPENRTS_ZONE("effects: begin frame");
    auto& impl = *impl_;
    if (clouds.map != impl.cloud_view) {
        impl.cloud_view = clouds.map;
        impl.release_groups();
    }
    if (sky.prefiltered != impl.sky.prefiltered || sky.brdf != impl.sky.brdf || sky.irradiance != impl.sky.irradiance) impl.release_groups();
    impl.sky = sky;
    impl.view = view;
    impl.active = active;
    impl.frame = {};
    auto& frame = impl.frame;
    const auto& s = impl.settings;

    const glm::vec3 to_sun = -glm::normalize(lighting.sun_direction);
    const float sun_luma = glm::dot(glm::vec3(lighting.sun_diffuse), glm::vec3{0.2126f, 0.7152f, 0.0722f});
    // Shadows while the sun is up and shines, out over its last degree above
    // the horizon. Until 2026-11-21 they faded from 8 degrees down to 1 while
    // its light did not: a rising sun shone through the walls, the cab's back
    // wall cast none, its desk lit with the sun behind (testers).
    const float sun_up = glm::clamp(to_sun.y / 0.0175f, 0.0f, 1.0f);
    const float shadow_strength = sun_up * glm::clamp(sun_luma * 1.6f, 0.0f, 1.0f);
    const bool shadows = active && s.shadows && shadow_strength > 0.01f;
    impl.ensure_shadow(active && s.shadows);
    impl.ensure_still(active && s.shadows && s.cache_far_cascades);
    impl.ensure_lamp_shadow(active && s.lamp_shadows > 0);
    impl.ensure_ao(active && (s.ambient_occlusion || s.bloom || s.light_shafts), (std::max)(view.width, 1u), (std::max)(view.height, 1u),
                   s.occlusion_low);
    impl.ensure_groups();

    const auto inverse_view = glm::inverse(view.view);
    const glm::vec3 forward = glm::normalize(glm::vec3(inverse_view * glm::vec4{0.0f, 0.0f, -1.0f, 0.0f}));
    frame.eye = glm::vec4{view.eye, view.near_plane};
    frame.forward = glm::vec4{forward, view.far_plane};
    frame.screen = glm::vec4{static_cast<float>(view.width), static_cast<float>(view.height), 1.0f / static_cast<float>((std::max)(view.width, 1u)),
                             1.0f / static_cast<float>((std::max)(view.height, 1u))};
    frame.sun = glm::vec4{to_sun, 0.0f};
    frame.sun_colour = glm::vec4{linear(lighting.sun_diffuse), 1.0f};
    frame.view = view.view;
    frame.inv_projection = glm::inverse(view.projection);
    frame.ao = glm::vec4{1.2f, 1.6f, std::tan(view.vertical_fov * 0.5f), view.aspect};

    impl.cascade_count = 0;
    impl.focus_active = false;
    frame.focus_params.w = min_penumbra;
    if (shadows && impl.shadow_layers[0]) {
        impl.compute_cascades(to_sun);
        frame.shadow = glm::vec4{static_cast<float>(impl.cascade_count), shadow_strength, std::clamp(s.shadow_softness, 0.5f, 4.0f),
                                 static_cast<float>(impl.map_size())};
    }

    // Clusters: 16 x 9 tiles, 24 slices of log depth from 0.1 m to the far plane.
    const float cluster_near = 0.1f;
    const float cluster_far = (std::max)(view.far_plane, 1.0f);
    const float scale = static_cast<float>(cluster_z) / std::log(cluster_far / cluster_near);
    frame.clusters[0] = cluster_x;
    frame.clusters[1] = cluster_y;
    frame.clusters[2] = cluster_z;
    frame.clusters[3] = cluster_capacity;
    frame.cluster_params = glm::vec4{std::ceil(static_cast<float>(view.width) / cluster_x), std::ceil(static_cast<float>(view.height) / cluster_y),
                                     scale, -scale * std::log(cluster_near)};
    impl.light_count = 0;
    impl.packed_lights.clear();
    impl.lamp_shadow_count = 0;
    if (active && (s.clustered_lights || s.lamp_shadows > 0)) {
        // The scene's lights, nearest first: the clustered list, and the
        // lamps the shadows are chosen from (also without clusters).
        struct Candidate {
            const ModelExtraLight* light;
            glm::vec3 centre;
            float radius;
            float distance;
        };
        std::vector<Candidate> candidates;
        candidates.reserve(lighting.light_rings.all.size());
        for (const auto& entry : lighting.light_rings.all) {
            const auto [centre, radius] = light_sphere(entry.light);
            if (radius <= 0.0f) continue;
            candidates.push_back({&entry.light, centre, radius, glm::distance(centre, view.eye) - radius});
        }
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; });
        const auto count = (std::min)(candidates.size(), static_cast<std::size_t>(max_lights));
        std::vector<detail::GpuLight> packed(count);
        std::vector<glm::vec4> spheres(3 * count, glm::vec4{0.0f, 0.0f, 0.0f, -1.0f});
        impl.packed_lights.clear();
        for (std::size_t i = 0; i < count; ++i) {
            detail::pack_light(packed[i], *candidates[i].light);
            impl.packed_lights.push_back(*candidates[i].light);
            spheres[3 * i] = glm::vec4{glm::vec3(view.view * glm::vec4{candidates[i].centre, 1.0f}), candidates[i].radius};
            if (const auto cone = extra_light_cone(*candidates[i].light)) {
                spheres[3 * i + 1] = glm::vec4{glm::vec3(view.view * glm::vec4{cone->apex, 1.0f}), cone->reach};
                spheres[3 * i + 2] = glm::vec4{glm::normalize(glm::vec3(view.view * glm::vec4{cone->axis, 0.0f})), cone->tangent};
            }
        }
        impl.light_count = s.clustered_lights ? static_cast<std::uint32_t>(count) : 0u;
        if (impl.light_count > 0) {
            const auto queue = wgpuDeviceGetQueue(impl.device);
            wgpuQueueWriteBuffer(queue, impl.lights, 0, packed.data(), packed.size() * sizeof(detail::GpuLight));
            wgpuQueueWriteBuffer(queue, impl.spheres, 0, spheres.data(), spheres.size() * sizeof(glm::vec4));
            wgpuQueueRelease(queue);
        }
    }
    frame.flags[0] = active && s.clustered_lights ? 1u : 0u;
    frame.flags[1] = active && s.ambient_occlusion && impl.ao_is_real ? (s.show_occlusion ? 2u : 1u) : 0u;
    impl.camera_depth_wanted = active && (s.ambient_occlusion || s.bloom || s.light_shafts) && impl.ao_is_real && impl.depth.view;
    frame.flags[2] = active && s.aerial_perspective ? 1u : 0u;
    frame.flags[3] = impl.light_count;
    impl.lamp_shadows_wanted = active && s.lamp_shadows > 0 && !impl.packed_lights.empty() && impl.lamp_layers[0];

    // Haze from the environment: a third of the light left at RTS's fog end
    // (the sky dome's edge), thinning with height over about 1.5 km; the
    // sky's light scattered in, a sun glow towards the sun; RTS's fog colour
    // over the last quarter of that distance.
    const float fog_end = lighting.fog_end > 0.0f ? lighting.fog_end : 3000.0f;
    frame.aerial = glm::vec4{1.0f / fog_end, 1.0f / 1500.0f, 0.76f, 0.25f * glm::clamp(sun_luma, 0.0f, 1.0f) * sun_up};
    // Under openRTS's sky the fog ends at the land's end, not at RTS's dome
    // (twice as far or more): a clearer air, the horizon behind as its colour.
    const auto& ring = lighting.fog_ring;
    frame.fog_sun = ring.on ? glm::vec4{ring.sun, 0.0f, 0.0f} : glm::vec4{0.0f};
    for (std::size_t i = 0; i < FogRing::count; ++i) frame.fog_ring[i] = glm::vec4{linear(glm::vec4{ring.colours[i], 1.0f}), 1.0f};
    frame.cloud_sun = glm::vec4{0.0f};
    frame.cloud_edge = glm::vec4{0.0f, 1.0f, 1.0f, 0.0f};
    if (active && clouds.map) {
        frame.cloud_area = clouds.area;
        frame.cloud_sun = clouds.sun;
        frame.cloud_edge = glm::vec4{clouds.edge, clouds.low_sun, 0.0f};
    }
    if (ring.on) frame.aerial.x = 0.6f / fog_end;
    // haze.w: RTS's fog strength, its fog colour's alpha (the fog shaders
    // scale their fog by it; the sky makes it 0 once the sun is 2 degrees
    // down, rts_sky.cpp): the land's end turns into the fog colour only so far.
    frame.haze = glm::vec4{linear(lighting.sun_ambient + lighting.flash), glm::clamp(lighting.fog_colour.w, 0.0f, 1.0f)};
    frame.horizon = glm::vec4{linear(lighting.fog_colour), lighting.fog_end};

    // Lamps by day: the brighter the sun or the sky (linear), the weaker,
    // down to lamps_in_daylight; as given at night.
    {
        constexpr glm::vec3 luma{0.2126f, 0.7152f, 0.0722f};
        const float light = (std::max)(glm::dot(linear(lighting.sun_diffuse), luma), glm::dot(linear(lighting.sun_ambient), luma));
        const float t = glm::clamp((light - 0.1f) / 0.5f, 0.0f, 1.0f);
        impl.daylight = active ? t * t * (3.0f - 2.0f * t) : 0.0f;
        const float strength = 1.0f - impl.daylight * (1.0f - std::clamp(s.lamps_in_daylight, 0.0f, 1.0f));
        // z: the cab's and salon's lamps, at least half (cab_lamps_in_daylight).
        const float cab_strength = 1.0f - impl.daylight * (1.0f - (std::max)(std::clamp(s.lamps_in_daylight, 0.0f, 1.0f), cab_lamps_in_daylight));
        // w: the light indoors (SceneEffectSettings::indoor_light).
        frame.lamps = glm::vec4{1.0f - strength, active ? std::clamp(s.ground_bounce, 0.0f, 0.5f) : 0.0f, 1.0f - cab_strength,
                                active ? std::clamp(s.indoor_light, 0.0f, 1.0f) : 0.0f};
    }
    // The openness near the eye, not below the tunnel's least.
    frame.sky_light = glm::vec4{active && sky.on() ? 1.0f : 0.0f, static_cast<float>(sky_light_mips - 1),
                                std::clamp((std::max)(sky.openness, std::clamp(s.sky_light_tunnel, 0.0f, 0.5f)), 0.0f, 1.0f), 0.0f};
    frame.sky_tune = glm::vec4{std::clamp(s.sky_light_strength, 0.0f, 2.0f), std::clamp(s.sky_reflection_strength, 0.0f, 2.0f),
                               std::clamp(s.sky_reflection_indoors, 0.0f, 1.0f), 0.0f};
    frame.views = glm::vec4{static_cast<float>(std::clamp(s.material_view, 0, 6)), s.parallax ? std::clamp(s.parallax_distance, 0.0f, 200.0f) : 0.0f,
                            static_cast<float>(std::clamp(s.parallax_steps, 8, 64)), 0.0f};

    const auto queue = wgpuDeviceGetQueue(impl.device);
    wgpuQueueWriteBuffer(queue, impl.uniform, 0, &frame, sizeof(frame));
    wgpuQueueRelease(queue);
}

void SceneEffects::Impl::fit_focus(const std::optional<ShadowFocus>& focus) {
    auto& impl = *this;
    impl.focus_active = false;
    frame.focus_params.x = 0.0f;
    const auto layer = impl.cascade_count;
    if (!impl.active || layer == 0 || !impl.settings.cab_shadows || !focus || focus->radius <= 0.0f || layer >= impl.shadow.layers
        || !impl.shadow_layers[layer]) {
        return;
    }
    // Seen from the sun, its square centred on the sphere and turned with
    // the axis (no snapping: the texels move with the cab); 300 m more
    // towards the sun for casters out of it, as the cascades.
    const glm::vec3 to_sun{frame.sun};
    const float radius = focus->radius;
    glm::vec3 up = focus->axis;
    if (glm::length(up) < 1e-3f || std::abs(glm::dot(glm::normalize(up), to_sun)) > 0.95f) {
        up = std::abs(to_sun.y) > 0.99f ? glm::vec3{0.0f, 0.0f, 1.0f} : glm::vec3{0.0f, 1.0f, 0.0f};
    }
    constexpr float behind = 300.0f;
    const glm::mat4 light_view = glm::lookAtRH(focus->centre + to_sun * (radius + behind), focus->centre, glm::normalize(up));
    const glm::mat4 projection = glm::orthoRH_ZO(-radius, radius, -radius, radius, 0.0f, 2.0f * radius + behind);
    const auto matrix = projection * light_view;
    impl.cascade_matrices[layer] = matrix;
    impl.focus_layer = layer;
    impl.focus_active = true;
    const auto size = static_cast<float>(impl.map_size());
    const float texel = 2.0f * radius / size;
    // A point is looked up in it while the filter and the push off its
    // surface stay inside: this many clip units from its edge.
    const float softness = std::clamp(impl.settings.shadow_softness, 0.5f, 4.0f);
    const float reach = (std::max)(softness, min_penumbra / texel) + 4.0f + 0.5f * softness;
    frame.focus = matrix;
    frame.focus_params = glm::vec4{static_cast<float>(layer + 1), texel, reach * 2.0f / size, min_penumbra};
}

// The lamps lighting the focus most throw shadows: each lamp's strength at
// seven points of the sphere (its middle and 0.4 of the radius each way)
// times its brightness, a lamp kept from the last frame counted half as
// much again (no flicker between two alike). Seen from the lamp at the
// sphere when the lamp is outside it; from inside, along its own cone (a
// sphere lamp there is left out: it would need six maps); a parallel
// (box) lamp orthographically along its axis.
void SceneEffects::Impl::choose_lamp_shadows(const std::optional<ShadowFocus>& focus, const bool fade) {
    lamp_shadow_count = 0;
    lamps_lighting = 0;
    lamp_radius = 0.0f;
    std::fill(std::begin(frame.lamp_shadow_ids), std::end(frame.lamp_shadow_ids), 0u);
    std::fill(std::begin(frame.lamp_shadow_keys), std::end(frame.lamp_shadow_keys), glm::vec4{0.0f});
    frame.lamp_shadow_params = glm::vec4{0.0f};
    frame.lamp_focus = glm::vec4{0.0f};
    if (!lamp_shadows_wanted) {
        lamp_shadow_kept.clear();
        for (auto& kept : lamp_kept) kept.valid = false;
        return;
    }
    glm::vec3 centre;
    float radius;
    if (focus && focus->radius > 0.0f) {
        centre = focus->centre;
        radius = focus->radius;
    } else {
        // Without a cab: 20 m around a point 12 m ahead of the camera.
        const auto forward = glm::normalize(glm::vec3(glm::inverse(view.view) * glm::vec4{0.0f, 0.0f, -1.0f, 0.0f}));
        centre = view.eye + forward * 12.0f;
        radius = 20.0f;
    }
    std::array<glm::vec3, 7> samples{centre, centre, centre, centre, centre, centre, centre};
    const float step = radius * 0.4f;
    samples[1].x += step;
    samples[2].x -= step;
    samples[3].y += step;
    samples[4].y -= step;
    samples[5].z += step;
    samples[6].z -= step;
    struct Pick {
        std::uint32_t index;
        float score;
    };
    std::vector<Pick> picks;
    constexpr glm::vec3 luma{0.2126f, 0.7152f, 0.0722f};
    for (std::uint32_t i = 0; i < packed_lights.size(); ++i) {
        const auto& light = packed_lights[i];
        const glm::vec3 position{glm::inverse(light.to_light)[3]};
        const int kind = static_cast<int>(light.kind) & 31;
        const bool inside = glm::distance(position, centre) < radius * 1.3f;
        if (kind == 2 && inside) continue;
        float strength = 0.0f;
        for (const auto& sample : samples) strength = (std::max)(strength, lamp_strength_at(light, sample));
        // Where its beam falls: a big sphere's seven points may all miss a
        // lamp lighting the ground between them. Nearer the middle counts more.
        {
            const glm::vec3 along = glm::length(light.direction) > 1e-4f ? glm::normalize(light.direction) : glm::vec3{0.0f, -1.0f, 0.0f};
            const float reach = (std::max)(light.size.z * (light.attenuation.y + light.attenuation.z), 1.0f);
            const glm::vec3 beam = position + along * (std::min)(reach * 0.5f, 40.0f);
            const float off = glm::distance(beam, centre);
            if (off < radius) strength = (std::max)(strength, lamp_strength_at(light, beam) * (1.0f - 0.5f * off / radius));
        }
        float score = strength * glm::dot(glm::vec3(light.diffuse), luma);
        if (score < 0.01f) continue;
        for (const auto& kept : lamp_shadow_kept) {
            if (glm::distance(kept, position) < 0.1f) {
                score *= 1.5f;
                break;
            }
        }
        picks.push_back({i, score});
    }
    const std::uint32_t slots = (std::min)(settings.lamp_shadows, max_lamp_shadows);
    const auto top = (std::min)(static_cast<std::uint32_t>(picks.size()), slots);
    std::partial_sort(picks.begin(), picks.begin() + top, picks.end(), [](const Pick& a, const Pick& b) { return a.score > b.score; });
    lamp_shadow_kept.clear();
    ++lamp_frame;
    const auto size = static_cast<float>(lamp_map_size());
    // The fade's step: the time since the last frame (at most a tenth of a second).
    const auto now = std::chrono::steady_clock::now();
    const float dt = lamp_clock ? std::clamp(std::chrono::duration<float>(now - *lamp_clock).count(), 0.0f, 0.1f) : 0.0f;
    lamp_clock = now;
    const auto same = [&](const LampKept& kept, const ModelExtraLight& light) {
        const int kind = static_cast<int>(light.kind) & 31;
        return kept.owner == light.owner && kept.kind == kind && glm::distance(kept.position, glm::vec3{glm::inverse(light.to_light)[3]}) < 1.0f;
    };
    // Each slot's lamp this frame (packed_lights' index, -1 none): the one it
    // drew if it is still among the best (staying), else while that lamp is
    // still there (going, fading out); then the best without a slot take the
    // free ones (fading in).
    std::array<int, max_lamp_shadows> slot_light{};
    slot_light.fill(-1);
    std::array<bool, max_lamp_shadows> going{};
    std::array<bool, max_lamp_shadows> matched{};
    std::array<bool, max_lamp_shadows> taken{};
    for (std::uint32_t k = 0; k < slots; ++k) {
        auto& kept = lamp_kept[k];
        if (!kept.valid || kept.generation != lamp_generation) {
            kept.valid = false;
            continue;
        }
        for (std::uint32_t p = 0; p < top && slot_light[k] < 0; ++p) {
            if (taken[p] || !same(kept, packed_lights[picks[p].index])) continue;
            taken[p] = true;
            slot_light[k] = static_cast<int>(picks[p].index);
        }
        if (slot_light[k] < 0) {
            for (std::uint32_t i = 0; i < packed_lights.size(); ++i) {
                if (!same(kept, packed_lights[i])) continue;
                slot_light[k] = static_cast<int>(i);
                going[k] = true;
                break;
            }
        }
        if (slot_light[k] < 0) {
            kept.valid = false; // its lamp gone: the slot free at once
            continue;
        }
        matched[k] = true;
    }
    for (std::uint32_t p = 0; p < top; ++p) {
        if (taken[p]) continue;
        for (std::uint32_t k = 0; k < slots; ++k) {
            if (slot_light[k] >= 0) continue;
            slot_light[k] = static_cast<int>(picks[p].index);
            lamp_weight[k] = 0.0f;
            break;
        }
    }
    std::uint32_t wanted = 0; // the slots in use: one past the last
    for (std::uint32_t k = 0; k < slots; ++k) {
        if (slot_light[k] < 0) continue;
        const float fade_step = dt / lamp_fade_time;
        lamp_weight[k] = std::clamp(lamp_weight[k] + (going[k] ? -fade_step : fade_step), 0.0f, 1.0f);
        if (going[k] && lamp_weight[k] <= 0.0f) {
            // Faded out: the slot is free for the next frame's best.
            slot_light[k] = -1;
            lamp_kept[k].valid = false;
            continue;
        }
        wanted = k + 1;
    }
    for (std::uint32_t k = 0; k < wanted; ++k) {
        if (slot_light[k] < 0) {
            // A free slot among those in use: nothing drawn, no lamp's.
            lamp_redraw[k] = false;
            lamp_owners[k] = 0;
            frame.lamp_shadow_ids[k] = 0;
            frame.lamp_shadow_keys[k] = glm::vec4{0.0f};
            continue;
        }
        const auto light_index = static_cast<std::uint32_t>(slot_light[k]);
        const auto& light = packed_lights[light_index];
        lamp_owners[k] = light.owner;
        const glm::mat4 to_world = glm::inverse(light.to_light);
        const glm::vec3 position{to_world[3]};
        glm::vec3 axis = glm::length(light.direction) > 1e-4f ? glm::normalize(light.direction) : glm::vec3{0.0f, -1.0f, 0.0f};
        const int kind = static_cast<int>(light.kind) & 31;
        const float distance = glm::distance(position, centre);
        const auto up_for = [](const glm::vec3& along) {
            return std::abs(along.y) > 0.99f ? glm::vec3{0.0f, 0.0f, 1.0f} : glm::vec3{0.0f, 1.0f, 0.0f};
        };
        glm::mat4 matrix{1.0f};
        float texel = 0.0f;
        if (kind == 3) {
            // Parallel: its plane at the sphere, orthographic along the axis.
            const float along = (std::max)(glm::dot(centre - position, axis), 0.0f);
            const glm::vec3 eye = centre - axis * along;
            const glm::mat4 light_view = glm::lookAtRH(eye, centre, up_for(axis));
            matrix = glm::orthoRH_ZO(-radius, radius, -radius, radius, 0.0f, along + radius) * light_view;
            texel = 2.0f * radius / size;
        } else if (distance > radius * 1.3f) {
            // From the lamp, just around the sphere.
            const glm::vec3 to = (centre - position) / distance;
            const float half = std::asin((std::min)(radius * 1.05f / distance, 0.98f));
            const float near_plane = std::clamp((distance - radius) * 0.5f, 0.1f, 1.0f);
            const glm::mat4 light_view = glm::lookAtRH(position, centre, up_for(to));
            matrix = glm::perspectiveRH_ZO(2.0f * half, 1.0f, near_plane, distance + radius * 1.2f) * light_view;
            texel = 2.0f * std::tan(half) / size;
        } else {
            // From inside: along its own cone, at most 120 degrees wide.
            const float spread = kind == 1 ? std::abs(light.size.y) : (std::max)(std::abs(light.size.x), std::abs(light.size.y));
            const float half = (std::min)(std::atan(spread), glm::radians(60.0f));
            const float reach = (std::max)(light.size.z * (light.attenuation.y + light.attenuation.z), 1.0f);
            const glm::mat4 light_view = glm::lookAtRH(position, position + axis, up_for(axis));
            matrix = glm::perspectiveRH_ZO(2.0f * (std::max)(half, glm::radians(5.0f)), 1.0f, 0.1f, reach) * light_view;
            texel = 2.0f * std::tan(half) / size;
        }
        // Kept from its last drawing, or drawn this frame.
        auto& kept = lamp_kept[k];
        const float moved = (std::max)(glm::distance(kept.position, position), glm::distance(kept.centre, centre));
        const bool turn = (lamp_frame + k) % 2 == 0;
        if (matched[k] && kept.generation == lamp_generation && moved <= 1e-4f && !turn) {
            matrix = kept.matrix;
            texel = kept.texel;
            lamp_redraw[k] = false;
        } else {
            kept = LampKept{position, centre, matrix, texel, light.owner, kind, lamp_generation, true};
            lamp_redraw[k] = true;
        }
        lamp_matrices[k] = matrix;
        frame.lamp_shadows[k] = matrix;
        frame.lamp_shadow_ids[k] = light_index + 1;
        // w: how much of its shadow shows (lamp_weight).
        frame.lamp_shadow_keys[k] = glm::vec4{glm::vec3(light.to_light[3]), lamp_weight[k]};
        frame.lamp_shadow_texel[static_cast<int>(k)] = texel;
        lamp_shadow_kept.push_back(position);
    }
    for (std::uint32_t k = wanted; k < max_lamp_shadows; ++k) {
        lamp_kept[k].valid = false;
        lamp_weight[k] = 0.0f;
    }
    lamp_shadow_count = wanted;
    lamps_lighting = static_cast<std::uint32_t>(picks.size());
    lamp_radius = radius;
    frame.lamp_shadow_params = glm::vec4{static_cast<float>(wanted), size, std::clamp(settings.shadow_softness, 0.5f, 4.0f) * 1.5f, 0.0f};
    if (fade && wanted > 0) frame.lamp_focus = glm::vec4{centre, radius};
}

void SceneEffects::set_focus(const std::optional<ShadowFocus>& focus, const std::optional<ShadowFocus>& lamps) {
    OPENRTS_ZONE("effects: focus and lamps");
    auto& impl = *impl_;
    impl.fit_focus(focus);
    const bool flight = lamps && lamps->radius > 0.0f;
    impl.choose_lamp_shadows(flight ? lamps : focus, flight);
    const auto queue = wgpuDeviceGetQueue(impl.device);
    wgpuQueueWriteBuffer(queue, impl.uniform, 0, &impl.frame, sizeof(impl.frame));
    wgpuQueueRelease(queue);
}

// One shadow map: a cascade or the focus.
// The cascade's window this frame in the light's plane, from its slice's
// sphere: what the cache has to draw (whole, or the strips the window moved
// into), its working matrix and the unwrap's offset.
void SceneEffects::Impl::place_still(const std::uint32_t i, const glm::vec3& centre, const float radius, const float split,
                                     const glm::vec3& to_sun, const std::uint32_t size) {
    constexpr double behind = 300.0; // m towards the sun for casters out of view, as the cascades
    constexpr double margin = 150.0; // m the slice may move along the light before the depth is anchored again
    // Frames between whole drawings, each cascade its own: objects whose
    // model came in after their strip was drawn, a model at another distance
    // level, signal arms, wait at most this (the check: on the move 0.1-0.6 %
    // of the texels differed at 120 frames, in bursts as models loaded).
    const std::uint64_t refresh = 30 + 7 * i;
    auto& k = still[i];
    const double t = 2.0 * static_cast<double>(radius) / static_cast<double>(size);
    const glm::dvec3 c{centre};
    const glm::dvec3 f = -glm::normalize(glm::dvec3{to_sun});
    const glm::dvec3 up = std::abs(to_sun.y) > 0.99f ? glm::dvec3{0.0, 0.0, 1.0} : glm::dvec3{0.0, 1.0, 0.0};
    const glm::dvec3 s = glm::normalize(glm::cross(f, up));
    const glm::dvec3 u = glm::cross(s, f);
    const auto n = static_cast<std::int64_t>(size);
    const auto corner = [&](const glm::dvec3& across, const glm::dvec3& upward, std::int64_t& col, std::int64_t& row) {
        col = static_cast<std::int64_t>(std::floor(glm::dot(across, c) / t)) - n / 2;
        row = static_cast<std::int64_t>(std::floor(-glm::dot(upward, c) / t)) - n / 2;
    };
    bool full = !k.valid || k.generation != still_generation || k.radius != radius || glm::dot(k.f, f) < 0.99995
                || frame_index - k.drawn >= refresh;
    std::int64_t col = 0, row = 0;
    if (!full) {
        corner(k.s, k.u, col, row);
        const double z = glm::dot(k.f, c);
        full = std::llabs(col - k.col) >= n || std::llabs(row - k.row) >= n || z - (radius + behind) < k.z0 || z + radius > k.z0 + k.zr;
    }
    struct Rect {
        std::int64_t c0, r0, c1, r1; // global texels, the ends past
    };
    std::array<Rect, 2> rects{};
    std::size_t rect_count = 0;
    if (full) {
        corner(s, u, col, row);
        const double z = glm::dot(f, c);
        k = {.valid = true,
             .generation = still_generation,
             .s = s,
             .u = u,
             .f = f,
             .radius = radius,
             .col = col,
             .row = row,
             .phase_col = col,
             .phase_row = row,
             .z0 = z - (radius + behind) - margin,
             .zr = 2.0 * radius + behind + 2.0 * margin,
             .drawn = frame_index};
        rects[rect_count++] = {col, row, col + n, row + n};
        ++still_full;
    } else {
        // The columns and the rows the window moved into.
        if (col > k.col) rects[rect_count++] = {k.col + n, row, col + n, row + n};
        else if (col < k.col) rects[rect_count++] = {col, row, k.col, row + n};
        if (row > k.row) rects[rect_count++] = {col, k.row + n, col + n, row + n};
        else if (row < k.row) rects[rect_count++] = {col, row, col + n, k.row};
        k.col = col;
        k.row = row;
    }
    // World -> NDC over global texels [a0, a0 + w) x [b0, b0 + h); depth
    // (dot(f, p) - z0) / zr. Columns grow along s, rows against u (the
    // texture's v down, as the cascades' uv = xy * (0.5, -0.5) + 0.5).
    const auto matrix = [&](const double a0, const double b0, const double w, const double h) {
        glm::mat4 m{0.0f};
        const glm::dvec3 rx = k.s * (2.0 / (t * w));
        const glm::dvec3 ry = k.u * (2.0 / (t * h));
        const glm::dvec3 rz = k.f / k.zr;
        for (int a = 0; a < 3; ++a) {
            m[a][0] = static_cast<float>(rx[a]);
            m[a][1] = static_cast<float>(ry[a]);
            m[a][2] = static_cast<float>(rz[a]);
        }
        m[3][0] = static_cast<float>(-2.0 * a0 / w - 1.0);
        m[3][1] = static_cast<float>(1.0 + 2.0 * b0 / h);
        m[3][2] = static_cast<float>(-k.z0 / k.zr);
        m[3][3] = 1.0f;
        return m;
    };
    const auto wrap = [n](const std::int64_t v) { return ((v % n) + n) % n; };
    // A range of global texels as it lies in the layer: a piece, or two where it wraps.
    const auto split_range = [&](const std::int64_t a, const std::int64_t b, const std::int64_t phase,
                                 std::array<std::pair<std::int64_t, std::int64_t>, 2>& out) {
        const auto first = (std::min)(b - a, n - wrap(a - phase));
        out[0] = {a, a + first};
        if (a + first < b) {
            out[1] = {a + first, b};
            return 2;
        }
        return 1;
    };
    auto& pieces = still_now[i];
    bool overflow = false;
    for (std::size_t r = 0; r < rect_count; ++r) {
        const auto& rect = rects[r];
        if (rect.c1 <= rect.c0 || rect.r1 <= rect.r0) continue;
        std::array<std::pair<std::int64_t, std::int64_t>, 2> cols{}, rows{};
        const int column_pieces = split_range(rect.c0, rect.c1, k.phase_col, cols);
        const int row_pieces = split_range(rect.r0, rect.r1, k.phase_row, rows);
        for (int a = 0; a < column_pieces; ++a) {
            for (int b = 0; b < row_pieces; ++b) {
                if (pieces.size() >= still_pieces) {
                    overflow = true;
                    continue;
                }
                const auto [c0, c1] = cols[static_cast<std::size_t>(a)];
                const auto [r0, r1] = rows[static_cast<std::size_t>(b)];
                const auto w = static_cast<double>(c1 - c0), h = static_cast<double>(r1 - r0);
                pieces.push_back({.matrix = matrix(static_cast<double>(c0), static_cast<double>(r0), w, h),
                                  .x = static_cast<std::uint32_t>(wrap(c0 - k.phase_col)),
                                  .y = static_cast<std::uint32_t>(wrap(r0 - k.phase_row)),
                                  .width = static_cast<std::uint32_t>(c1 - c0),
                                  .height = static_cast<std::uint32_t>(r1 - r0)});
                still_texels += static_cast<std::uint64_t>(c1 - c0) * static_cast<std::uint64_t>(r1 - r0);
            }
        }
    }
    if (overflow) k.valid = false; // drawn whole next frame
    const auto working = matrix(static_cast<double>(col), static_cast<double>(row), static_cast<double>(n), static_cast<double>(n));
    cascade_matrices[i] = working;
    frame.cascades[i] = working;
    frame.splits[static_cast<int>(i)] = split;
    frame.texel[static_cast<int>(i)] = static_cast<float>(t);
    redraw[i] = true;
    unwrap_at[i * 4 + 0] = static_cast<std::uint32_t>(wrap(col - k.phase_col));
    unwrap_at[i * 4 + 1] = static_cast<std::uint32_t>(wrap(row - k.phase_row));
    unwrap_at[i * 4 + 2] = i;
    unwrap_at[i * 4 + 3] = size;
}

// The cache's pieces of cascade `i`: each set to the far plane, then the
// still casters drawn into it.
void SceneEffects::Impl::record_still(const WGPUCommandEncoder encoder, const std::uint32_t i, const DepthDraw& draw) const {
    const auto& pieces = still_now[i];
    if (pieces.empty() || !still_layers[i] || !fill_far) return;
    const WGPURenderPassDepthStencilAttachment attachment{
        .view = still_layers[i],
        .depthLoadOp = WGPULoadOp_Load,
        .depthStoreOp = WGPUStoreOp_Store,
        .depthClearValue = 1.0f,
        .depthReadOnly = false,
        .stencilLoadOp = WGPULoadOp_Undefined,
        .stencilStoreOp = WGPUStoreOp_Undefined,
        .stencilReadOnly = true,
    };
    const auto timestamps = render_pass_timestamps(GpuPass::sun_shadow_cache);
    const WGPURenderPassDescriptor descriptor{.depthStencilAttachment = &attachment, .timestampWrites = timestamps ? &*timestamps : nullptr};
    const auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
    for (std::size_t k = 0; k < pieces.size(); ++k) {
        const auto& piece = pieces[k];
        wgpuRenderPassEncoderSetViewport(pass, static_cast<float>(piece.x), static_cast<float>(piece.y), static_cast<float>(piece.width),
                                         static_cast<float>(piece.height), 0.0f, 1.0f);
        wgpuRenderPassEncoderSetScissorRect(pass, piece.x, piece.y, piece.width, piece.height);
        wgpuRenderPassEncoderSetPipeline(pass, fill_far);
        wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
        if (draw) draw(pass, still_slot + i * still_pieces + static_cast<std::uint32_t>(k), piece.matrix, DepthPassKind::shadow, DepthCasters::still);
    }
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
}

// Cascade `i`'s working map: the cache's layer read at the window's offset,
// then the moving casters; with the check, the cascade drawn whole too.
void SceneEffects::Impl::record_working(const WGPUCommandEncoder encoder, const std::uint32_t i, const DepthDraw& draw) const {
    const auto begin = [&](const WGPUTextureView view, const std::optional<GpuPass> timed) {
        const WGPURenderPassDepthStencilAttachment attachment{
            .view = view,
            .depthLoadOp = WGPULoadOp_Clear,
            .depthStoreOp = WGPUStoreOp_Store,
            .depthClearValue = 1.0f,
            .depthReadOnly = false,
            .stencilLoadOp = WGPULoadOp_Undefined,
            .stencilStoreOp = WGPUStoreOp_Undefined,
            .stencilReadOnly = true,
        };
        const auto timestamps = timed ? render_pass_timestamps(*timed) : std::nullopt;
        const WGPURenderPassDescriptor descriptor{.depthStencilAttachment = &attachment, .timestampWrites = timestamps ? &*timestamps : nullptr};
        return wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
    };
    const auto pass = begin(shadow_layers[i], GpuPass::sun_shadows);
    wgpuRenderPassEncoderSetPipeline(pass, unwrap);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, unwrap_group, 0, nullptr);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, i);
    if (draw) draw(pass, i, cascade_matrices[i], DepthPassKind::shadow, DepthCasters::moving);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
    if (check_on && whole_layers[i]) {
        const auto checked = begin(whole_layers[i], std::nullopt);
        if (draw) draw(checked, check_slot + i, cascade_matrices[i], DepthPassKind::shadow, DepthCasters::all);
        wgpuRenderPassEncoderEnd(checked);
        wgpuRenderPassEncoderRelease(checked);
    }
}

void SceneEffects::Impl::record_shadow(const WGPUCommandEncoder encoder, const std::uint32_t layer, const std::uint32_t slot,
                                       const DepthDraw& draw) const {
    record_depth(encoder, shadow_layers[layer], cascade_matrices[layer], slot, draw,
                 slot == focus_slot ? GpuPass::cab_shadow : GpuPass::sun_shadows);
}

// A depth-only shadow pass into `target`.
void SceneEffects::Impl::record_depth(const WGPUCommandEncoder encoder, const WGPUTextureView target, const glm::mat4& matrix,
                                      const std::uint32_t slot, const DepthDraw& draw, const GpuPass timed) const {
    const WGPURenderPassDepthStencilAttachment attachment{
        .view = target,
        .depthLoadOp = WGPULoadOp_Clear,
        .depthStoreOp = WGPUStoreOp_Store,
        .depthClearValue = 1.0f,
        .depthReadOnly = false,
        .stencilLoadOp = WGPULoadOp_Undefined,
        .stencilStoreOp = WGPUStoreOp_Undefined,
        .stencilReadOnly = true,
    };
    const auto timestamps = render_pass_timestamps(timed);
    const WGPURenderPassDescriptor descriptor{.depthStencilAttachment = &attachment, .timestampWrites = timestamps ? &*timestamps : nullptr};
    const auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
    if (draw) draw(pass, slot, matrix, DepthPassKind::shadow, DepthCasters::all);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
}

// The camera's depth with the occlusion from it, then the light clusters.
void SceneEffects::Impl::record_camera(const WGPUCommandEncoder encoder, const DepthDraw& draw) const {
    if (camera_depth_wanted && (frame.flags[1] == 0u || (gtao_group && blur_group))) {
        const WGPURenderPassDepthStencilAttachment depth_attachment{
            .view = depth.view,
            .depthLoadOp = WGPULoadOp_Clear,
            .depthStoreOp = WGPUStoreOp_Store,
            .depthClearValue = depth_clear_value,
            .depthReadOnly = false,
            .stencilLoadOp = WGPULoadOp_Undefined,
            .stencilStoreOp = WGPUStoreOp_Undefined,
            .stencilReadOnly = true,
        };
        const auto timestamps = render_pass_timestamps(GpuPass::depth_prepass);
        const WGPURenderPassDescriptor descriptor{.depthStencilAttachment = &depth_attachment,
                                                  .timestampWrites = timestamps ? &*timestamps : nullptr};
        const auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
        if (draw) draw(pass, max_cascades, view.projection * view.view, DepthPassKind::camera, DepthCasters::all);
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);

        // The occlusion at ao_raw's size (half the frame's at low), the blur
        // at the frame's: a compute pass each, so each has its GPU time.
        const auto compute_one = [&](const GpuPass timed, const WGPUComputePipeline pipeline, const WGPUBindGroup group,
                                     const std::uint32_t width, const std::uint32_t height) {
            const auto writes = compute_pass_timestamps(timed);
            const WGPUComputePassDescriptor compute_descriptor{.timestampWrites = writes ? &*writes : nullptr};
            const auto compute = wgpuCommandEncoderBeginComputePass(encoder, &compute_descriptor);
            wgpuComputePassEncoderSetPipeline(compute, pipeline);
            wgpuComputePassEncoderSetBindGroup(compute, 0, group, 0, nullptr);
            wgpuComputePassEncoderDispatchWorkgroups(compute, (width + 7) / 8, (height + 7) / 8, 1);
            wgpuComputePassEncoderEnd(compute);
            wgpuComputePassEncoderRelease(compute);
        };
        if (frame.flags[1] != 0u && prefilter_group) {
            // The view depth's levels, then GTAO: one pass (its time the two's).
            const auto writes = compute_pass_timestamps(GpuPass::occlusion);
            const WGPUComputePassDescriptor compute_descriptor{.timestampWrites = writes ? &*writes : nullptr};
            const auto compute = wgpuCommandEncoderBeginComputePass(encoder, &compute_descriptor);
            wgpuComputePassEncoderSetPipeline(compute, prefilter);
            wgpuComputePassEncoderSetBindGroup(compute, 0, prefilter_group, 0, nullptr);
            wgpuComputePassEncoderDispatchWorkgroups(compute, (view_depth.width + 15) / 16, (view_depth.height + 15) / 16, 1);
            wgpuComputePassEncoderSetPipeline(compute, ao_half ? gtao_low : gtao);
            wgpuComputePassEncoderSetBindGroup(compute, 0, gtao_group, 0, nullptr);
            wgpuComputePassEncoderDispatchWorkgroups(compute, (ao_raw.width + 7) / 8, (ao_raw.height + 7) / 8, 1);
            wgpuComputePassEncoderEnd(compute);
            wgpuComputePassEncoderRelease(compute);
            compute_one(GpuPass::occlusion_blur, ao_half ? blur_low : blur, blur_group, ao.width, ao.height);
        }
    }
    if (frame.flags[0] != 0 && cluster_group) {
        const auto writes = compute_pass_timestamps(GpuPass::light_clusters);
        const WGPUComputePassDescriptor compute_descriptor{.timestampWrites = writes ? &*writes : nullptr};
        const auto compute = wgpuCommandEncoderBeginComputePass(encoder, &compute_descriptor);
        wgpuComputePassEncoderSetPipeline(compute, cluster);
        wgpuComputePassEncoderSetBindGroup(compute, 0, cluster_group, 0, nullptr);
        wgpuComputePassEncoderDispatchWorkgroups(compute, (cluster_count + 63) / 64, 1, 1);
        wgpuComputePassEncoderEnd(compute);
        wgpuComputePassEncoderRelease(compute);
    }
}

std::uint32_t SceneEffects::depth_slot_owner(const std::uint32_t slot) const noexcept {
    const auto& impl = *impl_;
    if (slot < Impl::lamp_slot || slot >= Impl::lamp_slot + impl.lamp_shadow_count) return 0;
    return impl.lamp_owners[slot - Impl::lamp_slot];
}

void SceneEffects::record(const WGPUCommandEncoder encoder, const DepthDraw& draw) {
    auto& impl = *impl_;
    if (!impl.active) return;
    for (std::uint32_t i = 0; i < impl.cascade_count; ++i) {
        if (impl.still_on) {
            impl.record_still(encoder, i, draw);
            if (!impl.map_skip[i]) impl.record_working(encoder, i, draw);
        } else if (impl.redraw[i]) {
            impl.record_shadow(encoder, i, i, draw);
        }
    }
    if (impl.focus_active && !impl.map_skip[max_cascades]) impl.record_shadow(encoder, impl.focus_layer, Impl::focus_slot, draw);
    for (std::uint32_t k = 0; k < impl.lamp_shadow_count; ++k) {
        if (impl.lamp_redraw[k]) impl.record_depth(encoder, impl.lamp_layers[k], impl.lamp_matrices[k], Impl::lamp_slot + k, draw, GpuPass::lamp_shadows);
    }
    impl.record_camera(encoder, draw);
}

void SceneEffects::record_parallel(const WGPUDevice device, TaskPool& pool, const DepthDraw& draw) {
    auto& impl = *impl_;
    impl.parallel_buffers.assign(impl.active ? impl.cascade_count + 2 + max_lamp_shadows : 0, nullptr);
    impl.parallel_ms.assign(impl.parallel_buffers.size(), 0.0f);
    if (!impl.active) return;
    std::vector<std::function<void()>> tasks;
    const auto encode = [device, &impl](const std::size_t index, const char* name, auto&& record) {
        OPENRTS_ZONE("effects: pass");
        OPENRTS_ZONE_TEXT(name, std::strlen(name));
        const auto start = std::chrono::steady_clock::now();
        const auto encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);
        {
            OPENRTS_ZONE("effects: record");
            record(encoder);
        }
        {
            // wgpu checks and encodes the pass's commands here.
            OPENRTS_ZONE("effects: finish");
            impl.parallel_buffers[index] = wgpuCommandEncoderFinish(encoder, nullptr);
        }
        wgpuCommandEncoderRelease(encoder);
        impl.parallel_ms[index] = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
    };
    // The longest first: the camera's passes, then the shadows (the pool
    // takes them in order; their buffers are submitted by index).
    tasks.emplace_back([encode, &impl, draw] {
        encode(impl.cascade_count + 1 + max_lamp_shadows, "camera depth, GTAO, clusters",
               [&](const WGPUCommandEncoder encoder) { impl.record_camera(encoder, draw); });
    });
    for (std::uint32_t i = 0; i < impl.cascade_count; ++i) {
        if (!impl.redraw[i]) continue;
        if (impl.still_on && impl.map_skip[i] && impl.still_now[i].empty()) continue;
        tasks.emplace_back([encode, &impl, i, draw] {
            static constexpr const char* names[] = {"sun cascade 1", "sun cascade 2", "sun cascade 3", "sun cascade 4"};
            encode(i, names[i % 4], [&](const WGPUCommandEncoder encoder) {
                if (impl.still_on) {
                    impl.record_still(encoder, i, draw);
                    if (!impl.map_skip[i]) impl.record_working(encoder, i, draw);
                } else {
                    impl.record_shadow(encoder, i, i, draw);
                }
            });
        });
    }
    if (impl.focus_active && !impl.map_skip[max_cascades]) {
        tasks.emplace_back([encode, &impl, draw] {
            encode(impl.cascade_count, "cab focus", [&](const WGPUCommandEncoder encoder) {
                impl.record_shadow(encoder, impl.focus_layer, Impl::focus_slot, draw);
            });
        });
    }
    for (std::uint32_t k = 0; k < impl.lamp_shadow_count; ++k) {
        if (!impl.lamp_redraw[k]) continue;
        tasks.emplace_back([encode, &impl, k, draw] {
            encode(impl.cascade_count + 1 + k, "lamp shadow", [&](const WGPUCommandEncoder encoder) {
                impl.record_depth(encoder, impl.lamp_layers[k], impl.lamp_matrices[k], Impl::lamp_slot + k, draw, GpuPass::lamp_shadows);
            });
        });
    }
    pool.start(std::move(tasks));
}

std::vector<WGPUCommandBuffer> SceneEffects::finish_parallel(TaskPool& pool) {
    pool.wait();
    std::vector<WGPUCommandBuffer> buffers;
    for (const auto buffer : impl_->parallel_buffers) {
        if (buffer) buffers.push_back(buffer);
    }
    impl_->parallel_buffers.clear();
    return buffers;
}

WGPUBindGroup SceneEffects::bind_group() const noexcept {
    return impl_->main_group;
}

float SceneEffects::shadow_range() const noexcept {
    if (!impl_->active) return 0.0f;
    const float sun = impl_->cascade_count > 0 ? impl_->settings.shadow_distance : 0.0f;
    const float lamps = impl_->lamp_shadows_wanted ? 80.0f : 0.0f; // casters near the focus
    return (std::max)(sun, lamps);
}

WGPUTextureView SceneEffects::camera_depth() const noexcept {
    return impl_->camera_depth_wanted ? impl_->depth.view : nullptr;
}

bool SceneEffects::wants_depth() const noexcept {
    return impl_->active && (impl_->cascade_count > 0 || impl_->frame.flags[1] != 0u || impl_->camera_depth_wanted
                             || impl_->lamp_shadows_wanted);
}

SceneEffectStats SceneEffects::stats() const noexcept {
    std::uint32_t drawn = 0;
    for (std::uint32_t i = 0; i < impl_->cascade_count; ++i) {
        drawn += (impl_->still_on ? !impl_->still_now[i].empty() : impl_->redraw[i]) ? 1u : 0u;
    }
    std::uint32_t still_pieces = 0;
    for (const auto& pieces : impl_->still_now) still_pieces += static_cast<std::uint32_t>(pieces.size());
    const double cascade_texels = static_cast<double>(impl_->map_size()) * impl_->map_size() * (std::max)(impl_->cascade_count, 1u);
    std::uint32_t lamps_drawn = 0;
    for (std::uint32_t k = 0; k < impl_->lamp_shadow_count; ++k) lamps_drawn += impl_->lamp_redraw[k] ? 1u : 0u;
    // The workers' passes: cascades first, the focus, the lamps, the camera last.
    std::array<float, 4> cascade_ms{};
    float lamps_ms = 0.0f;
    const auto& ms = impl_->parallel_ms;
    for (std::size_t i = 0; i < impl_->cascade_count && i < cascade_ms.size() && i < ms.size(); ++i) cascade_ms[i] = ms[i];
    for (std::size_t k = 0; k < impl_->lamp_shadow_count; ++k) {
        const auto index = impl_->cascade_count + 1 + k;
        if (index < ms.size()) lamps_ms += ms[index];
    }
    return {.cascades = impl_->cascade_count,
            .cascades_drawn = drawn,
            .lights = impl_->light_count,
            .focus = impl_->focus_active,
            .focus_texel = impl_->focus_active ? impl_->frame.focus_params.y : 0.0f,
            .near_texel = impl_->cascade_count > 0 ? impl_->frame.texel.x : 0.0f,
            .daylight = impl_->daylight,
            .lamp_strength = 1.0f - impl_->frame.lamps.x,
            .lamp_shadows = impl_->lamp_shadow_count,
            .lamp_shadows_drawn = lamps_drawn,
            .lamps_lighting = impl_->lamps_lighting,
            .lamp_radius = impl_->lamp_radius,
            .record_cascade_ms = cascade_ms,
            .record_focus_ms = impl_->parallel_ms.size() > impl_->cascade_count ? impl_->parallel_ms[impl_->cascade_count] : 0.0f,
            .record_lamps_ms = lamps_ms,
            .record_camera_ms = impl_->parallel_ms.empty() ? 0.0f : impl_->parallel_ms.back(),
            .still_cache = impl_->still_on,
            .still_full = impl_->still_full,
            .still_pieces = still_pieces,
            .still_share = static_cast<float>(static_cast<double>(impl_->still_texels) / cascade_texels),
            .maps_kept = static_cast<std::uint32_t>(std::count(impl_->map_skip.begin(), impl_->map_skip.end(), true))};
}

void SceneEffects::set_shadow_cache_check(const bool on) {
    impl_->check_on = on && impl_->check && impl_->check_counts;
}

void SceneEffects::set_moving_signature(const std::uint64_t signature, const std::uint64_t outside) {
    auto& impl = *impl_;
    impl.map_skip.fill(false);
    if (!impl.active || impl.check_on) return;
    const auto keep = [&](const std::size_t slot, const glm::mat4& matrix, const bool unchanged, const std::uint64_t period,
                          const std::uint64_t signature) {
        auto& kept = impl.map_kept[slot];
        const bool same = kept.valid && unchanged && kept.shadow_generation == impl.shadow_generation
                          && kept.still_generation == impl.still_generation && kept.signature == signature && kept.matrix == matrix
                          && impl.frame_index - kept.drawn < period;
        if (same) {
            impl.map_skip[slot] = true;
            return;
        }
        kept = {.valid = true,
                .shadow_generation = impl.shadow_generation,
                .still_generation = impl.still_generation,
                .signature = signature,
                .drawn = impl.frame_index,
                .matrix = matrix};
    };
    // A cascade: the cache drew nothing into it this frame (its own whole
    // drawing every 30 frames or so redraws it too).
    for (std::uint32_t i = 0; i < impl.cascade_count; ++i) {
        if (impl.still_on) keep(i, impl.cascade_matrices[i], impl.still_now[i].empty(), ~0ull, impl.focus_active ? outside : signature);
    }
    // The cab's map holds the still casters too: drawn again at least every
    // 30 frames (objects that came in).
    if (impl.focus_active) keep(max_cascades, impl.cascade_matrices[impl.focus_layer], true, 30, signature);
}

void SceneEffects::record_check(const WGPUCommandEncoder encoder) {
    auto& impl = *impl_;
    if (!impl.check_on || !impl.still_on || !impl.whole.view || !impl.shadow.view || impl.cascade_count == 0) return;
    std::array<WGPUBindGroupEntry, 3> entries{};
    entries[0] = {.binding = 0, .textureView = impl.shadow.view};
    entries[1] = {.binding = 1, .textureView = impl.whole.view};
    entries[2] = {.binding = 2, .buffer = impl.check_counts, .size = 8 * sizeof(std::uint32_t)};
    const WGPUBindGroupDescriptor descriptor{.layout = impl.check_layout, .entryCount = entries.size(), .entries = entries.data()};
    const auto group = wgpuDeviceCreateBindGroup(impl.device, &descriptor);
    const auto pass = wgpuCommandEncoderBeginComputePass(encoder, nullptr);
    wgpuComputePassEncoderSetPipeline(pass, impl.check);
    wgpuComputePassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
    const auto groups = (impl.map_size() + 7) / 8;
    wgpuComputePassEncoderDispatchWorkgroups(pass, groups, groups, impl.cascade_count);
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);
    wgpuBindGroupRelease(group);
}

std::string SceneEffects::shadow_cache_check_report(const WGPUInstance instance) {
    auto& impl = *impl_;
    if (!impl.check_on || !impl.check_counts) return {};
    if (++impl.check_frames % 120 != 0) return {};
    constexpr std::uint64_t bytes = 8 * sizeof(std::uint32_t);
    const WGPUBufferDescriptor read{.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst, .size = bytes};
    const auto buffer = wgpuDeviceCreateBuffer(impl.device, &read);
    if (!buffer) return {};
    const auto queue = wgpuDeviceGetQueue(impl.device);
    const auto encoder = wgpuDeviceCreateCommandEncoder(impl.device, nullptr);
    wgpuCommandEncoderCopyBufferToBuffer(encoder, impl.check_counts, 0, buffer, 0, bytes);
    const auto commands = wgpuCommandEncoderFinish(encoder, nullptr);
    wgpuQueueSubmit(queue, 1, &commands);
    wgpuCommandBufferRelease(commands);
    wgpuCommandEncoderRelease(encoder);
    bool done = false, mapped = false;
    struct State {
        bool* done;
        bool* mapped;
    } state{&done, &mapped};
    const WGPUBufferMapCallbackInfo callback{
        .mode = WGPUCallbackMode_AllowProcessEvents,
        .callback = [](const WGPUMapAsyncStatus status, WGPUStringView, void* data, void*) {
            auto* s = static_cast<State*>(data);
            *s->mapped = status == WGPUMapAsyncStatus_Success;
            *s->done = true;
        },
        .userdata1 = &state,
    };
    wgpuBufferMapAsync(buffer, WGPUMapMode_Read, 0, bytes, callback);
    for (int k = 0; k < 1000 && !done; ++k) {
        wgpuDevicePoll(impl.device, true, nullptr);
        if (instance) wgpuInstanceProcessEvents(instance);
    }
    std::array<std::uint32_t, 8> counts{};
    if (mapped) {
        std::memcpy(counts.data(), wgpuBufferGetConstMappedRange(buffer, 0, bytes), bytes);
        wgpuBufferUnmap(buffer);
    }
    wgpuBufferRelease(buffer);
    const std::array<std::uint32_t, 8> zero{};
    wgpuQueueWriteBuffer(queue, impl.check_counts, 0, zero.data(), bytes);
    wgpuQueueRelease(queue);
    std::ostringstream out;
    out << "shadow cache check (120 frames):";
    for (std::uint32_t i = 0; i < impl.cascade_count && i < max_cascades; ++i) {
        const double casting = counts[i + 4];
        out << " cascade " << i << ' ' << counts[i] << " texels differ of " << counts[i + 4] << " casting ("
            << (casting > 0.0 ? 100.0 * counts[i] / casting : 0.0) << " %);";
    }
    return out.str() + "\n";
}

} // namespace openrts::renderer
#endif
