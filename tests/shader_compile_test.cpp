// Every scene shader of the renderer builds into its pipelines on a headless
// device: the model renderer (both shading paths, every pipeline variant), the
// terrain and the tone mapping pass, with and without MSAA and with the HDR
// frame. A WGSL error or a binding mismatch shows as a validation error here
// instead of a crash when the viewer starts.
#include <iostream>
#include <memory>
#include <string>

#include <openrts/core/vfs.hpp>
#include <openrts/renderer/depth.hpp>
#include <openrts/renderer/model_renderer.hpp>
#include <openrts/renderer/terrain_renderer.hpp>
#include <openrts/renderer/tone_map.hpp>
#include <openrts/renderer/webgpu_context.hpp>
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

} // namespace

int main() {
    auto context = renderer::create_headless_wgpu_device();
    if (!context.ok()) {
        std::cout << "no WebGPU device (" << context.error << "): skipped\n";
        return 0;
    }
    const auto device = context.context.device();
    const auto vfs = std::make_shared<const core::Vfs>();
    const auto route = core::VirtualPath::parse("ROUTES/none");
    wgpuDevicePushErrorScope(device, WGPUErrorFilter_Validation);
    for (const std::uint32_t samples : {1u, 4u}) {
        for (const bool hdr : {false, true}) {
            renderer::render_sample_count() = samples;
            renderer::scene_hdr() = hdr;
            const auto format = hdr ? renderer::hdr_format : WGPUTextureFormat_BGRA8Unorm;
            const auto what = std::to_string(samples) + " samples" + (hdr ? ", HDR" : "");
            const renderer::ModelRenderer models(device, format);
            check(models.ready(), "model renderer (" + what + ")");
            if (route) {
                const renderer::TerrainRenderer terrain(device, format, vfs, *route, {});
                check(terrain.ready(), "terrain renderer (" + what + ")");
            }
        }
    }
    const renderer::ToneMapper tone(device, WGPUTextureFormat_BGRA8Unorm);
    check(tone.ready(), "tone mapper");
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
    renderer::scene_hdr() = false;
    std::cout << (failures ? "FAILED" : "passed") << '\n';
    return failures ? 1 : 0;
}
