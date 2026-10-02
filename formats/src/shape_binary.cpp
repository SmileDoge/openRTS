#include <openrts/formats/shape_binary.hpp>

#include <cstdint>
#include <optional>

namespace openrts::formats {
namespace {

constexpr std::uint32_t token_image = 13;
constexpr std::uint32_t token_images = 14;
constexpr std::uint32_t token_shape = 71;

struct Block {
    std::uint32_t token{};
    std::size_t payload{};
    std::size_t end{};
};

[[nodiscard]] std::optional<std::uint32_t> read_u32(std::string_view bytes, std::size_t& offset, std::size_t limit) {
    if (offset + 4 > limit) return std::nullopt;
    const auto value = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset]))
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 1])) << 8)
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 2])) << 16)
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 3])) << 24);
    offset += 4;
    return value;
}

[[nodiscard]] std::optional<Block> read_block(std::string_view bytes, std::size_t& offset, std::size_t limit) {
    const auto token = read_u32(bytes, offset, limit);
    const auto payload_bytes = read_u32(bytes, offset, limit);
    if (!token || !payload_bytes || *payload_bytes > limit - offset) return std::nullopt;
    const auto payload_start = offset;
    const auto end = payload_start + *payload_bytes;
    if (payload_start >= end) return std::nullopt;
    const auto label_units = static_cast<unsigned char>(bytes[offset++]);
    if (label_units > (end - offset) / 2) return std::nullopt;
    offset += static_cast<std::size_t>(label_units) * 2;
    return Block{.token = *token, .payload = offset, .end = end};
}

[[nodiscard]] std::optional<std::string> read_utf16le_string(std::string_view bytes, std::size_t& offset, std::size_t limit) {
    if (offset + 2 > limit) return std::nullopt;
    const auto units = static_cast<std::size_t>(static_cast<unsigned char>(bytes[offset])
                                                | (static_cast<unsigned char>(bytes[offset + 1]) << 8));
    offset += 2;
    if (units > (limit - offset) / 2) return std::nullopt;
    std::string value;
    value.reserve(units);
    for (std::size_t index = 0; index < units; ++index) {
        const auto unit = static_cast<unsigned char>(bytes[offset])
            | (static_cast<unsigned char>(bytes[offset + 1]) << 8);
        if (unit > 0x7F) return std::nullopt; // UTF-8 conversion is a later general string layer.
        value.push_back(static_cast<char>(unit));
        offset += 2;
    }
    return value;
}

[[nodiscard]] bool is_binary_subheader(std::string_view body) {
    return body.size() >= 16 && body.substr(0, 8) == "JINX0s1b";
}

} // namespace

ShapeImageTable extract_image_filenames_from_binary_shape(std::string_view body) {
    if (!is_binary_subheader(body)) return {.error = "Binary shape body has no JINX0s1b subheader."};
    std::size_t offset = 16;
    const auto root = read_block(body, offset, body.size());
    if (!root || root->token != token_shape) return {.error = "Binary shape has no bounded root shape block."};

    offset = root->payload;
    while (offset < root->end) {
        const auto child = read_block(body, offset, root->end);
        if (!child) return {.error = "Malformed root child block."};
        if (child->token != token_images) {
            offset = child->end;
            continue;
        }

        std::size_t table_offset = child->payload;
        const auto count = read_u32(body, table_offset, child->end);
        if (!count) return {.error = "Images table has no count."};
        ShapeImageTable result;
        for (std::uint32_t index = 0; index < *count; ++index) {
            const auto image = read_block(body, table_offset, child->end);
            if (!image || image->token != token_image) return {.error = "Images table has an invalid image entry."};
            std::size_t image_offset = image->payload;
            const auto filename = read_utf16le_string(body, image_offset, image->end);
            if (!filename || image_offset != image->end) return {.error = "Image entry has an invalid filename."};
            result.filenames.emplace_back(*filename);
            table_offset = image->end;
        }
        if (table_offset != child->end) return {.error = "Images table has trailing bytes."};
        return result;
    }
    return {.error = "Binary shape has no images table."};
}

} // namespace openrts::formats
