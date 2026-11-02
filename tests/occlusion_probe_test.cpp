// renderer::OcclusionProbe (labels hidden behind the scene) and
// renderer::WideLineRenderer (the helper lines), single and multisampled, on a
// headless device: a depth target cleared to a known depth, two wide lines,
// then a point nearer than that depth (visible), one farther (hidden) and
// one only a little farther (within the 4 % pull: visible).
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include <glm/mat4x4.hpp>
#include <openrts/renderer/depth.hpp>
#include <openrts/renderer/occlusion_probe.hpp>
#include <openrts/renderer/webgpu_context.hpp>
#include <openrts/renderer/wide_lines.hpp>
#include <webgpu/wgpu.h>

namespace {

using namespace openrts;

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

std::vector<bool> run(const WGPUDevice device, const std::uint32_t samples, const float cleared) {
    renderer::render_sample_count() = samples;
    constexpr std::uint32_t size = 64;
    const WGPUTextureDescriptor depth_descriptor{
        .usage = WGPUTextureUsage_RenderAttachment,
        .dimension = WGPUTextureDimension_2D,
        .size = {.width = size, .height = size, .depthOrArrayLayers = 1},
        .format = renderer::depth_format,
        .mipLevelCount = 1,
        .sampleCount = samples,
    };
    const auto depth = wgpuDeviceCreateTexture(device, &depth_descriptor);
    const auto depth_view = wgpuTextureCreateView(depth, nullptr);
    auto colour_descriptor = depth_descriptor;
    colour_descriptor.format = WGPUTextureFormat_RGBA8Unorm;
    const auto colour = wgpuDeviceCreateTexture(device, &colour_descriptor);
    const auto colour_view = wgpuTextureCreateView(colour, nullptr);

    renderer::WideLineRenderer lines(device, WGPUTextureFormat_RGBA8Unorm);
    check(lines.ready(), "wide line pipeline");
    renderer::OcclusionProbe probe(device, WGPUTextureFormat_RGBA8Unorm);
    check(probe.ready(), "occlusion probe");
    probe.request({{.ndc = {-0.5f, 0.0f}, .depth = cleared * 2.0f},    // nearer
                   {.ndc = {0.0f, 0.0f}, .depth = cleared * 0.5f},     // farther
                   {.ndc = {0.5f, 0.0f}, .depth = cleared * 0.98f}},   // 2 % farther
                  11, {static_cast<float>(size), static_cast<float>(size)});

    const auto encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);
    const WGPURenderPassColorAttachment attachment{
        .view = colour_view,
        .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
        .loadOp = WGPULoadOp_Clear,
        .storeOp = WGPUStoreOp_Store,
        .clearValue = {0, 0, 0, 1},
    };
    const WGPURenderPassDepthStencilAttachment depth_attachment{
        .view = depth_view,
        .depthLoadOp = WGPULoadOp_Clear,
        .depthStoreOp = WGPUStoreOp_Discard,
        .depthClearValue = cleared,
    };
    const WGPURenderPassDescriptor pass_descriptor{
        .colorAttachmentCount = 1,
        .colorAttachments = &attachment,
        .depthStencilAttachment = &depth_attachment,
        .occlusionQuerySet = probe.query_set(),
    };
    const auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_descriptor);
    // Clip space directly (w = 1): z is the reversed-Z depth.
    const std::vector<renderer::WideLine> strips{
        {{-1.0f, -0.5f, 0.9f}, {1.0f, -0.5f, 0.9f}, 0xff0000ffu, 4.0f},
        {{-1.0f, 0.5f, 0.1f}, {1.0f, 0.5f, 0.1f}, 0xff00ff00u, 4.0f},
    };
    lines.render(pass, strips, glm::mat4{1.0f}, {static_cast<float>(size), static_cast<float>(size)});
    probe.draw(pass);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
    probe.resolve(encoder);
    const auto commands = wgpuCommandEncoderFinish(encoder, nullptr);
    const auto queue = wgpuDeviceGetQueue(device);
    wgpuQueueSubmit(queue, 1, &commands);
    probe.submitted();

    std::uint64_t tag = 0;
    std::vector<bool> visible;
    for (int i = 0; i < 100 && !probe.take(tag, visible); ++i) wgpuDevicePoll(device, true, nullptr);
    check(tag == 11, "the result carries its tag");

    wgpuQueueRelease(queue);
    wgpuCommandBufferRelease(commands);
    wgpuCommandEncoderRelease(encoder);
    wgpuTextureViewRelease(colour_view);
    wgpuTextureRelease(colour);
    wgpuTextureViewRelease(depth_view);
    wgpuTextureRelease(depth);
    return visible;
}

} // namespace

int main() {
    auto context = renderer::create_headless_wgpu_device();
    if (!context.ok()) {
        std::cout << "no WebGPU device (" << context.error << "): skipped\n";
        return 0;
    }
    const auto device = context.context.device();
    wgpuDevicePushErrorScope(device, WGPUErrorFilter_Validation);
    for (const std::uint32_t samples : {1u, 4u}) {
        const auto visible = run(device, samples, 0.25f);
        const auto n = std::to_string(samples);
        check(visible.size() == 3, "three results (" + n + " samples)");
        if (visible.size() == 3) {
            check(visible[0], "a point in front is visible (" + n + " samples)");
            check(!visible[1], "a point behind is hidden (" + n + " samples)");
            check(visible[2], "a point 2 % behind counts as in front (" + n + " samples)");
        }
    }
    const WGPUPopErrorScopeCallbackInfo pop{
        .mode = WGPUCallbackMode_AllowProcessEvents,
        .callback = [](const WGPUPopErrorScopeStatus, const WGPUErrorType type, const WGPUStringView message, void*, void*) {
            if (type == WGPUErrorType_NoError) return;
            std::cerr << "WebGPU validation: " << std::string(message.data ? message.data : "", message.data ? message.length : 0) << '\n';
            ++failures;
        },
    };
    wgpuDevicePopErrorScope(device, pop);
    wgpuDevicePoll(device, true, nullptr);
    wgpuInstanceProcessEvents(context.context.instance());
    renderer::render_sample_count() = 1;
    std::cout << (failures ? "FAILED" : "passed") << '\n';
    return failures ? 1 : 0;
}
