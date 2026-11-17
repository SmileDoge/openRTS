// The block decoders (formats/block_compression.hpp) against a GPU's: blocks
// of every format (random bits; BC7 each of its 8 modes and 64 partitions)
// uploaded as they are, read back texel by texel through a compute shader,
// compared with the CPU's decoding. BC7 must match to the bit (the format
// fixes its arithmetic). BC1-BC5's levels between the ends are the vendor's:
// measured on an RTX 4070 Ti SUPER (D3D12), BC1's green up to 5 steps off
// the ideal (2a + b) / 3, BC4's weights floor(k * 64 / 7) and
// floor(k * 64 / 5) of 64, up to 7 steps; within 8 they are taken (a
// wrong bit layout is tens of steps off). Without a GPU (CI) it is skipped.

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include <openrts/formats/block_compression.hpp>
#include <openrts/renderer/webgpu_context.hpp>
#include <openrts/renderer/webgpu_texture.hpp>

#include <webgpu/webgpu.h>
#include <webgpu/wgpu.h>

namespace {

using namespace openrts;
using formats::BlockFormat;
int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

[[nodiscard]] WGPUStringView text(const char* value) {
    return {.data = value, .length = WGPU_STRLEN};
}

constexpr const char* shader_code = R"(
@group(0) @binding(0) var source: texture_2d<f32>;
@group(0) @binding(1) var<storage, read_write> texels: array<u32>;
@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) id: vec3<u32>) {
    let size = textureDimensions(source);
    if (id.x >= size.x || id.y >= size.y) {
        return;
    }
    texels[id.y * size.x + id.x] = pack4x8unorm(textureLoad(source, vec2<i32>(id.xy), 0));
}
)";

struct Gpu {
    WGPUInstance instance = nullptr;
    WGPUDevice device = nullptr;
    WGPUQueue queue = nullptr;
    WGPUShaderModule shader = nullptr;
    WGPUBindGroupLayout layout = nullptr;
    WGPUComputePipeline pipeline = nullptr;
};

// The GPU's texels of a level of blocks (RGBA8 row by row); empty on failure.
std::vector<std::uint8_t> gpu_decode(const Gpu& gpu, const BlockFormat format, const std::uint32_t width, const std::uint32_t height,
                                     const std::vector<std::uint8_t>& blocks) {
    const WGPUTextureFormat formats_by[6] = {WGPUTextureFormat_BC1RGBAUnorm, WGPUTextureFormat_BC2RGBAUnorm, WGPUTextureFormat_BC3RGBAUnorm,
                                             WGPUTextureFormat_BC4RUnorm,    WGPUTextureFormat_BC5RGUnorm,   WGPUTextureFormat_BC7RGBAUnorm};
    const auto upload = renderer::upload_compressed_texture(gpu.device, width, height, {blocks}, "bc check",
                                                            formats_by[static_cast<int>(format)], formats::block_bytes(format));
    if (!upload.ok()) {
        std::cerr << "upload: " << upload.error << '\n';
        return {};
    }
    const auto view = wgpuTextureCreateView(upload.texture.get(), nullptr);
    const std::uint64_t size = static_cast<std::uint64_t>(width) * height * 4;
    const WGPUBufferDescriptor storage_descriptor{.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc, .size = size};
    const auto storage = wgpuDeviceCreateBuffer(gpu.device, &storage_descriptor);
    const WGPUBufferDescriptor read_descriptor{.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst, .size = size};
    const auto read = wgpuDeviceCreateBuffer(gpu.device, &read_descriptor);
    std::array<WGPUBindGroupEntry, 2> entries{};
    entries[0].binding = 0;
    entries[0].textureView = view;
    entries[1].binding = 1;
    entries[1].buffer = storage;
    entries[1].size = size;
    const WGPUBindGroupDescriptor group_descriptor{.layout = gpu.layout, .entryCount = entries.size(), .entries = entries.data()};
    const auto group = wgpuDeviceCreateBindGroup(gpu.device, &group_descriptor);
    const auto encoder = wgpuDeviceCreateCommandEncoder(gpu.device, nullptr);
    const WGPUComputePassDescriptor pass_descriptor{};
    const auto pass = wgpuCommandEncoderBeginComputePass(encoder, &pass_descriptor);
    wgpuComputePassEncoderSetPipeline(pass, gpu.pipeline);
    wgpuComputePassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(pass, (width + 7) / 8, (height + 7) / 8, 1);
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);
    wgpuCommandEncoderCopyBufferToBuffer(encoder, storage, 0, read, 0, size);
    const auto commands = wgpuCommandEncoderFinish(encoder, nullptr);
    wgpuQueueSubmit(gpu.queue, 1, &commands);
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
    wgpuBufferMapAsync(read, WGPUMapMode_Read, 0, size, callback);
    for (int i = 0; i < 10000 && !done; ++i) {
        wgpuDevicePoll(gpu.device, true, nullptr);
        wgpuInstanceProcessEvents(gpu.instance);
    }
    std::vector<std::uint8_t> out;
    if (mapped) {
        out.resize(size);
        std::memcpy(out.data(), wgpuBufferGetConstMappedRange(read, 0, size), size);
        wgpuBufferUnmap(read);
    }
    wgpuBufferRelease(read);
    wgpuBufferRelease(storage);
    wgpuBindGroupRelease(group);
    wgpuTextureViewRelease(view);
    return out;
}

// Sets `count` bits of a block from bit `at` (low bit first).
void set_bits(std::uint8_t* block, const int at, const int count, const std::uint32_t value) {
    for (int i = 0; i < count; ++i) {
        const int bit = at + i;
        if ((value >> i) & 1u) block[bit >> 3] = static_cast<std::uint8_t>(block[bit >> 3] | (1u << (bit & 7)));
        else block[bit >> 3] = static_cast<std::uint8_t>(block[bit >> 3] & ~(1u << (bit & 7)));
    }
}

void compare(const Gpu& gpu, const BlockFormat format, const int tolerance) {
    const std::uint32_t width = 256, height = 128;
    const std::uint32_t bx = width / 4, by = height / 4;
    const auto bytes = formats::block_bytes(format);
    std::mt19937 random(1234 + static_cast<int>(format));
    std::vector<std::uint8_t> blocks(static_cast<std::size_t>(bx) * by * bytes);
    for (auto& b : blocks) b = static_cast<std::uint8_t>(random());
    if (format == BlockFormat::bc7) {
        // Each block a mode (j % 8) and, where it has them, a partition ((j / 8) % 64).
        constexpr int partition_bits[8] = {4, 6, 6, 6, 0, 0, 0, 6};
        for (std::uint32_t j = 0; j < bx * by; ++j) {
            auto* block = &blocks[static_cast<std::size_t>(j) * 16];
            const int mode = static_cast<int>(j % 8);
            set_bits(block, 0, mode + 1, 1u << mode);
            if (partition_bits[mode] > 0) set_bits(block, mode + 1, partition_bits[mode], (j / 8) % (1u << partition_bits[mode]));
        }
        // And a few with no mode (transparent black).
        for (std::uint32_t j = 0; j < 8; ++j) blocks[static_cast<std::size_t>(j) * 16 * 61] = 0;
    }
    const auto gpu_texels = gpu_decode(gpu, format, width, height, blocks);
    check(!gpu_texels.empty(), std::string(formats::block_format_name(format)) + ": the GPU read back");
    if (gpu_texels.empty()) return;
    const auto cpu = formats::decode_blocks(format, width, height, blocks.data(), blocks.size());
    std::size_t off = 0, told = 0;
    int worst = 0;
    std::array<int, 9> off_by_mode{};
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto at = (static_cast<std::size_t>(y) * width + x) * 4;
            int diff = 0;
            for (int ch = 0; ch < 4; ++ch) diff = (std::max)(diff, std::abs(int(cpu[at + ch]) - int(gpu_texels[at + ch])));
            worst = (std::max)(worst, diff);
            if (diff <= tolerance) continue;
            ++off;
            const auto j = (y / 4) * bx + x / 4;
            const int mode = format == BlockFormat::bc7 ? formats::bc7_mode(&blocks[static_cast<std::size_t>(j) * bytes]) : 0;
            ++off_by_mode[static_cast<std::size_t>(mode)];
            if (told++ < 6) {
                std::cerr << "  " << formats::block_format_name(format) << " block " << j << " (mode " << mode << ", partition "
                          << (j / 8) % 64 << ") texel " << (y % 4) * 4 + x % 4 << ": CPU " << int(cpu[at]) << ' ' << int(cpu[at + 1]) << ' '
                          << int(cpu[at + 2]) << ' ' << int(cpu[at + 3]) << ", GPU " << int(gpu_texels[at]) << ' ' << int(gpu_texels[at + 1])
                          << ' ' << int(gpu_texels[at + 2]) << ' ' << int(gpu_texels[at + 3]) << "; block";
                for (std::uint32_t b = 0; b < bytes; ++b) std::cerr << ' ' << int(blocks[static_cast<std::size_t>(j) * bytes + b]);
                std::cerr << '\n';
            }
        }
    }
    std::cout << "bc gpu: " << formats::block_format_name(format) << ": " << off << " of " << width * height << " texels off by more than "
              << tolerance << ", the most " << worst;
    if (format == BlockFormat::bc7) {
        std::cout << " (by mode:";
        for (int m = 0; m < 9; ++m) std::cout << ' ' << off_by_mode[static_cast<std::size_t>(m)];
        std::cout << ')';
    }
    std::cout << '\n';
    check(off == 0, std::string(formats::block_format_name(format)) + " decodes as the GPU does");
}

} // namespace

int main() {
    auto created = renderer::create_headless_wgpu_device();
    if (!created.ok()) {
        std::cout << "bc gpu: no GPU here (" << created.error << "), skipped\n";
        return 0;
    }
    Gpu gpu;
    gpu.instance = created.context.instance();
    gpu.device = created.context.device();
    if (!wgpuDeviceHasFeature(gpu.device, WGPUFeatureName_TextureCompressionBC)) {
        std::cout << "bc gpu: the GPU has no BC textures, skipped\n";
        return 0;
    }
    gpu.queue = wgpuDeviceGetQueue(gpu.device);
    const WGPUShaderSourceWGSL source{.chain = {.sType = WGPUSType_ShaderSourceWGSL}, .code = text(shader_code)};
    const WGPUShaderModuleDescriptor shader{.nextInChain = &source.chain};
    gpu.shader = wgpuDeviceCreateShaderModule(gpu.device, &shader);
    std::array<WGPUBindGroupLayoutEntry, 2> entries{};
    entries[0].binding = 0;
    entries[0].visibility = WGPUShaderStage_Compute;
    entries[0].texture.sampleType = WGPUTextureSampleType_Float;
    entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
    entries[1].binding = 1;
    entries[1].visibility = WGPUShaderStage_Compute;
    entries[1].buffer.type = WGPUBufferBindingType_Storage;
    const WGPUBindGroupLayoutDescriptor layout{.entryCount = entries.size(), .entries = entries.data()};
    gpu.layout = wgpuDeviceCreateBindGroupLayout(gpu.device, &layout);
    const WGPUPipelineLayoutDescriptor pipeline_layout_descriptor{.bindGroupLayoutCount = 1, .bindGroupLayouts = &gpu.layout};
    const auto pipeline_layout = wgpuDeviceCreatePipelineLayout(gpu.device, &pipeline_layout_descriptor);
    const WGPUComputePipelineDescriptor descriptor{.layout = pipeline_layout, .compute = {.module = gpu.shader, .entryPoint = text("main")}};
    gpu.pipeline = wgpuDeviceCreateComputePipeline(gpu.device, &descriptor);
    wgpuPipelineLayoutRelease(pipeline_layout);

    compare(gpu, BlockFormat::bc7, 0);
    compare(gpu, BlockFormat::bc1, 8);
    compare(gpu, BlockFormat::bc2, 8);
    compare(gpu, BlockFormat::bc3, 8);
    compare(gpu, BlockFormat::bc4, 8);
    compare(gpu, BlockFormat::bc5, 8);

    wgpuComputePipelineRelease(gpu.pipeline);
    wgpuBindGroupLayoutRelease(gpu.layout);
    wgpuShaderModuleRelease(gpu.shader);
    wgpuQueueRelease(gpu.queue);
    if (failures == 0) std::cout << "bc gpu: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
