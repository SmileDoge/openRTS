#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openrts::formats {

enum class AceRepresentation { invalid, uncompressed, compressed };

[[nodiscard]] std::string_view to_string(AceRepresentation representation) noexcept;

// These fields remain raw: no renderer pixel format is inferred from them yet.
struct AceHeader {
    std::uint32_t version = 0;
    std::uint32_t options = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t format_code = 0;
    std::uint32_t channel_count = 0;
};

struct AceFileProbe {
    AceRepresentation representation = AceRepresentation::invalid;
    std::optional<std::uint32_t> declared_inflated_size;
    std::optional<AceHeader> header;
    std::string diagnostic;

    [[nodiscard]] bool valid() const noexcept { return representation != AceRepresentation::invalid; }
};

struct AceRgbaImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;
};

struct AceDecodeResult {
    std::optional<AceRgbaImage> image;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return image.has_value(); }
};

// Inspects only the envelope and fixed uncompressed header. Compressed ACE
// files are identified, but their payload is not decompressed or decoded.
[[nodiscard]] AceFileProbe probe_ace_file(std::string_view bytes);
[[nodiscard]] AceFileProbe probe_ace_file_path(const std::filesystem::path& file);

// Decodes mip level zero of an uncompressed, structured ACE into RGBA8.
// Raw (DXT/packed) and compressed ACE files deliberately return unsupported.
[[nodiscard]] AceDecodeResult decode_structured_ace_rgba(std::string_view bytes);
[[nodiscard]] AceDecodeResult decode_structured_ace_rgba_path(const std::filesystem::path& file);

} // namespace openrts::formats
