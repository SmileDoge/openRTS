// Opacity micromaps' baking (renderer/opacity_micromap.hpp): the bird-curve
// micro-triangles tile the triangle once each, and a micro-triangle said
// opaque or transparent is so at every point of it.
#include <cmath>
#include <cstdint>
#include <iostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <openrts/renderer/opacity_micromap.hpp>

namespace {

using namespace openrts::renderer;

int failures = 0;

void check(const bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << std::endl;
    }
}

float area(const std::array<std::array<float, 2>, 3>& t) {
    return 0.5f * std::abs((t[1][0] - t[0][0]) * (t[2][1] - t[0][1]) - (t[2][0] - t[0][0]) * (t[1][1] - t[0][1]));
}

bool inside(const std::array<std::array<float, 2>, 3>& t, const float u, const float v) {
    const auto side = [&](int a, int b) {
        return (t[b][0] - t[a][0]) * (v - t[a][1]) - (t[b][1] - t[a][1]) * (u - t[a][0]);
    };
    const float s0 = side(0, 1), s1 = side(1, 2), s2 = side(2, 0);
    return (s0 >= -1e-6f && s1 >= -1e-6f && s2 >= -1e-6f) || (s0 <= 1e-6f && s1 <= 1e-6f && s2 <= 1e-6f);
}

MicroState state_of(const BakedMicromaps& baked, const std::size_t triangle, const std::uint32_t micro) {
    const auto index = baked.indices[triangle];
    if (index == micromap_fully_opaque) return MicroState::opaque;
    if (index == micromap_fully_transparent) return MicroState::transparent;
    const std::size_t bytes = std::max<std::size_t>(((std::size_t{1} << (2 * baked.level)) * 2 + 7) / 8, 1);
    const auto byte = baked.data[static_cast<std::size_t>(index) * bytes + micro / 4];
    return static_cast<MicroState>((byte >> ((micro % 4) * 2)) & 3u);
}

} // namespace

int main() {
    // The micro-triangles tile the triangle: areas add up, every middle
    // inside it, no two the same.
    for (std::uint32_t level = 0; level <= 5; ++level) {
        const std::uint32_t count = 1u << (2 * level);
        double sum = 0.0;
        bool in = true;
        std::set<std::pair<long, long>> middles;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto t = micro_triangle(i, level);
            sum += area(t);
            const float mu = (t[0][0] + t[1][0] + t[2][0]) / 3.0f, mv = (t[0][1] + t[1][1] + t[2][1]) / 3.0f;
            in = in && mu > 0.0f && mv > 0.0f && mu + mv < 1.0f;
            middles.emplace(std::lround(mu * 3e4), std::lround(mv * 3e4));
        }
        check(std::abs(sum - 0.5) < 1e-5, "level " + std::to_string(level) + ": areas add up to the triangle's");
        check(in, "level " + std::to_string(level) + ": every micro-triangle inside");
        check(middles.size() == count, "level " + std::to_string(level) + ": no two the same");
    }

    // A mask: left half solid, a hole in it, right half open.
    const std::uint32_t size = 64;
    std::vector<std::uint8_t> mask(size * size, 0);
    for (std::uint32_t y = 0; y < size; ++y)
        for (std::uint32_t x = 0; x < size; ++x)
            mask[y * size + x] = (x < 32 && !(x >= 8 && x < 12 && y >= 8 && y < 12)) ? 255 : 0;
    const auto at = [&](float u, float v) {
        const auto x = static_cast<std::uint32_t>(std::min(std::floor(u * size), size - 1.0f));
        const auto y = static_cast<std::uint32_t>(std::min(std::floor(v * size), size - 1.0f));
        return mask[y * size + x] != 0;
    };
    const std::vector<float> uvs = {
        0.05f, 0.05f, 0.45f, 0.05f, 0.05f, 0.45f, // in the left: solid but for the hole
        0.6f, 0.6f, 0.9f, 0.6f, 0.6f, 0.9f,       // in the right: all open
        0.15f, 0.7f, 0.4f, 0.7f, 0.15f, 0.95f,    // in the left away from the hole: all solid
        0.2f, 0.2f, 0.8f, 0.2f, 0.2f, 0.8f,       // across the edge
    };
    const std::uint32_t level = 4;
    const auto baked = bake_opacity_micromaps(uvs.data(), 4, mask.data(), size, level);
    check(baked.indices.size() == 4, "one index a triangle");
    check(baked.indices[1] == micromap_fully_transparent, "all open: the special index");
    check(baked.indices[2] == micromap_fully_opaque, "all solid: the special index");
    check(baked.indices[0] >= 0 && baked.indices[3] >= 0, "the mixed ones: a micromap each");
    check(baked.count == 2 && baked.data.size() == 2 * 64, "two micromaps of 64 bytes (level 4)");

    // Every point of an opaque micro-triangle solid, of a transparent one open.
    bool honest = true;
    std::size_t unknown = 0, known = 0;
    for (std::size_t t = 0; t < 4; ++t) {
        const float* c = uvs.data() + t * 6;
        for (std::uint32_t m = 0; m < (1u << (2 * level)); ++m) {
            const auto micro = micro_triangle(m, level);
            const auto state = state_of(baked, t, m);
            if (state == MicroState::unknown_opaque || state == MicroState::unknown_transparent) {
                ++unknown;
                continue;
            }
            ++known;
            for (int s = 0; s < 25; ++s) {
                const float a = static_cast<float>(s % 5) / 5.0f + 0.1f, b = static_cast<float>(s / 5) / 5.0f + 0.1f;
                if (a + b >= 1.0f) continue;
                const float u = micro[0][0] + (micro[1][0] - micro[0][0]) * a + (micro[2][0] - micro[0][0]) * b;
                const float v = micro[0][1] + (micro[1][1] - micro[0][1]) * a + (micro[2][1] - micro[0][1]) * b;
                const float w = 1.0f - u - v;
                const float tu = c[0] * w + c[2] * u + c[4] * v, tv = c[1] * w + c[3] * u + c[5] * v;
                if (at(tu, tv) != (state == MicroState::opaque)) honest = false;
            }
        }
    }
    check(honest, "an opaque or transparent micro-triangle is so all over");
    check(known > unknown * 4, "most micro-triangles known (" + std::to_string(known) + " known, " + std::to_string(unknown) + " unknown)");

    std::cout << (failures ? "FAILED" : "opacity micromaps: all passed") << std::endl;
    return failures ? 1 : 0;
}
