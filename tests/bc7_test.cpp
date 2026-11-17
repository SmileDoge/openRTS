// The trains' textures without DXT1's squares (renderer/bc7.hpp): BC7
// mode 6 back within a few levels, the smoothing of block edges (a real edge
// left), and on an installation's 2ES6 body the blocks' edges brought down
// to the texture's own grain.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include <openrts/formats/ace_file.hpp>
#include <openrts/renderer/bc7.hpp>

namespace {

using namespace openrts;
int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

// The image back from its BC7 blocks.
std::vector<std::uint8_t> decode(const std::uint32_t w, const std::uint32_t h, const std::vector<std::uint8_t>& blocks) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * h * 4);
    const std::uint32_t bx = (w + 3) / 4;
    for (std::uint32_t by = 0; by < (h + 3) / 4; ++by) {
        for (std::uint32_t b = 0; b < bx; ++b) {
            std::uint8_t texels[64];
            if (!renderer::decode_bc7_mode6(&blocks[(static_cast<std::size_t>(by) * bx + b) * 16], texels)) return {};
            for (std::uint32_t i = 0; i < 16; ++i) {
                const auto x = b * 4 + i % 4, y = by * 4 + i / 4;
                if (x >= w || y >= h) continue;
                for (int ch = 0; ch < 4; ++ch) out[(static_cast<std::size_t>(y) * w + x) * 4 + ch] = texels[i * 4 + ch];
            }
        }
    }
    return out;
}

double psnr(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    double sum = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) sum += (double(a[i]) - b[i]) * (double(a[i]) - b[i]);
    const double mse = sum / static_cast<double>(a.size());
    return mse <= 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

// Hard steps where 4 x 4 blocks meet in dark flat shade (DXT1's quilt):
// a step of 2 levels or more between texels each level with the one
// before it inside their block (luma under `darker_than`).
std::size_t quilt_steps(const std::uint32_t w, const std::uint32_t h, const std::vector<std::uint8_t>& rgba, const int darker_than = 256,
                        const int least = 2) {
    const auto luma = [&](const std::uint32_t x, const std::uint32_t y) {
        const auto* p = &rgba[(static_cast<std::size_t>(y) * w + x) * 4];
        return static_cast<int>(std::lround(0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]));
    };
    std::size_t steps = 0;
    const auto edge = [&](const int p1, const int p0, const int q0, const int q1) {
        if (p0 >= darker_than && q0 >= darker_than) return;
        if (std::abs(q0 - p0) >= least && std::abs(p1 - p0) <= 1 && std::abs(q1 - q0) <= 1) ++steps;
    };
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 4; x + 1 < w; x += 4) edge(luma(x - 2, y), luma(x - 1, y), luma(x, y), luma(x + 1, y));
    for (std::uint32_t y = 4; y + 1 < h; y += 4)
        for (std::uint32_t x = 0; x < w; ++x) edge(luma(x, y - 2), luma(x, y - 1), luma(x, y), luma(x, y + 1));
    return steps;
}

void codec() {
    // A smooth gradient with a little grain and alpha.
    const std::uint32_t w = 64, h = 64;
    std::vector<std::uint8_t> image(w * h * 4);
    std::srand(7);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            auto* p = &image[(y * w + x) * 4];
            p[0] = static_cast<std::uint8_t>(20 + x * 2 + std::rand() % 3);
            p[1] = static_cast<std::uint8_t>(10 + y * 3);
            p[2] = static_cast<std::uint8_t>(200 - x - y);
            p[3] = 255;
        }
    }
    const auto blocks = renderer::encode_bc7(w, h, image.data());
    check(blocks.size() == (w / 4) * (h / 4) * 16, "a 16-byte block for each 4 x 4");
    const auto back = decode(w, h, blocks);
    check(!back.empty(), "every block decodes as mode 6");
    const double quality = psnr(image, back);
    std::cout << "bc7: gradient " << quality << " dB\n";
    check(quality > 42.0, "a smooth gradient back within a level or two (" + std::to_string(quality) + " dB)");
    // One colour: back within a level.
    std::vector<std::uint8_t> flat(16 * 4);
    for (int i = 0; i < 16; ++i) {
        flat[i * 4] = 37;
        flat[i * 4 + 1] = 141;
        flat[i * 4 + 2] = 222;
        flat[i * 4 + 3] = 255;
    }
    const auto one = renderer::encode_bc7(4, 4, flat.data());
    std::uint8_t texels[64];
    check(renderer::decode_bc7_mode6(one.data(), texels) && std::abs(texels[0] - 37) <= 1 && std::abs(texels[1] - 141) <= 1 &&
              std::abs(texels[2] - 222) <= 1 && texels[3] == 255,
          "one colour back within a level");
    // A side not of whole blocks repeats its last texel.
    const auto odd = renderer::encode_bc7(3, 2, image.data());
    check(odd.size() == 16, "a 3 x 2 image is one block");
}

void smoothing() {
    // Dark blocks a step apart, as DXT1 leaves a dark gradient; a real edge.
    const std::uint32_t w = 32, h = 8;
    std::vector<std::uint8_t> image(w * h * 4, 255);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            auto* p = &image[(y * w + x) * 4];
            const std::uint8_t v = x < 24 ? static_cast<std::uint8_t>(20 + (x / 4) * 8) : static_cast<std::uint8_t>(200);
            p[0] = p[1] = p[2] = v;
        }
    }
    const auto before = quilt_steps(w, h, image);
    auto smoothed = image;
    renderer::deblock_dxt1(w, h, smoothed);
    const auto after = quilt_steps(w, h, smoothed);
    std::cout << "deblock: hard steps " << before << " -> " << after << "\n";
    check(before > 0 && after * 4 <= before, "the blocks' hard steps smoothed");
    const auto value = [&](const std::uint32_t x) { return int(smoothed[(2 * w + x) * 4]); };
    check(value(23) < 80 && value(24) == 200, "a real edge (60 to 200) left");
    // A texel with no alpha is left.
    image[(0 * w + 3) * 4 + 3] = 0;
    auto masked = image;
    renderer::deblock_dxt1(w, h, masked);
    check(masked[(0 * w + 3) * 4] == image[(0 * w + 3) * 4], "a transparent texel is not smoothed");
}

void content(const std::filesystem::path& root) {
    const auto file = root / "TRAINS" / "TRAINSET" / "mdd_2ES6-162" / "kuz1a.ace";
    if (!std::filesystem::exists(file)) {
        std::cout << "no " << file.string() << ": skipped\n";
        return;
    }
    const auto decoded = formats::decode_structured_ace_rgba_path(file);
    check(decoded.ok(), "the 2ES6's body decodes");
    if (!decoded.ok()) return;
    const auto& image = *decoded.image;
    check(!image.bc1.empty(), "it is DXT1");
    // Steps of 3 levels and more: DXT1's are 4 to 8 a channel; BC7's own
    // rounding leaves single levels, below what a display shows in the dark.
    const auto before = quilt_steps(image.width, image.height, image.pixels, 60, 3);
    auto smoothed = image.pixels;
    renderer::deblock_dxt1(image.width, image.height, smoothed);
    const auto after = quilt_steps(image.width, image.height, smoothed, 60, 3);
    const auto blocks = renderer::encode_bc7(image.width, image.height, smoothed.data());
    const auto back = decode(image.width, image.height, blocks);
    const auto encoded = quilt_steps(image.width, image.height, back, 60, 3);
    std::cout << "  (steps of 2: " << quilt_steps(image.width, image.height, image.pixels, 60) << " -> "
              << quilt_steps(image.width, image.height, back, 60) << " in BC7)\n";
    const double changed = psnr(image.pixels, smoothed);
    const double quality = psnr(smoothed, back);
    std::cout << "2ES6 kuz1a (dark): hard block steps " << before << " -> " << after << " smoothed, " << encoded << " in BC7; smoothed "
              << changed << " dB from the DXT1, BC7 " << quality << " dB from the smoothed\n";
    check(after * 3 <= before, "the dark quilt's hard steps mostly gone");
    check(encoded * 3 <= before && quality > 38.0, "BC7 keeps it");
    check(changed > 30.0, "the texture otherwise as it was");
}

// The uncompressed textures' encoders (compressed_levels): mode 5 where a
// block's alpha varies (a leaf's edge: clear by opaque, the alpha kept
// exact), BC5 of a normal map's x and y; and how fast they go.
void uncompressed() {
    const std::uint32_t w = 256, h = 256;
    std::mt19937 random(7);
    std::vector<std::uint8_t> leaves(static_cast<std::size_t>(w) * h * 4), normals(leaves.size());
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            auto* p = &leaves[(static_cast<std::size_t>(y) * w + x) * 4];
            // Green leaves of some shades, clear (black) between them.
            const bool leaf = ((x / 3 + y / 5 + (x * y) % 7) % 3) != 0;
            p[0] = static_cast<std::uint8_t>(leaf ? 40 + random() % 30 : 0);
            p[1] = static_cast<std::uint8_t>(leaf ? 90 + random() % 60 : 0);
            p[2] = static_cast<std::uint8_t>(leaf ? 20 + random() % 20 : 0);
            p[3] = leaf ? 255 : 0;
            // A bumpy normal map: x and y of a slope that turns.
            auto* n = &normals[(static_cast<std::size_t>(y) * w + x) * 4];
            n[0] = static_cast<std::uint8_t>(128 + 100 * std::sin(x * 0.11) * std::cos(y * 0.07));
            n[1] = static_cast<std::uint8_t>(128 + 100 * std::cos(x * 0.05 + y * 0.13));
            n[2] = 255;
            n[3] = 255;
        }
    }
    const auto blocks = renderer::encode_bc7(w, h, leaves.data());
    std::vector<std::uint8_t> back(leaves.size());
    std::size_t mode5 = 0, alpha_wrong = 0;
    bool decoded = true;
    for (std::uint32_t by = 0; by < h / 4; ++by) {
        for (std::uint32_t bx = 0; bx < w / 4; ++bx) {
            const auto* block = &blocks[(static_cast<std::size_t>(by) * (w / 4) + bx) * 16];
            if ((block[0] & 0x3Fu) == 0x20u) ++mode5;
            std::uint8_t texels[64];
            if (!renderer::decode_bc7_block(block, texels)) decoded = false;
            for (std::uint32_t i = 0; i < 16; ++i) {
                const auto at = ((static_cast<std::size_t>(by) * 4 + i / 4) * w + bx * 4 + i % 4) * 4;
                for (int ch = 0; ch < 4; ++ch) back[at + ch] = texels[i * 4 + ch];
                if ((back[at + 3] >= 128) != (leaves[at + 3] >= 128)) ++alpha_wrong;
            }
        }
    }
    check(decoded, "every BC7 block of the leaves decodes (mode 5 or 6)");
    check(mode5 > 0, "blocks with clear and opaque texels take mode 5");
    check(alpha_wrong == 0, "the leaves' alpha test keeps every texel (" + std::to_string(alpha_wrong) + " flipped)");
    // The colour where it shows (opaque texels).
    double sum = 0.0;
    std::size_t count = 0;
    for (std::size_t i = 0; i < leaves.size(); i += 4) {
        if (leaves[i + 3] < 128) continue;
        for (int ch = 0; ch < 3; ++ch) sum += (double(leaves[i + ch]) - back[i + ch]) * (double(leaves[i + ch]) - back[i + ch]);
        count += 3;
    }
    const double leaf_psnr = 10.0 * std::log10(255.0 * 255.0 / (std::max)(sum / static_cast<double>(count), 1e-9));
    check(leaf_psnr > 30.0, "the leaves' colour within 30 dB (" + std::to_string(leaf_psnr) + ")");

    const auto bc5 = renderer::encode_bc5(w, h, normals.data());
    double nsum = 0.0;
    for (std::uint32_t by = 0; by < h / 4; ++by) {
        for (std::uint32_t bx = 0; bx < w / 4; ++bx) {
            std::uint8_t texels[64];
            renderer::decode_bc5_block(&bc5[(static_cast<std::size_t>(by) * (w / 4) + bx) * 16], texels);
            for (std::uint32_t i = 0; i < 16; ++i) {
                const auto at = ((static_cast<std::size_t>(by) * 4 + i / 4) * w + bx * 4 + i % 4) * 4;
                for (int ch = 0; ch < 2; ++ch) nsum += (double(normals[at + ch]) - texels[i * 4 + ch]) * (double(normals[at + ch]) - texels[i * 4 + ch]);
            }
        }
    }
    const double normal_psnr = 10.0 * std::log10(255.0 * 255.0 / (std::max)(nsum / (w * h * 2.0), 1e-9));
    check(normal_psnr > 40.0, "BC5 keeps a normal map's x and y within 40 dB (" + std::to_string(normal_psnr) + ")");

    // How fast: blocks a second on one thread.
    const auto started = std::chrono::steady_clock::now();
    const auto timed = renderer::encode_bc7(w, h, leaves.data(), 1);
    const auto opaque_started = std::chrono::steady_clock::now();
    std::vector<std::uint8_t> opaque = leaves;
    for (std::size_t i = 3; i < opaque.size(); i += 4) opaque[i] = 255;
    const auto timed_opaque = renderer::encode_bc7(w, h, opaque.data(), 1);
    const auto ended = std::chrono::steady_clock::now();
    const double leaf_us = std::chrono::duration<double, std::micro>(opaque_started - started).count() / (w * h / 16.0);
    const double opaque_us = std::chrono::duration<double, std::micro>(ended - opaque_started).count() / (w * h / 16.0);
    std::cout << "bc7: " << leaf_us << " us a block with alpha, " << opaque_us << " us opaque (one thread); " << mode5 << " of "
              << w * h / 16 << " blocks in mode 5; leaves " << leaf_psnr << " dB, normals (BC5) " << normal_psnr << " dB\n";
    (void)timed;
    (void)timed_opaque;
}

// The disk cache counted and cleared: its own files only.
void cache_folder() {
    const auto folder = std::filesystem::temp_directory_path() / "openrts_bc7_cache_test";
    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
    std::filesystem::create_directories(folder, ec);
    const auto put = [&](const char* name, const std::size_t bytes) {
        std::ofstream(folder / name, std::ios::binary) << std::string(bytes, 'x');
    };
    put("a.r7", 100);
    put("b.n5", 50);
    put("c.bc7", 25);
    put("d.r7.part123", 5);
    put("notes.txt", 7);
    const auto size = renderer::texture_cache_size(folder);
    check(size.files == 4 && size.bytes == 180, "the cache counts its 4 files, 180 bytes (" + std::to_string(size.files) + ", " +
                                                    std::to_string(size.bytes) + ")");
    const auto removed = renderer::clear_texture_cache(folder);
    check(removed.files == 4 && removed.bytes == 180, "clearing removes the 4");
    check(renderer::texture_cache_size(folder).files == 0, "nothing of the cache left");
    check(std::filesystem::exists(folder / "notes.txt"), "a file not the cache's stays");
    std::filesystem::remove_all(folder, ec);
}

} // namespace

int main(const int argc, char** argv) {
    codec();
    smoothing();
    uncompressed();
    cache_folder();
    if (argc > 1) content(argv[1]);
    if (failures == 0) std::cout << "bc7: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
