#include <openrts/renderer/webgpu_texture.hpp>

#include <algorithm>
#include <limits>

namespace openrts::renderer {
namespace {

[[nodiscard]] std::uint32_t align_to_256(const std::uint32_t value) {
    return (value + 255u) & ~255u;
}

} // namespace

WgpuTexture::~WgpuTexture() {
    if (texture_) wgpuTextureRelease(texture_);
}

WgpuTexture::WgpuTexture(WgpuTexture&& other) noexcept : texture_(other.texture_) {
    other.texture_ = nullptr;
}

WgpuTexture& WgpuTexture::operator=(WgpuTexture&& other) noexcept {
    if (this == &other) return *this;
    if (texture_) wgpuTextureRelease(texture_);
    texture_ = other.texture_;
    other.texture_ = nullptr;
    return *this;
}

WgpuTextureUploadResult upload_rgba8_texture(
    const WGPUDevice device, const Rgba8ImageView image, const std::string_view label) {
    if (!device) return {.error = "Cannot upload a texture without a WebGPU device."};
    if (!image.pixels || image.width == 0 || image.height == 0)
        return {.error = "RGBA8 image dimensions or pixels are invalid."};
    const auto source_row_bytes64 = static_cast<std::uint64_t>(image.width) * 4;
    const auto source_size64 = source_row_bytes64 * image.height;
    if (source_row_bytes64 > std::numeric_limits<std::uint32_t>::max()
        || source_size64 != image.pixels->size())
        return {.error = "RGBA8 image byte count does not match its dimensions."};

    // The mipmap levels that fit the chain; a wrong-sized one ends it.
    std::vector<const std::vector<std::uint8_t>*> levels{image.pixels};
    if (image.mips) {
        for (std::size_t k = 1; k <= image.mips->size(); ++k) {
            const auto w = (std::max)(image.width >> k, 1u);
            const auto h = (std::max)(image.height >> k, 1u);
            const auto& level = (*image.mips)[k - 1];
            if (level.size() != static_cast<std::size_t>(w) * h * 4) break;
            levels.push_back(&level);
            if (w == 1 && h == 1) break;
        }
    }

    const WGPUTextureDescriptor descriptor{
        .nextInChain = nullptr,
        .label = {.data = label.data(), .length = label.size()},
        .usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst,
        .dimension = WGPUTextureDimension_2D,
        .size = {.width = image.width, .height = image.height, .depthOrArrayLayers = 1},
        .format = WGPUTextureFormat_RGBA8Unorm,
        .mipLevelCount = static_cast<std::uint32_t>(levels.size()),
        .sampleCount = 1,
        .viewFormatCount = 0,
        .viewFormats = nullptr,
    };
    WGPUTexture texture = wgpuDeviceCreateTexture(device, &descriptor);
    if (!texture) return {.error = "wgpuDeviceCreateTexture returned null."};

    const WGPUQueue queue = wgpuDeviceGetQueue(device);
    if (!queue) {
        wgpuTextureRelease(texture);
        return {.error = "wgpuDeviceGetQueue returned null."};
    }
    std::vector<std::uint8_t> padded_rows;
    for (std::size_t k = 0; k < levels.size(); ++k) {
        const auto width = (std::max)(image.width >> k, 1u);
        const auto height = (std::max)(image.height >> k, 1u);
        const auto source_row_bytes = width * 4;
        const auto upload_row_bytes = align_to_256(source_row_bytes);
        const std::uint8_t* upload_data = levels[k]->data();
        if (upload_row_bytes != source_row_bytes) {
            padded_rows.assign(static_cast<std::size_t>(upload_row_bytes) * height, 0);
            for (std::uint32_t y = 0; y < height; ++y) {
                std::copy_n(levels[k]->data() + static_cast<std::size_t>(y) * source_row_bytes, source_row_bytes,
                            padded_rows.data() + static_cast<std::size_t>(y) * upload_row_bytes);
            }
            upload_data = padded_rows.data();
        }
        const WGPUTexelCopyTextureInfo destination{
            .texture = texture,
            .mipLevel = static_cast<std::uint32_t>(k),
            .origin = {.x = 0, .y = 0, .z = 0},
            .aspect = WGPUTextureAspect_All,
        };
        const WGPUTexelCopyBufferLayout layout{
            .offset = 0,
            .bytesPerRow = upload_row_bytes,
            .rowsPerImage = height,
        };
        const WGPUExtent3D size{.width = width, .height = height, .depthOrArrayLayers = 1};
        wgpuQueueWriteTexture(queue, &destination, upload_data, static_cast<std::size_t>(upload_row_bytes) * height, &layout,
                              &size);
    }
    wgpuQueueRelease(queue);
    return {.texture = WgpuTexture(texture)};
}

} // namespace openrts::renderer
