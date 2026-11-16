// The trains' textures without DXT1's squares (renderer/bc7.hpp): BC7
// mode 6 back within a few levels, the smoothing of block edges (a real edge
// left), and on an installation's 2ES6 body the blocks' edges brought down
// to the texture's own grain.

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
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

} // namespace

int main(const int argc, char** argv) {
    codec();
    smoothing();
    if (argc > 1) content(argv[1]);
    if (failures == 0) std::cout << "bc7: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
