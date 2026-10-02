#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace openrts::formats {

enum class ShapeRepresentation {
    invalid,
    text,
    binary,
    compressed_binary,
    compressed_utf16,
};

[[nodiscard]] std::string_view to_string(ShapeRepresentation representation) noexcept;

struct ShapeFileProbe {
    ShapeRepresentation representation = ShapeRepresentation::invalid;
    std::optional<std::uint32_t> declared_inflated_size;
    std::string diagnostic;

    [[nodiscard]] bool valid() const noexcept { return representation != ShapeRepresentation::invalid; }
};

struct ShapeInflateResult {
    std::optional<std::string> body;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return body.has_value(); }
};

// Inspects the SIMIS envelope only. It never decompresses or interprets shape
// blocks, and therefore does not claim to enumerate texture dependencies.
[[nodiscard]] ShapeFileProbe probe_shape_file(std::string_view bytes);
[[nodiscard]] ShapeFileProbe probe_shape_file(const std::filesystem::path& file);

// Inflates the narrow compressed shape body after validating its declared
// output size. It does not interpret MSTS binary tokens.
[[nodiscard]] ShapeInflateResult inflate_narrow_compressed_shape(std::string_view bytes);
[[nodiscard]] ShapeInflateResult inflate_narrow_compressed_shape(const std::filesystem::path& file);

} // namespace openrts::formats
