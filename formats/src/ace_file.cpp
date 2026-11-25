#include <openrts/formats/ace_file.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>

#include <zlib.h>

namespace openrts::formats {
namespace {

// The see-through texels next to seen ones take their colour (openRTS,
// 2026-11-25): the filtering between them then does not bring in a colour
// no one sees (a fence's white planks got a red rim, the red under alpha 0).
void bleed_into_transparent(const std::uint32_t width, const std::uint32_t height, std::vector<std::uint8_t>& pixels) {
    if (width == 0 || height == 0 || pixels.size() != static_cast<std::size_t>(width) * height * 4) return;
    bool any = false;
    for (std::size_t i = 3; i < pixels.size() && !any; i += 4) any = pixels[i] == 0;
    if (!any) return;
    const auto source = pixels;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            auto* texel = &pixels[(static_cast<std::size_t>(y) * width + x) * 4];
            if (texel[3] != 0) continue;
            unsigned sum[3] = {0, 0, 0}, count = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const auto nx = static_cast<std::int64_t>(x) + dx, ny = static_cast<std::int64_t>(y) + dy;
                    if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
                    const auto* n = &source[(static_cast<std::size_t>(ny) * width + static_cast<std::size_t>(nx)) * 4];
                    if (n[3] == 0) continue;
                    for (int c = 0; c < 3; ++c) sum[c] += n[c];
                    ++count;
                }
            }
            if (count == 0) continue;
            for (int c = 0; c < 3; ++c) texel[c] = static_cast<std::uint8_t>((sum[c] + count / 2) / count);
        }
    }
}

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

// One DXT1 level's blocks as they are; nothing when truncated, or when a
// maskless texture's three-colour block uses its fourth colour (a GPU's BC1
// would make it transparent where the RGBA8 decode keeps it opaque black).
[[nodiscard]] std::optional<std::vector<std::uint8_t>> dxt1_level_blocks(const std::string_view payload, const std::size_t offset,
                                                                         const std::uint32_t width, const std::uint32_t height,
                                                                         const bool punch_through) {
    if (!range_fits(offset, 4, payload.size())) return std::nullopt;
    const auto size = read_u32_le(payload, offset);
    const auto expected = ((static_cast<std::size_t>(width) + 3) / 4) * ((static_cast<std::size_t>(height) + 3) / 4) * 8;
    if (size < expected || !range_fits(offset + 4, expected, payload.size())) return std::nullopt;
    const auto blocks = payload.substr(offset + 4, expected);
    if (!punch_through) {
        for (std::size_t b = 0; b < expected; b += 8) {
            const auto c0 = static_cast<unsigned char>(blocks[b]) | (static_cast<unsigned char>(blocks[b + 1]) << 8);
            const auto c1 = static_cast<unsigned char>(blocks[b + 2]) | (static_cast<unsigned char>(blocks[b + 3]) << 8);
            if (c0 > c1) continue;
            const auto indices = read_u32_le(blocks, b + 4);
            for (int texel = 0; texel < 16; ++texel)
                if (((indices >> (2 * texel)) & 3) == 3) return std::nullopt;
        }
    }
    return std::vector<std::uint8_t>(blocks.begin(), blocks.end());
}

// One DXT1 level at `offset` of the raw ACE payload (a u32 byte count, then
// the 8-byte blocks) as RGBA8.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> decode_dxt1_level(const std::string_view payload,
                                                                         const std::size_t offset,
                                                                         const std::uint32_t width,
                                                                         const std::uint32_t height,
                                                                         const bool punch_through) {
    if (!range_fits(offset, 4, payload.size())) return std::nullopt;
    const auto size = read_u32_le(payload, offset);
    const auto blocks_x = (static_cast<std::size_t>(width) + 3) / 4;
    const auto blocks_y = (static_cast<std::size_t>(height) + 3) / 4;
    const auto expected = blocks_x * blocks_y * 8;
    if (size < expected || !range_fits(offset + 4, expected, payload.size())) return std::nullopt;

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
    std::uint8_t tile[16][4];
    for (std::size_t by = 0; by < blocks_y; ++by) {
        for (std::size_t bx = 0; bx < blocks_x; ++bx) {
            decode_dxt1_block(payload.substr(offset + 4 + (by * blocks_x + bx) * 8, 8), punch_through, tile);
            for (std::size_t ty = 0; ty < 4; ++ty) {
                const auto y = by * 4 + ty;
                if (y >= height) break;
                for (std::size_t tx = 0; tx < 4; ++tx) {
                    const auto x = bx * 4 + tx;
                    if (x >= width) break;
                    const auto target = (y * width + x) * 4;
                    for (int channel = 0; channel < 4; ++channel) pixels[target + channel] = tile[ty * 4 + tx][channel];
                }
            }
        }
    }
    return pixels;
}

// Raw ACE: a table of mip offsets (one u32 per level) follows the channel
// definitions; each level is a u32 byte count followed by compressed blocks.
// Only DXT1 occurs in content. RTS uploads the file's levels when the options
// are 17 or 21 (0x44F554); a square texture's chain goes down to 1x1.
// openRTS's block formats (codes 0x20-0x24): every level, down to 1 x 1, a
// u32 byte count and its blocks; a chain when the options' bit 0 is set,
// any size. Decoded for the CPU, the blocks kept for the GPU.
[[nodiscard]] AceDecodeResult decode_block_ace(const std::string_view payload, const std::size_t header_offset, const AceHeader& header,
                                                const BlockFormat format) {
    const auto table_offset = header_offset + 24 + 128 + static_cast<std::size_t>(header.channel_count) * 16;
    std::uint32_t levels = 1;
    if ((header.options & 1u) != 0)
        for (auto side = (std::max)(header.width, header.height); side > 1; side /= 2) ++levels;
    if (!range_fits(table_offset, static_cast<std::size_t>(levels) * 4, payload.size())) return {.error = "Truncated ACE level table."};
    AceRgbaImage image{.width = header.width, .height = header.height};
    image.block_format = format;
    for (std::uint32_t k = 0; k < levels; ++k) {
        const auto w = (std::max)(header.width >> k, 1u), h = (std::max)(header.height >> k, 1u);
        const auto at = header_offset + read_u32_le(payload, table_offset + static_cast<std::size_t>(k) * 4);
        const auto size = level_block_bytes(format, w, h);
        if (!range_fits(at, 4, payload.size()) || read_u32_le(payload, at) != size || !range_fits(at + 4, size, payload.size()))
            return {.error = "ACE level " + std::to_string(k) + " is not " + std::string(block_format_name(format)) + " blocks of its size."};
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(payload.data() + at + 4);
        auto pixels = decode_blocks(format, w, h, bytes, size);
        if (k == 0) image.pixels = std::move(pixels);
        else image.mips.push_back(std::move(pixels));
        image.blocks.emplace_back(bytes, bytes + size);
    }
    if (levels > 1) {
        image.mips_from_file = true;
    } else {
        image.mips = build_rgba_mipmaps(image.width, image.height, image.pixels);
        image.blocks.clear(); // one level: the GPU's chain is made from the pixels
    }
    return {.image = std::move(image)};
}

[[nodiscard]] AceDecodeResult decode_raw_ace(const std::string_view payload,
                                              const std::size_t header_offset,
                                              const AceHeader& header,
                                              const bool has_mask_channel) {
    if (const auto block = ace_block_format(header.format_code)) return decode_block_ace(payload, header_offset, header, *block);
    if (header.format_code != ace_format_dxt1) {
        return {.error = "Raw ACE format " + std::to_string(header.format_code) + " is not supported (DXT1, openRTS's 0x20-0x24)."};
    }

    const auto table_offset = header_offset + 24 + 128 + static_cast<std::size_t>(header.channel_count) * 16;
    if (!range_fits(table_offset, 4, payload.size())) return {.error = "Truncated raw ACE mip table."};
    const auto level_offset = [&](const std::uint32_t level) -> std::optional<std::size_t> {
        const auto entry = table_offset + static_cast<std::size_t>(level) * 4;
        if (!range_fits(entry, 4, payload.size())) return std::nullopt;
        return header_offset + read_u32_le(payload, entry);
    };
    auto level0 = decode_dxt1_level(payload, *level_offset(0), header.width, header.height, has_mask_channel);
    if (!level0) return {.error = "Truncated raw ACE DXT1 data."};

    AceRgbaImage image{.width = header.width, .height = header.height, .pixels = std::move(*level0)};
    // The file's DXT1 levels go down to 4x4. Its 2x2 and 1x1 entries are not
    // DXT1 blocks (7carfloat.ace ends in one RGB texel), so those two levels
    // are built from the 4x4 one.
    if ((header.options == 17 || header.options == 21) && header.width == header.height && header.width >= 8) {
        std::vector<std::vector<std::uint8_t>> mips;
        bool complete = true;
        std::uint32_t level = 1;
        auto size = header.width / 2;
        for (; size >= 4 && complete; size /= 2, ++level) {
            const auto offset = level_offset(level);
            auto pixels = offset ? decode_dxt1_level(payload, *offset, size, size, has_mask_channel) : std::nullopt;
            if (pixels) mips.push_back(std::move(*pixels));
            else complete = false;
        }
        if (complete) {
            auto rest = build_rgba_mipmaps(size * 2, size * 2, mips.back());
            for (auto& each : rest) mips.push_back(std::move(each));
            image.mips = std::move(mips);
            image.mips_from_file = true;
            // The same levels' blocks for the GPU (down to 4x4).
            std::vector<std::vector<std::uint8_t>> bc1;
            for (std::uint32_t k = 0; k < level; ++k) {
                const auto side = (std::max)(header.width >> k, 1u);
                const auto offset = level_offset(k);
                auto blocks = offset ? dxt1_level_blocks(payload, *offset, side, side, has_mask_channel) : std::nullopt;
                if (!blocks) {
                    bc1.clear();
                    break;
                }
                bc1.push_back(std::move(*blocks));
            }
            image.bc1 = std::move(bc1);
        }
    }
    if (!image.mips_from_file) {
        bleed_into_transparent(image.width, image.height, image.pixels);
        image.mips = build_rgba_mipmaps(image.width, image.height, image.pixels);
    }
    return {.image = std::move(image)};
}

} // namespace

std::optional<BlockFormat> ace_block_format(const std::uint32_t format_code) noexcept {
    switch (format_code) {
    case ace_format_bc2: return BlockFormat::bc2;
    case ace_format_bc3: return BlockFormat::bc3;
    case ace_format_bc5: return BlockFormat::bc5;
    case ace_format_bc7: return BlockFormat::bc7;
    case ace_format_bc4: return BlockFormat::bc4;
    default: return std::nullopt;
    }
}

std::uint32_t ace_format_code(const BlockFormat format) noexcept {
    switch (format) {
    case BlockFormat::bc1: return ace_format_dxt1;
    case BlockFormat::bc2: return ace_format_bc2;
    case BlockFormat::bc3: return ace_format_bc3;
    case BlockFormat::bc4: return ace_format_bc4;
    case BlockFormat::bc5: return ace_format_bc5;
    case BlockFormat::bc7: return ace_format_bc7;
    }
    return 0;
}

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
    const auto row_bytes = [&](const std::uint32_t width) {
        std::size_t size = 0;
        for (const auto& channel : channels) {
            size += channel.bits == 8 ? static_cast<std::size_t>(width) : (static_cast<std::size_t>(width) + 7) / 8;
        }
        return size;
    };
    // The levels the file holds (options bit 0: a mipmap chain, its scanline
    // table one entry per row of every level) and the ones RTS reads: all of
    // a square texture's with options exactly 5, else level 0 (0x44F554).
    struct Level {
        std::uint32_t width;
        std::uint32_t height;
    };
    std::vector<Level> levels{{header.width, header.height}};
    if ((header.options & 1u) != 0) {
        for (auto w = header.width, h = header.height; h > 1;) {
            w = (std::max)(w / 2, 1u);
            h /= 2;
            levels.push_back({w, h});
        }
    }
    const bool file_mips = header.options == 5 && header.width == header.height && levels.size() > 1;
    const std::size_t read_levels = file_mips ? levels.size() : 1;
    std::size_t entries = 0;
    for (const auto& level : levels) entries += level.height;
    if (!range_fits(scanlines_offset, static_cast<std::size_t>(header.height) * 4, payload.size()))
        return {.error = "Truncated ACE scanline offset table."};
    if (file_mips && !range_fits(scanlines_offset, entries * 4, payload.size()))
        return {.error = "Truncated ACE scanline offset table."};

    // Some tools write a broken scanline table (Pavlovsk's terrain.ace: a
    // 6144-byte row step for 1536-byte rows) while the rows follow each other
    // right after it, as the file size shows; the games read them. When the
    // table points outside the file, the rows are read in order after the
    // whole table (every mipmap level's entries). RTS always reads them in
    // order.
    bool table_ok = true;
    for (std::size_t level = 0, row = 0; level < read_levels && table_ok; ++level) {
        const auto size = row_bytes(levels[level].width);
        for (std::uint32_t y = 0; y < levels[level].height && table_ok; ++y, ++row) {
            const auto row_offset = header_offset + read_u32_le(payload, scanlines_offset + row * 4);
            table_ok = range_fits(row_offset, size, payload.size());
        }
    }
    const std::size_t sequential_start = scanlines_offset + ((header.options & 1u) != 0 ? entries : header.height) * 4;
    if (!table_ok) {
        std::size_t total = 0;
        for (std::size_t level = 0; level < read_levels; ++level) total += row_bytes(levels[level].width) * levels[level].height;
        if (!range_fits(sequential_start, total, payload.size()))
            return {.error = "ACE scanline offset points beyond file data."};
    }

    std::vector<std::vector<std::uint8_t>> decoded;
    for (std::size_t level = 0, first_row = 0, sequential = sequential_start; level < read_levels; ++level) {
        const auto width = levels[level].width;
        const auto height = levels[level].height;
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4, 0xFF);
        const auto size = row_bytes(width);
        for (std::uint32_t y = 0; y < height; ++y) {
            // Scanline offsets are relative to the ACE body, which in an
            // uncompressed file starts after the 16-byte SIMISA envelope.
            const auto row_offset = table_ok
                ? header_offset + read_u32_le(payload, scanlines_offset + (first_row + y) * 4)
                : sequential + static_cast<std::size_t>(y) * size;
            if (row_offset >= payload.size()) return {.error = "ACE scanline offset points beyond file data."};
            std::size_t cursor = row_offset;
            for (const auto& channel : channels) {
                const auto plane_size = channel.bits == 8 ? static_cast<std::size_t>(width)
                                                           : (static_cast<std::size_t>(width) + 7) / 8;
                if (!range_fits(cursor, plane_size, payload.size())) return {.error = "Truncated ACE channel plane."};
                for (std::uint32_t x = 0; x < width; ++x) {
                    const auto value = channel.bits == 8
                        ? static_cast<std::uint8_t>(payload[cursor + x])
                        : static_cast<std::uint8_t>(((static_cast<unsigned char>(payload[cursor + x / 8])
                             >> (7 - (x % 8))) & 1) * 255);
                    const auto rgba = (static_cast<std::size_t>(y) * width + x) * 4;
                    if (channel.type == red_channel) pixels[rgba] = value;
                    else if (channel.type == green_channel) pixels[rgba + 1] = value;
                    else if (channel.type == blue_channel) pixels[rgba + 2] = value;
                    else if (channel.type == alpha_channel || channel.type == mask_channel) pixels[rgba + 3] = value;
                }
                cursor += plane_size;
            }
        }
        decoded.push_back(std::move(pixels));
        first_row += height;
        sequential += size * height;
    }

    AceRgbaImage image{.width = header.width, .height = header.height, .pixels = std::move(decoded.front())};
    if (file_mips) {
        image.mips.assign(std::make_move_iterator(decoded.begin() + 1), std::make_move_iterator(decoded.end()));
        image.mips_from_file = true;
    } else {
        bleed_into_transparent(image.width, image.height, image.pixels);
        image.mips = build_rgba_mipmaps(image.width, image.height, image.pixels);
    }
    return {.image = std::move(image)};
}

std::vector<std::vector<std::uint8_t>> build_rgba_mipmaps(const std::uint32_t width, const std::uint32_t height,
                                                          const std::vector<std::uint8_t>& pixels) {
    std::vector<std::vector<std::uint8_t>> mips;
    if (width == 0 || height == 0 || pixels.size() != static_cast<std::size_t>(width) * height * 4) return mips;
    const std::vector<std::uint8_t>* source = &pixels;
    auto w = width;
    auto h = height;
    while (w > 1 || h > 1) {
        const auto next_w = (std::max)(w / 2, 1u);
        const auto next_h = (std::max)(h / 2, 1u);
        std::vector<std::uint8_t> level(static_cast<std::size_t>(next_w) * next_h * 4);
        const auto at = [&](const std::uint32_t sx, const std::uint32_t sy, const int c) {
            return static_cast<unsigned>((*source)[(static_cast<std::size_t>(sy) * w + sx) * 4 + c]);
        };
        for (std::uint32_t y = 0; y < next_h; ++y) {
            for (std::uint32_t x = 0; x < next_w; ++x) {
                // The texels this one is made of.
                std::uint32_t xs[4], ys[4];
                int count = 0;
                if (w > 1 && h > 1) {
                    const std::uint32_t px[4] = {2 * x, 2 * x + 1, 2 * x, 2 * x + 1}, py[4] = {2 * y, 2 * y, 2 * y + 1, 2 * y + 1};
                    for (int k = 0; k < 4; ++k) xs[k] = px[k], ys[k] = py[k];
                    count = 4;
                } else if (w > 1) {
                    xs[0] = 2 * x, ys[0] = 0, xs[1] = 2 * x + 1, ys[1] = 0;
                    count = 2;
                } else {
                    xs[0] = 0, ys[0] = 2 * y, xs[1] = 0, ys[1] = 2 * y + 1;
                    count = 2;
                }
                // The colour weighted by the alpha (openRTS, 2026-11-25): a
                // see-through texel's colour (a fence's red under alpha 0)
                // is not seen, so it does not tint the smaller levels.
                unsigned alpha = 0, weight = 0, rgb[3] = {0, 0, 0}, plain[3] = {0, 0, 0};
                for (int k = 0; k < count; ++k) {
                    const auto a = at(xs[k], ys[k], 3);
                    alpha += a;
                    weight += a;
                    for (int c = 0; c < 3; ++c) {
                        rgb[c] += at(xs[k], ys[k], c) * a;
                        plain[c] += at(xs[k], ys[k], c);
                    }
                }
                auto* out = &level[(static_cast<std::size_t>(y) * next_w + x) * 4];
                for (int c = 0; c < 3; ++c)
                    out[c] = static_cast<std::uint8_t>(weight > 0 ? (rgb[c] + weight / 2) / weight
                                                                  : (plain[c] + static_cast<unsigned>(count) / 2) / static_cast<unsigned>(count));
                out[3] = static_cast<std::uint8_t>((alpha + static_cast<unsigned>(count) / 2) / static_cast<unsigned>(count));
            }
        }
        mips.push_back(std::move(level));
        source = &mips.back();
        w = next_w;
        h = next_h;
    }
    return mips;
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
