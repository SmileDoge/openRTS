#pragma once

// PNG to and from RGBA8 (openrts-ace: ACE textures to PNG and back). Reads
// every standard PNG: grey, grey + alpha, RGB, RGBA, a palette (with tRNS),
// bit depths 1..16 (16 taken to 8), interlaced (Adam7) or not. Writes RGBA8,
// each row's filter the one that packs best.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openrts::formats {

struct PngImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels; // RGBA8, rows top to bottom
};

struct PngDecodeResult {
    std::optional<PngImage> image;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return image.has_value(); }
};

[[nodiscard]] PngDecodeResult decode_png(std::string_view bytes);
[[nodiscard]] PngDecodeResult decode_png_path(const std::filesystem::path& file);
// An RGBA8 image as PNG bytes (empty when the sizes do not match the pixels).
[[nodiscard]] std::string encode_png(std::uint32_t width, std::uint32_t height, const std::vector<std::uint8_t>& rgba);

} // namespace openrts::formats
