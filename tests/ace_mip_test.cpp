// ACE mipmaps as RTS has them (0x44F554, 0x44150C): built levels (the box
// filter of gluBuild2DMipmaps) and, with an installation (argument), the
// file's own chains of a raw ACE with options 5 and a DXT1 ACE with options 21.
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <openrts/core/installation.hpp>
#include <openrts/core/vfs.hpp>
#include <openrts/formats/ace_file.hpp>

namespace {

using namespace openrts;

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::cout << "FAIL: " << what << '\n';
    }
}

void test_build() {
    // 2x2 -> 1x1, rounded: (10 + 20 + 30 + 41 + 2) / 4 = 25.
    const std::vector<std::uint8_t> square{10, 0, 0, 255, 20, 0, 0, 255, 30, 0, 0, 0, 41, 0, 0, 0};
    const auto one = formats::build_rgba_mipmaps(2, 2, square);
    check(one.size() == 1 && one[0].size() == 4, "2x2 has one level, 1x1");
    if (one.size() == 1) {
        check(one[0][0] == 25, "box filter rounds");
        check(one[0][3] == 128, "alpha is averaged too");
    }

    // 4x1 -> 2x1 -> 1x1: a side of 1 averages two texels.
    const std::vector<std::uint8_t> row{0, 0, 0, 0, 3, 0, 0, 0, 100, 0, 0, 0, 200, 0, 0, 0};
    const auto chain = formats::build_rgba_mipmaps(4, 1, row);
    check(chain.size() == 2, "4x1 has two levels");
    if (chain.size() == 2) {
        check(chain[0].size() == 8 && chain[0][0] == 2 && chain[0][4] == 150, "4x1 -> 2x1");
        check(chain[1].size() == 4 && chain[1][0] == 76, "2x1 -> 1x1");
    }

    // 3x3 -> 1x1 drops the odd row and column; a wrong pixel count builds nothing.
    check(formats::build_rgba_mipmaps(3, 3, std::vector<std::uint8_t>(36, 7)).size() == 1, "3x3 -> 1x1");
    check(formats::build_rgba_mipmaps(4, 4, std::vector<std::uint8_t>(5)).empty(), "bad size builds nothing");
}

void expect_chain(const core::Vfs& vfs, const std::string& path, const bool from_file) {
    const auto file = core::VirtualPath::parse(path);
    if (!file || !vfs.exists(*file)) {
        std::cout << path << ": not installed, skipped\n";
        return;
    }
    const auto decoded = formats::decode_ace_rgba(vfs, *file);
    check(decoded.ok(), path + " decodes");
    if (!decoded.ok()) return;
    const auto& image = *decoded.image;
    check(image.mips_from_file == from_file, path + (from_file ? ": the file's levels" : ": built levels"));
    // A full chain down to 1x1, every level of the right size.
    std::size_t expected = 0;
    for (auto side = (std::max)(image.width, image.height); side > 1; side /= 2) ++expected;
    check(image.mips.size() == expected, path + ": full chain");
    for (std::size_t k = 1; k <= image.mips.size(); ++k) {
        const auto w = (std::max)(image.width >> k, 1u);
        const auto h = (std::max)(image.height >> k, 1u);
        check(image.mips[k - 1].size() == static_cast<std::size_t>(w) * h * 4, path + ": level " + std::to_string(k) + " size");
    }
    // The last level is roughly the image's mean colour, whoever made it.
    if (!image.mips.empty()) {
        double mean = 0;
        for (std::size_t i = 0; i < image.pixels.size(); i += 4) mean += image.pixels[i + 1];
        mean /= static_cast<double>(image.pixels.size() / 4);
        const double last = image.mips.back()[1];
        check(last > mean - 24 && last < mean + 24, path + ": last level near the mean green");
    }
}

// A DXT1 ACE with its own levels keeps their blocks for the GPU (BC1), level 0
// down to 4x4; any other ACE keeps none.
void expect_blocks(const core::Vfs& vfs, const std::string& path, const bool dxt1) {
    const auto file = core::VirtualPath::parse(path);
    if (!file || !vfs.exists(*file)) return;
    const auto decoded = formats::decode_ace_rgba(vfs, *file);
    if (!decoded.ok()) return;
    const auto& image = *decoded.image;
    if (!dxt1) {
        check(image.bc1.empty(), path + ": no BC1 blocks");
        return;
    }
    std::size_t expected = 0;
    for (auto side = image.width; side >= 4; side /= 2) ++expected;
    check(image.bc1.size() == expected, path + ": BC1 levels down to 4x4");
    for (std::size_t k = 0; k < image.bc1.size(); ++k) {
        const auto side = (std::max)(image.width >> k, 1u);
        check(image.bc1[k].size() == static_cast<std::size_t>((side + 3) / 4) * ((side + 3) / 4) * 8,
              path + ": BC1 level " + std::to_string(k) + " size");
    }
}

void test_content(const std::filesystem::path& root) {
    core::Vfs vfs;
    core::mount_installation(vfs, root, core::load_installation_config(root));
    expect_chain(vfs, "TRAINS/TRAINSET/mdd_ED4m-Alias/CAB/cabnosv.ace", true); // raw, options 5, 2048x2048
    expect_chain(vfs, "TRAINS/TRAINSET/mdd_ED4m-Alias/CAB/cab_02.ace", false);  // options 0
    expect_chain(vfs, "GLOBAL/TEXTURES/7carfloat.ace", true);                   // DXT1, options 21
    expect_blocks(vfs, "TRAINS/TRAINSET/mdd_ED4m-Alias/CAB/cabnosv.ace", false);
    expect_blocks(vfs, "GLOBAL/TEXTURES/7carfloat.ace", true);
}

} // namespace

int main(int argc, char** argv) {
    test_build();
    if (argc > 1) test_content(argv[1]);
    std::cout << (failures ? "FAILED" : "passed") << '\n';
    return failures ? 1 : 0;
}
