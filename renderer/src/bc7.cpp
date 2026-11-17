#include <openrts/renderer/bc7.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <system_error>
#include <thread>

#include <openrts/formats/ace_file.hpp>

namespace openrts::renderer {
namespace {

// BC7's 4-bit and 2-bit index weights (of 64).
constexpr std::array<int, 16> weights4{0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
constexpr std::array<int, 4> weights2{0, 21, 43, 64};

// The cache's files: what they are made with. Changing the smoothing or the
// encoder makes old files unused (their names change).
constexpr std::uint32_t cache_version = 1; // mode 5 came later: older files (mode 6 only) still decode right
constexpr char cache_magic[4] = {'O', 'R', 'B', '7'};

// The smoothing: a step across a block edge of at most `step_most` (of 255,
// any channel) between sides whose texels by it differ by at most
// `flat_most` is taken for DXT1's, not the picture's.
constexpr int step_most = 32;
constexpr int flat_most = 14;

struct Bits {
    std::array<std::uint8_t, 16> bytes{};
    int at = 0;
    void put(const std::uint32_t value, const int count) {
        for (int i = 0; i < count; ++i, ++at) {
            if ((value >> i) & 1u) bytes[static_cast<std::size_t>(at >> 3)] |= static_cast<std::uint8_t>(1u << (at & 7));
        }
    }
};

struct Trial {
    std::array<int, 4> q0{}, q1{}; // 7-bit endpoints
    int p0 = 0, p1 = 0;            // their low bits
    std::array<std::uint8_t, 16> index{};
    long long error = LLONG_MAX;
};

using Texels = std::array<std::array<int, 4>, 16>;

void palette(const int* c0, const int* c1, int out[16][4]) {
    for (int k = 0; k < 16; ++k) {
        for (int ch = 0; ch < 4; ++ch) out[k][ch] = ((64 - weights4[k]) * c0[ch] + weights4[k] * c1[ch] + 32) >> 6;
    }
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

// The same, each texel's level from where it falls along the line between
// the two colours, it and the levels either side tried (the palette's
// rounding moves a level a little off the line): a fifth of the work.
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
            palette(c0, c1, pal);
            t.error = fit_indices_on_line(px, c0, c1, pal, t.index);
            if (t.error < best.error) best = t;
        }
    }
}

std::array<std::uint8_t, 16> encode_block(const Texels& px) {
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

// How far a block's texels are from what it decodes to: a texel's colour by
// how opaque it is (a clear one's barely shows, drawn or blended), its
// alpha twice an opaque colour's channel (where the edge of a leaf falls).
long long block_error(const Texels& px, const std::uint8_t* rgba) {
    long long total = 0;
    for (int i = 0; i < 16; ++i) {
        const long long weight = px[i][3] + 8;
        for (int ch = 0; ch < 3; ++ch) {
            const long long d = px[i][ch] - rgba[i * 4 + ch];
            total += weight * d * d;
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
std::array<std::uint8_t, 16> encode_block_mode5(const Texels& px) {
    // The colour's line through the texels, each by how opaque it is.
    float weight[16];
    float total = 0.0f, mean[3]{};
    for (int i = 0; i < 16; ++i) {
        weight[i] = static_cast<float>(px[i][3] + 8);
        total += weight[i];
        for (int ch = 0; ch < 3; ++ch) mean[ch] += weight[i] * static_cast<float>(px[i][ch]);
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
            for (int c = 0; c < 3; ++c) cov[r][c] += weight[i] * d[r] * d[c];
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
            for (int ch = 0; ch < 3; ++ch) pal[k][ch] = ((64 - weights2[k]) * e0[ch] + weights2[k] * e1[ch] + 32) >> 6;
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
            c.error += static_cast<long long>(weight[i]) * best;
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
            aa += weight[i] * u * u;
            ab += weight[i] * u * w;
            bb += weight[i] * w * w;
            for (int ch = 0; ch < 3; ++ch) {
                ax[ch] += weight[i] * u * static_cast<float>(px[i][ch]);
                bx[ch] += weight[i] * w * static_cast<float>(px[i][ch]);
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
        for (int k = 0; k < 4; ++k) pal[k] = ((64 - weights2[k]) * a0 + weights2[k] * a1 + 32) >> 6;
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

bool decode_mode5(const std::uint8_t* block, std::uint8_t* rgba) {
    int at = 0;
    const auto get = [&](const int count) {
        std::uint32_t v = 0;
        for (int i = 0; i < count; ++i, ++at) v |= static_cast<std::uint32_t>((block[at >> 3] >> (at & 7)) & 1u) << i;
        return v;
    };
    if (get(6) != (1u << 5)) return false;
    const auto rotation = get(2);
    int e[2][4];
    for (int ch = 0; ch < 3; ++ch) {
        e[0][ch] = widen7(static_cast<int>(get(7)));
        e[1][ch] = widen7(static_cast<int>(get(7)));
    }
    e[0][3] = static_cast<int>(get(8));
    e[1][3] = static_cast<int>(get(8));
    int colour[16], alpha[16];
    for (int i = 0; i < 16; ++i) colour[i] = static_cast<int>(get(i == 0 ? 1 : 2));
    for (int i = 0; i < 16; ++i) alpha[i] = static_cast<int>(get(i == 0 ? 1 : 2));
    for (int i = 0; i < 16; ++i) {
        for (int ch = 0; ch < 4; ++ch) {
            const int w = weights2[static_cast<std::size_t>(ch == 3 ? alpha[i] : colour[i])];
            rgba[i * 4 + ch] = static_cast<std::uint8_t>(((64 - w) * e[0][ch] + w * e[1][ch] + 32) >> 6);
        }
        if (rotation != 0) std::swap(rgba[i * 4 + 3], rgba[i * 4 + static_cast<int>(rotation) - 1]);
    }
    return true;
}

// The block in BC7: mode 6, or mode 5 where the alpha varies and it comes nearer.
std::array<std::uint8_t, 16> encode_bc7_block(const Texels& px) {
    auto block = encode_block(px);
    int lowest = 255, highest = 0;
    for (const auto& p : px) {
        lowest = (std::min)(lowest, p[3]);
        highest = (std::max)(highest, p[3]);
    }
    if (lowest == highest) return block;
    const auto other = encode_block_mode5(px);
    std::uint8_t six[64], five[64];
    if (!decode_mode5(other.data(), five)) return block;
    if (!decode_bc7_mode6(block.data(), six)) return other;
    return block_error(px, five) < block_error(px, six) ? other : block;
}

// BC4: one channel's 8 bits between its least and most in 8 levels.
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

// An image's 4 x 4 blocks (a side not of whole blocks repeats its last
// texel) encoded by `encode` into `block_bytes` each, rows on `threads`.
template <class Encode>
std::vector<std::uint8_t> encode_blocks(const std::uint32_t width, const std::uint32_t height, const std::uint8_t* rgba, unsigned threads,
                                        const std::size_t block_bytes, const Encode& encode) {
    const std::uint32_t bx = (width + 3) / 4, by = (height + 3) / 4;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(bx) * by * block_bytes);
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
                encode(px, &out[(static_cast<std::size_t>(row) * bx + col) * block_bytes]);
            }
        }
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; ++t) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();
    return out;
}

// A fast hash of bytes (8 at a time): the uncompressed textures' cache key.
[[nodiscard]] std::uint64_t hash_words(const std::uint8_t* data, const std::size_t size, std::uint64_t hash) {
    std::size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        std::uint64_t v;
        std::memcpy(&v, data + i, 8);
        hash ^= v * 0x9E3779B97F4A7C15ull;
        hash = ((hash << 27) | (hash >> 37)) * 0xC2B2AE3D27D4EB4Full;
    }
    for (; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ull;
    }
    return hash ^ (hash >> 31);
}

[[nodiscard]] std::uint64_t fnv1a(const std::uint8_t* data, const std::size_t size, std::uint64_t hash = 1469598103934665603ull) {
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

std::optional<std::vector<std::vector<std::uint8_t>>> read_cache(const std::filesystem::path& file, const std::uint32_t width,
                                                                 const std::uint32_t height) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    char magic[4]{};
    std::uint32_t head[4]{};
    in.read(magic, 4);
    in.read(reinterpret_cast<char*>(head), sizeof head);
    if (!in || std::memcmp(magic, cache_magic, 4) != 0 || head[0] != cache_version || head[1] != width || head[2] != height || head[3] == 0 ||
        head[3] > 16)
        return std::nullopt;
    std::vector<std::vector<std::uint8_t>> levels(head[3]);
    for (std::uint32_t k = 0; k < head[3]; ++k) {
        const auto w = (std::max)(width >> k, 1u), h = (std::max)(height >> k, 1u);
        levels[k].resize(static_cast<std::size_t>((w + 3) / 4) * ((h + 3) / 4) * 16);
        in.read(reinterpret_cast<char*>(levels[k].data()), static_cast<std::streamsize>(levels[k].size()));
        if (!in) return std::nullopt;
    }
    return levels;
}

void write_cache(const std::filesystem::path& file, const std::uint32_t width, const std::uint32_t height,
                 const std::vector<std::vector<std::uint8_t>>& levels) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    // Written aside and renamed: a reader never takes half a file.
    auto temporary = file;
    temporary += ".part" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return;
        const std::uint32_t head[4] = {cache_version, width, height, static_cast<std::uint32_t>(levels.size())};
        out.write(cache_magic, 4);
        out.write(reinterpret_cast<const char*>(head), sizeof head);
        for (const auto& level : levels) out.write(reinterpret_cast<const char*>(level.data()), static_cast<std::streamsize>(level.size()));
        if (!out) {
            out.close();
            std::filesystem::remove(temporary, ec);
            return;
        }
    }
    std::filesystem::rename(temporary, file, ec);
    if (ec) std::filesystem::remove(temporary, ec);
}

} // namespace

void deblock_dxt1(const std::uint32_t width, const std::uint32_t height, std::vector<std::uint8_t>& rgba) {
    if (rgba.size() != static_cast<std::size_t>(width) * height * 4) return;
    const auto at = [&](const std::uint32_t x, const std::uint32_t y) { return &rgba[(static_cast<std::size_t>(y) * width + x) * 4]; };
    const auto ramp = [](std::uint8_t* p1, std::uint8_t* p0, std::uint8_t* q0, std::uint8_t* q1) {
        if (p1[3] < 128 || p0[3] < 128 || q0[3] < 128 || q1[3] < 128) return;
        int step = 0, flat = 0;
        for (int ch = 0; ch < 3; ++ch) {
            step = (std::max)(step, std::abs(q0[ch] - p0[ch]));
            flat = (std::max)(flat, (std::max)(std::abs(p1[ch] - p0[ch]), std::abs(q1[ch] - q0[ch])));
        }
        if (step == 0 || step > step_most || flat > flat_most) return;
        const auto share = [](const int d, const int eighths) { return (d * eighths + (d >= 0 ? 4 : -4)) / 8; };
        for (int ch = 0; ch < 3; ++ch) {
            const int d = q0[ch] - p0[ch];
            p1[ch] = static_cast<std::uint8_t>(std::clamp(p1[ch] + share(d, 1), 0, 255));
            p0[ch] = static_cast<std::uint8_t>(std::clamp(p0[ch] + share(d, 3), 0, 255));
            q0[ch] = static_cast<std::uint8_t>(std::clamp(q0[ch] - share(d, 3), 0, 255));
            q1[ch] = static_cast<std::uint8_t>(std::clamp(q1[ch] - share(d, 1), 0, 255));
        }
    };
    // Flat blocks (a dark shade DXT1 left at one or two levels a block: the
    // quilt of squares) become a smooth surface: each texel takes the means
    // of the blocks about it, bilinearly between their centres, of those
    // flat as well and near its own in colour (else its own block's), its
    // own small grain kept.
    const std::uint32_t bw = width / 4, bh = height / 4;
    if (bw >= 2 && bh >= 2) {
        std::vector<std::array<float, 3>> mean(static_cast<std::size_t>(bw) * bh);
        std::vector<std::uint8_t> flat(static_cast<std::size_t>(bw) * bh);
        for (std::uint32_t by = 0; by < bh; ++by) {
            for (std::uint32_t bx = 0; bx < bw; ++bx) {
                int low[3] = {255, 255, 255}, high[3] = {0, 0, 0}, sum[3] = {0, 0, 0}, alpha = 255;
                for (std::uint32_t i = 0; i < 16; ++i) {
                    const auto* p = at(bx * 4 + i % 4, by * 4 + i / 4);
                    alpha = (std::min)(alpha, static_cast<int>(p[3]));
                    for (int ch = 0; ch < 3; ++ch) {
                        low[ch] = (std::min)(low[ch], static_cast<int>(p[ch]));
                        high[ch] = (std::max)(high[ch], static_cast<int>(p[ch]));
                        sum[ch] += p[ch];
                    }
                }
                const auto b = static_cast<std::size_t>(by) * bw + bx;
                int range = 0;
                for (int ch = 0; ch < 3; ++ch) {
                    mean[b][ch] = static_cast<float>(sum[ch]) / 16.0f;
                    range = (std::max)(range, high[ch] - low[ch]);
                }
                flat[b] = range <= flat_most && alpha >= 128 ? 1 : 0;
            }
        }
        auto out = rgba;
        for (std::uint32_t by = 0; by < bh; ++by) {
            for (std::uint32_t bx = 0; bx < bw; ++bx) {
                const auto b = static_cast<std::size_t>(by) * bw + bx;
                if (!flat[b]) continue;
                const auto& own = mean[b];
                const auto sample = [&](const int sx, const int sy) -> const std::array<float, 3>& {
                    if (sx < 0 || sy < 0 || sx >= static_cast<int>(bw) || sy >= static_cast<int>(bh)) return own;
                    const auto n = static_cast<std::size_t>(sy) * bw + static_cast<std::size_t>(sx);
                    if (!flat[n]) return own;
                    for (int ch = 0; ch < 3; ++ch)
                        if (std::abs(mean[n][ch] - own[ch]) > static_cast<float>(step_most)) return own;
                    return mean[n];
                };
                for (std::uint32_t i = 0; i < 16; ++i) {
                    const auto x = bx * 4 + i % 4, y = by * 4 + i / 4;
                    const float fx = (static_cast<float>(x) + 0.5f) / 4.0f - 0.5f, fy = (static_cast<float>(y) + 0.5f) / 4.0f - 0.5f;
                    const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
                    const float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0);
                    const auto& m00 = sample(x0, y0);
                    const auto& m10 = sample(x0 + 1, y0);
                    const auto& m01 = sample(x0, y0 + 1);
                    const auto& m11 = sample(x0 + 1, y0 + 1);
                    auto* p = &out[(static_cast<std::size_t>(y) * width + x) * 4];
                    const auto* source = at(x, y);
                    for (int ch = 0; ch < 3; ++ch) {
                        const float m = (m00[ch] * (1.0f - tx) + m10[ch] * tx) * (1.0f - ty) + (m01[ch] * (1.0f - tx) + m11[ch] * tx) * ty;
                        p[ch] = static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(static_cast<float>(source[ch]) - own[ch] + m)), 0, 255));
                    }
                }
            }
        }
        rgba = std::move(out);
    }
    // Across the edges between columns of blocks, then between rows.
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 4; x + 1 < width; x += 4) ramp(at(x - 2, y), at(x - 1, y), at(x, y), at(x + 1, y));
    for (std::uint32_t y = 4; y + 1 < height; y += 4)
        for (std::uint32_t x = 0; x < width; ++x) ramp(at(x, y - 2), at(x, y - 1), at(x, y), at(x, y + 1));
}

std::vector<std::uint8_t> encode_bc7(const std::uint32_t width, const std::uint32_t height, const std::uint8_t* rgba, unsigned threads) {
    return encode_blocks(width, height, rgba, threads, 16, [](const Texels& px, std::uint8_t* out) {
        const auto block = encode_bc7_block(px);
        std::memcpy(out, block.data(), 16);
    });
}

std::vector<std::uint8_t> encode_bc5(const std::uint32_t width, const std::uint32_t height, const std::uint8_t* rgba, unsigned threads) {
    return encode_blocks(width, height, rgba, threads, 16, [](const Texels& px, std::uint8_t* out) {
        int value[16];
        for (int ch = 0; ch < 2; ++ch) {
            for (int i = 0; i < 16; ++i) value[i] = px[i][static_cast<std::size_t>(ch)];
            encode_bc4(value, out + 8 * ch);
        }
    });
}

void decode_bc5_block(const std::uint8_t* block, std::uint8_t* rgba) {
    decode_bc4(block, rgba, 0);
    decode_bc4(block + 8, rgba, 1);
    for (int i = 0; i < 16; ++i) {
        rgba[i * 4 + 2] = 0;
        rgba[i * 4 + 3] = 255;
    }
}

bool decode_bc7_block(const std::uint8_t* block, std::uint8_t* rgba) {
    if ((block[0] & 0x7Fu) == 0x40u) return decode_bc7_mode6(block, rgba);
    if ((block[0] & 0x3Fu) == 0x20u) return decode_mode5(block, rgba);
    return false;
}

bool decode_bc7_mode6(const std::uint8_t* block, std::uint8_t* rgba) {
    int at = 0;
    const auto get = [&](const int count) {
        std::uint32_t v = 0;
        for (int i = 0; i < count; ++i, ++at) v |= static_cast<std::uint32_t>((block[at >> 3] >> (at & 7)) & 1u) << i;
        return v;
    };
    if (get(7) != (1u << 6)) return false;
    int q[2][4];
    for (int ch = 0; ch < 4; ++ch) {
        q[0][ch] = static_cast<int>(get(7));
        q[1][ch] = static_cast<int>(get(7));
    }
    const int p0 = static_cast<int>(get(1)), p1 = static_cast<int>(get(1));
    int c0[4], c1[4];
    for (int ch = 0; ch < 4; ++ch) {
        c0[ch] = (q[0][ch] << 1) | p0;
        c1[ch] = (q[1][ch] << 1) | p1;
    }
    int pal[16][4];
    palette(c0, c1, pal);
    for (int i = 0; i < 16; ++i) {
        const auto k = static_cast<int>(get(i == 0 ? 3 : 4));
        for (int ch = 0; ch < 4; ++ch) rgba[i * 4 + ch] = static_cast<std::uint8_t>(pal[k][ch]);
    }
    return true;
}

std::optional<std::vector<std::vector<std::uint8_t>>> smoothed_bc7_levels(const std::uint32_t width, const std::uint32_t height,
                                                                          const std::vector<std::uint8_t>& rgba,
                                                                          const std::vector<std::uint8_t>& dxt1_level0) {
    if (width == 0 || height == 0 || width % 4 != 0 || height % 4 != 0 || rgba.size() != static_cast<std::size_t>(width) * height * 4)
        return std::nullopt;
    std::filesystem::path file;
    if (const auto& folder = texture_cache_directory(); !folder.empty()) {
        auto hash = fnv1a(dxt1_level0.data(), dxt1_level0.size());
        const std::uint32_t tag[3] = {width, height, cache_version};
        hash = fnv1a(reinterpret_cast<const std::uint8_t*>(tag), sizeof tag, hash);
        char name[32];
        std::snprintf(name, sizeof name, "%016llx.bc7", static_cast<unsigned long long>(hash));
        file = folder / name;
        if (auto cached = read_cache(file, width, height)) return cached;
    }
    auto smoothed = rgba;
    deblock_dxt1(width, height, smoothed);
    std::vector<std::vector<std::uint8_t>> levels;
    levels.push_back(encode_bc7(width, height, smoothed.data()));
    const auto mips = formats::build_rgba_mipmaps(width, height, smoothed);
    for (std::size_t k = 0; k < mips.size(); ++k) {
        const auto w = (std::max)(width >> (k + 1), 1u), h = (std::max)(height >> (k + 1), 1u);
        if (mips[k].size() != static_cast<std::size_t>(w) * h * 4) break;
        levels.push_back(encode_bc7(w, h, mips[k].data()));
    }
    if (!file.empty()) write_cache(file, width, height, levels);
    return levels;
}

namespace {

// One of the cache's own files (not anything else put in the folder).
[[nodiscard]] bool cache_file(const std::filesystem::path& file) {
    const auto extension = file.extension().string();
    if (extension == ".bc7" || extension == ".r7" || extension == ".n5") return true;
    return extension.rfind(".part", 0) == 0;
}

template <class Each>
TextureCacheSize each_cache_file(const std::filesystem::path& folder, const Each& each) {
    TextureCacheSize size;
    std::error_code ec;
    if (folder.empty() || !std::filesystem::is_directory(folder, ec)) return size;
    for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || !cache_file(it->path())) continue;
        const auto bytes = it->file_size(ec);
        if (ec) {
            ec.clear();
            continue;
        }
        if (each(it->path())) {
            ++size.files;
            size.bytes += bytes;
        }
    }
    return size;
}

} // namespace

TextureCacheSize texture_cache_size(const std::filesystem::path& folder) {
    return each_cache_file(folder, [](const std::filesystem::path&) { return true; });
}

TextureCacheSize clear_texture_cache(const std::filesystem::path& folder) {
    return each_cache_file(folder, [](const std::filesystem::path& file) {
        std::error_code ec;
        return std::filesystem::remove(file, ec) && !ec;
    });
}

std::optional<std::vector<std::vector<std::uint8_t>>> compressed_levels(const std::uint32_t width, const std::uint32_t height,
                                                                        const std::vector<std::uint8_t>& rgba,
                                                                        const std::vector<std::vector<std::uint8_t>>& mips,
                                                                        const BlockFormat format, const bool disk) {
    if (width == 0 || height == 0 || width % 4 != 0 || height % 4 != 0 || rgba.size() != static_cast<std::size_t>(width) * height * 4)
        return std::nullopt;
    std::vector<std::vector<std::uint8_t>> built;
    const auto& levels_in = mips.empty() ? (built = formats::build_rgba_mipmaps(width, height, rgba)) : mips;
    std::filesystem::path file;
    if (const auto& folder = texture_cache_directory(); disk && !folder.empty()) {
        auto hash = hash_words(rgba.data(), rgba.size(), 1469598103934665603ull);
        for (const auto& level : levels_in) hash = hash_words(level.data(), level.size(), hash);
        const std::uint32_t tag[5] = {width, height, cache_version, static_cast<std::uint32_t>(format),
                                      static_cast<std::uint32_t>(levels_in.size())};
        hash = hash_words(reinterpret_cast<const std::uint8_t*>(tag), sizeof tag, hash);
        char name[32];
        std::snprintf(name, sizeof name, "%016llx.%s", static_cast<unsigned long long>(hash), format == BlockFormat::bc5 ? "n5" : "r7");
        file = folder / name;
        if (auto cached = read_cache(file, width, height)) return cached;
    }
    const auto encode = [&](const std::uint32_t w, const std::uint32_t h, const std::uint8_t* texels) {
        return format == BlockFormat::bc5 ? encode_bc5(w, h, texels) : encode_bc7(w, h, texels);
    };
    std::vector<std::vector<std::uint8_t>> levels;
    levels.push_back(encode(width, height, rgba.data()));
    for (std::size_t k = 0; k < levels_in.size(); ++k) {
        const auto w = (std::max)(width >> (k + 1), 1u), h = (std::max)(height >> (k + 1), 1u);
        if (levels_in[k].size() != static_cast<std::size_t>(w) * h * 4) break;
        levels.push_back(encode(w, h, levels_in[k].data()));
        if (w == 1 && h == 1) break;
    }
    if (!file.empty()) write_cache(file, width, height, levels);
    return levels;
}

} // namespace openrts::renderer
