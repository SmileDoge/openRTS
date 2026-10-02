#include <openrts/formats/ace_file.hpp>

#include <fstream>
#include <iterator>
#include <limits>

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

[[nodiscard]] std::uint64_t read_u64_le(const std::string_view bytes, const std::size_t offset) {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[offset + shift / 8])) << shift;
    }
    return value;
}

[[nodiscard]] bool range_fits(const std::size_t offset, const std::size_t size, const std::size_t total) {
    return offset <= total && size <= total - offset;
}

} // namespace

std::string_view to_string(const AceRepresentation representation) noexcept {
    switch (representation) {
    case AceRepresentation::uncompressed: return "uncompressed";
    case AceRepresentation::compressed: return "compressed";
    case AceRepresentation::invalid: return "invalid";
    }
    return "invalid";
}

AceFileProbe probe_ace_file(const std::string_view bytes) {
    if (starts_with(bytes, "SIMISA@F")) {
        if (bytes.size() < 16) return {.diagnostic = "Truncated compressed ACE envelope."};
        return {.representation = AceRepresentation::compressed,
                .declared_inflated_size = read_u32_le(bytes, 8),
                .diagnostic = "Narrow compressed SIMIS ACE; decoded header is not inspected yet."};
    }
    if (!starts_with(bytes, "SIMISA@@@@@@@@@@")) return {.diagnostic = "Unrecognised ACE envelope."};

    constexpr std::size_t header_offset = 16;
    constexpr std::size_t header_size = 24;
    if (bytes.size() < header_offset + header_size) return {.diagnostic = "Truncated uncompressed ACE header."};

    return {.representation = AceRepresentation::uncompressed,
            .header = AceHeader{.version = read_u32_le(bytes, header_offset),
                                .options = read_u32_le(bytes, header_offset + 4),
                                .width = read_u32_le(bytes, header_offset + 8),
                                .height = read_u32_le(bytes, header_offset + 12),
                                .format_code = read_u32_le(bytes, header_offset + 16),
                                .channel_count = read_u32_le(bytes, header_offset + 20)},
            .diagnostic = "Uncompressed ACE with a fixed header; pixel data was not interpreted."};
}

AceFileProbe probe_ace_file_path(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) return {.diagnostic = "Cannot open ACE file: " + file.string()};
    const std::string bytes(std::istreambuf_iterator<char>(input), {});
    return probe_ace_file(std::string_view(bytes));
}

AceDecodeResult decode_structured_ace_rgba(const std::string_view bytes) {
    constexpr std::uint32_t raw_data_flag = 0x10;
    constexpr std::uint64_t mask_channel = 2;
    constexpr std::uint64_t red_channel = 3;
    constexpr std::uint64_t green_channel = 4;
    constexpr std::uint64_t blue_channel = 5;
    constexpr std::uint64_t alpha_channel = 6;

    const auto probe = probe_ace_file(bytes);
    if (probe.representation != AceRepresentation::uncompressed || !probe.header)
        return {.error = "Only uncompressed ACE files can be decoded."};
    const auto& header = *probe.header;
    if ((header.options & raw_data_flag) != 0)
        return {.error = "Raw ACE pixel data (packed/DXT) is not implemented."};
    if (header.width == 0 || header.height == 0 || header.width > 16384 || header.height > 16384)
        return {.error = "ACE dimensions are invalid or exceed the decoder limit."};
    const auto pixel_count = static_cast<std::uint64_t>(header.width) * header.height;
    if (pixel_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max() / 4))
        return {.error = "ACE dimensions overflow the output buffer."};
    if (header.channel_count < 3 || header.channel_count > 5)
        return {.error = "Structured ACE has an unsupported channel count."};

    constexpr std::size_t channels_offset = 16 + 24 + 128;
    const std::size_t channels_size = static_cast<std::size_t>(header.channel_count) * 16;
    if (!range_fits(channels_offset, channels_size, bytes.size()))
        return {.error = "Truncated ACE channel definitions."};

    struct Channel { std::uint8_t bits; std::uint64_t type; };
    std::vector<Channel> channels;
    channels.reserve(header.channel_count);
    bool has_red = false;
    bool has_green = false;
    bool has_blue = false;
    for (std::uint32_t index = 0; index < header.channel_count; ++index) {
        const auto offset = channels_offset + static_cast<std::size_t>(index) * 16;
        const auto bits = read_u64_le(bytes, offset);
        const auto type = read_u64_le(bytes, offset + 8);
        if ((bits != 1 && bits != 8)
            || (type != mask_channel && type != red_channel && type != green_channel
                && type != blue_channel && type != alpha_channel))
            return {.error = "Structured ACE contains an unsupported channel definition."};
        has_red |= type == red_channel;
        has_green |= type == green_channel;
        has_blue |= type == blue_channel;
        channels.push_back({static_cast<std::uint8_t>(bits), type});
    }
    if (!has_red || !has_green || !has_blue)
        return {.error = "Structured ACE has no complete RGB channels."};

    const std::size_t scanlines_offset = channels_offset + channels_size;
    const std::size_t scanlines_size = static_cast<std::size_t>(header.height) * 4;
    if (!range_fits(scanlines_offset, scanlines_size, bytes.size()))
        return {.error = "Truncated ACE scanline offset table."};

    AceRgbaImage image{.width = header.width, .height = header.height,
        .pixels = std::vector<std::uint8_t>(static_cast<std::size_t>(pixel_count) * 4, 0xFF)};
    for (std::uint32_t y = 0; y < header.height; ++y) {
        const auto row_offset = read_u32_le(bytes, scanlines_offset + static_cast<std::size_t>(y) * 4);
        if (row_offset >= bytes.size()) return {.error = "ACE scanline offset points beyond file data."};
        std::size_t cursor = row_offset;
        for (const auto& channel : channels) {
            const auto plane_size = channel.bits == 8 ? static_cast<std::size_t>(header.width)
                                                       : (static_cast<std::size_t>(header.width) + 7) / 8;
            if (!range_fits(cursor, plane_size, bytes.size())) return {.error = "Truncated ACE channel plane."};
            for (std::uint32_t x = 0; x < header.width; ++x) {
                const auto value = channel.bits == 8
                    ? static_cast<std::uint8_t>(bytes[cursor + x])
                    : static_cast<std::uint8_t>(((static_cast<unsigned char>(bytes[cursor + x / 8])
                         >> (7 - (x % 8))) & 1) * 255);
                const auto rgba = (static_cast<std::size_t>(y) * header.width + x) * 4;
                if (channel.type == red_channel) image.pixels[rgba] = value;
                else if (channel.type == green_channel) image.pixels[rgba + 1] = value;
                else if (channel.type == blue_channel) image.pixels[rgba + 2] = value;
                else if (channel.type == alpha_channel || channel.type == mask_channel) image.pixels[rgba + 3] = value;
            }
            cursor += plane_size;
        }
    }
    return {.image = std::move(image)};
}

AceDecodeResult decode_structured_ace_rgba_path(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) return {.error = "Cannot open ACE file: " + file.string()};
    const std::string bytes(std::istreambuf_iterator<char>(input), {});
    return decode_structured_ace_rgba(std::string_view(bytes));
}

} // namespace openrts::formats
