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
#include <openrts/formats/block_compression.hpp>

namespace openrts::renderer {
namespace {

// The cache's files: what they are made with. Changing the smoothing or the
// encoder makes old files unused (their names change).
constexpr std::uint32_t cache_version = 1; // mode 5 came later: older files (mode 6 only) still decode right
constexpr char cache_magic[4] = {'O', 'R', 'B', '7'};

// The smoothing: a step across a block edge of at most `step_most` (of 255,
// any channel) between sides whose texels by it differ by at most
// `flat_most` is taken for DXT1's, not the picture's.
constexpr int step_most = 32;
constexpr int flat_most = 14;

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
                                                                 const std::uint32_t height, const std::uint32_t block_bytes = 16) {
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
        levels[k].resize(static_cast<std::size_t>((w + 3) / 4) * ((h + 3) / 4) * block_bytes);
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
    return formats::encode_blocks(formats::BlockFormat::bc7, width, height, rgba, threads);
}

std::vector<std::uint8_t> encode_bc5(const std::uint32_t width, const std::uint32_t height, const std::uint8_t* rgba, unsigned threads) {
    return formats::encode_blocks(formats::BlockFormat::bc5, width, height, rgba, threads);
}

void decode_bc5_block(const std::uint8_t* block, std::uint8_t* rgba) {
    formats::decode_block(formats::BlockFormat::bc5, block, rgba);
}

bool decode_bc7_block(const std::uint8_t* block, std::uint8_t* rgba) {
    formats::decode_block(formats::BlockFormat::bc7, block, rgba);
    return formats::bc7_mode(block) < 8;
}

bool decode_bc7_mode6(const std::uint8_t* block, std::uint8_t* rgba) {
    if (formats::bc7_mode(block) != 6) return false;
    formats::decode_block(formats::BlockFormat::bc7, block, rgba);
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
    if (extension == ".bc7" || extension == ".r7" || extension == ".n5" || extension == ".h4") return true;
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
        const std::uint32_t tag[5] = {width, height, cache_version, format == BlockFormat::bc5 ? 1u : format == BlockFormat::bc4 ? 2u : 0u,
                                      static_cast<std::uint32_t>(levels_in.size())};
        hash = hash_words(reinterpret_cast<const std::uint8_t*>(tag), sizeof tag, hash);
        char name[32];
        std::snprintf(name, sizeof name, "%016llx.%s", static_cast<unsigned long long>(hash),
                      format == BlockFormat::bc5 ? "n5" : format == BlockFormat::bc4 ? "h4" : "r7");
        file = folder / name;
        if (auto cached = read_cache(file, width, height, formats::block_bytes(format))) return cached;
    }
    const auto encode = [&](const std::uint32_t w, const std::uint32_t h, const std::uint8_t* texels) {
        return formats::encode_blocks(format, w, h, texels);
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
