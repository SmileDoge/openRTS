#include <openrts/formats/shape_file.hpp>

#include <fstream>
#include <iterator>

#include <zlib.h>

namespace openrts::formats {
namespace {

[[nodiscard]] bool starts_with(const std::string_view value, const std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

[[nodiscard]] std::uint32_t read_u32_le(const std::string_view bytes, const std::size_t offset) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset]))
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 1])) << 8)
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 2])) << 16)
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 3])) << 24);
}

[[nodiscard]] bool starts_with_utf16le_ascii(const std::string_view bytes,
                                              const std::size_t offset,
                                              const std::string_view text) {
    if (bytes.size() < offset + text.size() * 2) return false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (bytes[offset + index * 2] != text[index] || bytes[offset + index * 2 + 1] != '\0') return false;
    }
    return true;
}

} // namespace

std::string_view to_string(const ShapeRepresentation representation) noexcept {
    switch (representation) {
    case ShapeRepresentation::text:
        return "text";
    case ShapeRepresentation::binary:
        return "binary";
    case ShapeRepresentation::compressed_binary:
        return "compressed-binary";
    case ShapeRepresentation::compressed_utf16:
        return "compressed-utf16";
    case ShapeRepresentation::invalid:
        return "invalid";
    }
    return "invalid";
}

ShapeFileProbe probe_shape_file(const std::string_view bytes) {
    // Narrow compressed SIMIS: 8-byte magic, little-endian inflated size,
    // four padding bytes, then zlib data beginning at offset 16.
    if (starts_with(bytes, "SIMISA@F")) {
        if (bytes.size() < 16) return {.diagnostic = "Truncated narrow compressed SIMIS envelope."};
        return {
            .representation = ShapeRepresentation::compressed_binary,
            .declared_inflated_size = read_u32_le(bytes, 8),
            .diagnostic = "Narrow compressed SIMIS shape; body begins with a zlib stream at byte 16.",
        };
    }

    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF
        && static_cast<unsigned char>(bytes[1]) == 0xFE) {
        if (starts_with_utf16le_ascii(bytes, 2, "SIMISA@F")) {
            if (bytes.size() < 34) return {.diagnostic = "Truncated UTF-16 compressed SIMIS envelope."};
            return {
                .representation = ShapeRepresentation::compressed_utf16,
                .declared_inflated_size = read_u32_le(bytes, 18),
                .diagnostic = "UTF-16LE compressed SIMIS shape; zlib body begins at byte 34.",
            };
        }
        if (starts_with_utf16le_ascii(bytes, 2, "SIMISA")) {
            return {
                .representation = ShapeRepresentation::text,
                .diagnostic = "UTF-16LE SIMIS text envelope.",
            };
        }
        return {.diagnostic = "Unrecognised UTF-16LE shape envelope."};
    }

    if (starts_with(bytes, "SIMISA@@@@@@@@@@JINX0s1b")) {
        return {
            .representation = ShapeRepresentation::binary,
            .diagnostic = "Uncompressed binary SIMIS shape envelope.",
        };
    }
    if (starts_with(bytes, "SIMISA@@@@@@@@@@JINX0s1t")) {
        return {
            .representation = ShapeRepresentation::text,
            .diagnostic = "Narrow text SIMIS shape envelope.",
        };
    }
    return {.diagnostic = "Unrecognised shape envelope; no body was interpreted."};
}

ShapeFileProbe probe_shape_file(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) return {.diagnostic = "Cannot open shape file: " + file.string()};
    const std::string bytes(std::istreambuf_iterator<char>(input), {});
    return probe_shape_file(std::string_view(bytes));
}

ShapeInflateResult inflate_narrow_compressed_shape(const std::string_view bytes) {
    const auto probe = probe_shape_file(bytes);
    if (probe.representation != ShapeRepresentation::compressed_binary) {
        return {.error = "Shape is not a narrow compressed binary SIMIS file."};
    }
    if (!probe.declared_inflated_size || *probe.declared_inflated_size == 0) {
        return {.error = "Compressed shape has no valid declared inflated size."};
    }

    std::string output(*probe.declared_inflated_size, '\0');
    uLongf output_size = static_cast<uLongf>(output.size());
    const auto status = uncompress(
        reinterpret_cast<Bytef*>(output.data()),
        &output_size,
        reinterpret_cast<const Bytef*>(bytes.data() + 16),
        static_cast<uLong>(bytes.size() - 16));
    if (status != Z_OK) {
        return {.error = "zlib failed to inflate shape body (status " + std::to_string(status) + ")."};
    }
    if (output_size != output.size()) {
        return {.error = "Inflated shape body size does not match the SIMIS envelope."};
    }
    return {.body = std::move(output)};
}

ShapeInflateResult inflate_narrow_compressed_shape(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) return {.error = "Cannot open shape file: " + file.string()};
    const std::string bytes(std::istreambuf_iterator<char>(input), {});
    return inflate_narrow_compressed_shape(std::string_view(bytes));
}

ShapeInflateResult load_binary_shape_body(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) return {.error = "Cannot open shape file: " + file.string()};
    const std::string bytes(std::istreambuf_iterator<char>(input), {});
    return load_binary_shape_body(std::string_view(bytes));
}

ShapeInflateResult load_binary_shape_body(const std::string_view bytes) {
    switch (probe_shape_file(bytes).representation) {
    case ShapeRepresentation::binary:
        return {.body = std::string(bytes.substr(16))};
    case ShapeRepresentation::compressed_binary:
        return inflate_narrow_compressed_shape(bytes);
    default:
        return {.error = "Unsupported shape representation; a narrow binary JINX0s1b shape is required."};
    }
}

ShapeInflateResult load_binary_shape_body(const core::Vfs& vfs, const core::VirtualPath& file) {
    std::string error;
    const auto bytes = vfs.read(file, &error);
    if (!bytes) return {.error = error};
    return load_binary_shape_body(std::string_view(*bytes));
}

} // namespace openrts::formats
