#include <openrts/formats/ace_writer.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include <openrts/formats/ace_file.hpp>

#include <zlib.h>

namespace openrts::formats {
namespace {

constexpr std::uint32_t format_rgb = 0x0E;
constexpr std::uint32_t format_rgb_mask = 0x10;
constexpr std::uint32_t format_rgba = 0x11;
constexpr std::uint32_t format_dxt1 = 0x12;
// Channel kinds (the decoder's): 2 mask, 3 red, 4 green, 5 blue, 6 alpha.
constexpr std::uint64_t channel_mask = 2, channel_red = 3, channel_green = 4, channel_blue = 5, channel_alpha = 6;

void put_u32(std::string& out, const std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) out += static_cast<char>((value >> shift) & 0xFF);
}

void put_u64(std::string& out, const std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) out += static_cast<char>((value >> shift) & 0xFF);
}

void set_u32(std::string& out, const std::size_t at, const std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out[at + i] = static_cast<char>((value >> (8 * i)) & 0xFF);
}

[[nodiscard]] bool power_of_two(const std::uint32_t value) { return value != 0 && (value & (value - 1)) == 0; }

// A 5:6:5 colour from 8-bit channels, and back the way the decoder expands it.
[[nodiscard]] std::uint16_t pack_565(const float r, const float g, const float b) {
    const auto q = [](const float v, const int top) {
        return static_cast<int>(std::lround(std::clamp(v, 0.0f, 255.0f) * static_cast<float>(top) / 255.0f));
    };
    return static_cast<std::uint16_t>((q(r, 31) << 11) | (q(g, 63) << 5) | q(b, 31));
}

void unpack_565(const std::uint16_t value, int (&rgb)[3]) {
    const int r = (value >> 11) & 0x1F, g = (value >> 5) & 0x3F, b = value & 0x1F;
    rgb[0] = (r << 3) | (r >> 2);
    rgb[1] = (g << 2) | (g >> 4);
    rgb[2] = (b << 3) | (b >> 2);
}

// Distance as the eye weighs it (green most).
[[nodiscard]] float distance(const float (&a)[3], const int (&b)[3]) {
    const float dr = a[0] - static_cast<float>(b[0]), dg = a[1] - static_cast<float>(b[1]), db = a[2] - static_cast<float>(b[2]);
    return 0.299f * dr * dr + 0.587f * dg * dg + 0.114f * db * db;
}

// A level's 4 x 4 tile at block (bx, by), the edge's texels repeated past it.
void tile_at(const std::vector<std::uint8_t>& pixels, const std::uint32_t width, const std::uint32_t height, const std::uint32_t bx,
             const std::uint32_t by, std::uint8_t (&tile)[16][4]) {
    for (std::uint32_t ty = 0; ty < 4; ++ty) {
        for (std::uint32_t tx = 0; tx < 4; ++tx) {
            const auto x = (std::min)(bx * 4 + tx, width - 1);
            const auto y = (std::min)(by * 4 + ty, height - 1);
            std::memcpy(tile[ty * 4 + tx], pixels.data() + (static_cast<std::size_t>(y) * width + x) * 4, 4);
        }
    }
}

// A level's rows of channel planes (structured ACE, and a DXT1 file's levels
// under 4 x 4): R, G, B, then the mask (bit 7 first, 1 opaque) and the alpha
// as `channels` lists them.
void put_rows(std::string& out, const std::vector<std::uint8_t>& pixels, const std::uint32_t width, const std::uint32_t height,
              const std::vector<std::uint64_t>& channels, std::vector<std::size_t>* row_starts = nullptr) {
    for (std::uint32_t y = 0; y < height; ++y) {
        if (row_starts) row_starts->push_back(out.size());
        const std::uint8_t* row = pixels.data() + static_cast<std::size_t>(y) * width * 4;
        for (const auto channel : channels) {
            if (channel == channel_mask) {
                std::string bits((width + 7) / 8, '\0');
                for (std::uint32_t x = 0; x < width; ++x)
                    if (row[x * 4 + 3] >= 128) bits[x / 8] = static_cast<char>(bits[x / 8] | (0x80 >> (x % 8)));
                out += bits;
                continue;
            }
            const int c = channel == channel_red ? 0 : channel == channel_green ? 1 : channel == channel_blue ? 2 : 3;
            for (std::uint32_t x = 0; x < width; ++x) out += static_cast<char>(row[x * 4 + c]);
        }
    }
}

[[nodiscard]] bool any_transparent(const std::vector<std::uint8_t>& rgba) {
    for (std::size_t i = 3; i < rgba.size(); i += 4)
        if (rgba[i] < 128) return true;
    return false;
}

} // namespace

std::array<std::uint8_t, 8> encode_dxt1_block(const std::uint8_t (&tile)[16][4], const bool punch_through) {
    bool clear[16]{};
    float colours[16][3];
    int opaque = 0;
    for (int i = 0; i < 16; ++i) {
        clear[i] = punch_through && tile[i][3] < 128;
        for (int c = 0; c < 3; ++c) colours[i][c] = static_cast<float>(tile[i][c]);
        if (!clear[i]) ++opaque;
    }
    // Nothing to see: both ends black, a three-colour block, every texel the
    // transparent entry.
    if (opaque == 0) return {0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF};
    const bool three = opaque < 16;

    // The colours' main axis (power iteration on their covariance): the line
    // the palette lies on.
    float mean[3]{};
    for (int i = 0; i < 16; ++i)
        if (!clear[i])
            for (int c = 0; c < 3; ++c) mean[c] += colours[i][c] / static_cast<float>(opaque);
    float cov[3][3]{};
    for (int i = 0; i < 16; ++i) {
        if (clear[i]) continue;
        const float d[3] = {colours[i][0] - mean[0], colours[i][1] - mean[1], colours[i][2] - mean[2]};
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b) cov[a][b] += d[a] * d[b];
    }
    float axis[3] = {1.0f, 1.0f, 1.0f};
    for (int k = 0; k < 8; ++k) {
        float next[3]{};
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b) next[a] += cov[a][b] * axis[b];
        const float length = std::sqrt(next[0] * next[0] + next[1] * next[1] + next[2] * next[2]);
        if (length < 1e-6f) break;
        for (int a = 0; a < 3; ++a) axis[a] = next[a] / length;
    }
    float low = std::numeric_limits<float>::max(), high = std::numeric_limits<float>::lowest();
    for (int i = 0; i < 16; ++i) {
        if (clear[i]) continue;
        const float t = (colours[i][0] - mean[0]) * axis[0] + (colours[i][1] - mean[1]) * axis[1] + (colours[i][2] - mean[2]) * axis[2];
        low = (std::min)(low, t);
        high = (std::max)(high, t);
    }
    float end0[3], end1[3];
    for (int c = 0; c < 3; ++c) {
        end0[c] = mean[c] + axis[c] * high;
        end1[c] = mean[c] + axis[c] * low;
    }

    std::array<std::uint8_t, 8> best{};
    float best_error = std::numeric_limits<float>::max();
    for (int round = 0; round < 3; ++round) {
        auto q0 = pack_565(end0[0], end0[1], end0[2]);
        auto q1 = pack_565(end1[0], end1[1], end1[2]);
        // Four colours need c0 > c1, three c0 <= c1 (the decoder's rule).
        if (three ? q0 > q1 : q0 < q1) std::swap(q0, q1);
        int p0[3], p1[3];
        unpack_565(q0, p0);
        unpack_565(q1, p1);
        int palette[4][3];
        int entries = 4;
        for (int c = 0; c < 3; ++c) {
            palette[0][c] = p0[c];
            palette[1][c] = p1[c];
            if (!three && q0 > q1) {
                palette[2][c] = (2 * p0[c] + p1[c]) / 3;
                palette[3][c] = (p0[c] + 2 * p1[c]) / 3;
            } else {
                palette[2][c] = (p0[c] + p1[c]) / 2;
                palette[3][c] = 0;
            }
        }
        if (three || q0 == q1) entries = 3; // the fourth is transparent (or black)
        // Each texel the nearest entry.
        std::uint32_t indices = 0;
        int chosen[16]{};
        float error = 0.0f;
        for (int i = 0; i < 16; ++i) {
            int index = 3;
            if (!clear[i]) {
                float nearest = std::numeric_limits<float>::max();
                for (int e = 0; e < entries; ++e) {
                    const float d = distance(colours[i], palette[e]);
                    if (d < nearest) {
                        nearest = d;
                        index = e;
                    }
                }
                error += nearest;
            }
            chosen[i] = index;
            indices |= static_cast<std::uint32_t>(index) << (2 * i);
        }
        if (error < best_error) {
            best_error = error;
            best = {static_cast<std::uint8_t>(q0 & 0xFF), static_cast<std::uint8_t>(q0 >> 8), static_cast<std::uint8_t>(q1 & 0xFF),
                    static_cast<std::uint8_t>(q1 >> 8), static_cast<std::uint8_t>(indices & 0xFF), static_cast<std::uint8_t>((indices >> 8) & 0xFF),
                    static_cast<std::uint8_t>((indices >> 16) & 0xFF), static_cast<std::uint8_t>(indices >> 24)};
        }
        if (error <= 0.0f) break;
        // The ends that fit the chosen entries best (least squares): each
        // entry a mix of the two ends.
        const float four[4] = {1.0f, 0.0f, 2.0f / 3.0f, 1.0f / 3.0f};
        const float halves[3] = {1.0f, 0.0f, 0.5f};
        float aa = 0, ab = 0, bb = 0, ax[3]{}, bx[3]{};
        for (int i = 0; i < 16; ++i) {
            if (clear[i] || chosen[i] >= entries) continue;
            const float a = entries == 4 ? four[chosen[i]] : halves[chosen[i]];
            const float b = 1.0f - a;
            aa += a * a;
            ab += a * b;
            bb += b * b;
            for (int c = 0; c < 3; ++c) {
                ax[c] += a * colours[i][c];
                bx[c] += b * colours[i][c];
            }
        }
        const float det = aa * bb - ab * ab;
        if (std::fabs(det) < 1e-6f) break;
        for (int c = 0; c < 3; ++c) {
            end0[c] = (ax[c] * bb - bx[c] * ab) / det;
            end1[c] = (bx[c] * aa - ax[c] * ab) / det;
        }
    }
    return best;
}

AceFormat suggested_ace_format(const std::vector<std::uint8_t>& rgba) {
    for (std::size_t i = 3; i < rgba.size(); i += 4)
        if (rgba[i] != 0 && rgba[i] != 255) return AceFormat::rgba;
    return AceFormat::dxt1;
}

AceEncodeResult encode_ace(const std::uint32_t width, const std::uint32_t height, const std::vector<std::uint8_t>& rgba,
                           const AceWriteOptions& options) {
    AceEncodeResult result;
    if (width == 0 || height == 0 || width > 16384 || height > 16384 || rgba.size() != static_cast<std::size_t>(width) * height * 4) {
        result.error = "the image's size is not 1..16384 or its pixels do not match it";
        return result;
    }
    bool mipmaps = options.mipmaps;
    if (mipmaps && (!power_of_two(width) || !power_of_two(height))) {
        mipmaps = false;
        result.notes.push_back("not a power of two: no mipmaps (RTS builds its own)");
    }
    const bool transparent = any_transparent(rgba);
    if (transparent && options.format == AceFormat::rgb) result.notes.push_back("RGB: its transparency is left out");

    std::vector<std::uint64_t> channels{channel_red, channel_green, channel_blue};
    std::uint32_t format = format_rgb;
    switch (options.format) {
    case AceFormat::dxt1:
        format = format_dxt1;
        if (transparent) channels.push_back(channel_mask);
        break;
    case AceFormat::rgb: break;
    case AceFormat::rgb_mask:
        format = format_rgb_mask;
        channels.push_back(channel_mask);
        break;
    case AceFormat::rgba:
        format = format_rgba;
        channels.push_back(channel_mask);
        channels.push_back(channel_alpha);
        break;
    }
    const bool raw = options.format == AceFormat::dxt1;
    const std::uint32_t flags = raw ? (mipmaps ? 21u : 16u) : (mipmaps ? 5u : 0u);

    // The levels: a DXT1 file's down to 1 x 1, a structured one's while its
    // height halves (the decoder's chains).
    struct Level {
        std::uint32_t width, height;
        const std::vector<std::uint8_t>* pixels;
    };
    const auto mips = mipmaps ? build_rgba_mipmaps(width, height, rgba) : std::vector<std::vector<std::uint8_t>>{};
    std::vector<Level> levels{{width, height, &rgba}};
    for (std::size_t k = 0; k < mips.size(); ++k) {
        const auto w = (std::max)(width >> (k + 1), 1u);
        const auto h = (std::max)(height >> (k + 1), 1u);
        if (!raw && levels.back().height <= 1) break;
        levels.push_back({w, h, &mips[k]});
    }

    std::string body;
    put_u32(body, 1);
    put_u32(body, flags);
    put_u32(body, width);
    put_u32(body, height);
    put_u32(body, format);
    put_u32(body, static_cast<std::uint32_t>(channels.size()));
    {
        std::string creator(128, '\0');
        const std::string text = "openRTS openrts-ace";
        std::memcpy(creator.data() + 4, text.data(), text.size());
        body += creator;
    }
    for (const auto channel : channels) {
        put_u64(body, channel == channel_mask ? 1 : 8);
        put_u64(body, channel);
    }

    if (raw) {
        // A table of the levels' offsets (from the body's start), then each
        // level: a byte count and its DXT1 blocks, or under 4 x 4 plain rows.
        const auto table = body.size();
        body.append(levels.size() * 4, '\0');
        for (std::size_t k = 0; k < levels.size(); ++k) {
            const auto& level = levels[k];
            set_u32(body, table + k * 4, static_cast<std::uint32_t>(body.size()));
            if (level.width < 4 || level.height < 4) {
                put_rows(body, *level.pixels, level.width, level.height, channels);
                continue;
            }
            const auto blocks_x = (level.width + 3) / 4, blocks_y = (level.height + 3) / 4;
            put_u32(body, blocks_x * blocks_y * 8);
            std::uint8_t tile[16][4];
            for (std::uint32_t by = 0; by < blocks_y; ++by) {
                for (std::uint32_t bx = 0; bx < blocks_x; ++bx) {
                    tile_at(*level.pixels, level.width, level.height, bx, by, tile);
                    const auto block = encode_dxt1_block(tile, transparent);
                    body.append(reinterpret_cast<const char*>(block.data()), block.size());
                }
            }
        }
    } else {
        // A table of every row's offset (every level's rows), then the rows.
        std::size_t rows = 0;
        for (const auto& level : levels) rows += level.height;
        const auto table = body.size();
        body.append(rows * 4, '\0');
        std::vector<std::size_t> starts;
        for (const auto& level : levels) put_rows(body, *level.pixels, level.width, level.height, channels, &starts);
        for (std::size_t r = 0; r < starts.size(); ++r) set_u32(body, table + r * 4, static_cast<std::uint32_t>(starts[r]));
    }

    if (!options.zlib) {
        result.bytes = "SIMISA@@@@@@@@@@" + body;
        return result;
    }
    uLongf packed_size = compressBound(static_cast<uLong>(body.size()));
    std::string packed(packed_size, '\0');
    if (compress2(reinterpret_cast<Bytef*>(packed.data()), &packed_size, reinterpret_cast<const Bytef*>(body.data()),
                  static_cast<uLong>(body.size()), 9) != Z_OK) {
        result.error = "zlib failed to pack the texture";
        return result;
    }
    packed.resize(packed_size);
    result.bytes = "SIMISA@F";
    put_u32(result.bytes, static_cast<std::uint32_t>(body.size()));
    result.bytes += "@@@@";
    result.bytes += packed;
    return result;
}

} // namespace openrts::formats
