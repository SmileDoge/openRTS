// Every scene shader of the renderer builds into its pipelines on a headless
// device: the model renderer (both shading paths, every pipeline variant), the
// terrain and the tone mapping pass, with and without MSAA and with the HDR
// frame. A WGSL error or a binding mismatch shows as a validation error here
// instead of a crash when the viewer starts.
#include <iostream>
#include <utility>
#include <memory>
#include <string>
#include <vector>

#include <openrts/core/vfs.hpp>
#include <openrts/renderer/depth.hpp>
#include <openrts/renderer/depth_export.hpp>
#include <openrts/renderer/model_renderer.hpp>
#include <openrts/renderer/screen_quads.hpp>
#include <openrts/renderer/terrain_renderer.hpp>
#include <openrts/renderer/tone_map.hpp>
#include <openrts/renderer/webgpu_context.hpp>
#include <openrts/renderer/webgpu_texture.hpp>
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
    // The supersampled screens' textures: a one-texel yellow line on black
    // transparent texels keeps its colour down the mipmaps (premultiplied,
    // in linear light): half covered, half the alpha, still yellow, not
    // darkened by the black under the transparent texels.
    {
        std::vector<std::uint8_t> pixels(4 * 4 * 4, 0);
        for (std::size_t y = 0; y < 4; ++y) {
            auto* p = &pixels[(y * 4 + 1) * 4];
            p[0] = 255;
            p[1] = 220;
            p[2] = 0;
            p[3] = 255;
        }
        const auto levels = renderer::premultiplied_linear_levels(4, 4, pixels);
        check(levels.size() == 3, "levels 4x4, 2x2, 1x1");
        if (levels.size() == 3) {
            const auto* half = &levels[1][0]; // 2x2: its left column half yellow
            check(half[3] == 128 || half[3] == 127, "alpha halves (" + std::to_string(half[3]) + ")");
            // Premultiplied: linear yellow / 2, in sRGB about 186 for red; a
            // straight average with the black would give 128 x half alpha.
            check(half[0] > 180 && half[0] < 192, "red premultiplied in linear light (" + std::to_string(half[0]) + ")");
            check(levels[0][4 * 1 + 0] == 255 && levels[0][3] == 0, "level 0: the line kept, the black transparent");
        }
    }
    auto context = renderer::create_headless_wgpu_device();
    if (!context.ok()) {
        std::cout << "no WebGPU device (" << context.error << "): skipped\n";
        return 0;
    }
    const auto device = context.context.device();
    const auto vfs = std::make_shared<const core::Vfs>();
    const auto route = core::VirtualPath::parse("ROUTES/none");
    // 1 and 4 always; 2 and 8 where this device takes them (the menu's MSAA).
    std::vector<std::uint32_t> counts{1u, 4u};
    for (const std::uint32_t wanted : {2u, 8u}) {
        if (renderer::supported_sample_count(context.context, wanted) == wanted) counts.push_back(wanted);
    }
    std::cout << "sample counts:";
    for (const auto c : counts) std::cout << ' ' << c;
    std::cout << '\n';
    check(renderer::supported_sample_count(context.context, 1) == 1 && renderer::supported_sample_count(context.context, 4) == 4,
          "1 and 4 samples always");
    check(renderer::supported_sample_count(context.context, 3) == 4, "an odd count: 4");
    wgpuDevicePushErrorScope(device, WGPUErrorFilter_Validation);
    for (const std::uint32_t samples : counts) {
        for (const bool hdr : {false, true}) {
            renderer::render_sample_count() = samples;
            renderer::scene_hdr() = hdr;
            const auto format = hdr ? renderer::hdr_format : WGPUTextureFormat_BGRA8Unorm;
            const auto what = std::to_string(samples) + " samples" + (hdr ? ", HDR" : "");
            const renderer::ModelRenderer models(device, format);
            check(models.ready(), "model renderer (" + what + ")");
            // The cab screens, both paths: a screen with one blended
            // object drawn into its image (mipmaps too), then on the panel.
            {
                renderer::ScreenQuadRenderer screens(device, format);
                check(screens.ready(), "cab screen renderer (" + what + ")");
                const std::vector<std::uint8_t> pixels(16 * 16 * 4, 200);
                const auto plain = screens.add_texture({.width = 16, .height = 16, .pixels = &pixels});
                const auto premultiplied = screens.add_texture({.width = 16, .height = 16, .pixels = &pixels}, true);
                check(plain != 0 && premultiplied != 0, "cab screen textures (" + what + ")");
                std::vector<renderer::ScreenQuadVertex> vertices;
                for (const auto& [x, y] : {std::pair{0.1f, 0.1f}, {0.1f, 0.9f}, {0.9f, 0.9f}, {0.1f, 0.1f}, {0.9f, 0.9f}, {0.9f, 0.1f}})
                    vertices.push_back({.position = {x, -y, 1.0f}, .uv = {x, y}, .local = {x, y}});
                const WGPUTextureDescriptor target{
                    .usage = WGPUTextureUsage_RenderAttachment,
                    .dimension = WGPUTextureDimension_2D,
                    .size = {.width = 64, .height = 64, .depthOrArrayLayers = 1},
                    .format = format,
                    .mipLevelCount = 1,
                    .sampleCount = samples,
                };
                const auto colour = wgpuDeviceCreateTexture(device, &target);
                auto depth_target = target;
                depth_target.format = renderer::depth_format;
                const auto depth = wgpuDeviceCreateTexture(device, &depth_target);
                const auto colour_view = wgpuTextureCreateView(colour, nullptr);
                const auto depth_view = wgpuTextureCreateView(depth, nullptr);
                for (const bool supersampled : {true, false}) {
                    renderer::cab_screens_supersampled() = supersampled;
                    screens.set(vertices, {{.texture = supersampled ? premultiplied : plain, .blend = true, .first = 0, .count = 6, .surface = 0}},
                                {{.top_left = {0.0f, 0.0f, 1.0f}, .top_right = {1.0f, 0.0f, 1.0f}, .bottom_right = {1.0f, -1.0f, 1.0f},
                                  .width = 0.2f, .height = 0.15f}});
                    const auto encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);
                    screens.prepare(encoder);
                    WGPURenderPassColorAttachment colour_attachment{.view = colour_view,
                                                                    .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
                                                                    .loadOp = WGPULoadOp_Clear,
                                                                    .storeOp = WGPUStoreOp_Store};
                    WGPURenderPassDepthStencilAttachment depth_attachment{.view = depth_view,
                                                                          .depthLoadOp = WGPULoadOp_Clear,
                                                                          .depthStoreOp = WGPUStoreOp_Store,
                                                                          .depthClearValue = renderer::depth_clear_value};
                    const WGPURenderPassDescriptor pass_descriptor{.colorAttachmentCount = 1,
                                                                   .colorAttachments = &colour_attachment,
                                                                   .depthStencilAttachment = &depth_attachment};
                    const auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_descriptor);
                    screens.render(pass, glm::mat4{1.0f}, glm::vec3{0.0f, 0.0f, 0.0f});
                    wgpuRenderPassEncoderEnd(pass);
                    wgpuRenderPassEncoderRelease(pass);
                    const auto commands = wgpuCommandEncoderFinish(encoder, nullptr);
                    const auto queue = wgpuDeviceGetQueue(device);
                    wgpuQueueSubmit(queue, 1, &commands);
                    wgpuQueueRelease(queue);
                    wgpuCommandBufferRelease(commands);
                    wgpuCommandEncoderRelease(encoder);
                }
                renderer::cab_screens_supersampled() = true;
                wgpuTextureViewRelease(colour_view);
                wgpuTextureViewRelease(depth_view);
                wgpuTextureRelease(colour);
                wgpuTextureRelease(depth);
            }
            if (route) {
                const renderer::TerrainRenderer terrain(device, format, vfs, *route, {});
                check(terrain.ready(), "terrain renderer (" + what + ")");
            }
        }
    }
    const renderer::ToneMapper tone(device, WGPUTextureFormat_BGRA8Unorm);
    check(tone.ready(), "tone mapper");
    // DXT1 ACE as BC1: an 8x8 chain to 4x4, and a 12x12 one whose levels are
    // not whole blocks (6x6, 3x3: copied as their physical 8x8, 4x4).
    if (wgpuDeviceHasFeature(device, WGPUFeatureName_TextureCompressionBC)) {
        const std::vector<std::vector<std::uint8_t>> square{std::vector<std::uint8_t>(4 * 8, 0x5A), std::vector<std::uint8_t>(8, 0xA5)};
        check(renderer::upload_bc1_texture(device, 8, 8, square, "bc1 8x8").ok(), "BC1 texture 8x8 with its 4x4 level");
        const std::vector<std::vector<std::uint8_t>> odd{std::vector<std::uint8_t>(9 * 8, 1), std::vector<std::uint8_t>(4 * 8, 2),
                                                         std::vector<std::uint8_t>(8, 3)};
        check(renderer::upload_bc1_texture(device, 12, 12, odd, "bc1 12x12").ok(), "BC1 texture 12x12, levels 6x6 and 3x3");
        check(!renderer::upload_bc1_texture(device, 10, 10, odd, "bc1 10x10").ok(), "BC1 refused: level 0 not in whole blocks");
    } else {
        std::cout << "no BC textures on this device: the BC1 upload skipped\n";
    }
    // The depth copy for ReShade, with a draw: a 4x depth sampled into a single-sample one.
    {
        renderer::DepthExport depth_export(device);
        check(depth_export.ready(), "depth export");
        const WGPUTextureDescriptor descriptor{
            .usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding,
            .dimension = WGPUTextureDimension_2D,
            .size = {.width = 64, .height = 32, .depthOrArrayLayers = 1},
            .format = renderer::depth_format,
            .mipLevelCount = 1,
            .sampleCount = 4,
        };
        const auto depth = wgpuDeviceCreateTexture(device, &descriptor);
        const auto view = wgpuTextureCreateView(depth, nullptr);
        const auto encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);
        depth_export.record(encoder, view, 64, 32);
        check(depth_export.texture() != nullptr, "depth export texture");
        const auto commands = wgpuCommandEncoderFinish(encoder, nullptr);
        const auto queue = wgpuDeviceGetQueue(device);
        wgpuQueueSubmit(queue, 1, &commands);
        wgpuQueueRelease(queue);
        wgpuCommandBufferRelease(commands);
        wgpuCommandEncoderRelease(encoder);
        wgpuTextureViewRelease(view);
        wgpuTextureRelease(depth);
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
    renderer::scene_hdr() = false;
    std::cout << (failures ? "FAILED" : "passed") << '\n';
    return failures ? 1 : 0;
}
