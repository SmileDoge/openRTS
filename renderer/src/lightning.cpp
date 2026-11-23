#include <openrts/renderer/lightning.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

#include <glm/geometric.hpp>

#ifdef OPENRTS_HAS_WGPU
#include <openrts/renderer/depth.hpp>
#include "shader_code.hpp"
#endif

namespace openrts::renderer {
namespace {

// Hashes and a small generator of their own: the same numbers on every
// client and compiler (the standard distributions are not).
[[nodiscard]] std::uint64_t mix64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

[[nodiscard]] std::uint64_t key3(const std::int64_t a, const std::int64_t b, const std::int64_t c, const std::uint64_t salt) {
    return mix64(mix64(mix64(static_cast<std::uint64_t>(a) ^ salt) ^ static_cast<std::uint64_t>(b)) ^ static_cast<std::uint64_t>(c));
}

[[nodiscard]] double unit(const std::uint64_t h) {
    return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0);
}

struct Random {
    std::uint64_t state;
    float next() {
        state = mix64(state);
        return static_cast<float>(unit(state));
    }
    // About a normal deviate (three uniforms: -1.5..1.5, sigma 0.5), times 2.
    float spread() { return (next() + next() + next() - 1.5f) * 2.0f; }
};

// The storm's gathering: value noise on a 16 km lattice, smooth between its
// points, drifting 6 m/s to the north-east.
constexpr double gathering_lattice = 16000.0;
const glm::dvec2 gathering_drift{4.5, 4.0};

[[nodiscard]] double lattice_value(const std::int64_t x, const std::int64_t z) {
    return unit(key3(x, z, 0, 0x57A0E1ull));
}

} // namespace

float lightning_gathering(const glm::dvec2 at, const double time) {
    const auto q = (at - gathering_drift * time) / gathering_lattice;
    const double fx = std::floor(q.x), fz = std::floor(q.y);
    const auto ix = static_cast<std::int64_t>(fx), iz = static_cast<std::int64_t>(fz);
    const auto smooth = [](const double t) { return t * t * (3.0 - 2.0 * t); };
    const double u = smooth(q.x - fx), v = smooth(q.y - fz);
    const double n = (lattice_value(ix, iz) * (1.0 - u) + lattice_value(ix + 1, iz) * u) * (1.0 - v)
                     + (lattice_value(ix, iz + 1) * (1.0 - u) + lattice_value(ix + 1, iz + 1) * u) * v;
    // The noise squared is 0.296 on average: 0.2 + 2.7 n^2 averages 1.
    return static_cast<float>(0.2 + 2.7 * n * n);
}

std::vector<LightningStrike> lightning_strikes(const glm::dvec2 eye, const double time, const float rate, const float radius) {
    std::vector<LightningStrike> strikes;
    if (rate <= 0.0f || radius <= 0.0f) return strikes;
    // A cell's chance a slot: the rate spread over the cells within
    // lightning_radius, times the storm's gathering there.
    const double cells = 3.14159265358979 * lightning_radius * lightning_radius / (lightning_cell * lightning_cell);
    const double chance = static_cast<double>(rate) / 60.0 * lightning_slot / cells;
    const auto slot = static_cast<std::int64_t>(std::floor(time / lightning_slot));
    const auto reach = static_cast<std::int64_t>(std::ceil(radius / lightning_cell)) + 1;
    const auto cx0 = static_cast<std::int64_t>(std::floor(eye.x / lightning_cell));
    const auto cz0 = static_cast<std::int64_t>(std::floor(eye.y / lightning_cell));
    for (std::int64_t s = slot - 1; s <= slot; ++s) {
        for (std::int64_t cz = cz0 - reach; cz <= cz0 + reach; ++cz) {
            for (std::int64_t cx = cx0 - reach; cx <= cx0 + reach; ++cx) {
                const auto h = key3(cx, cz, s, 0x11A7ull);
                Random r{h};
                const double roll = r.next();
                const glm::dvec2 ground{(static_cast<double>(cx) + r.next()) * lightning_cell,
                                        (static_cast<double>(cz) + r.next()) * lightning_cell};
                const double start = (static_cast<double>(s) + r.next()) * lightning_slot;
                const double age = time - start;
                if (age < 0.0 || age >= lightning_duration) continue;
                if (glm::length(ground - eye) > static_cast<double>(radius)) continue;
                if (roll >= chance * lightning_gathering(ground, start)) continue;
                LightningStrike strike;
                strike.id = h;
                strike.ground = ground;
                strike.start = start;
                strike.to_ground = r.next() < 0.6f;
                strike.seed = static_cast<std::uint32_t>(mix64(h) >> 32);
                strike.strength = 0.5f + r.next();
                strikes.push_back(strike);
            }
        }
    }
    std::sort(strikes.begin(), strikes.end(), [](const LightningStrike& a, const LightningStrike& b) { return a.start > b.start; });
    return strikes;
}

float lightning_flash(const LightningStrike& strike, const double age) {
    if (age < 0.0 || age >= lightning_duration) return 0.0f;
    Random r{strike.seed * 0x9E3779B97F4A7C15ull + 1u};
    const auto a = static_cast<float>(age);
    float light = 0.0f;
    if (strike.to_ground) {
        // The stepped leader's faint glow, then the return strokes (1..4),
        // each rising in 3 ms and dying in a few tens; some go on glowing
        // (a continuing current) for a tenth of a second or two.
        float t = 0.02f + 0.03f * r.next();
        if (a < t) light = 0.05f * a / t;
        const float pick = r.next();
        const int strokes = pick < 0.2f ? 1 : pick < 0.5f ? 2 : pick < 0.8f ? 3 : 4;
        for (int i = 0; i < strokes; ++i) {
            const float peak = i == 0 ? 1.0f : 0.4f + 0.45f * r.next();
            const float decay = 0.018f + 0.022f * r.next();
            const bool current = r.next() < 0.3f;
            const float lasts = 0.08f + 0.12f * r.next();
            const float gap = 0.035f + 0.09f * r.next();
            const float d = a - t;
            if (d >= 0.0f) {
                constexpr float rise = 0.003f;
                light = (std::max)(light, peak * (d < rise ? d / rise : std::exp(-(d - rise) / decay)));
                if (current) light = (std::max)(light, 0.22f * peak * (d < lasts ? 1.0f : std::exp(-(d - lasts) / 0.05f)));
            }
            t += gap;
            if (t > static_cast<float>(lightning_duration) - 0.3f) break;
        }
    } else {
        // Within the cloud: 2..6 softer pulses over half a second or so.
        const int pulses = 2 + static_cast<int>(r.next() * 5.0f);
        float t = 0.0f;
        for (int i = 0; i < pulses; ++i) {
            const float peak = 0.3f + 0.45f * r.next();
            const float decay = 0.03f + 0.05f * r.next();
            const float gap = 0.04f + 0.14f * r.next();
            const float d = a - t;
            if (d >= 0.0f) {
                constexpr float rise = 0.01f;
                light = (std::max)(light, peak * (d < rise ? d / rise : std::exp(-(d - rise) / decay)));
            }
            t += gap;
            if (t > static_cast<float>(lightning_duration) - 0.3f) break;
        }
    }
    // Out by the strike's end.
    const float end = std::clamp((static_cast<float>(lightning_duration) - a) / 0.1f, 0.0f, 1.0f);
    return light * strike.strength * end;
}

float lightning_flash_seen(const LightningStrike& strike, const double age, const double span) {
    float light = lightning_flash(strike, age);
    if (span <= 0.0) return light;
    // Every 2 ms through the frame (at most 50 samples: 0.1 s).
    const int samples = (std::min)(50, static_cast<int>(std::ceil((std::min)(span, 0.1) / 0.002)));
    for (int k = 1; k <= samples; ++k) light = (std::max)(light, lightning_flash(strike, age - span * k / samples));
    return light;
}

std::vector<LightningSegment> lightning_channel(const LightningStrike& strike, const float height) {
    std::vector<LightningSegment> segments;
    if (!strike.to_ground || height <= 1.0f) return segments;
    Random r{static_cast<std::uint64_t>(strike.seed) * 0xD1B54A32D192ED03ull + 7u};
    // Midpoint displacement: each piece halved, its middle pushed aside by a
    // share of its length; `levels` times.
    const auto subdivide = [&r](std::vector<glm::vec3> path, const int levels, const float roughness) {
        for (int level = 0; level < levels; ++level) {
            std::vector<glm::vec3> finer;
            finer.reserve(path.size() * 2);
            for (std::size_t i = 0; i + 1 < path.size(); ++i) {
                const auto a = path[i], b = path[i + 1];
                const float length = glm::distance(a, b);
                const glm::vec3 aside{r.spread(), r.spread() * 0.4f, r.spread()};
                finer.push_back(a);
                finer.push_back((a + b) * 0.5f + aside * (length * roughness));
            }
            finer.push_back(path.back());
            path = std::move(finer);
        }
        return path;
    };
    // The top fades into the cloud over its last eighth.
    const auto fade = [height](const glm::vec3& p) { return std::clamp((height - p.y) / (0.12f * height), 0.0f, 1.0f); };
    const auto add = [&](const std::vector<glm::vec3>& path, const float width, const float brightness) {
        for (std::size_t i = 0; i + 1 < path.size(); ++i) {
            segments.push_back({path[i], path[i + 1], width, brightness * fade(path[i]), brightness * fade(path[i + 1])});
        }
    };
    const float lean = height * 0.25f;
    const glm::vec3 top{(r.next() - 0.5f) * 2.0f * lean, height, (r.next() - 0.5f) * 2.0f * lean};
    auto main = subdivide({top, glm::vec3{0.0f}}, 7, 0.13f);
    main.back() = glm::vec3{0.0f};
    add(main, 1.0f, 1.0f);
    const int branches = 3 + static_cast<int>(r.next() * 5.0f);
    for (int k = 0; k < branches; ++k) {
        const auto at = static_cast<std::size_t>((0.06f + 0.6f * r.next()) * static_cast<float>(main.size() - 1));
        const auto start = main[at];
        const glm::vec3 way = glm::normalize(glm::vec3{(r.next() - 0.5f) * 2.0f, -(0.5f + 0.8f * r.next()), (r.next() - 0.5f) * 2.0f});
        const float length = height * (0.06f + 0.22f * r.next());
        auto end = start + way * length;
        end.y = (std::max)(end.y, height * 0.04f);
        const float brightness = 0.2f + 0.3f * r.next();
        const auto branch = subdivide({start, end}, 5, 0.16f);
        add(branch, 0.55f, brightness);
        // Now and then a twig off the branch.
        if (r.next() < 0.5f && branch.size() > 8) {
            const auto from = branch[branch.size() / 3];
            const glm::vec3 twig_way = glm::normalize(glm::vec3{(r.next() - 0.5f) * 2.0f, -(0.4f + r.next()), (r.next() - 0.5f) * 2.0f});
            auto twig_end = from + twig_way * (length * (0.3f + 0.3f * r.next()));
            twig_end.y = (std::max)(twig_end.y, height * 0.03f);
            add(subdivide({from, twig_end}, 4, 0.18f), 0.4f, brightness * 0.6f);
        }
    }
    return segments;
}

#ifdef OPENRTS_HAS_WGPU
namespace {

struct FrameUniform {
    glm::mat4 view_projection{1.0f};
    float viewport[4]{};
};

[[nodiscard]] WGPUStringView wgsl_text(const char* value) {
    return {.data = value, .length = WGPU_STRLEN};
}

} // namespace

struct LightningRenderer::Impl {
    WGPUDevice device = nullptr;
    WGPUBindGroupLayout layout = nullptr;
    WGPUBindGroup group = nullptr;
    WGPUBuffer uniform = nullptr;
    WGPUBuffer strips_buffer = nullptr;
    std::uint64_t capacity = 0;
    WGPURenderPipeline pipeline = nullptr;
    WGPURenderPipeline bloom_pipeline = nullptr;
    std::vector<LightningStrip> strips;

    ~Impl() {
        if (group) wgpuBindGroupRelease(group);
        if (layout) wgpuBindGroupLayoutRelease(layout);
        if (uniform) wgpuBufferRelease(uniform);
        if (strips_buffer) wgpuBufferRelease(strips_buffer);
        if (pipeline) wgpuRenderPipelineRelease(pipeline);
        if (bloom_pipeline) wgpuRenderPipelineRelease(bloom_pipeline);
    }

    void write_frame(const glm::mat4& view_projection, const glm::vec2 viewport) const {
        FrameUniform frame;
        frame.view_projection = view_projection;
        frame.viewport[0] = viewport.x;
        frame.viewport[1] = viewport.y;
        frame.viewport[2] = frame_linear() ? 1.0f : 0.0f;
        const auto queue = wgpuDeviceGetQueue(device);
        wgpuQueueWriteBuffer(queue, uniform, 0, &frame, sizeof(frame));
        wgpuQueueRelease(queue);
    }

    void draw(const WGPURenderPassEncoder pass, const WGPURenderPipeline with, const glm::mat4& view_projection,
              const glm::vec2 viewport) const {
        if (!with || strips.empty() || !strips_buffer) return;
        write_frame(view_projection, viewport);
        wgpuRenderPassEncoderSetPipeline(pass, with);
        wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
        wgpuRenderPassEncoderSetVertexBuffer(pass, 0, strips_buffer, 0, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderDraw(pass, 6, static_cast<std::uint32_t>(strips.size()), 0, 0);
    }
};

LightningRenderer::LightningRenderer(const WGPUDevice device, const WGPUTextureFormat target_format) : impl_(std::make_unique<Impl>()) {
    auto& impl = *impl_;
    impl.device = device;
    WGPUBindGroupLayoutEntry entry{};
    entry.binding = 0;
    entry.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment; // the fragment: the frame's encoding
    entry.buffer.type = WGPUBufferBindingType_Uniform;
    entry.buffer.minBindingSize = sizeof(FrameUniform);
    const WGPUBindGroupLayoutDescriptor layout_descriptor{.entryCount = 1, .entries = &entry};
    impl.layout = wgpuDeviceCreateBindGroupLayout(device, &layout_descriptor);
    const WGPUBufferDescriptor uniform_descriptor{.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, .size = sizeof(FrameUniform)};
    impl.uniform = wgpuDeviceCreateBuffer(device, &uniform_descriptor);
    if (!impl.layout || !impl.uniform) return;
    const WGPUBindGroupEntry group_entry{.binding = 0, .buffer = impl.uniform, .offset = 0, .size = sizeof(FrameUniform)};
    const WGPUBindGroupDescriptor group_descriptor{.layout = impl.layout, .entryCount = 1, .entries = &group_entry};
    impl.group = wgpuDeviceCreateBindGroup(device, &group_descriptor);

    const WGPUShaderSourceWGSL source{.chain = {.sType = WGPUSType_ShaderSourceWGSL}, .code = wgsl_text(detail::shader_code("sky/lightning"))};
    const WGPUShaderModuleDescriptor shader_descriptor{.nextInChain = &source.chain, .label = wgsl_text("lightning")};
    const auto shader = wgpuDeviceCreateShaderModule(device, &shader_descriptor);
    const WGPUPipelineLayoutDescriptor pipeline_layout_descriptor{.bindGroupLayoutCount = 1, .bindGroupLayouts = &impl.layout};
    const auto pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &pipeline_layout_descriptor);
    const std::array<WGPUVertexAttribute, 4> attributes{{
        {.format = WGPUVertexFormat_Float32x4, .offset = offsetof(LightningStrip, a), .shaderLocation = 0},
        {.format = WGPUVertexFormat_Float32x4, .offset = offsetof(LightningStrip, b), .shaderLocation = 1},
        {.format = WGPUVertexFormat_Float32x4, .offset = offsetof(LightningStrip, light_a), .shaderLocation = 2},
        {.format = WGPUVertexFormat_Float32x4, .offset = offsetof(LightningStrip, light_b), .shaderLocation = 3},
    }};
    const WGPUVertexBufferLayout vertex_layout{
        .stepMode = WGPUVertexStepMode_Instance,
        .arrayStride = sizeof(LightningStrip),
        .attributeCount = attributes.size(),
        .attributes = attributes.data(),
    };
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
    const WGPUBlendState added{
        .color = {.operation = WGPUBlendOperation_Add, .srcFactor = WGPUBlendFactor_One, .dstFactor = WGPUBlendFactor_One},
        .alpha = {.operation = WGPUBlendOperation_Add, .srcFactor = WGPUBlendFactor_Zero, .dstFactor = WGPUBlendFactor_One},
    };
    for (const bool bloom : {false, true}) {
        const WGPUColorTargetState target{.format = bloom ? hdr_format : target_format, .blend = &added, .writeMask = WGPUColorWriteMask_All};
        const WGPUFragmentState fragment{.module = shader, .entryPoint = wgsl_text("fs_main"), .targetCount = 1, .targets = &target};
        const WGPURenderPipelineDescriptor descriptor{
            .layout = pipeline_layout,
            .vertex = {.module = shader, .entryPoint = wgsl_text("vs_main"), .bufferCount = 1, .buffers = &vertex_layout},
            .primitive = {.topology = WGPUPrimitiveTopology_TriangleList},
            .depthStencil = &depth,
            .multisample = bloom ? WGPUMultisampleState{.count = 1, .mask = ~0u} : scene_multisample(),
            .fragment = &fragment,
        };
        (bloom ? impl.bloom_pipeline : impl.pipeline) = wgpuDeviceCreateRenderPipeline(device, &descriptor);
    }
    wgpuPipelineLayoutRelease(pipeline_layout);
    wgpuShaderModuleRelease(shader);
}

LightningRenderer::~LightningRenderer() = default;

bool LightningRenderer::ready() const noexcept {
    return impl_ && impl_->pipeline && impl_->group;
}

bool LightningRenderer::empty() const noexcept {
    return !impl_ || impl_->strips.empty();
}

void LightningRenderer::set_strips(std::vector<LightningStrip> strips) {
    auto& impl = *impl_;
    impl.strips = std::move(strips);
    if (impl.strips.empty()) return;
    const auto bytes = static_cast<std::uint64_t>(impl.strips.size()) * sizeof(LightningStrip);
    if (bytes > impl.capacity) {
        if (impl.strips_buffer) wgpuBufferRelease(impl.strips_buffer);
        impl.capacity = bytes * 2;
        const WGPUBufferDescriptor descriptor{.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst, .size = impl.capacity};
        impl.strips_buffer = wgpuDeviceCreateBuffer(impl.device, &descriptor);
    }
    if (!impl.strips_buffer) return;
    const auto queue = wgpuDeviceGetQueue(impl.device);
    wgpuQueueWriteBuffer(queue, impl.strips_buffer, 0, impl.strips.data(), bytes);
    wgpuQueueRelease(queue);
}

void LightningRenderer::render(const WGPURenderPassEncoder pass, const glm::mat4& view_projection, const glm::vec2 viewport) {
    if (ready()) impl_->draw(pass, impl_->pipeline, view_projection, viewport);
}

void LightningRenderer::render_bloom(const WGPURenderPassEncoder pass, const glm::mat4& view_projection, const glm::vec2 viewport) {
    if (ready()) impl_->draw(pass, impl_->bloom_pipeline, view_projection, viewport);
}
#endif

} // namespace openrts::renderer
