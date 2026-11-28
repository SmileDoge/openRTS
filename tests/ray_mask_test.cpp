// The cut-outs' masks for the rays (renderer/ray_scene.hpp, coverage_mask):
// made smaller, a texture keeps its share of solid texels against its cut-off.
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include <openrts/renderer/ray_scene.hpp>

namespace {

using openrts::renderer::detail::coverage_mask;

int failures = 0;

void check(const bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << std::endl;
    }
}

// A texture `side` a side, alpha by `alpha(x, y)`.
std::vector<std::uint8_t> texture(const std::uint32_t side, const std::function<std::uint8_t(std::uint32_t, std::uint32_t)>& alpha) {
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(side) * side * 4, 128);
    for (std::uint32_t y = 0; y < side; ++y)
        for (std::uint32_t x = 0; x < side; ++x) rgba[(static_cast<std::size_t>(y) * side + x) * 4 + 3] = alpha(x, y);
    return rgba;
}

double share(const std::vector<std::uint8_t>& rgba, const std::uint8_t cutoff) {
    std::size_t solid = 0;
    for (std::size_t i = 3; i < rgba.size(); i += 4) solid += rgba[i] > cutoff ? 1 : 0;
    return static_cast<double>(solid) / static_cast<double>(rgba.size() / 4);
}

double share(const std::vector<std::uint8_t>& mask) {
    std::size_t solid = 0;
    for (const auto m : mask) solid += m == 255 ? 1 : 0;
    return static_cast<double>(solid) / static_cast<double>(mask.size());
}

void keeps(const std::string& name, const std::vector<std::uint8_t>& rgba, const std::uint32_t side, const std::uint8_t cutoff,
           const std::uint32_t size, const double within = 0.01) {
    const auto mask = coverage_mask(side, side, rgba.data(), cutoff, size);
    bool binary = true;
    for (const auto m : mask) binary = binary && (m == 0 || m == 255);
    check(binary, name + ": 0 or 255 only");
    const double want = share(rgba, cutoff), got = share(mask);
    check(std::abs(want - got) <= within,
          name + " (cut-off " + std::to_string(cutoff) + ", " + std::to_string(size) + "): " + std::to_string(want) + " solid, mask " + std::to_string(got));
}

} // namespace

int main() {
    const std::uint32_t side = 2048;
    const auto solid = texture(side, [](auto, auto) { return std::uint8_t{255}; });
    const auto clear = texture(side, [](auto, auto) { return std::uint8_t{0}; });
    check(share(coverage_mask(side, side, solid.data(), 76, 512)) == 1.0, "opaque: all solid");
    check(share(coverage_mask(side, side, clear.data(), 76, 512)) == 0.0, "transparent: none solid");

    // Leaves: blobs of a pseudo-random size and place.
    std::uint32_t seed = 12345;
    std::vector<std::uint8_t> leaves(static_cast<std::size_t>(side) * side, 0);
    for (int n = 0; n < 4000; ++n) {
        seed = seed * 1664525u + 1013904223u;
        const auto cx = seed % side, cy = (seed >> 11) % side, r = 3 + (seed >> 22) % 12;
        for (std::uint32_t y = cy > r ? cy - r : 0; y < (std::min)(cy + r, side); ++y)
            for (std::uint32_t x = cx > r ? cx - r : 0; x < (std::min)(cx + r, side); ++x)
                if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) leaves[static_cast<std::size_t>(y) * side + x] = 255;
    }
    const auto foliage = texture(side, [&](auto x, auto y) { return leaves[static_cast<std::size_t>(y) * side + x]; });
    for (const std::uint32_t size : {512u, 256u, 128u}) keeps("leaves", foliage, side, 76, size);

    // A grating of bars half a mask texel wide (2 of 4 source texels at 512),
    // one in 16, starting off the mask's grid: each bar a whole column of the
    // mask (a shadow's bar a little wider, never gone; the nearest texel
    // dropped one in two by where it fell).
    const auto bars = texture(side, [](auto x, auto) { return static_cast<std::uint8_t>(x % 16 == 1 || x % 16 == 2 ? 255 : 0); });
    {
        const auto mask = coverage_mask(side, side, bars.data(), 76, 512);
        std::size_t columns = 0;
        for (std::uint32_t x = 0; x < 512; ++x) columns += mask[static_cast<std::size_t>(100) * 512 + x] == 255 ? 1 : 0;
        check(columns == 128, "half-texel bars: each a column of the mask (" + std::to_string(columns) + " of 128)");
        check(share(mask) <= 2.0 * share(bars, 76) + 1e-9, "half-texel bars: at most twice as wide");
    }
    // Bars a quarter of a mask texel wide crossing (a fine grating): its
    // share kept (the bars themselves too thin for 512, the crossings stay).
    const auto grating = texture(side, [](auto x, auto y) { return static_cast<std::uint8_t>(x % 8 == 0 || y % 8 == 0 ? 255 : 0); });
    keeps("fine grating", grating, side, 76, 512, 0.02);

    // Holes a mask texel or more: open where they are.
    const auto holes = texture(side, [](auto x, auto y) { return static_cast<std::uint8_t>(x % 16 < 4 && y % 16 < 4 ? 0 : 255); });
    keeps("holes", holes, side, 76, 512);
    {
        const auto mask = coverage_mask(side, side, holes.data(), 76, 512);
        check(mask[0] == 0 && mask[1 * 512 + 1] == 255 && mask[4 * 512 + 4] == 0, "holes: open in place");
    }

    // A gradient: its share by the cut-off.
    const auto gradient = texture(side, [](auto x, auto) { return static_cast<std::uint8_t>(x * 255 / (side - 1)); });
    for (const std::uint8_t cutoff : {std::uint8_t{76}, std::uint8_t{127}, std::uint8_t{229}}) keeps("gradient", gradient, side, cutoff, 512);

    // A texture smaller than the mask: each texel as it is.
    const auto small = texture(64, [](auto x, auto y) { return static_cast<std::uint8_t>((x / 4 + y / 4) % 2 ? 255 : 0); });
    keeps("small texture", small, 64, 76, 512);
    {
        const auto mask = coverage_mask(64, 64, small.data(), 76, 512);
        check(mask[0] == 0 && mask[32] == 255, "small texture: its checker in place");
    }

    std::cout << (failures ? "FAILED" : "ray masks: all passed") << std::endl;
    return failures ? 1 : 0;
}
