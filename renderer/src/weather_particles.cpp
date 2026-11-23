#include <openrts/renderer/weather_particles.hpp>

#ifdef OPENRTS_HAS_WGPU
#include <openrts/renderer/depth.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace openrts::renderer {

namespace {

struct Uniform {
    glm::mat4 view_projection;
    glm::mat4 view;
    float eye[4];      // xyz the eye, w the time
    float flow[4];     // xyz the wind (m/s, world), w 1: snow
    float eye_vel[4];  // xyz the camera's velocity, w the projection's y scale
    float light[4];    // rgb the light (display), a 1: the frame is linear
    float params[4];   // x the box's half width, y its half height, z the count, w the viewport's height
    glm::mat4 to_room;
    float room_lower[4]; // w 1: there is a room
    float room_upper[4];
    float origin[4];     // the origin's shift, within the box
};

constexpr char shader[] = R"(
struct Uniform {
    view_projection: mat4x4<f32>,
    view: mat4x4<f32>,
    eye: vec4<f32>,
    flow: vec4<f32>,
    eye_vel: vec4<f32>,
    light: vec4<f32>,
    params: vec4<f32>,
    to_room: mat4x4<f32>,
    room_lower: vec4<f32>,
    room_upper: vec4<f32>,
    origin: vec4<f32>,
};
@group(0) @binding(0) var<uniform> u: Uniform;

struct Out {
    @builtin(position) clip: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) alpha: f32,
    @location(2) @interpolate(flat) snow: f32,
};

fn hash(n: u32) -> vec4<f32> {
    var x = n * 747796405u + 2891336453u;
    var r: vec4<f32>;
    for (var k = 0u; k < 4u; k = k + 1u) {
        x = (x ^ (x >> 16u)) * 2246822519u;
        x = (x ^ (x >> 13u)) * 3266489917u;
        x = x ^ (x >> 16u);
        r[k] = f32(x & 0xFFFFFFu) / 16777216.0;
    }
    return r;
}

@vertex
fn vs_main(@builtin(vertex_index) vertex: u32) -> Out {
    var out: Out;
    let i = vertex / 6u;
    let corner = vertex % 6u;
    let h = hash(i);
    let h2 = hash(i + 7919u);
    let snow = u.flow.w > 0.5;
    out.snow = select(0.0, 1.0, snow);
    let t = u.eye.w;
    let half = vec3<f32>(u.params.x, u.params.y, u.params.x);
    let box = half * 2.0;
    // Its own fall: rain 7..9.5 m/s, snow 0.7..1.4 m/s tumbling.
    var v = u.flow.xyz;
    var drift = vec3<f32>(0.0);
    if (snow) {
        v.y = v.y - (0.7 + 0.7 * h.w);
        let ph = h2.xyz * 6.2831853;
        drift = vec3<f32>(sin(t * (0.9 + h2.w) + ph.x), 0.0, cos(t * (0.7 + h2.w * 0.8) + ph.y)) * (0.25 + 0.3 * h2.z);
    } else {
        v.y = v.y - (7.0 + 2.5 * h.w);
    }
    let raw = h.xyz * box + v * t + drift - u.origin.xyz;
    let p = u.eye.xyz + (fract((raw - u.eye.xyz) / box) - vec3<f32>(0.5)) * box;
    let rel = v - u.eye_vel.xyz;
    // Fading in near the eye and out at the box's edge.
    let away = length(p - u.eye.xyz);
    var fade = clamp((away - 0.35) / 0.6, 0.0, 1.0) * clamp((u.params.x - away) / (u.params.x * 0.35), 0.0, 1.0);
    // None inside the cab he is in (its glass stops them).
    if (u.room_lower.w > 0.5) {
        let r = (u.to_room * vec4<f32>(p, 1.0)).xyz;
        if (all(r > u.room_lower.xyz - vec3<f32>(0.05)) && all(r < u.room_upper.xyz + vec3<f32>(0.05))) {
            fade = 0.0;
        }
    }
    if (fade <= 0.0) {
        out.clip = vec4<f32>(2.0, 2.0, 2.0, 1.0);
        out.alpha = 0.0;
        return out;
    }

    let right = vec3<f32>(u.view[0][0], u.view[1][0], u.view[2][0]);
    let up = vec3<f32>(u.view[0][1], u.view[1][1], u.view[2][1]);
    var a: vec3<f32>;
    var b: vec3<f32>;
    var width: f32;
    if (snow) {
        let r = 0.012 + 0.016 * h2.y;
        // A little drawn out by the speed against it.
        let stretch = clamp(length(rel) * 0.004, 0.0, 0.05);
        let along = select(up, normalize(rel), length(rel) > 0.1);
        a = p + along * (r + stretch);
        b = p - along * (r + stretch);
        width = r * 2.0;
    } else {
        // A 1/40 s exposure, at most 3 m.
        var trail = rel * (1.0 / 40.0);
        let l = length(trail);
        if (l > 3.0) {
            trail = trail * (3.0 / l);
        }
        a = p;
        b = p - trail;
        width = 0.004 + 0.003 * h2.y; // wider than a drop: its blur
    }
    let ca = u.view_projection * vec4<f32>(a, 1.0);
    let cb = u.view_projection * vec4<f32>(b, 1.0);
    if (ca.w < 0.05 || cb.w < 0.05) {
        out.clip = vec4<f32>(2.0, 2.0, 2.0, 1.0);
        out.alpha = 0.0;
        return out;
    }
    let na = ca.xy / ca.w;
    let nb = cb.xy / cb.w;
    // The width in pixels (at least one, its light shrinking with it).
    let pixels_per_unit = u.eye_vel.w * u.params.w * 0.5;
    let wa = width * pixels_per_unit / ca.w;
    let wb = width * pixels_per_unit / cb.w;
    let wpx = max(max(wa, wb), 1.0);
    fade = fade * sqrt(min((wa + wb) * 0.5 / wpx, 1.0));
    var dir = nb - na;
    let aspect = u.params.z;
    var d2 = vec2<f32>(dir.x * aspect, dir.y);
    if (length(d2) < 1e-6) {
        d2 = vec2<f32>(0.0, 1.0);
    }
    let n2 = normalize(vec2<f32>(-d2.y, d2.x));
    let offset = vec2<f32>(n2.x / aspect, n2.y) * (wpx / u.params.w);
    // Corners: 0 a-, 1 a+, 2 b+, 3 a-, 4 b+, 5 b-.
    let at_b = corner == 2u || corner == 4u || corner == 5u;
    let side = select(-1.0, 1.0, corner == 1u || corner == 2u || corner == 4u);
    let c = select(ca, cb, at_b);
    let n = select(na, nb, at_b);
    out.clip = vec4<f32>((n + offset * side) * c.w, c.z, c.w);
    out.uv = vec2<f32>(side, select(-1.0, 1.0, at_b));
    out.alpha = fade;
    return out;
}

fn encode(colour: vec3<f32>) -> vec3<f32> {
    return select(colour, pow(max(colour, vec3<f32>(0.0)), vec3<f32>(2.2)), u.light.w > 0.5);
}

@fragment
fn fs_main(input: Out) -> @location(0) vec4<f32> {
    var a: f32;
    var colour: vec3<f32>;
    if (input.snow > 0.5) {
        // A soft disk.
        let r = length(input.uv);
        a = (1.0 - smoothstep(0.35, 1.0, r)) * 0.9 * input.alpha;
        colour = min(u.light.xyz * 1.1, vec3<f32>(1.0));
    } else {
        // A thin streak, soft across, its ends fading.
        let across = 1.0 - abs(input.uv.x);
        let along = 1.0 - abs(input.uv.y);
        a = across * smoothstep(0.0, 0.35, along) * 0.55 * input.alpha;
        colour = min(u.light.xyz * 1.1, vec3<f32>(1.0)); // the scene's light: dark at night
    }
    if (a <= 0.002) {
        discard;
    }
    return vec4<f32>(encode(colour) * a, a);
}
)";

WGPUStringView name(const char* text) {
    return {text, std::strlen(text)};
}

} // namespace

struct WeatherParticleRenderer::Impl {
    WGPUDevice device = nullptr;
    WGPUBindGroupLayout layout = nullptr;
    WGPUBuffer uniform = nullptr;
    WGPUBindGroup group = nullptr;
    WGPURenderPipeline pipeline = nullptr;
    ~Impl() {
        if (pipeline) wgpuRenderPipelineRelease(pipeline);
        if (group) wgpuBindGroupRelease(group);
        if (uniform) wgpuBufferRelease(uniform);
        if (layout) wgpuBindGroupLayoutRelease(layout);
    }
};

std::uint32_t WeatherParticleRenderer::count(const float amount, const bool snow) noexcept {
    const float n = (snow ? 40000.0f : 60000.0f) * std::clamp(amount, 0.0f, 2.0f);
    return static_cast<std::uint32_t>(n);
}

WeatherParticleRenderer::WeatherParticleRenderer(const WGPUDevice device, const WGPUTextureFormat target_format)
    : impl_(std::make_unique<Impl>()) {
    auto& impl = *impl_;
    impl.device = device;
    WGPUBindGroupLayoutEntry entry{};
    entry.binding = 0;
    entry.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    entry.buffer.type = WGPUBufferBindingType_Uniform;
    entry.buffer.minBindingSize = sizeof(Uniform);
    const WGPUBindGroupLayoutDescriptor layout_descriptor{.entryCount = 1, .entries = &entry};
    impl.layout = wgpuDeviceCreateBindGroupLayout(device, &layout_descriptor);
    const WGPUBufferDescriptor buffer{.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, .size = sizeof(Uniform)};
    impl.uniform = wgpuDeviceCreateBuffer(device, &buffer);
    WGPUBindGroupEntry bound{};
    bound.binding = 0;
    bound.buffer = impl.uniform;
    bound.size = sizeof(Uniform);
    const WGPUBindGroupDescriptor group{.layout = impl.layout, .entryCount = 1, .entries = &bound};
    impl.group = wgpuDeviceCreateBindGroup(device, &group);

    const WGPUShaderSourceWGSL source{.chain = {.sType = WGPUSType_ShaderSourceWGSL}, .code = name(shader)};
    const WGPUShaderModuleDescriptor shader_descriptor{.nextInChain = &source.chain};
    const auto module = wgpuDeviceCreateShaderModule(device, &shader_descriptor);
    const WGPUPipelineLayoutDescriptor pipeline_layout_descriptor{.bindGroupLayoutCount = 1, .bindGroupLayouts = &impl.layout};
    const auto pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &pipeline_layout_descriptor);
    constexpr WGPUStencilFaceState keep{
        .compare = WGPUCompareFunction_Always,
        .failOp = WGPUStencilOperation_Keep,
        .depthFailOp = WGPUStencilOperation_Keep,
        .passOp = WGPUStencilOperation_Keep,
    };
    const WGPUDepthStencilState depth{
        .format = depth_format,
        .depthWriteEnabled = WGPUOptionalBool_False,
        .depthCompare = depth_nearer_or_equal,
        .stencilFront = keep,
        .stencilBack = keep,
        .stencilReadMask = ~0u,
        .stencilWriteMask = ~0u,
    };
    const WGPUBlendState over{
        .color = {.operation = WGPUBlendOperation_Add, .srcFactor = WGPUBlendFactor_One, .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha},
        .alpha = {.operation = WGPUBlendOperation_Add, .srcFactor = WGPUBlendFactor_Zero, .dstFactor = WGPUBlendFactor_One},
    };
    const WGPUColorTargetState target{.format = target_format, .blend = &over, .writeMask = WGPUColorWriteMask_All};
    const WGPUFragmentState fragment{.module = module, .entryPoint = name("fs_main"), .targetCount = 1, .targets = &target};
    const WGPURenderPipelineDescriptor descriptor{
        .layout = pipeline_layout,
        .vertex = {.module = module, .entryPoint = name("vs_main")},
        .primitive = {.topology = WGPUPrimitiveTopology_TriangleList},
        .depthStencil = &depth,
        .multisample = scene_multisample(),
        .fragment = &fragment,
    };
    impl.pipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor);
    wgpuPipelineLayoutRelease(pipeline_layout);
    wgpuShaderModuleRelease(module);
}

WeatherParticleRenderer::~WeatherParticleRenderer() = default;

bool WeatherParticleRenderer::ready() const noexcept {
    return impl_->pipeline && impl_->group;
}

void WeatherParticleRenderer::render(const WGPURenderPassEncoder pass, const glm::mat4& projection, const glm::mat4& view,
                                     const WeatherParticles& weather) {
    auto& impl = *impl_;
    const auto n = count(weather.amount, weather.snow);
    if (!ready() || n == 0 || weather.viewport_height <= 0.0f) return;
    Uniform u{};
    u.view_projection = projection * view;
    u.view = view;
    u.eye[0] = weather.eye.x;
    u.eye[1] = weather.eye.y;
    u.eye[2] = weather.eye.z;
    u.eye[3] = weather.time;
    u.flow[0] = weather.wind.x;
    u.flow[1] = weather.wind.y;
    u.flow[2] = weather.wind.z;
    u.flow[3] = weather.snow ? 1.0f : 0.0f;
    u.eye_vel[0] = weather.eye_velocity.x;
    u.eye_vel[1] = weather.eye_velocity.y;
    u.eye_vel[2] = weather.eye_velocity.z;
    u.eye_vel[3] = projection[1][1];
    for (int k = 0; k < 3; ++k) u.light[k] = std::clamp(weather.light[k], 0.0f, 1.0f);
    u.light[3] = frame_linear() ? 1.0f : 0.0f;
    u.params[0] = weather.snow ? 18.0f : 22.0f;
    u.params[1] = weather.snow ? 10.0f : 14.0f;
    // The viewport's aspect (projection's x over y scale, inverted).
    u.params[2] = projection[0][0] != 0.0f ? projection[1][1] / projection[0][0] : 1.0f;
    u.params[3] = weather.viewport_height;
    u.to_room = weather.to_room;
    {
        const double box[3] = {2.0 * u.params[0], 2.0 * u.params[1], 2.0 * u.params[0]};
        for (int k = 0; k < 3; ++k) u.origin[k] = static_cast<float>(std::fmod(weather.origin_shift[k], box[k]));
    }
    if (weather.has_room) {
        for (int k = 0; k < 3; ++k) {
            u.room_lower[k] = weather.room_lower[k];
            u.room_upper[k] = weather.room_upper[k];
        }
        u.room_lower[3] = 1.0f;
    }
    const auto queue = wgpuDeviceGetQueue(impl.device);
    if (!queue) return;
    wgpuQueueWriteBuffer(queue, impl.uniform, 0, &u, sizeof(u));
    wgpuQueueRelease(queue);
    wgpuRenderPassEncoderSetPipeline(pass, impl.pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, impl.group, 0, nullptr);
    wgpuRenderPassEncoderDraw(pass, n * 6u, 1, 0, 0);
}

} // namespace openrts::renderer
#endif
