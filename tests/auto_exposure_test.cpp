// Auto exposure (PBR stage 3, renderer/auto_exposure.hpp) on a headless
// device: a frame of one known luminance measures as its log2, gives the
// factor the formula says (the key: 1; darker: up to the limit; brighter:
// down), adapts over time from one to another, faster towards light.
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <openrts/renderer/auto_exposure.hpp>
#include <openrts/renderer/webgpu_context.hpp>
#include <webgpu/wgpu.h>

using namespace openrts;

namespace {

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

// A 64 x 64 float frame of grey `luminance` (linear light).
WGPUTexture grey_frame(const WGPUDevice device, const float luminance) {
    const WGPUTextureDescriptor descriptor{
        .usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst,
        .dimension = WGPUTextureDimension_2D,
        .size = {.width = 64, .height = 64, .depthOrArrayLayers = 1},
        .format = WGPUTextureFormat_RGBA32Float,
        .mipLevelCount = 1,
        .sampleCount = 1,
    };
    const auto texture = wgpuDeviceCreateTexture(device, &descriptor);
    std::vector<float> texels(64 * 64 * 4);
    for (std::size_t i = 0; i < 64 * 64; ++i) {
        texels[i * 4 + 0] = texels[i * 4 + 1] = texels[i * 4 + 2] = luminance;
        texels[i * 4 + 3] = 1.0f;
    }
    const auto queue = wgpuDeviceGetQueue(device);
    const WGPUTexelCopyTextureInfo target{.texture = texture};
    const WGPUTexelCopyBufferLayout layout{.bytesPerRow = 64 * 16, .rowsPerImage = 64};
    const WGPUExtent3D extent{.width = 64, .height = 64, .depthOrArrayLayers = 1};
    wgpuQueueWriteTexture(queue, &target, texels.data(), texels.size() * sizeof(float), &layout, &extent);
    wgpuQueueRelease(queue);
    return texture;
}

renderer::AutoExposureState measure(const renderer::WgpuDeviceContextResult& context, renderer::AutoExposure& exposure, const WGPUTexture frame,
                                    const float elapsed, const renderer::AutoExposureSettings& settings) {
    const auto device = context.context.device();
    const auto view = wgpuTextureCreateView(frame, nullptr);
    const auto encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);
    exposure.measure(encoder, view, 64, 64, true, elapsed, settings);
    const auto commands = wgpuCommandEncoderFinish(encoder, nullptr);
    const auto queue = wgpuDeviceGetQueue(device);
    wgpuQueueSubmit(queue, 1, &commands);
    wgpuQueueRelease(queue);
    wgpuCommandBufferRelease(commands);
    wgpuCommandEncoderRelease(encoder);
    wgpuTextureViewRelease(view);
    return exposure.read_state(context.context.instance());
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
    renderer::AutoExposure exposure(device);
    check(exposure.ready(), "the auto exposure's pipelines");
    const renderer::AutoExposureSettings settings{.on = true, .strength = 0.7f, .down = 1.5f, .up = 1.0f, .speed = 1.0f};
    // At the key: measured as its log2, the factor 1.
    const auto at_key = grey_frame(device, std::exp2(renderer::auto_exposure_key));
    exposure.reset();
    auto s = measure(context, exposure, at_key, 0.0f, settings);
    check(s.valid && std::abs(s.measured - renderer::auto_exposure_key) < 0.2f,
          "a frame at the key measures as it (" + std::to_string(s.measured) + ")");
    check(std::abs(s.factor - 1.0f) < 0.1f, "at the key the factor is 1 (" + std::to_string(s.factor) + ")");
    // Four stops darker: 0.7 x 4 = 2.8 stops, up to the 1-stop limit: x 2.
    const auto dark = grey_frame(device, std::exp2(renderer::auto_exposure_key - 4.0f));
    exposure.reset();
    s = measure(context, exposure, dark, 0.0f, settings);
    check(std::abs(s.factor - 2.0f) < 0.05f, "a dark frame: brightened up to the limit, x 2 (" + std::to_string(s.factor) + ")");
    // One stop brighter: 0.7 stops down.
    const auto bright = grey_frame(device, std::exp2(renderer::auto_exposure_key + 1.0f));
    exposure.reset();
    s = measure(context, exposure, bright, 0.0f, settings);
    check(std::abs(s.factor - std::exp2(-0.7f)) < 0.05f, "a bright frame: 0.7 stops down (" + std::to_string(s.factor) + ")");
    // Adapting: from the dark frame to the key in 1 s takes 1 - e^-3 of the
    // way (towards light), from the key to the dark in 1 s 1 - e^-1.
    exposure.reset();
    measure(context, exposure, dark, 0.0f, settings);
    s = measure(context, exposure, at_key, 1.0f, settings);
    const float towards_light = (s.adapted - (renderer::auto_exposure_key - 4.0f)) / 4.0f;
    check(std::abs(towards_light - (1.0f - std::exp(-3.0f))) < 0.08f, "towards light 95 % in a second (" + std::to_string(towards_light) + ")");
    exposure.reset();
    measure(context, exposure, at_key, 0.0f, settings);
    s = measure(context, exposure, dark, 1.0f, settings);
    const float towards_dark = (renderer::auto_exposure_key - s.adapted) / 4.0f;
    check(std::abs(towards_dark - (1.0f - std::exp(-1.0f))) < 0.08f, "towards the dark 63 % in a second (" + std::to_string(towards_dark) + ")");
    for (const auto texture : {at_key, dark, bright}) wgpuTextureRelease(texture);

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
    std::cout << (failures ? "FAILED" : "auto exposure: all checks passed") << '\n';
    return failures ? 1 : 0;
}
