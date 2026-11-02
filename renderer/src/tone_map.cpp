#include <openrts/renderer/tone_map.hpp>

#ifdef OPENRTS_HAS_WGPU
#include <array>

namespace openrts::renderer {
namespace {

constexpr const char* tone_shader = R"(
struct Settings {
    curve: u32,     // 0 none, 1 Khronos PBR Neutral, 2 AgX, 3 ACES, 4 day Khronos / night AgX
    exposure: f32,
    night: f32,
    night_exposure: f32,
    saturation: f32,
    pad0: f32,
    pad1: f32,
    pad2: f32,
};
@group(0) @binding(0) var scene: texture_2d<f32>;
@group(0) @binding(1) var<uniform> settings: Settings;

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> @builtin(position) vec4<f32> {
    let uv = vec2<f32>(f32((index << 1u) & 2u), f32(index & 2u));
    return vec4<f32>(uv * 2.0 - 1.0, 0.0, 1.0);
}

// AgX (Troy Sobotka), the 6th-order fit by Benjamin Wrensch: scene-linear
// Rec.709 in, display-encoded out.
fn agx_contrast(x: vec3<f32>) -> vec3<f32> {
    let x2 = x * x;
    let x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

fn agx(linear: vec3<f32>) -> vec3<f32> {
    let inset = mat3x3<f32>(vec3<f32>(0.842479062253094, 0.0423282422610123, 0.0423756549057051),
                            vec3<f32>(0.0784335999999992, 0.878468636469772, 0.0784336),
                            vec3<f32>(0.0792237451477643, 0.0791661274605434, 0.879142973793104));
    let outset = mat3x3<f32>(vec3<f32>(1.19687900512017, -0.0528968517574562, -0.0529716355144438),
                             vec3<f32>(-0.0980208811401368, 1.15190312990417, -0.0980434501171241),
                             vec3<f32>(-0.0990297440797205, -0.0989611768448433, 1.15107367264116));
    let min_ev = -12.47393;
    let max_ev = 4.026069;
    var v = inset * max(linear, vec3<f32>(1e-10));
    v = clamp(log2(v), vec3<f32>(min_ev), vec3<f32>(max_ev));
    v = (v - min_ev) / (max_ev - min_ev);
    var display = agx_contrast(v);
    // The "punchy" look (Wrensch): more contrast and saturation than base AgX,
    // which reads washed out on content painted for a plain display.
    display = pow(max(display, vec3<f32>(0.0)), vec3<f32>(1.35));
    let luma = dot(display, vec3<f32>(0.2126, 0.7152, 0.0722));
    display = luma + 1.4 * (display - luma);
    return clamp(outset * display, vec3<f32>(0.0), vec3<f32>(1.0));
}

// ACES, Stephen Hill's fit (RRT + sRGB ODT): scene-linear in, display-linear out.
fn aces(c: vec3<f32>) -> vec3<f32> {
    let input = mat3x3<f32>(vec3<f32>(0.59719, 0.07600, 0.02840),
                            vec3<f32>(0.35458, 0.90834, 0.13383),
                            vec3<f32>(0.04823, 0.01566, 0.83777));
    let output = mat3x3<f32>(vec3<f32>(1.60475, -0.10208, -0.00327),
                             vec3<f32>(-0.53108, 1.10813, -0.07276),
                             vec3<f32>(-0.07367, -0.00605, 1.07602));
    let v = input * c;
    let a = v * (v + 0.0245786) - 0.000090537;
    let b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return output * (a / b);
}

// Khronos PBR Neutral: scene-linear in, display-linear out.
fn khronos_neutral(c_in: vec3<f32>) -> vec3<f32> {
    let start_compression = 0.8 - 0.04;
    let desaturation = 0.15;
    var c = c_in;
    let x = min(c.r, min(c.g, c.b));
    let offset = select(0.04, x - 6.25 * x * x, x < 0.08);
    c = c - offset;
    let peak = max(c.r, max(c.g, c.b));
    if (peak < start_compression) {
        return c;
    }
    let d = 1.0 - start_compression;
    let new_peak = 1.0 - d * d / (peak + d - start_compression);
    c = c * (new_peak / peak);
    let g = 1.0 - 1.0 / (desaturation * (peak - new_peak) + 1.0);
    return mix(c, vec3<f32>(new_peak), g);
}

// The display-encoded picture through the chosen curve.
fn curved(encoded: vec3<f32>) -> vec3<f32> {
    if (settings.curve == 0u) {
        // The scene as drawn: the RTS look.
        return clamp(encoded, vec3<f32>(0.0), vec3<f32>(1.0));
    }
    let scene_linear = pow(max(encoded, vec3<f32>(0.0)), vec3<f32>(2.2));
    let linear = scene_linear * settings.exposure;
    if (settings.curve == 4u) {
        // Khronos keeps the day as authored; AgX, brighter, holds the night.
        let day = pow(clamp(khronos_neutral(linear), vec3<f32>(0.0), vec3<f32>(1.0)), vec3<f32>(1.0 / 2.2));
        if (settings.night <= 0.0) {
            return day;
        }
        let night = agx(scene_linear * settings.night_exposure);
        return mix(day, night, settings.night);
    }
    if (settings.curve == 1u) {
        let display = khronos_neutral(linear);
        return pow(clamp(display, vec3<f32>(0.0), vec3<f32>(1.0)), vec3<f32>(1.0 / 2.2));
    }
    if (settings.curve == 3u) {
        let display = aces(linear);
        return pow(clamp(display, vec3<f32>(0.0), vec3<f32>(1.0)), vec3<f32>(1.0 / 2.2));
    }
    return agx(linear);
}

@fragment
fn fs_main(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> {
    let encoded = textureLoad(scene, vec2<i32>(position.xy), 0).rgb;
    var display = curved(encoded);
    if (settings.saturation != 1.0) {
        // Saturation around the pixel's own luma, as a display adjusts it.
        let luma = dot(display, vec3<f32>(0.2126, 0.7152, 0.0722));
        display = clamp(vec3<f32>(luma) + (display - vec3<f32>(luma)) * settings.saturation, vec3<f32>(0.0), vec3<f32>(1.0));
    }
    return vec4<f32>(display, 1.0);
}
)";

[[nodiscard]] WGPUStringView wgsl_name(const char* name) {
    return {.data = name, .length = WGPU_STRLEN};
}

struct SettingsUniform {
    std::uint32_t curve{};
    float exposure = 1.0f;
    float night = 0.0f;
    float night_exposure = 2.0f;
    float saturation = 1.0f;
    float pad[3]{};
};

} // namespace

struct ToneMapper::Impl {
    WGPUDevice device = nullptr;
    WGPUBindGroupLayout layout = nullptr;
    WGPURenderPipeline pipeline = nullptr;
    WGPUBuffer settings = nullptr;

    ~Impl() {
        if (settings) wgpuBufferRelease(settings);
        if (pipeline) wgpuRenderPipelineRelease(pipeline);
        if (layout) wgpuBindGroupLayoutRelease(layout);
    }
};

ToneMapper::ToneMapper(const WGPUDevice device, const WGPUTextureFormat output_format) : impl_(std::make_unique<Impl>()) {
    auto& impl = *impl_;
    impl.device = device;
    std::array<WGPUBindGroupLayoutEntry, 2> entries{};
    entries[0].binding = 0;
    entries[0].visibility = WGPUShaderStage_Fragment;
    entries[0].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
    entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
    entries[1].binding = 1;
    entries[1].visibility = WGPUShaderStage_Fragment;
    entries[1].buffer.type = WGPUBufferBindingType_Uniform;
    entries[1].buffer.minBindingSize = sizeof(SettingsUniform);
    const WGPUBindGroupLayoutDescriptor layout_descriptor{.entryCount = entries.size(), .entries = entries.data()};
    impl.layout = wgpuDeviceCreateBindGroupLayout(device, &layout_descriptor);
    const WGPUBufferDescriptor buffer{.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, .size = sizeof(SettingsUniform)};
    impl.settings = wgpuDeviceCreateBuffer(device, &buffer);

    const WGPUShaderSourceWGSL source{.chain = {.sType = WGPUSType_ShaderSourceWGSL}, .code = wgsl_name(tone_shader)};
    const WGPUShaderModuleDescriptor shader_descriptor{.nextInChain = &source.chain};
    const auto shader = wgpuDeviceCreateShaderModule(device, &shader_descriptor);
    const WGPUPipelineLayoutDescriptor pipeline_layout_descriptor{.bindGroupLayoutCount = 1, .bindGroupLayouts = &impl.layout};
    const auto pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &pipeline_layout_descriptor);
    const WGPUColorTargetState target{.format = output_format, .writeMask = WGPUColorWriteMask_All};
    const WGPUFragmentState fragment{.module = shader, .entryPoint = wgsl_name("fs_main"), .targetCount = 1, .targets = &target};
    const WGPURenderPipelineDescriptor descriptor{
        .layout = pipeline_layout,
        .vertex = {.module = shader, .entryPoint = wgsl_name("vs_main")},
        .primitive = {.topology = WGPUPrimitiveTopology_TriangleList},
        .multisample = {.count = 1, .mask = ~0u},
        .fragment = &fragment,
    };
    impl.pipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor);
    wgpuPipelineLayoutRelease(pipeline_layout);
    wgpuShaderModuleRelease(shader);
}

ToneMapper::~ToneMapper() = default;

bool ToneMapper::ready() const noexcept { return impl_->pipeline && impl_->layout && impl_->settings; }

void ToneMapper::render(const WGPUCommandEncoder encoder, const WGPUTextureView scene, const WGPUTextureView output,
                        const ToneSettings& tone) {
    auto& impl = *impl_;
    if (!ready() || !scene || !output) return;
    const SettingsUniform settings{.curve = static_cast<std::uint32_t>(tone.curve),
                                   .exposure = tone.exposure,
                                   .night = tone.night,
                                   .night_exposure = tone.night_exposure,
                                   .saturation = tone.saturation};
    if (const auto queue = wgpuDeviceGetQueue(impl.device)) {
        wgpuQueueWriteBuffer(queue, impl.settings, 0, &settings, sizeof(settings));
        wgpuQueueRelease(queue);
    }
    std::array<WGPUBindGroupEntry, 2> entries{};
    entries[0].binding = 0;
    entries[0].textureView = scene;
    entries[1].binding = 1;
    entries[1].buffer = impl.settings;
    entries[1].size = sizeof(SettingsUniform);
    const WGPUBindGroupDescriptor group_descriptor{.layout = impl.layout, .entryCount = entries.size(), .entries = entries.data()};
    const auto group = wgpuDeviceCreateBindGroup(impl.device, &group_descriptor);
    const WGPURenderPassColorAttachment colour{
        .view = output,
        .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
        .loadOp = WGPULoadOp_Clear,
        .storeOp = WGPUStoreOp_Store,
        .clearValue = {0.0, 0.0, 0.0, 1.0},
    };
    const WGPURenderPassDescriptor pass_descriptor{.colorAttachmentCount = 1, .colorAttachments = &colour};
    const auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_descriptor);
    wgpuRenderPassEncoderSetPipeline(pass, impl.pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
    wgpuBindGroupRelease(group);
}

} // namespace openrts::renderer
#endif
