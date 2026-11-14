#pragma once

#ifndef OPENRTS_HAS_WGPU
#error "webgpu_texture.hpp requires OPENRTS_ENABLE_WGPU and an installed wgpu-native SDK."
#endif

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <webgpu/webgpu.h>

namespace openrts::renderer {

struct Rgba8ImageView {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    const std::vector<std::uint8_t>* pixels = nullptr;
    // Mipmap levels 1.. (level k: max(1, width >> k) x max(1, height >> k)),
    // e.g. formats::AceRgbaImage::mips; none: a single level.
    const std::vector<std::vector<std::uint8_t>>* mips = nullptr;
};

// The bytes of every texture uploaded through this file's functions that is
// alive (models, terrain, sky, sprites, cab screens).
[[nodiscard]] std::uint64_t uploaded_texture_bytes() noexcept;

class WgpuTexture {
public:
    WgpuTexture() = default;
    explicit WgpuTexture(WGPUTexture texture, std::uint64_t bytes = 0) noexcept;
    ~WgpuTexture();

    WgpuTexture(const WgpuTexture&) = delete;
    WgpuTexture& operator=(const WgpuTexture&) = delete;
    WgpuTexture(WgpuTexture&& other) noexcept;
    WgpuTexture& operator=(WgpuTexture&& other) noexcept;

    [[nodiscard]] WGPUTexture get() const noexcept { return texture_; }
    [[nodiscard]] explicit operator bool() const noexcept { return texture_ != nullptr; }

private:
    WGPUTexture texture_ = nullptr;
    std::uint64_t bytes_ = 0;
};

struct WgpuTextureUploadResult {
    WgpuTexture texture;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return static_cast<bool>(texture); }
};

// Creates a sampled RGBA8Unorm 2D texture and uploads all pixels immediately,
// with the image's mipmap levels (the leading ones of the right size).
// `format`: RGBA8Unorm, or RGBA8UnormSrgb for bytes that are sRGB-encoded
// (sampled as linear values, filtered in linear light).
// The caller owns the WGPUDevice; returned texture owns its WGPUTexture handle.
[[nodiscard]] WgpuTextureUploadResult upload_rgba8_texture(
    WGPUDevice device, Rgba8ImageView image, std::string_view label = {},
    WGPUTextureFormat format = WGPUTextureFormat_RGBA8Unorm);

// A BC1 (DXT1) texture from its levels' 8-byte blocks (level k:
// max(1, width >> k) x max(1, height >> k) texels, rows of ceil(w/4)
// blocks). Needs the device's TextureCompressionBC and level 0's sides in
// whole blocks; the chain may stop at any level.
[[nodiscard]] WgpuTextureUploadResult upload_bc1_texture(
    WGPUDevice device, std::uint32_t width, std::uint32_t height, const std::vector<std::vector<std::uint8_t>>& levels,
    std::string_view label = {});

} // namespace openrts::renderer
