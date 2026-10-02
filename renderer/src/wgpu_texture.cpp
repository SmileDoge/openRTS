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

    const auto source_row_bytes = static_cast<std::uint32_t>(source_row_bytes64);
    const auto upload_row_bytes = align_to_256(source_row_bytes);
    if (upload_row_bytes < source_row_bytes)
        return {.error = "RGBA8 row alignment overflow."};

    const std::uint8_t* upload_data = image.pixels->data();
    std::vector<std::uint8_t> padded_rows;
    if (upload_row_bytes != source_row_bytes) {
        padded_rows.assign(static_cast<std::size_t>(upload_row_bytes) * image.height, 0);
        for (std::uint32_t y = 0; y < image.height; ++y) {
            std::copy_n(image.pixels->data() + static_cast<std::size_t>(y) * source_row_bytes,
                        source_row_bytes,
                        padded_rows.data() + static_cast<std::size_t>(y) * upload_row_bytes);
        }
        upload_data = padded_rows.data();
    }

    const WGPUTextureDescriptor descriptor{
        .nextInChain = nullptr,
        .label = {.data = label.data(), .length = label.size()},
        .usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst,
        .dimension = WGPUTextureDimension_2D,
        .size = {.width = image.width, .height = image.height, .depthOrArrayLayers = 1},
        .format = WGPUTextureFormat_RGBA8Unorm,
        .mipLevelCount = 1,
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
    const WGPUTexelCopyTextureInfo destination{
        .texture = texture,
        .mipLevel = 0,
        .origin = {.x = 0, .y = 0, .z = 0},
        .aspect = WGPUTextureAspect_All,
    };
    const WGPUTexelCopyBufferLayout layout{
        .offset = 0,
        .bytesPerRow = upload_row_bytes,
        .rowsPerImage = image.height,
    };
    const WGPUExtent3D size{.width = image.width, .height = image.height, .depthOrArrayLayers = 1};
    wgpuQueueWriteTexture(queue, &destination, upload_data,
                          static_cast<std::size_t>(upload_row_bytes) * image.height, &layout, &size);
    wgpuQueueRelease(queue);
    return {.texture = WgpuTexture(texture)};
}

} // namespace openrts::renderer
