#include <openrts/formats/block_compression.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstring>
#include <thread>

#include <openrts/formats/ace_writer.hpp>

namespace openrts::formats {
namespace {

// BC7's index weights (of 64) for 2, 3 and 4 bits.
constexpr std::array<int, 4> weights2{0, 21, 43, 64};
constexpr std::array<int, 8> weights3{0, 9, 18, 27, 37, 46, 55, 64};
constexpr std::array<int, 16> weights4{0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

[[nodiscard]] int weight(const int bits, const int index) {
    return bits == 2 ? weights2[static_cast<std::size_t>(index)] : bits == 3 ? weights3[static_cast<std::size_t>(index)]
                                                                            : weights4[static_cast<std::size_t>(index)];
}

[[nodiscard]] int interpolate(const int a, const int b, const int w) {
    return ((64 - w) * a + w * b + 32) >> 6;
}

// BC7's modes (the D3D11 format's table).
struct Bc7Mode {
    int parts, partition_bits, rotation_bits, selection_bits, colour_bits, alpha_bits, endpoint_pbits, shared_pbits, index_bits, index2_bits;
};
constexpr std::array<Bc7Mode, 8> bc7_modes{{
    {3, 4, 0, 0, 4, 0, 1, 0, 3, 0},
    {2, 6, 0, 0, 6, 0, 0, 1, 3, 0},
    {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
    {2, 6, 0, 0, 7, 0, 1, 0, 2, 0},
    {1, 0, 2, 1, 5, 6, 0, 0, 2, 3},
    {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
    {1, 0, 0, 0, 7, 7, 1, 0, 4, 0},
    {2, 6, 0, 0, 5, 5, 1, 0, 2, 0},
}};

// Two parts: bit i of the mask is texel i's part.
constexpr std::array<std::uint16_t, 64> partitions2{
    0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8, 0xFF00, 0xFFF0, 0xF000,
    0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE, 0x088C, 0x3110, 0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C,
    0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696, 0xA55A, 0x73CE, 0x13C8, 0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660,
    0x0272, 0x04E4, 0x4E40, 0x2720, 0xC936, 0x936C, 0x39C6, 0x639C, 0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22};

// Three parts: texel i's part, row by row.
constexpr std::array<const char*, 64> partitions3{
    "0011001102212222", "0001001122112221", "0000200122112211", "0222002200110111", "0000000011221122", "0011001100220022",
    "0022002211111111", "0011001122112211", "0000000011112222", "0000111111112222", "0000111122222222", "0012001200120012",
    "0112011201120112", "0122012201220122", "0011011211221222", "0011200122002220", "0001001101121122", "0111001120012200",
    "0000112211221122", "0022002200221111", "0111011102220222", "0001000122212221", "0000001101220122", "0000110022102210",
    "0122012200110000", "0012001211222222", "0110122112210110", "0000011012211221", "0022110211020022", "0110011020022222",
    "0011012201220011", "0000200022112221", "0000000211221222", "0222002200120011", "0011001200220222", "0120012001200120",
    "0000111122220000", "0120120120120120", "0120201212010120", "0011220011220011", "0011112222000011", "0101010122222222",
    "0000000021212121", "0022112200221122", "0022001100220011", "0220122102201221", "0101222222220101", "0000212121212121",
    "0101010101012222", "0222011102220111", "0002111200021112", "0000211221122112", "0222011101110222", "0002111211120002",
    "0110011001102222", "0000000021122112", "0110011022222222", "0022001100110022", "0022112211220022", "0000000000002112",
    "0002000100020001", "0222122202221222", "0101222222222222", "0111201122012220"};

// The anchors: part 1 of two parts; parts 1 and 2 of three (part 0's is texel 0).
constexpr std::array<std::uint8_t, 64> anchors2{15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2,  8,  2,  2,  8,
                                                8,  15, 2,  8,  2,  2,  8,  8,  2,  2,  15, 15, 6,  8,  2,  8,  15, 15, 2,  8,  2,  2,
                                                2,  15, 15, 6,  6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2,  15};
constexpr std::array<std::uint8_t, 64> anchors3a{3,  3,  15, 15, 8, 3,  15, 15, 8,  8,  6,  6,  6,  5,  3, 3,  3,  3,  8,  15, 3,  3,
                                                 6,  10, 5,  8,  8, 6,  8,  5,  15, 15, 8,  15, 3,  5,  6, 10, 8,  15, 15, 3,  15, 5,
                                                 15, 15, 15, 15, 3, 15, 5,  5,  5,  8,  5,  10, 5,  10, 8, 13, 15, 12, 3,  3};
constexpr std::array<std::uint8_t, 64> anchors3b{15, 8,  8,  3,  15, 15, 3,  8,  15, 15, 15, 15, 15, 15, 15, 8,  15, 8,  15, 3,  15, 8,
                                                 15, 8,  3,  15, 6,  10, 15, 15, 10, 8,  15, 3,  15, 10, 10, 8,  9,  10, 6,  15, 8,  15,
                                                 3,  6,  6,  8,  15, 3,  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3,  15, 15, 8};

struct BitReader {
    const std::uint8_t* block;
    int at = 0;
    std::uint32_t get(const int count) {
        std::uint32_t v = 0;
        for (int i = 0; i < count; ++i, ++at) v |= static_cast<std::uint32_t>((block[at >> 3] >> (at & 7)) & 1u) << i;
        return v;
    }
};

struct Bits {
    std::array<std::uint8_t, 16> bytes{};
    int at = 0;
    void put(const std::uint32_t value, const int count) {
        for (int i = 0; i < count; ++i, ++at) {
            if ((value >> i) & 1u) bytes[static_cast<std::size_t>(at >> 3)] |= static_cast<std::uint8_t>(1u << (at & 7));
        }
    }
};

void decode_bc7(const std::uint8_t* block, std::uint8_t* rgba) {
    const int mode = bc7_mode(block);
    if (mode >= 8) {
        std::memset(rgba, 0, 64);
        return;
    }
    const auto& m = bc7_modes[static_cast<std::size_t>(mode)];
    BitReader r{block};
    r.get(mode + 1);
    const int partition = static_cast<int>(r.get(m.partition_bits));
    const int rotation = static_cast<int>(r.get(m.rotation_bits));
    const int selection = static_cast<int>(r.get(m.selection_bits));
    const int ends = m.parts * 2;
    int e[6][4]{};
    for (int ch = 0; ch < 3; ++ch)
        for (int k = 0; k < ends; ++k) e[k][ch] = static_cast<int>(r.get(m.colour_bits));
    if (m.alpha_bits > 0)
        for (int k = 0; k < ends; ++k) e[k][3] = static_cast<int>(r.get(m.alpha_bits));
    int colour_bits = m.colour_bits, alpha_bits = m.alpha_bits;
    if (m.endpoint_pbits) {
        for (int k = 0; k < ends; ++k) {
            const int p = static_cast<int>(r.get(1));
            for (int ch = 0; ch < 4; ++ch) e[k][ch] = (e[k][ch] << 1) | p;
        }
        ++colour_bits;
        if (alpha_bits > 0) ++alpha_bits;
    }
    if (m.shared_pbits) {
        for (int s = 0; s < m.parts; ++s) {
            const int p = static_cast<int>(r.get(1));
            for (int k = s * 2; k < s * 2 + 2; ++k)
                for (int ch = 0; ch < 4; ++ch) e[k][ch] = (e[k][ch] << 1) | p;
        }
        ++colour_bits;
        if (alpha_bits > 0) ++alpha_bits;
    }
    // Widened to 8 bits: the top bits again below.
    for (int k = 0; k < ends; ++k) {
        for (int ch = 0; ch < 3; ++ch) e[k][ch] = (e[k][ch] << (8 - colour_bits)) | (e[k][ch] >> (2 * colour_bits - 8));
        e[k][3] = alpha_bits > 0 ? (e[k][3] << (8 - alpha_bits)) | (e[k][3] >> (2 * alpha_bits - 8)) : 255;
    }
    int primary[16]{}, secondary[16]{};
    for (int i = 0; i < 16; ++i) primary[i] = static_cast<int>(r.get(m.index_bits - (bc7_anchor(m.parts, partition, i) ? 1 : 0)));
    if (m.index2_bits > 0)
        for (int i = 0; i < 16; ++i) secondary[i] = static_cast<int>(r.get(m.index2_bits - (i == 0 ? 1 : 0)));
    for (int i = 0; i < 16; ++i) {
        const int part = bc7_part(m.parts, partition, i);
        const int* a = e[part * 2];
        const int* b = e[part * 2 + 1];
        int colour_w, alpha_w;
        if (m.index2_bits == 0) {
            colour_w = alpha_w = weight(m.index_bits, primary[i]);
        } else if (selection == 0) {
            colour_w = weight(m.index_bits, primary[i]);
            alpha_w = weight(m.index2_bits, secondary[i]);
        } else {
            colour_w = weight(m.index2_bits, secondary[i]);
            alpha_w = weight(m.index_bits, primary[i]);
        }
        std::uint8_t* p = rgba + i * 4;
        for (int ch = 0; ch < 3; ++ch) p[ch] = static_cast<std::uint8_t>(interpolate(a[ch], b[ch], colour_w));
        p[3] = static_cast<std::uint8_t>(interpolate(a[3], b[3], alpha_w));
        if (rotation != 0) std::swap(p[3], p[rotation - 1]);
    }
}

// BC1's colour (8 bytes): with `four` always the four-level palette (BC2, BC3).
void decode_colour(const std::uint8_t* block, const bool four, std::uint8_t* rgba) {
    const int c0 = block[0] | (block[1] << 8), c1 = block[2] | (block[3] << 8);
    const auto widen = [](const int c, int (&rgb)[3]) {
        const int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
        rgb[0] = (r << 3) | (r >> 2);
        rgb[1] = (g << 2) | (g >> 4);
        rgb[2] = (b << 3) | (b >> 2);
    };
    int a[3], b[3];
    widen(c0, a);
    widen(c1, b);
    int pal[4][4];
    for (int ch = 0; ch < 3; ++ch) {
        pal[0][ch] = a[ch];
        pal[1][ch] = b[ch];
        if (four || c0 > c1) {
            pal[2][ch] = (2 * a[ch] + b[ch] + 1) / 3;
            pal[3][ch] = (a[ch] + 2 * b[ch] + 1) / 3;
        } else {
            pal[2][ch] = (a[ch] + b[ch] + 1) / 2;
            pal[3][ch] = 0;
        }
    }
    pal[0][3] = pal[1][3] = pal[2][3] = 255;
    pal[3][3] = four || c0 > c1 ? 255 : 0;
    const std::uint32_t indices = block[4] | (block[5] << 8) | (block[6] << 16) | (static_cast<std::uint32_t>(block[7]) << 24);
    for (int i = 0; i < 16; ++i) {
        const auto k = (indices >> (2 * i)) & 3u;
        for (int ch = 0; ch < 4; ++ch) rgba[i * 4 + ch] = static_cast<std::uint8_t>(pal[k][ch]);
    }
}

// BC4 (8 bytes) into one channel of the texels.
void decode_bc4(const std::uint8_t* block, std::uint8_t* rgba, const int channel) {
    const int e0 = block[0], e1 = block[1];
    int pal[8];
    pal[0] = e0;
    pal[1] = e1;
    if (e0 > e1) {
        for (int k = 2; k < 8; ++k) pal[k] = ((8 - k) * e0 + (k - 1) * e1 + 3) / 7;
    } else {
        for (int k = 2; k < 6; ++k) pal[k] = ((6 - k) * e0 + (k - 1) * e1 + 2) / 5;
        pal[6] = 0;
        pal[7] = 255;
    }
    std::uint64_t bits = 0;
    for (int b = 0; b < 6; ++b) bits |= static_cast<std::uint64_t>(block[2 + b]) << (8 * b);
    for (int i = 0; i < 16; ++i) rgba[i * 4 + channel] = static_cast<std::uint8_t>(pal[(bits >> (3 * i)) & 7u]);
}

using Texels = std::array<std::array<int, 4>, 16>;

// ---- BC7 mode 6 -----------------------------------------------------------

struct Trial {
    std::array<int, 4> q0{}, q1{}; // 7-bit endpoints
    int p0 = 0, p1 = 0;            // their low bits
    std::array<std::uint8_t, 16> index{};
    long long error = LLONG_MAX;
};

void palette16(const int* c0, const int* c1, int out[16][4]) {
    for (int k = 0; k < 16; ++k)
        for (int ch = 0; ch < 4; ++ch) out[k][ch] = interpolate(c0[ch], c1[ch], weights4[static_cast<std::size_t>(k)]);
}

long long fit_indices(const Texels& px, const int pal[16][4], std::array<std::uint8_t, 16>& index) {
    long long total = 0;
    for (int i = 0; i < 16; ++i) {
        long long best = LLONG_MAX;
        int chosen = 0;
        for (int k = 0; k < 16; ++k) {
            long long e = 0;
            for (int ch = 0; ch < 4; ++ch) {
                const int d = px[i][ch] - pal[k][ch];
                e += d * d;
            }
            if (e < best) {
                best = e;
                chosen = k;
            }
        }
        index[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(chosen);
        total += best;
    }
    return total;
}

// Each texel's level from where it falls along the line between the two
// colours, it and the levels either side tried (the palette's rounding
// moves a level a little off the line).
long long fit_indices_on_line(const Texels& px, const int* c0, const int* c1, const int pal[16][4], std::array<std::uint8_t, 16>& index) {
    float d[4];
    float length2 = 0.0f;
    for (int ch = 0; ch < 4; ++ch) {
        d[ch] = static_cast<float>(c1[ch] - c0[ch]);
        length2 += d[ch] * d[ch];
    }
    if (length2 < 0.5f) return fit_indices(px, pal, index);
    const float scale = 15.0f / length2;
    long long total = 0;
    for (int i = 0; i < 16; ++i) {
        float t = 0.0f;
        for (int ch = 0; ch < 4; ++ch) t += static_cast<float>(px[i][ch] - c0[ch]) * d[ch];
        const int middle = std::clamp(static_cast<int>(std::lround(t * scale)), 0, 15);
        long long best = LLONG_MAX;
        int chosen = middle;
        for (int k = (std::max)(middle - 1, 0); k <= (std::min)(middle + 1, 15); ++k) {
            long long e = 0;
            for (int ch = 0; ch < 4; ++ch) {
                const int diff = px[i][ch] - pal[k][ch];
                e += diff * diff;
            }
            if (e < best) {
                best = e;
                chosen = k;
            }
        }
        index[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(chosen);
        total += best;
    }
    return total;
}

// The two colours a and b (0..255) in 7 bits and a low bit each, the four
// pairs of low bits tried: the best kept in `best`.
void try_endpoints(const float* a, const float* b, const Texels& px, Trial& best) {
    for (int p0 = 0; p0 < 2; ++p0) {
        for (int p1 = 0; p1 < 2; ++p1) {
            Trial t;
            t.p0 = p0;
            t.p1 = p1;
            int c0[4], c1[4];
            for (int ch = 0; ch < 4; ++ch) {
                t.q0[ch] = std::clamp(static_cast<int>(std::lround((a[ch] - static_cast<float>(p0)) * 0.5f)), 0, 127);
                t.q1[ch] = std::clamp(static_cast<int>(std::lround((b[ch] - static_cast<float>(p1)) * 0.5f)), 0, 127);
                c0[ch] = (t.q0[ch] << 1) | p0;
                c1[ch] = (t.q1[ch] << 1) | p1;
            }
            int pal[16][4];
            palette16(c0, c1, pal);
            t.error = fit_indices_on_line(px, c0, c1, pal, t.index);
            if (t.error < best.error) best = t;
        }
    }
}

std::array<std::uint8_t, 16> encode_mode6(const Texels& px) {
    // The line through the texels: their mean and principal axis.
    float mean[4]{};
    for (const auto& p : px)
        for (int ch = 0; ch < 4; ++ch) mean[ch] += static_cast<float>(p[ch]);
    for (float& m : mean) m /= 16.0f;
    float cov[4][4]{};
    int low[4] = {255, 255, 255, 255}, high[4] = {0, 0, 0, 0};
    for (const auto& p : px) {
        float d[4];
        for (int ch = 0; ch < 4; ++ch) {
            d[ch] = static_cast<float>(p[ch]) - mean[ch];
            low[ch] = (std::min)(low[ch], p[ch]);
            high[ch] = (std::max)(high[ch], p[ch]);
        }
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) cov[r][c] += d[r] * d[c];
    }
    float axis[4];
    for (int ch = 0; ch < 4; ++ch) axis[ch] = static_cast<float>(high[ch] - low[ch]);
    for (int it = 0; it < 8; ++it) {
        float next[4]{};
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) next[r] += cov[r][c] * axis[c];
        const float length = std::sqrt(next[0] * next[0] + next[1] * next[1] + next[2] * next[2] + next[3] * next[3]);
        if (length < 1e-6f) break;
        for (int ch = 0; ch < 4; ++ch) axis[ch] = next[ch] / length;
    }
    float length = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2] + axis[3] * axis[3]);
    if (length < 1e-6f) {
        axis[0] = axis[1] = axis[2] = 0.57735f;
        axis[3] = 0.0f;
        length = 1.0f;
    }
    for (float& v : axis) v /= length;
    float lo_t = 1e9f, hi_t = -1e9f;
    for (const auto& p : px) {
        float t = 0.0f;
        for (int ch = 0; ch < 4; ++ch) t += (static_cast<float>(p[ch]) - mean[ch]) * axis[ch];
        lo_t = (std::min)(lo_t, t);
        hi_t = (std::max)(hi_t, t);
    }
    float a[4], b[4];
    for (int ch = 0; ch < 4; ++ch) {
        a[ch] = std::clamp(mean[ch] + lo_t * axis[ch], 0.0f, 255.0f);
        b[ch] = std::clamp(mean[ch] + hi_t * axis[ch], 0.0f, 255.0f);
    }
    Trial best;
    try_endpoints(a, b, px, best);
    // The two colours refitted (least squares) to the levels the texels took.
    for (int pass = 0; pass < 2 && best.error > 0; ++pass) {
        float aa = 0.0f, ab = 0.0f, bb = 0.0f, ax[4]{}, bx[4]{};
        for (int i = 0; i < 16; ++i) {
            const float w = static_cast<float>(weights4[best.index[static_cast<std::size_t>(i)]]) / 64.0f;
            const float u = 1.0f - w;
            aa += u * u;
            ab += u * w;
            bb += w * w;
            for (int ch = 0; ch < 4; ++ch) {
                ax[ch] += u * static_cast<float>(px[i][ch]);
                bx[ch] += w * static_cast<float>(px[i][ch]);
            }
        }
        const float det = aa * bb - ab * ab;
        if (std::abs(det) < 1e-6f) break;
        float na[4], nb[4];
        for (int ch = 0; ch < 4; ++ch) {
            na[ch] = std::clamp((ax[ch] * bb - bx[ch] * ab) / det, 0.0f, 255.0f);
            nb[ch] = std::clamp((bx[ch] * aa - ax[ch] * ab) / det, 0.0f, 255.0f);
        }
        const long long before = best.error;
        try_endpoints(na, nb, px, best);
        if (best.error >= before) break;
    }
    // The first texel's index has no top bit: below 8, the colours swapped else.
    if (best.index[0] >= 8) {
        std::swap(best.q0, best.q1);
        std::swap(best.p0, best.p1);
        for (auto& i : best.index) i = static_cast<std::uint8_t>(15 - i);
    }
    Bits bits;
    bits.put(1u << 6, 7); // mode 6
    for (int ch = 0; ch < 4; ++ch) {
        bits.put(static_cast<std::uint32_t>(best.q0[ch]), 7);
        bits.put(static_cast<std::uint32_t>(best.q1[ch]), 7);
    }
    bits.put(static_cast<std::uint32_t>(best.p0), 1);
    bits.put(static_cast<std::uint32_t>(best.p1), 1);
    bits.put(best.index[0], 3);
    for (int i = 1; i < 16; ++i) bits.put(best.index[static_cast<std::size_t>(i)], 4);
    return bits.bytes;
}

// ---- BC7 mode 5 -----------------------------------------------------------

// How far a block's texels are from what it decodes to: a texel's colour by
// how opaque it is (a clear one's barely shows, drawn or blended), its
// alpha twice an opaque colour's channel (where the edge of a leaf falls).
long long block_error(const Texels& px, const std::uint8_t* rgba) {
    long long total = 0;
    for (int i = 0; i < 16; ++i) {
        const long long w = px[i][3] + 8;
        for (int ch = 0; ch < 3; ++ch) {
            const long long d = px[i][ch] - rgba[i * 4 + ch];
            total += w * d * d;
        }
        const long long d = px[i][3] - rgba[i * 4 + 3];
        total += 526 * d * d;
    }
    return total;
}

// A 7-bit endpoint as BC7 widens it to 8 bits (no low bit of its own).
int widen7(const int q) {
    return (q << 1) | (q >> 6);
}

// Mode 5: the colour on a line of its own (7-bit ends, 4 levels), the
// alpha on another (8-bit ends, 4 levels), no rotation.
std::array<std::uint8_t, 16> encode_mode5(const Texels& px) {
    // The colour's line through the texels, each by how opaque it is.
    float w8[16];
    float total = 0.0f, mean[3]{};
    for (int i = 0; i < 16; ++i) {
        w8[i] = static_cast<float>(px[i][3] + 8);
        total += w8[i];
        for (int ch = 0; ch < 3; ++ch) mean[ch] += w8[i] * static_cast<float>(px[i][ch]);
    }
    for (float& m : mean) m /= total;
    float cov[3][3]{};
    int low[3] = {255, 255, 255}, high[3] = {0, 0, 0};
    for (int i = 0; i < 16; ++i) {
        float d[3];
        for (int ch = 0; ch < 3; ++ch) {
            d[ch] = static_cast<float>(px[i][ch]) - mean[ch];
            low[ch] = (std::min)(low[ch], px[i][ch]);
            high[ch] = (std::max)(high[ch], px[i][ch]);
        }
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) cov[r][c] += w8[i] * d[r] * d[c];
    }
    float axis[3];
    for (int ch = 0; ch < 3; ++ch) axis[ch] = static_cast<float>(high[ch] - low[ch]);
    for (int it = 0; it < 8; ++it) {
        float next[3]{};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) next[r] += cov[r][c] * axis[c];
        const float length = std::sqrt(next[0] * next[0] + next[1] * next[1] + next[2] * next[2]);
        if (length < 1e-6f) break;
        for (int ch = 0; ch < 3; ++ch) axis[ch] = next[ch] / length;
    }
    float length = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (length < 1e-6f) {
        axis[0] = axis[1] = axis[2] = 0.57735f;
        length = 1.0f;
    }
    for (float& v : axis) v /= length;
    // The line's ends over the texels that show (alpha 16 and up; all if none).
    float lo_t = 1e9f, hi_t = -1e9f;
    bool any = false;
    for (int pass = 0; pass < 2 && !any; ++pass) {
        for (int i = 0; i < 16; ++i) {
            if (pass == 0 && px[i][3] < 16) continue;
            float t = 0.0f;
            for (int ch = 0; ch < 3; ++ch) t += (static_cast<float>(px[i][ch]) - mean[ch]) * axis[ch];
            lo_t = (std::min)(lo_t, t);
            hi_t = (std::max)(hi_t, t);
            any = true;
        }
    }
    struct Colour {
        int q0[3]{}, q1[3]{};
        std::array<std::uint8_t, 16> index{};
        long long error = LLONG_MAX;
    };
    const auto fit = [&](const float* a, const float* b) {
        Colour c;
        int e0[3], e1[3];
        for (int ch = 0; ch < 3; ++ch) {
            c.q0[ch] = std::clamp(static_cast<int>(std::lround(a[ch] * 127.0f / 255.0f)), 0, 127);
            c.q1[ch] = std::clamp(static_cast<int>(std::lround(b[ch] * 127.0f / 255.0f)), 0, 127);
            e0[ch] = widen7(c.q0[ch]);
            e1[ch] = widen7(c.q1[ch]);
        }
        int pal[4][3];
        for (int k = 0; k < 4; ++k)
            for (int ch = 0; ch < 3; ++ch) pal[k][ch] = interpolate(e0[ch], e1[ch], weights2[static_cast<std::size_t>(k)]);
        c.error = 0;
        for (int i = 0; i < 16; ++i) {
            long long best = LLONG_MAX;
            int chosen = 0;
            for (int k = 0; k < 4; ++k) {
                long long e = 0;
                for (int ch = 0; ch < 3; ++ch) {
                    const int d = px[i][ch] - pal[k][ch];
                    e += d * d;
                }
                if (e < best) {
                    best = e;
                    chosen = k;
                }
            }
            c.index[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(chosen);
            c.error += static_cast<long long>(w8[i]) * best;
        }
        return c;
    };
    float a[3], b[3];
    for (int ch = 0; ch < 3; ++ch) {
        a[ch] = std::clamp(mean[ch] + lo_t * axis[ch], 0.0f, 255.0f);
        b[ch] = std::clamp(mean[ch] + hi_t * axis[ch], 0.0f, 255.0f);
    }
    Colour best = fit(a, b);
    // The two colours refitted (weighted least squares) to the levels taken.
    for (int pass = 0; pass < 2 && best.error > 0; ++pass) {
        float aa = 0.0f, ab = 0.0f, bb = 0.0f, ax[3]{}, bx[3]{};
        for (int i = 0; i < 16; ++i) {
            const float w = static_cast<float>(weights2[best.index[static_cast<std::size_t>(i)]]) / 64.0f;
            const float u = 1.0f - w;
            aa += w8[i] * u * u;
            ab += w8[i] * u * w;
            bb += w8[i] * w * w;
            for (int ch = 0; ch < 3; ++ch) {
                ax[ch] += w8[i] * u * static_cast<float>(px[i][ch]);
                bx[ch] += w8[i] * w * static_cast<float>(px[i][ch]);
            }
        }
        const float det = aa * bb - ab * ab;
        if (std::abs(det) < 1e-3f) break;
        float na[3], nb[3];
        for (int ch = 0; ch < 3; ++ch) {
            na[ch] = std::clamp((ax[ch] * bb - bx[ch] * ab) / det, 0.0f, 255.0f);
            nb[ch] = std::clamp((bx[ch] * aa - ax[ch] * ab) / det, 0.0f, 255.0f);
        }
        const auto trial = fit(na, nb);
        if (trial.error >= best.error) break;
        best = trial;
    }
    // The alpha: its least and most, 4 levels between.
    int a0 = 255, a1 = 0;
    for (const auto& p : px) {
        a0 = (std::min)(a0, p[3]);
        a1 = (std::max)(a1, p[3]);
    }
    std::array<std::uint8_t, 16> alpha_index{};
    {
        int pal[4];
        for (int k = 0; k < 4; ++k) pal[k] = interpolate(a0, a1, weights2[static_cast<std::size_t>(k)]);
        for (int i = 0; i < 16; ++i) {
            int chosen = 0;
            for (int k = 1; k < 4; ++k)
                if (std::abs(px[i][3] - pal[k]) < std::abs(px[i][3] - pal[chosen])) chosen = k;
            alpha_index[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(chosen);
        }
    }
    // Each first index has no top bit: below 2, the ends swapped else.
    if (best.index[0] >= 2) {
        std::swap(best.q0, best.q1);
        for (auto& i : best.index) i = static_cast<std::uint8_t>(3 - i);
    }
    if (alpha_index[0] >= 2) {
        std::swap(a0, a1);
        for (auto& i : alpha_index) i = static_cast<std::uint8_t>(3 - i);
    }
    Bits bits;
    bits.put(1u << 5, 6); // mode 5
    bits.put(0, 2);       // no rotation
    for (int ch = 0; ch < 3; ++ch) {
        bits.put(static_cast<std::uint32_t>(best.q0[ch]), 7);
        bits.put(static_cast<std::uint32_t>(best.q1[ch]), 7);
    }
    bits.put(static_cast<std::uint32_t>(a0), 8);
    bits.put(static_cast<std::uint32_t>(a1), 8);
    bits.put(best.index[0], 1);
    for (int i = 1; i < 16; ++i) bits.put(best.index[static_cast<std::size_t>(i)], 2);
    bits.put(alpha_index[0], 1);
    for (int i = 1; i < 16; ++i) bits.put(alpha_index[static_cast<std::size_t>(i)], 2);
    return bits.bytes;
}

// The block in BC7: mode 6, or mode 5 where the alpha varies and it comes nearer.
std::array<std::uint8_t, 16> encode_bc7_block(const Texels& px) {
    auto block = encode_mode6(px);
    int lowest = 255, highest = 0;
    for (const auto& p : px) {
        lowest = (std::min)(lowest, p[3]);
        highest = (std::max)(highest, p[3]);
    }
    if (lowest == highest) return block;
    const auto other = encode_mode5(px);
    std::uint8_t six[64], five[64];
    decode_bc7(other.data(), five);
    decode_bc7(block.data(), six);
    return block_error(px, five) < block_error(px, six) ? other : block;
}

// ---- BC4, BC1's colour, BC2 / BC3 ---------------------------------------

// One channel's 8 bits between its least and most in 8 levels.
void encode_bc4(const int* value, std::uint8_t* out) {
    int lo = 255, hi = 0;
    for (int i = 0; i < 16; ++i) {
        lo = (std::min)(lo, value[i]);
        hi = (std::max)(hi, value[i]);
    }
    out[0] = static_cast<std::uint8_t>(hi);
    out[1] = static_cast<std::uint8_t>(lo);
    std::uint64_t bits = 0;
    if (hi > lo) {
        int pal[8];
        pal[0] = hi;
        pal[1] = lo;
        for (int k = 2; k < 8; ++k) pal[k] = ((8 - k) * hi + (k - 1) * lo + 3) / 7;
        for (int i = 0; i < 16; ++i) {
            int chosen = 0;
            for (int k = 1; k < 8; ++k)
                if (std::abs(value[i] - pal[k]) < std::abs(value[i] - pal[chosen])) chosen = k;
            bits |= static_cast<std::uint64_t>(chosen) << (3 * i);
        }
    }
    for (int b = 0; b < 6; ++b) out[2 + b] = static_cast<std::uint8_t>(bits >> (8 * b));
}

// BC1's colour block of the texels (alpha left out: four levels; with
// `punch_through` a texel under 128 the transparent entry).
void encode_colour(const Texels& px, const bool punch_through, std::uint8_t* out) {
    std::uint8_t tile[16][4];
    for (int i = 0; i < 16; ++i)
        for (int ch = 0; ch < 4; ++ch) tile[i][ch] = static_cast<std::uint8_t>(punch_through || ch < 3 ? px[i][ch] : 255);
    const auto block = encode_dxt1_block(tile, punch_through);
    std::memcpy(out, block.data(), 8);
}

// An image's 4 x 4 blocks encoded by `encode` into `bytes` each, rows on `threads`.
template <class Encode>
std::vector<std::uint8_t> encode_each(const std::uint32_t width, const std::uint32_t height, const std::uint8_t* rgba, unsigned threads,
                                      const std::size_t bytes, const Encode& encode) {
    const std::uint32_t bx = (width + 3) / 4, by = (height + 3) / 4;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(bx) * by * bytes);
    if (width == 0 || height == 0 || !rgba) return out;
    if (threads == 0) threads = (std::max)(1u, std::thread::hardware_concurrency());
    threads = (std::min)(threads, by);
    std::atomic<std::uint32_t> next{0};
    const auto work = [&] {
        for (std::uint32_t row = next++; row < by; row = next++) {
            for (std::uint32_t col = 0; col < bx; ++col) {
                Texels px{};
                for (std::uint32_t i = 0; i < 16; ++i) {
                    const auto x = (std::min)(col * 4 + i % 4, width - 1);
                    const auto y = (std::min)(row * 4 + i / 4, height - 1);
                    const auto* p = rgba + (static_cast<std::size_t>(y) * width + x) * 4;
                    for (int ch = 0; ch < 4; ++ch) px[i][static_cast<std::size_t>(ch)] = p[ch];
                }
                encode(px, &out[(static_cast<std::size_t>(row) * bx + col) * bytes]);
            }
        }
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; ++t) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();
    return out;
}

} // namespace

std::string_view block_format_name(const BlockFormat format) noexcept {
    switch (format) {
    case BlockFormat::bc1: return "BC1 (DXT1)";
    case BlockFormat::bc2: return "BC2 (DXT3)";
    case BlockFormat::bc3: return "BC3 (DXT5)";
    case BlockFormat::bc4: return "BC4";
    case BlockFormat::bc5: return "BC5";
    case BlockFormat::bc7: return "BC7";
    }
    return "?";
}

int bc7_mode(const std::uint8_t* block) noexcept {
    int mode = 0;
    while (mode < 8 && !(block[0] & (1u << mode))) ++mode;
    return mode;
}

int bc7_part(const int parts, const int partition, const int texel) noexcept {
    if (parts == 2) return (partitions2[static_cast<std::size_t>(partition & 63)] >> texel) & 1;
    if (parts == 3) return partitions3[static_cast<std::size_t>(partition & 63)][texel] - '0';
    return 0;
}

bool bc7_anchor(const int parts, const int partition, const int texel) noexcept {
    if (texel == 0) return true;
    const auto p = static_cast<std::size_t>(partition & 63);
    if (parts == 2) return texel == anchors2[p];
    if (parts == 3) return texel == anchors3a[p] || texel == anchors3b[p];
    return false;
}

void decode_block(const BlockFormat format, const std::uint8_t* block, std::uint8_t* rgba) {
    switch (format) {
    case BlockFormat::bc1: decode_colour(block, false, rgba); return;
    case BlockFormat::bc2:
        decode_colour(block + 8, true, rgba);
        for (int i = 0; i < 16; ++i) {
            const int a = (block[i / 2] >> ((i & 1) * 4)) & 15;
            rgba[i * 4 + 3] = static_cast<std::uint8_t>(a * 17);
        }
        return;
    case BlockFormat::bc3:
        decode_colour(block + 8, true, rgba);
        decode_bc4(block, rgba, 3);
        return;
    case BlockFormat::bc4:
        for (int i = 0; i < 16; ++i) {
            rgba[i * 4 + 1] = rgba[i * 4 + 2] = 0;
            rgba[i * 4 + 3] = 255;
        }
        decode_bc4(block, rgba, 0);
        return;
    case BlockFormat::bc5:
        for (int i = 0; i < 16; ++i) {
            rgba[i * 4 + 2] = 0;
            rgba[i * 4 + 3] = 255;
        }
        decode_bc4(block, rgba, 0);
        decode_bc4(block + 8, rgba, 1);
        return;
    case BlockFormat::bc7: decode_bc7(block, rgba); return;
    }
}

std::vector<std::uint8_t> decode_blocks(const BlockFormat format, const std::uint32_t width, const std::uint32_t height,
                                        const std::uint8_t* blocks, const std::size_t size) {
    if (width == 0 || height == 0 || !blocks || size != level_block_bytes(format, width, height)) return {};
    std::vector<std::uint8_t> out(static_cast<std::size_t>(width) * height * 4);
    const std::uint32_t bx = (width + 3) / 4, by = (height + 3) / 4;
    const auto bytes = block_bytes(format);
    std::uint8_t texels[64];
    for (std::uint32_t row = 0; row < by; ++row) {
        for (std::uint32_t col = 0; col < bx; ++col) {
            decode_block(format, blocks + (static_cast<std::size_t>(row) * bx + col) * bytes, texels);
            for (std::uint32_t i = 0; i < 16; ++i) {
                const auto x = col * 4 + i % 4, y = row * 4 + i / 4;
                if (x >= width || y >= height) continue;
                std::memcpy(&out[(static_cast<std::size_t>(y) * width + x) * 4], &texels[i * 4], 4);
            }
        }
    }
    return out;
}

std::vector<std::uint8_t> encode_blocks(const BlockFormat format, const std::uint32_t width, const std::uint32_t height, const std::uint8_t* rgba,
                                        const unsigned threads) {
    const auto bytes = block_bytes(format);
    switch (format) {
    case BlockFormat::bc1:
        return encode_each(width, height, rgba, threads, bytes, [](const Texels& px, std::uint8_t* out) {
            bool clear = false;
            for (const auto& p : px) clear |= p[3] < 128;
            encode_colour(px, clear, out);
        });
    case BlockFormat::bc2:
        return encode_each(width, height, rgba, threads, bytes, [](const Texels& px, std::uint8_t* out) {
            std::memset(out, 0, 8);
            for (int i = 0; i < 16; ++i) {
                const int a = (px[i][3] * 15 + 127) / 255;
                out[i / 2] = static_cast<std::uint8_t>(out[i / 2] | (a << ((i & 1) * 4)));
            }
            encode_colour(px, false, out + 8);
        });
    case BlockFormat::bc3:
        return encode_each(width, height, rgba, threads, bytes, [](const Texels& px, std::uint8_t* out) {
            int alpha[16];
            for (int i = 0; i < 16; ++i) alpha[i] = px[i][3];
            encode_bc4(alpha, out);
            encode_colour(px, false, out + 8);
        });
    case BlockFormat::bc4:
        return encode_each(width, height, rgba, threads, bytes, [](const Texels& px, std::uint8_t* out) {
            int value[16];
            for (int i = 0; i < 16; ++i) value[i] = px[i][0];
            encode_bc4(value, out);
        });
    case BlockFormat::bc5:
        return encode_each(width, height, rgba, threads, bytes, [](const Texels& px, std::uint8_t* out) {
            int value[16];
            for (int ch = 0; ch < 2; ++ch) {
                for (int i = 0; i < 16; ++i) value[i] = px[i][static_cast<std::size_t>(ch)];
                encode_bc4(value, out + 8 * ch);
            }
        });
    case BlockFormat::bc7:
        return encode_each(width, height, rgba, threads, bytes, [](const Texels& px, std::uint8_t* out) {
            const auto block = encode_bc7_block(px);
            std::memcpy(out, block.data(), 16);
        });
    }
    return {};
}

} // namespace openrts::formats
