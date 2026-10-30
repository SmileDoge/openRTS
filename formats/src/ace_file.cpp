#include <openrts/formats/ace_file.hpp>

#include <fstream>
#include <iterator>
#include <limits>

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

constexpr std::uint32_t ace_format_dxt1 = 0x12;
constexpr std::uint64_t ace_mask_channel = 2;

// Expands a 5:6:5 colour to 8 bits per channel.
void unpack_565(const std::uint16_t value, std::uint8_t (&rgb)[3]) {
    const auto r = (value >> 11) & 0x1F;
    const auto g = (value >> 5) & 0x3F;
    const auto b = value & 0x1F;
    rgb[0] = static_cast<std::uint8_t>((r << 3) | (r >> 2));
    rgb[1] = static_cast<std::uint8_t>((g << 2) | (g >> 4));
    rgb[2] = static_cast<std::uint8_t>((b << 3) | (b >> 2));
}

// Decodes one 8-byte DXT1 block into a 4x4 RGBA tile. With `punch_through`
// the fourth palette entry of three-colour blocks is transparent black.
void decode_dxt1_block(const std::string_view block, const bool punch_through, std::uint8_t (&tile)[16][4]) {
    const auto c0 = static_cast<std::uint16_t>(static_cast<unsigned char>(block[0]) | (static_cast<unsigned char>(block[1]) << 8));
    const auto c1 = static_cast<std::uint16_t>(static_cast<unsigned char>(block[2]) | (static_cast<unsigned char>(block[3]) << 8));
    const auto indices = read_u32_le(block, 4);

    std::uint8_t palette[4][4]{};
    std::uint8_t a[3];
    std::uint8_t b[3];
    unpack_565(c0, a);
    unpack_565(c1, b);
    for (int channel = 0; channel < 3; ++channel) {
        palette[0][channel] = a[channel];
        palette[1][channel] = b[channel];
        if (c0 > c1) {
            palette[2][channel] = static_cast<std::uint8_t>((2 * a[channel] + b[channel]) / 3);
            palette[3][channel] = static_cast<std::uint8_t>((a[channel] + 2 * b[channel]) / 3);
        } else {
            palette[2][channel] = static_cast<std::uint8_t>((a[channel] + b[channel]) / 2);
            palette[3][channel] = 0;
        }
    }
    palette[0][3] = palette[1][3] = palette[2][3] = 255;
    palette[3][3] = (c0 <= c1 && punch_through) ? 0 : 255;

    for (int texel = 0; texel < 16; ++texel) {
        const auto index = (indices >> (2 * texel)) & 3;
        for (int channel = 0; channel < 4; ++channel) tile[texel][channel] = palette[index][channel];
    }
}

// Raw ACE: a table of mip offsets follows the channel definitions; each mip is
// a u32 byte count followed by compressed blocks. Only DXT1 occurs in content.
[[nodiscard]] AceDecodeResult decode_raw_ace(const std::string_view payload,
                                              const std::size_t header_offset,
                                              const AceHeader& header,
                                              const bool has_mask_channel) {
    if (header.format_code != ace_format_dxt1) {
        return {.error = "Raw ACE format " + std::to_string(header.format_code) + " is not supported (only DXT1)."};
    }

    const auto table_offset = header_offset + 24 + 128 + static_cast<std::size_t>(header.channel_count) * 16;
    if (!range_fits(table_offset, 4, payload.size())) return {.error = "Truncated raw ACE mip table."};
    const auto mip_offset = header_offset + read_u32_le(payload, table_offset);
    if (!range_fits(mip_offset, 4, payload.size())) return {.error = "Raw ACE mip offset points beyond file data."};
    const auto mip_size = read_u32_le(payload, mip_offset);

    const auto blocks_x = (static_cast<std::size_t>(header.width) + 3) / 4;
    const auto blocks_y = (static_cast<std::size_t>(header.height) + 3) / 4;
    const auto expected = blocks_x * blocks_y * 8;
    if (mip_size < expected || !range_fits(mip_offset + 4, expected, payload.size())) {
        return {.error = "Truncated raw ACE DXT1 data."};
    }

    AceRgbaImage image{.width = header.width, .height = header.height,
        .pixels = std::vector<std::uint8_t>(static_cast<std::size_t>(header.width) * header.height * 4)};
    std::uint8_t tile[16][4];
    for (std::size_t by = 0; by < blocks_y; ++by) {
        for (std::size_t bx = 0; bx < blocks_x; ++bx) {
            decode_dxt1_block(payload.substr(mip_offset + 4 + (by * blocks_x + bx) * 8, 8), has_mask_channel, tile);
            for (std::size_t ty = 0; ty < 4; ++ty) {
                const auto y = by * 4 + ty;
                if (y >= header.height) break;
                for (std::size_t tx = 0; tx < 4; ++tx) {
                    const auto x = bx * 4 + tx;
                    if (x >= header.width) break;
                    const auto target = (y * header.width + x) * 4;
                    for (int channel = 0; channel < 4; ++channel) image.pixels[target + channel] = tile[ty * 4 + tx][channel];
                }
            }
        }
    }
    return {.image = std::move(image)};
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
    std::string inflated;
    std::string_view payload;
    std::size_t header_offset = 0;
    if (probe.representation == AceRepresentation::compressed) {
        if (!probe.declared_inflated_size || *probe.declared_inflated_size == 0)
            return {.error = "Compressed ACE has no valid declared inflated size."};
        inflated.resize(*probe.declared_inflated_size);
        uLongf inflated_size = static_cast<uLongf>(inflated.size());
        const auto status = uncompress(
            reinterpret_cast<Bytef*>(inflated.data()),
            &inflated_size,
            reinterpret_cast<const Bytef*>(bytes.data() + 16),
            static_cast<uLong>(bytes.size() - 16));
        if (status != Z_OK)
            return {.error = "zlib failed to inflate ACE body (status " + std::to_string(status) + ")."};
        if (inflated_size != inflated.size())
            return {.error = "Inflated ACE body size does not match the SIMIS envelope."};
        payload = inflated;
    } else if (probe.representation == AceRepresentation::uncompressed) {
        payload = bytes;
        header_offset = 16;
    } else {
        return {.error = "Unrecognised ACE envelope."};
    }

    constexpr std::size_t header_size = 24;
    if (!range_fits(header_offset, header_size, payload.size()))
        return {.error = "Truncated ACE header."};
    const AceHeader header{
        .version = read_u32_le(payload, header_offset),
        .options = read_u32_le(payload, header_offset + 4),
        .width = read_u32_le(payload, header_offset + 8),
        .height = read_u32_le(payload, header_offset + 12),
        .format_code = read_u32_le(payload, header_offset + 16),
        .channel_count = read_u32_le(payload, header_offset + 20),
    };
    if (header.width == 0 || header.height == 0 || header.width > 16384 || header.height > 16384)
        return {.error = "ACE dimensions are invalid or exceed the decoder limit."};
    if ((header.options & raw_data_flag) != 0) {
        bool has_mask = false;
        const std::size_t definitions = header_offset + header_size + 128;
        for (std::uint32_t index = 0; index < header.channel_count && index < 8; ++index) {
            const auto offset = definitions + static_cast<std::size_t>(index) * 16;
            if (range_fits(offset, 16, payload.size()) && read_u64_le(payload, offset + 8) == ace_mask_channel) has_mask = true;
        }
        return decode_raw_ace(payload, header_offset, header, has_mask);
    }
    const auto pixel_count = static_cast<std::uint64_t>(header.width) * header.height;
    if (pixel_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max() / 4))
        return {.error = "ACE dimensions overflow the output buffer."};
    if (header.channel_count < 3 || header.channel_count > 5)
        return {.error = "Structured ACE has an unsupported channel count."};

    const std::size_t channels_offset = header_offset + header_size + 128;
    const std::size_t channels_size = static_cast<std::size_t>(header.channel_count) * 16;
    if (!range_fits(channels_offset, channels_size, payload.size()))
        return {.error = "Truncated ACE channel definitions."};

    struct Channel { std::uint8_t bits; std::uint64_t type; };
    std::vector<Channel> channels;
    channels.reserve(header.channel_count);
    bool has_red = false;
    bool has_green = false;
    bool has_blue = false;
    for (std::uint32_t index = 0; index < header.channel_count; ++index) {
        const auto offset = channels_offset + static_cast<std::size_t>(index) * 16;
        const auto bits = read_u64_le(payload, offset);
        const auto type = read_u64_le(payload, offset + 8);
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
    if (!range_fits(scanlines_offset, scanlines_size, payload.size()))
        return {.error = "Truncated ACE scanline offset table."};

    AceRgbaImage image{.width = header.width, .height = header.height,
        .pixels = std::vector<std::uint8_t>(static_cast<std::size_t>(pixel_count) * 4, 0xFF)};
    for (std::uint32_t y = 0; y < header.height; ++y) {
        // Scanline offsets are relative to the ACE body, which in an
        // uncompressed file starts after the 16-byte SIMISA envelope.
        const auto row_offset = header_offset + read_u32_le(payload, scanlines_offset + static_cast<std::size_t>(y) * 4);
        if (row_offset >= payload.size()) return {.error = "ACE scanline offset points beyond file data."};
        std::size_t cursor = row_offset;
        for (const auto& channel : channels) {
            const auto plane_size = channel.bits == 8 ? static_cast<std::size_t>(header.width)
                                                       : (static_cast<std::size_t>(header.width) + 7) / 8;
            if (!range_fits(cursor, plane_size, payload.size())) return {.error = "Truncated ACE channel plane."};
            for (std::uint32_t x = 0; x < header.width; ++x) {
                const auto value = channel.bits == 8
                    ? static_cast<std::uint8_t>(payload[cursor + x])
                    : static_cast<std::uint8_t>(((static_cast<unsigned char>(payload[cursor + x / 8])
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

AceDecodeResult decode_ace_rgba(const core::Vfs& vfs, const core::VirtualPath& file) {
    std::string error;
    const auto bytes = vfs.read(file, &error);
    if (!bytes) return {.error = error};
    return decode_structured_ace_rgba(std::string_view(*bytes));
}

AceDecodeResult decode_structured_ace_rgba_path(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) return {.error = "Cannot open ACE file: " + file.string()};
    const std::string bytes(std::istreambuf_iterator<char>(input), {});
    return decode_structured_ace_rgba(std::string_view(bytes));
}

} // namespace openrts::formats
