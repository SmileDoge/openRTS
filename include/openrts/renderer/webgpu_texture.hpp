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

class WgpuTexture {
public:
    WgpuTexture() = default;
    explicit WgpuTexture(WGPUTexture texture) noexcept : texture_(texture) {}
    ~WgpuTexture();

    WgpuTexture(const WgpuTexture&) = delete;
    WgpuTexture& operator=(const WgpuTexture&) = delete;
    WgpuTexture(WgpuTexture&& other) noexcept;
    WgpuTexture& operator=(WgpuTexture&& other) noexcept;

    [[nodiscard]] WGPUTexture get() const noexcept { return texture_; }
    [[nodiscard]] explicit operator bool() const noexcept { return texture_ != nullptr; }

private:
    WGPUTexture texture_ = nullptr;
};

struct WgpuTextureUploadResult {
    WgpuTexture texture;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return static_cast<bool>(texture); }
};

// Creates a sampled RGBA8Unorm 2D texture and uploads all pixels immediately,
// with the image's mipmap levels (the leading ones of the right size).
// The caller owns the WGPUDevice; returned texture owns its WGPUTexture handle.
[[nodiscard]] WgpuTextureUploadResult upload_rgba8_texture(
    WGPUDevice device, Rgba8ImageView image, std::string_view label = {});

} // namespace openrts::renderer
