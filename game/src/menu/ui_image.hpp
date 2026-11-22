#pragma once

// The shell's pictures (game/assets/menu/*.png) for ImGui: decoded with
// Windows Imaging Component and kept as wgpu textures while the menu lives.
// The ImGui WGPU backend's texture id is the texture's view.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

#include <webgpu/webgpu.h>

namespace openrts::game::menu {

class ImageCache {
public:
    ImageCache() = default;
    ~ImageCache();
    ImageCache(const ImageCache&) = delete;
    ImageCache& operator=(const ImageCache&) = delete;

    void set_device(WGPUDevice device);
    // Decoded once: the same path gives the same texture. Null when the file
    // or the decoder is not there.
    [[nodiscard]] WGPUTextureView load(const std::filesystem::path& file);
    [[nodiscard]] std::pair<int, int> size(const std::filesystem::path& file);
    void clear();

private:
    struct Image {
        WGPUTexture texture = nullptr;
        WGPUTextureView view = nullptr;
        int width = 0;
        int height = 0;
        ~Image();
        Image() = default;
        Image(const Image&) = delete;
        Image& operator=(const Image&) = delete;
    };
    [[nodiscard]] Image* find(const std::filesystem::path& file);
    WGPUDevice device_ = nullptr;
    std::unordered_map<std::string, std::unique_ptr<Image>> images_;
};

} // namespace openrts::game::menu
