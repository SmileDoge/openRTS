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

// BC7's 4-bit index weights (of 64).
constexpr std::array<int, 16> weights4{0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

// The cache's files: what they are made with. Changing the smoothing or the
// encoder makes old files unused (their names change).
constexpr std::uint32_t cache_version = 1;
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
            t.error = fit_indices(px, pal, t.index);
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
    const std::uint32_t bx = (width + 3) / 4, by = (height + 3) / 4;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(bx) * by * 16);
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
                const auto block = encode_block(px);
                std::memcpy(&out[(static_cast<std::size_t>(row) * bx + col) * 16], block.data(), 16);
            }
        }
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; ++t) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();
    return out;
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

} // namespace openrts::renderer
