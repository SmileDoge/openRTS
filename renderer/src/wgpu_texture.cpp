#include <openrts/renderer/webgpu_texture.hpp>

#include <algorithm>
#include <atomic>
#include <limits>

namespace openrts::renderer {
namespace {

[[nodiscard]] std::uint32_t align_to_256(const std::uint32_t value) {
    return (value + 255u) & ~255u;
}

std::atomic<std::uint64_t> live_bytes{0};

} // namespace

std::uint64_t uploaded_texture_bytes() noexcept {
    return live_bytes.load();
}

WgpuTexture::WgpuTexture(const WGPUTexture texture, const std::uint64_t bytes) noexcept : texture_(texture), bytes_(bytes) {
    live_bytes += bytes_;
}

WgpuTexture::~WgpuTexture() {
    live_bytes -= bytes_;
    if (texture_) wgpuTextureRelease(texture_);
}

WgpuTexture::WgpuTexture(WgpuTexture&& other) noexcept : texture_(other.texture_), bytes_(other.bytes_) {
    other.texture_ = nullptr;
    other.bytes_ = 0;
}

WgpuTexture& WgpuTexture::operator=(WgpuTexture&& other) noexcept {
    if (this == &other) return *this;
    live_bytes -= bytes_;
    if (texture_) wgpuTextureRelease(texture_);
    texture_ = other.texture_;
    bytes_ = other.bytes_;
    other.texture_ = nullptr;
    other.bytes_ = 0;
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
    std::uint64_t bytes = 0;
    for (const auto* level : levels) bytes += level->size();
    return {.texture = WgpuTexture(texture, bytes)};
}

WgpuTextureUploadResult upload_bc1_texture(const WGPUDevice device, const std::uint32_t width, const std::uint32_t height,
                                           const std::vector<std::vector<std::uint8_t>>& levels, const std::string_view label) {
    if (!device) return {.error = "Cannot upload a texture without a WebGPU device."};
    if (!wgpuDeviceHasFeature(device, WGPUFeatureName_TextureCompressionBC)) return {.error = "The device has no BC textures."};
    if (width == 0 || height == 0 || width % 4 != 0 || height % 4 != 0 || levels.empty())
        return {.error = "BC1 level 0 must be whole 4x4 blocks."};
    const auto blocks_of = [](const std::uint32_t side) { return (side + 3u) / 4u; };
    std::size_t count = 0;
    for (; count < levels.size(); ++count) {
        const auto w = (std::max)(width >> count, 1u);
        const auto h = (std::max)(height >> count, 1u);
        if (levels[count].size() != static_cast<std::size_t>(blocks_of(w)) * blocks_of(h) * 8) break;
        if (w == 1 && h == 1) {
            ++count;
            break;
        }
    }
    if (count == 0) return {.error = "BC1 level 0 has the wrong number of blocks."};
    const WGPUTextureDescriptor descriptor{
        .nextInChain = nullptr,
        .label = {.data = label.data(), .length = label.size()},
        .usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst,
        .dimension = WGPUTextureDimension_2D,
        .size = {.width = width, .height = height, .depthOrArrayLayers = 1},
        .format = WGPUTextureFormat_BC1RGBAUnorm,
        .mipLevelCount = static_cast<std::uint32_t>(count),
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
    for (std::size_t k = 0; k < count; ++k) {
        // A level's copy covers its whole blocks (its physical size).
        const auto bx = blocks_of((std::max)(width >> k, 1u));
        const auto by = blocks_of((std::max)(height >> k, 1u));
        const WGPUTexelCopyTextureInfo destination{
            .texture = texture,
            .mipLevel = static_cast<std::uint32_t>(k),
            .origin = {.x = 0, .y = 0, .z = 0},
            .aspect = WGPUTextureAspect_All,
        };
        const WGPUTexelCopyBufferLayout layout{.offset = 0, .bytesPerRow = bx * 8, .rowsPerImage = by};
        const WGPUExtent3D size{.width = bx * 4, .height = by * 4, .depthOrArrayLayers = 1};
        wgpuQueueWriteTexture(queue, &destination, levels[k].data(), levels[k].size(), &layout, &size);
    }
    wgpuQueueRelease(queue);
    std::uint64_t bytes = 0;
    for (std::size_t k = 0; k < count; ++k) bytes += levels[k].size();
    return {.texture = WgpuTexture(texture, bytes)};
}

} // namespace openrts::renderer
