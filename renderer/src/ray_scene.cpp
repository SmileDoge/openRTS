#include "openrts/renderer/ray_scene.hpp"

#include "shader_code.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>
#include <memory>
#include <vector>

namespace openrts::renderer::detail {

const char* ray_shadow_code() {
#ifdef OPENRTS_HAS_RAY_QUERY
    if (ray_queries()) return shader_code("common/ray_shadow");
#endif
    return shader_code("common/ray_shadow_off");
}

#ifdef OPENRTS_HAS_RAY_QUERY
RayScene& RayScene::get() {
    static RayScene scene;
    return scene;
}

WGPUTlas RayScene::tlas(const WGPUDevice device) {
    if (!tlas_ && device && ray_queries()) {
        const WGPUTlasDescriptor descriptor{
            .label = {.data = "ray scene", .length = WGPU_STRLEN},
            .maxInstances = max_instances,
            .flags = WGPUAccelerationStructureFlag_PreferFastTrace,
            .updateMode = WGPUAccelerationStructureUpdateMode_Build,
        };
        tlas_ = wgpuDeviceCreateTlas(device, &descriptor);
        instances_ = new WGPUTlasInstance[max_instances];
    }
    if (tlas_) ensure_cut_outs(device);
    return tlas_;
}

void RayScene::ensure_cut_outs(const WGPUDevice device) {
    const std::lock_guard lock(mutex_);
    if (mask_texture_ || !device) return;
    const WGPUTextureDescriptor texture{
        .label = {.data = "ray cut-out masks", .length = WGPU_STRLEN},
        .usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst,
        .dimension = WGPUTextureDimension_2D,
        .size = {.width = mask_size, .height = mask_size, .depthOrArrayLayers = max_masks},
        .format = WGPUTextureFormat_R8Unorm,
        .mipLevelCount = 1,
        .sampleCount = 1,
    };
    mask_texture_ = wgpuDeviceCreateTexture(device, &texture);
    const WGPUTextureViewDescriptor view{
        .format = WGPUTextureFormat_R8Unorm,
        .dimension = WGPUTextureViewDimension_2DArray,
        .baseMipLevel = 0,
        .mipLevelCount = 1,
        .baseArrayLayer = 0,
        .arrayLayerCount = max_masks,
        .aspect = WGPUTextureAspect_All,
    };
    if (mask_texture_) mask_view_ = wgpuTextureCreateView(mask_texture_, &view);
    const WGPUBufferDescriptor uvs{.label = {.data = "ray cut-out uvs", .length = WGPU_STRLEN},
                                   .usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst,
                                   .size = static_cast<std::uint64_t>(max_uvs) * 8};
    uv_buffer_ = wgpuDeviceCreateBuffer(device, &uvs);
    const WGPUBufferDescriptor table{.label = {.data = "ray cut-out table", .length = WGPU_STRLEN},
                                     .usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst,
                                     .size = static_cast<std::uint64_t>(max_entries) * entry_words * 4};
    table_buffer_ = wgpuDeviceCreateBuffer(device, &table);
    // Entry 0: opaque (the terrain, models with no cut-out).
    const std::uint32_t opaque[entry_words] = {0u, opaque_layer, 0u, 0u};
    const auto queue = wgpuDeviceGetQueue(device);
    if (table_buffer_) wgpuQueueWriteBuffer(queue, table_buffer_, 0, opaque, sizeof(opaque));
    wgpuQueueRelease(queue);
    entry_count_ = 1;
}

std::vector<std::uint8_t> coverage_mask(const std::uint32_t width, const std::uint32_t height, const std::uint8_t* rgba,
                                        const std::uint8_t cutoff, const std::uint32_t size) {
    const auto count = static_cast<std::size_t>(size) * size;
    std::vector<std::uint8_t> mask(count, 0);
    if (!rgba || width == 0 || height == 0 || size == 0) return mask;
    // Each texel's footprint in the source (at least one source texel).
    const auto span = [](const std::uint32_t i, const std::uint32_t from, const std::uint32_t to) {
        auto first = static_cast<std::uint32_t>(static_cast<std::uint64_t>(i) * from / to);
        auto last = static_cast<std::uint32_t>(static_cast<std::uint64_t>(i + 1) * from / to);
        first = (std::min)(first, from - 1);
        last = (std::max)(last, first + 1);
        return std::pair{first, (std::min)(last, from)};
    };
    // The shares in 256ths, and how many texels each share has.
    std::vector<std::uint8_t> share(count);
    std::array<std::size_t, 256> histogram{};
    double solid_share = 0.0; // the source's, over the texels
    for (std::uint32_t y = 0; y < size; ++y) {
        const auto [y0, y1] = span(y, height, size);
        for (std::uint32_t x = 0; x < size; ++x) {
            const auto [x0, x1] = span(x, width, size);
            std::uint32_t solid = 0, all = 0;
            for (auto sy = y0; sy < y1; ++sy)
                for (auto sx = x0; sx < x1; ++sx) {
                    solid += rgba[(static_cast<std::size_t>(sy) * width + sx) * 4 + 3] > cutoff ? 1u : 0u;
                    ++all;
                }
            const double s = static_cast<double>(solid) / all;
            solid_share += s;
            const auto q = static_cast<std::uint8_t>((std::min)(std::lround(s * 255.0), 255L));
            share[static_cast<std::size_t>(y) * size + x] = q;
            ++histogram[q];
        }
    }
    // As many solid as the source has, the highest shares first (a share
    // 0 never: a texel with nothing solid under it stays open).
    const auto wanted = static_cast<std::size_t>(std::lround(solid_share));
    std::size_t taken = 0;
    int lowest = 256;
    while (lowest > 1 && taken < wanted) taken += histogram[static_cast<std::size_t>(--lowest)];
    for (std::size_t i = 0; i < count; ++i) mask[i] = share[i] >= lowest && share[i] > 0 ? 255 : 0;
    return mask;
}

std::optional<std::uint32_t> RayScene::mask_layer(const WGPUDevice device, const std::string& key, const std::uint8_t cutoff,
                                                  const std::uint32_t width, const std::uint32_t height, const std::uint8_t* rgba) {
    ensure_cut_outs(device);
    const std::lock_guard lock(mutex_);
    const auto name = key + '#' + std::to_string(cutoff);
    if (const auto it = mask_layers_.find(name); it != mask_layers_.end()) return it->second;
    if (!mask_texture_ || !rgba || width == 0 || height == 0 || mask_layers_.size() >= max_masks) return std::nullopt;
    const auto layer = static_cast<std::uint32_t>(mask_layers_.size());
    const auto alpha = coverage_mask(width, height, rgba, cutoff, mask_size);
    if (mask_pixels_.size() <= layer) mask_pixels_.resize(layer + 1);
    mask_pixels_[layer] = alpha;
    const WGPUTexelCopyTextureInfo target{.texture = mask_texture_, .origin = {.x = 0, .y = 0, .z = layer}};
    const WGPUTexelCopyBufferLayout layout{.bytesPerRow = mask_size, .rowsPerImage = mask_size};
    const WGPUExtent3D extent{.width = mask_size, .height = mask_size, .depthOrArrayLayers = 1};
    const auto queue = wgpuDeviceGetQueue(device);
    wgpuQueueWriteTexture(queue, &target, alpha.data(), alpha.size(), &layout, &extent);
    wgpuQueueRelease(queue);
    mask_layers_.emplace(name, layer);
    return layer;
}

std::optional<std::uint32_t> RayScene::alpha_layer(const WGPUDevice device, const std::string& key, const std::uint32_t width,
                                                   const std::uint32_t height, const std::uint8_t* rgba) {
    ensure_cut_outs(device);
    const std::lock_guard lock(mutex_);
    const auto name = key + "#alpha";
    if (const auto it = mask_layers_.find(name); it != mask_layers_.end()) return it->second;
    if (!mask_texture_ || !rgba || width == 0 || height == 0 || mask_layers_.size() >= max_masks) return std::nullopt;
    const auto layer = static_cast<std::uint32_t>(mask_layers_.size());
    std::vector<std::uint8_t> alpha(static_cast<std::size_t>(mask_size) * mask_size);
    for (std::uint32_t y = 0; y < mask_size; ++y) {
        const auto y0 = (std::min)(static_cast<std::uint32_t>(static_cast<std::uint64_t>(y) * height / mask_size), height - 1);
        const auto y1 = (std::max)(y0 + 1, (std::min)(static_cast<std::uint32_t>(static_cast<std::uint64_t>(y + 1) * height / mask_size), height));
        for (std::uint32_t x = 0; x < mask_size; ++x) {
            const auto x0 = (std::min)(static_cast<std::uint32_t>(static_cast<std::uint64_t>(x) * width / mask_size), width - 1);
            const auto x1 = (std::max)(x0 + 1, (std::min)(static_cast<std::uint32_t>(static_cast<std::uint64_t>(x + 1) * width / mask_size), width));
            std::uint32_t sum = 0, n = 0;
            for (auto sy = y0; sy < y1; ++sy)
                for (auto sx = x0; sx < x1; ++sx, ++n) sum += rgba[(static_cast<std::size_t>(sy) * width + sx) * 4 + 3];
            alpha[static_cast<std::size_t>(y) * mask_size + x] = static_cast<std::uint8_t>((sum + n / 2) / n);
        }
    }
    const WGPUTexelCopyTextureInfo target{.texture = mask_texture_, .origin = {.x = 0, .y = 0, .z = layer}};
    const WGPUTexelCopyBufferLayout layout{.bytesPerRow = mask_size, .rowsPerImage = mask_size};
    const WGPUExtent3D extent{.width = mask_size, .height = mask_size, .depthOrArrayLayers = 1};
    const auto queue = wgpuDeviceGetQueue(device);
    wgpuQueueWriteTexture(queue, &target, alpha.data(), alpha.size(), &layout, &extent);
    wgpuQueueRelease(queue);
    mask_layers_.emplace(name, layer);
    return layer;
}

std::vector<std::uint8_t> RayScene::mask_pixels(const std::uint32_t layer) {
    const std::lock_guard lock(mutex_);
    return layer < mask_pixels_.size() ? mask_pixels_[layer] : std::vector<std::uint8_t>{};
}

bool RayScene::opacity_micromaps(const WGPUDevice device) {
    if (opacity_micromaps_ < 0) {
        const char* off = std::getenv("OPENRTS_OMM");
        opacity_micromaps_ = device && !(off && off[0] == '0') && wgpuDeviceSupportsOpacityMicromaps(device) ? 1 : 0;
        std::printf("ray shadows: opacity micromaps %s\n", opacity_micromaps_ ? "on" : "off");
    }
    return opacity_micromaps_ == 1;
}

std::optional<std::uint32_t> RayScene::add_uvs(const WGPUDevice device, const float* uv, const std::size_t count) {
    ensure_cut_outs(device);
    const std::lock_guard lock(mutex_);
    if (!uv_buffer_ || count == 0 || uv_count_ + count > max_uvs) return std::nullopt;
    const auto first = uv_count_;
    const auto queue = wgpuDeviceGetQueue(device);
    wgpuQueueWriteBuffer(queue, uv_buffer_, static_cast<std::uint64_t>(first) * 8, uv, count * 8);
    wgpuQueueRelease(queue);
    uv_count_ += static_cast<std::uint32_t>(count);
    return first;
}

std::optional<std::uint32_t> RayScene::add_entries(const WGPUDevice device, const std::uint32_t* entries, const std::size_t count) {
    ensure_cut_outs(device);
    const std::lock_guard lock(mutex_);
    if (!table_buffer_ || count == 0 || entry_count_ + count > max_entries) return std::nullopt;
    const auto first = entry_count_;
    const auto queue = wgpuDeviceGetQueue(device);
    wgpuQueueWriteBuffer(queue, table_buffer_, static_cast<std::uint64_t>(first) * entry_words * 4, entries, count * entry_words * 4);
    wgpuQueueRelease(queue);
    entry_count_ += static_cast<std::uint32_t>(count);
    return first;
}

void RayScene::add(const WGPUBlas blas, const glm::mat4& m, const std::uint32_t custom) {
    if (!instances_ || !blas || count_ >= max_instances) return;
    auto& instance = instances_[count_++];
    instance.blas = blas;
    // Row-major 3x4: rows of the matrix's upper three.
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 4; ++column) instance.transform[row * 4 + column] = m[column][row];
    }
    instance.customData = custom & 0xFFFFFFu;
    instance.mask = 0xFF;
}

void RayScene::build(const WGPUDevice device, const WGPUBlasBuildEntry* blas, const std::size_t blas_count) {
    if (!tlas(device)) return;
    for (const auto& [_, adder] : adders_) adder(*this);
    const WGPUTlasBuildEntry entry{.tlas = tlas_, .instanceCount = count_, .instances = instances_};
    const auto encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);
    wgpuCommandEncoderBuildAccelerationStructures(encoder, blas_count, blas, 1, &entry);
    const auto commands = wgpuCommandEncoderFinish(encoder, nullptr);
    const auto queue = wgpuDeviceGetQueue(device);
    wgpuQueueSubmit(queue, 1, &commands);
    wgpuQueueRelease(queue);
    wgpuCommandBufferRelease(commands);
    wgpuCommandEncoderRelease(encoder);
    count_ = 0;
}

void RayScene::set_adder(const void* owner, std::function<void(RayScene&)> adder) { adders_[owner] = std::move(adder); }

void RayScene::remove_adder(const void* owner) { adders_.erase(owner); }

void RayScene::build_blas(const WGPUDevice device, const WGPUBlasBuildEntry& blas) {
    if (!device) return;
    const auto encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);
    wgpuCommandEncoderBuildAccelerationStructures(encoder, 1, &blas, 0, nullptr);
    const auto commands = wgpuCommandEncoderFinish(encoder, nullptr);
    const auto queue = wgpuDeviceGetQueue(device);
    wgpuQueueSubmit(queue, 1, &commands);
    wgpuQueueRelease(queue);
    wgpuCommandBufferRelease(commands);
    wgpuCommandEncoderRelease(encoder);
}
#endif

} // namespace openrts::renderer::detail
