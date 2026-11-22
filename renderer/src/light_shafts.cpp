#include <openrts/renderer/light_shafts.hpp>

#ifdef OPENRTS_HAS_WGPU
#include <algorithm>
#include <array>
#include <string>

#include <openrts/renderer/gpu_pass_timer.hpp>
#include <openrts/renderer/scene_effects.hpp>

#include "scene_effects_wgsl.hpp"

namespace openrts::renderer {
namespace {

constexpr char shafts_wgsl[] = R"(
struct Shafts {
    inv_view_projection: mat4x4<f32>,
    to_room: mat4x4<f32>,
    eye: vec4<f32>,        // xyz: the camera, w: 1 with a room
    room_lower: vec4<f32>, // w: the room's dust (per metre)
    room_upper: vec4<f32>, // w: the outdoor dust (per metre)
    sizes: vec4<f32>,      // the half image's width, height; the frame's width, height
    march: vec4<f32>,      // x: the cascades' end (m), y: the clouds' march's end (m), z: the light's strength
};
@group(0) @binding(0) var<uniform> shafts: Shafts;
@group(0) @binding(1) var scene_depth: texture_depth_2d;
@group(0) @binding(2) var input_image: texture_2d<f32>;

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> @builtin(position) vec4<f32> {
    let p = vec2<f32>(f32((index << 1u) & 2u), f32(index & 2u));
    return vec4<f32>(p * 2.0 - 1.0, 0.0, 1.0);
}

// A point of the frame (uv, reversed-Z depth) in the world.
fn world_at(uv: vec2<f32>, depth: f32) -> vec3<f32> {
    let w = shafts.inv_view_projection * vec4<f32>(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    return w.xyz / w.w;
}

// How far from the eye the frame shows something at a full-size texel (the
// sky: far).
fn distance_at(texel: vec2<i32>) -> f32 {
    let depth = textureLoad(scene_depth, texel, 0);
    if (depth <= 0.0) { return 1.0e6; }
    let uv = (vec2<f32>(texel) + vec2<f32>(0.5)) / shafts.sizes.zw;
    return min(distance(world_at(uv, depth), shafts.eye.xyz), 1.0e6);
}
)";

constexpr char march_wgsl[] = R"(
@group(1) @binding(0) var<uniform> effects: Effects;
@group(1) @binding(1) var shadow_map: texture_depth_2d_array;
@group(1) @binding(2) var shadow_sampler: sampler_comparison;
@group(1) @binding(8) var cloud_shadow_map: texture_2d<f32>;
@group(1) @binding(9) var cloud_sampler: sampler;

// The clouds' shadow at a point (as effects_cloud_shadow in the models).
fn cloud_light(world: vec3<f32>) -> f32 {
    let strength = effects.cloud_sun.w;
    if (strength <= 0.0) { return effects.cloud_edge.z; }
    let sun = effects.cloud_sun.xyz;
    let d = world - effects.eye.xyz;
    let q = d.xz + sun.xz * ((effects.cloud_area.w - d.y) / max(sun.y, 0.05));
    let uv = (q - effects.cloud_area.xy) / (2.0 * effects.cloud_area.z) + vec2<f32>(0.5);
    let lit = smoothstep(effects.cloud_edge.x, effects.cloud_edge.y, textureSampleLevel(cloud_shadow_map, cloud_sampler, uv, 0.0).r);
    return effects.cloud_edge.z - strength * (1.0 - lit);
}

// Whether the sun reaches a point of the air: the cab's map where it covers
// it, else the cascade of its view depth (one filtered compare); 1 past the
// last cascade.
fn sun_reaches(p: vec3<f32>) -> f32 {
    let count = u32(effects.shadow.x);
    if (effects.focus_params.x > 0.0) {
        let c = effects.focus * vec4<f32>(p, 1.0);
        if (all(abs(c.xy) < vec2<f32>(1.0 - effects.focus_params.z)) && c.z >= 0.0 && c.z <= 1.0) {
            let uv = c.xy * vec2<f32>(0.5, -0.5) + vec2<f32>(0.5);
            return textureSampleCompareLevel(shadow_map, shadow_sampler, uv, u32(effects.focus_params.x) - 1u, c.z);
        }
    }
    let depth = dot(p - effects.eye.xyz, effects.forward.xyz);
    for (var i = 0u; i < count; i = i + 1u) {
        if (depth < effects.splits[i]) {
            let c = effects.cascades[i] * vec4<f32>(p, 1.0);
            let uv = c.xy * vec2<f32>(0.5, -0.5) + vec2<f32>(0.5);
            if (any(uv < vec2<f32>(0.0)) || any(uv > vec2<f32>(1.0)) || c.z > 1.0 || c.z < 0.0) { return 1.0; }
            return textureSampleCompareLevel(shadow_map, shadow_sampler, uv, i, c.z);
        }
    }
    return 1.0;
}

// The dust at a point: thin outdoors, thinning with height above the eye;
// thicker in the room.
fn dust(p: vec3<f32>) -> f32 {
    var d = shafts.room_upper.w * exp(-max(p.y - shafts.eye.y, 0.0) / 300.0);
    if (shafts.eye.w > 0.0) {
        let q = (shafts.to_room * vec4<f32>(p, 1.0)).xyz;
        if (all(q >= shafts.room_lower.xyz) && all(q <= shafts.room_upper.xyz)) { d = d + shafts.room_lower.w; }
    }
    return d;
}

@fragment
fn fs_march(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> {
    // The nearest of the four full-size texels under this one.
    let size = vec2<i32>(textureDimensions(scene_depth));
    let base = vec2<i32>(position.xy) * 2;
    var nearest = 1.0e6;
    for (var k = 0; k < 4; k = k + 1) {
        let texel = min(base + vec2<i32>(k & 1, k >> 1), size - vec2<i32>(1));
        nearest = min(nearest, distance_at(texel));
    }
    let count = u32(effects.shadow.x);
    if (count == 0u || shafts.march.z <= 0.0) { return vec4<f32>(0.0, 0.0, 0.0, nearest); }
    let uv = (vec2<f32>(base) + vec2<f32>(1.0)) / shafts.sizes.zw;
    let eye = shafts.eye.xyz;
    let dir = normalize(world_at(uv, 1.0) - eye);
    // Henyey-Greenstein (g 0.7) and a little of an even scatter, times pi:
    // the scene lights a surface with its albedo x the sun (no 1 / pi), so
    // the air's light is weighed the same way.
    let c = dot(dir, effects.sun.xyz);
    let g = 0.7;
    let hg = (1.0 - g * g) / (12.566371 * pow(max(1.0 + g * g - 2.0 * g * c, 1e-4), 1.5));
    let phase = 3.14159265 * (0.85 * hg + 0.15 / 12.566371);
    // Interleaved gradient noise: each pixel's steps start apart (the blur
    // joins them).
    let jitter = fract(52.9829189 * fract(dot(floor(position.xy), vec2<f32>(0.06711056, 0.00583715))));
    let steps = 32u;
    let near_end = min(nearest * 0.98, shafts.march.x);
    var light = 0.0;
    var optical = 0.0;
    for (var i = 0u; i < steps; i = i + 1u) {
        // Closer together near the eye: t = end x ((i + jitter) / n)^2.
        let a = (f32(i) + jitter) / f32(steps);
        let t = near_end * a * a;
        let dt = near_end * (2.0 * f32(i) + 1.0) / f32(steps * steps);
        let p = eye + dir * t;
        let sigma = dust(p);
        light = light + sigma * exp(-optical) * sun_reaches(p) * cloud_light(p) * dt;
        optical = optical + sigma * dt;
    }
    // On under the clouds' shadow only (no cascade covers it there).
    let far_end = min(nearest * 0.98, shafts.march.y);
    if (effects.cloud_sun.w > 0.0 && far_end > near_end) {
        let far_steps = 8u;
        let dt = (far_end - near_end) / f32(far_steps);
        for (var i = 0u; i < far_steps; i = i + 1u) {
            let p = eye + dir * (near_end + (f32(i) + jitter) * dt);
            let sigma = 0.5 * dust(p);
            light = light + sigma * exp(-optical) * cloud_light(p) * dt;
            optical = optical + sigma * dt;
        }
    }
    let colour = effects.sun_colour.rgb * (phase * light * effects.shadow.y * shafts.march.z);
    return vec4<f32>(min(colour, vec3<f32>(16.0)), nearest);
}
)";

constexpr char filter_wgsl[] = R"(
// A 9-tap Gaussian along x or y over texels at about the same distance.
fn blur(position: vec4<f32>, axis: vec2<i32>) -> vec4<f32> {
    let size = vec2<i32>(textureDimensions(input_image));
    let centre = vec2<i32>(position.xy);
    let middle = textureLoad(input_image, centre, 0);
    let d0 = middle.a;
    var sum = vec3<f32>(0.0);
    var weight = 0.0;
    for (var k = -4; k <= 4; k = k + 1) {
        let texel = clamp(centre + axis * k, vec2<i32>(0), size - vec2<i32>(1));
        let tap = textureLoad(input_image, texel, 0);
        let w = exp(-f32(k * k) / 8.0) * exp(-abs(tap.a - d0) / (0.1 * d0 + 0.2));
        sum = sum + tap.rgb * w;
        weight = weight + w;
    }
    return vec4<f32>(sum / max(weight, 1e-5), d0);
}

@fragment
fn fs_blur_x(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> {
    return blur(position, vec2<i32>(1, 0));
}

@fragment
fn fs_blur_y(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> {
    return blur(position, vec2<i32>(0, 1));
}

// Up to the frame's size: the four half-size texels round it, bilinearly,
// those at about this texel's distance (a tree's edge against the sky keeps
// its own).
@fragment
fn fs_up(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> {
    let d = distance_at(vec2<i32>(position.xy));
    let half_size = vec2<i32>(textureDimensions(input_image));
    let at = position.xy * 0.5 - vec2<f32>(0.5);
    let origin = vec2<i32>(floor(at));
    let f = at - floor(at);
    var sum = vec3<f32>(0.0);
    var weight = 0.0;
    var closest = vec3<f32>(0.0);
    var closest_gap = 1.0e9;
    for (var k = 0; k < 4; k = k + 1) {
        let offset = vec2<i32>(k & 1, k >> 1);
        let texel = clamp(origin + offset, vec2<i32>(0), half_size - vec2<i32>(1));
        let tap = textureLoad(input_image, texel, 0);
        let bilinear = select(1.0 - f.x, f.x, offset.x == 1) * select(1.0 - f.y, f.y, offset.y == 1);
        let gap = abs(tap.a - d);
        let w = bilinear * exp(-gap / (0.1 * d + 0.2)) + 1e-6 * bilinear;
        sum = sum + tap.rgb * w;
        weight = weight + w;
        if (gap < closest_gap) {
            closest_gap = gap;
            closest = tap.rgb;
        }
    }
    if (weight < 1e-3) { return vec4<f32>(closest, 1.0); }
    return vec4<f32>(sum / weight, 1.0);
}
)";

[[nodiscard]] WGPUStringView wgsl_name(const char* name) {
    return {.data = name, .length = WGPU_STRLEN};
}

struct ShaftUniform {
    glm::mat4 inv_view_projection{1.0f};
    glm::mat4 to_room{1.0f};
    glm::vec4 eye{};
    glm::vec4 room_lower{};
    glm::vec4 room_upper{};
    glm::vec4 sizes{};
    glm::vec4 march{};
};

constexpr WGPUTextureFormat shaft_format = WGPUTextureFormat_RGBA16Float;

struct Image {
    WGPUTexture texture = nullptr;
    WGPUTextureView view = nullptr;
    std::uint32_t width = 0, height = 0;
    void release() {
        if (view) wgpuTextureViewRelease(view);
        if (texture) wgpuTextureRelease(texture);
        view = nullptr;
        texture = nullptr;
        width = height = 0;
    }
    void ensure(const WGPUDevice device, const std::uint32_t w, const std::uint32_t h, const char* label) {
        if (texture && width == w && height == h) return;
        release();
        const WGPUTextureDescriptor descriptor{
            .label = wgsl_name(label),
            .usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc,
            .dimension = WGPUTextureDimension_2D,
            .size = {.width = w, .height = h, .depthOrArrayLayers = 1},
            .format = shaft_format,
            .mipLevelCount = 1,
            .sampleCount = 1,
        };
        texture = wgpuDeviceCreateTexture(device, &descriptor);
        if (texture) view = wgpuTextureCreateView(texture, nullptr);
        width = w;
        height = h;
    }
};

} // namespace

struct LightShaftRenderer::Impl {
    WGPUDevice device = nullptr;
    WGPUBindGroupLayout layout = nullptr;         // group 0 of every pass
    WGPUBindGroupLayout effects_layout = nullptr; // group 1 of the march (the effects')
    WGPURenderPipeline march = nullptr;
    WGPURenderPipeline blur_x = nullptr;
    WGPURenderPipeline blur_y = nullptr;
    WGPURenderPipeline up = nullptr;
    WGPUBuffer uniform = nullptr;
    Image a, b, full;

    ~Impl() {
        a.release();
        b.release();
        full.release();
        for (auto* p : {march, blur_x, blur_y, up})
            if (p) wgpuRenderPipelineRelease(p);
        if (uniform) wgpuBufferRelease(uniform);
        if (effects_layout) wgpuBindGroupLayoutRelease(effects_layout);
        if (layout) wgpuBindGroupLayoutRelease(layout);
    }

    WGPURenderPipeline pipeline(const std::string& code, const char* entry, const bool with_effects) {
        const WGPUShaderSourceWGSL source{.chain = {.sType = WGPUSType_ShaderSourceWGSL}, .code = {.data = code.c_str(), .length = code.size()}};
        const WGPUShaderModuleDescriptor shader_descriptor{.nextInChain = &source.chain, .label = wgsl_name("light shafts")};
        const auto shader = wgpuDeviceCreateShaderModule(device, &shader_descriptor);
        if (!shader) return nullptr;
        const WGPUBindGroupLayout layouts[] = {layout, effects_layout};
        const WGPUPipelineLayoutDescriptor layout_descriptor{.bindGroupLayoutCount = with_effects ? 2u : 1u, .bindGroupLayouts = layouts};
        const auto pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &layout_descriptor);
        const WGPUColorTargetState target{.format = shaft_format, .writeMask = WGPUColorWriteMask_All};
        const WGPUFragmentState fragment{.module = shader, .entryPoint = wgsl_name(entry), .targetCount = 1, .targets = &target};
        const WGPURenderPipelineDescriptor descriptor{
            .label = wgsl_name(entry),
            .layout = pipeline_layout,
            .vertex = {.module = shader, .entryPoint = wgsl_name("vs_main")},
            .primitive = {.topology = WGPUPrimitiveTopology_TriangleList},
            .multisample = {.count = 1, .mask = ~0u},
            .fragment = &fragment,
        };
        const auto made = wgpuDeviceCreateRenderPipeline(device, &descriptor);
        wgpuPipelineLayoutRelease(pipeline_layout);
        wgpuShaderModuleRelease(shader);
        return made;
    }

    [[nodiscard]] WGPUBindGroup group(const WGPUTextureView depth, const WGPUTextureView input) const {
        std::array<WGPUBindGroupEntry, 3> entries{};
        entries[0].binding = 0;
        entries[0].buffer = uniform;
        entries[0].size = sizeof(ShaftUniform);
        entries[1].binding = 1;
        entries[1].textureView = depth;
        entries[2].binding = 2;
        entries[2].textureView = input;
        const WGPUBindGroupDescriptor descriptor{.layout = layout, .entryCount = entries.size(), .entries = entries.data()};
        return wgpuDeviceCreateBindGroup(device, &descriptor);
    }

    void pass(const WGPUCommandEncoder encoder, const WGPUTextureView target, const WGPURenderPipeline pipeline, const WGPUBindGroup own,
              const WGPUBindGroup effects, const bool timed) const {
        const WGPURenderPassColorAttachment colour{
            .view = target,
            .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
            .loadOp = WGPULoadOp_Clear,
            .storeOp = WGPUStoreOp_Store,
            .clearValue = {0.0, 0.0, 0.0, 0.0},
        };
        const auto timestamps = timed ? render_pass_timestamps(GpuPass::light_shafts) : std::nullopt;
        const WGPURenderPassDescriptor descriptor{
            .colorAttachmentCount = 1, .colorAttachments = &colour, .timestampWrites = timestamps ? &*timestamps : nullptr};
        const auto encoded = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
        wgpuRenderPassEncoderSetPipeline(encoded, pipeline);
        wgpuRenderPassEncoderSetBindGroup(encoded, 0, own, 0, nullptr);
        if (effects) wgpuRenderPassEncoderSetBindGroup(encoded, 1, effects, 0, nullptr);
        wgpuRenderPassEncoderDraw(encoded, 3, 1, 0, 0);
        wgpuRenderPassEncoderEnd(encoded);
        wgpuRenderPassEncoderRelease(encoded);
    }
};

LightShaftRenderer::LightShaftRenderer(const WGPUDevice device) : impl_(std::make_unique<Impl>()) {
    auto& impl = *impl_;
    impl.device = device;
    std::array<WGPUBindGroupLayoutEntry, 3> entries{};
    entries[0].binding = 0;
    entries[0].visibility = WGPUShaderStage_Fragment;
    entries[0].buffer.type = WGPUBufferBindingType_Uniform;
    entries[0].buffer.minBindingSize = sizeof(ShaftUniform);
    entries[1].binding = 1;
    entries[1].visibility = WGPUShaderStage_Fragment;
    entries[1].texture.sampleType = WGPUTextureSampleType_Depth;
    entries[1].texture.viewDimension = WGPUTextureViewDimension_2D;
    entries[2].binding = 2;
    entries[2].visibility = WGPUShaderStage_Fragment;
    entries[2].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
    entries[2].texture.viewDimension = WGPUTextureViewDimension_2D;
    const WGPUBindGroupLayoutDescriptor layout_descriptor{.entryCount = entries.size(), .entries = entries.data()};
    impl.layout = wgpuDeviceCreateBindGroupLayout(device, &layout_descriptor);
    impl.effects_layout = create_scene_effects_layout(device);
    const WGPUBufferDescriptor buffer{.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, .size = sizeof(ShaftUniform)};
    impl.uniform = wgpuDeviceCreateBuffer(device, &buffer);
    if (!impl.layout || !impl.effects_layout || !impl.uniform) return;
    const std::string march = std::string(detail::effects_struct_wgsl) + shafts_wgsl + march_wgsl;
    const std::string filter = std::string(shafts_wgsl) + filter_wgsl;
    impl.march = impl.pipeline(march, "fs_march", true);
    impl.blur_x = impl.pipeline(filter, "fs_blur_x", false);
    impl.blur_y = impl.pipeline(filter, "fs_blur_y", false);
    impl.up = impl.pipeline(filter, "fs_up", false);
}

LightShaftRenderer::~LightShaftRenderer() = default;

WGPUTexture LightShaftRenderer::texture() const noexcept {
    return impl_->full.texture;
}

bool LightShaftRenderer::ready() const noexcept {
    const auto& impl = *impl_;
    return impl.march && impl.blur_x && impl.blur_y && impl.up && impl.uniform;
}

WGPUTextureView LightShaftRenderer::render(const WGPUCommandEncoder encoder, const WGPUBindGroup effects, const WGPUTextureView depth,
                                           const LightShaftFrame& frame) {
    auto& impl = *impl_;
    if (!ready() || !effects || !depth || frame.width == 0 || frame.height == 0) return nullptr;
    const std::uint32_t half_w = (std::max)((frame.width + 1) / 2, 1u), half_h = (std::max)((frame.height + 1) / 2, 1u);
    impl.a.ensure(impl.device, half_w, half_h, "light shafts a");
    impl.b.ensure(impl.device, half_w, half_h, "light shafts b");
    impl.full.ensure(impl.device, frame.width, frame.height, "light shafts");
    if (!impl.a.view || !impl.b.view || !impl.full.view) return nullptr;

    const float strength = std::clamp(frame.strength, 0.0f, 4.0f);
    const float fog_end = frame.fog_end > 1.0f ? frame.fog_end : 3000.0f;
    ShaftUniform u;
    u.inv_view_projection = glm::inverse(frame.projection * frame.view);
    u.eye = glm::vec4{frame.eye, frame.room ? 1.0f : 0.0f};
    // Outdoors a dust of 1/1500 per metre and half of RTS's haze; in a room
    // 0.3 per metre (a beam through a window shows over a metre).
    const float outdoor = strength * (1.0f / 1500.0f + 0.5f / fog_end);
    const float indoor = strength * 0.3f;
    if (frame.room) {
        u.to_room = frame.room->to_room;
        u.room_lower = glm::vec4{frame.room->lower, indoor};
        u.room_upper = glm::vec4{frame.room->upper, outdoor};
    } else {
        u.room_lower = glm::vec4{0.0f, 0.0f, 0.0f, 0.0f};
        u.room_upper = glm::vec4{0.0f, 0.0f, 0.0f, outdoor};
    }
    u.sizes = glm::vec4{static_cast<float>(half_w), static_cast<float>(half_h), static_cast<float>(frame.width),
                        static_cast<float>(frame.height)};
    u.march = glm::vec4{(std::max)(frame.shadow_distance, 10.0f), 1500.0f, strength > 0.0f ? 1.0f : 0.0f, 0.0f};
    if (const auto queue = wgpuDeviceGetQueue(impl.device)) {
        wgpuQueueWriteBuffer(queue, impl.uniform, 0, &u, sizeof(u));
        wgpuQueueRelease(queue);
    }
    const auto march_group = impl.group(depth, impl.b.view);
    const auto blur_x_group = impl.group(depth, impl.a.view);
    const auto blur_y_group = impl.group(depth, impl.b.view);
    const auto up_group = impl.group(depth, impl.a.view);
    impl.pass(encoder, impl.a.view, impl.march, march_group, effects, true);
    impl.pass(encoder, impl.b.view, impl.blur_x, blur_x_group, nullptr, false);
    impl.pass(encoder, impl.a.view, impl.blur_y, blur_y_group, nullptr, false);
    impl.pass(encoder, impl.full.view, impl.up, up_group, nullptr, false);
    for (auto* g : {march_group, blur_x_group, blur_y_group, up_group})
        if (g) wgpuBindGroupRelease(g);
    return impl.full.view;
}

} // namespace openrts::renderer
#endif
