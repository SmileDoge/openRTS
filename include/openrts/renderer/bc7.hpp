#pragma once

// Rolling stock textures without DXT1's blocks (openRTS's own; off: as RTS).
// MSTS / RTS content keeps its textures in DXT1: every 4 x 4 texels have two
// colours of 5:6:5 and two between them, so a dark, grimy body or a cab seen
// close shows the blocks as squares where they meet (testers 2026-11-15, the
// 2ES6 under Khronos PBR Neutral, which shows shadows as drawn). Here the
// steps where blocks meet are smoothed when they are small (a real edge is
// left), and the result kept in BC7 (16 levels between two colours of 7
// bits and more a block, 1 byte a texel: twice DXT1, an eighth of RGBA8).

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace openrts::renderer {

// The smoothing for TRAINS/TRAINSET's DXT1 textures (the Graphics panel's
// choice, read when a texture is loaded) and where encoded ones are kept
// (empty: encoded each time).
inline bool& smooth_train_textures() {
    static bool on = false;
    return on;
}
inline std::filesystem::path& texture_cache_directory() {
    static std::filesystem::path folder;
    return folder;
}

// Smooths the steps where DXT1's 4 x 4 blocks meet in an RGBA8 image, in
// place: across each block edge, where the two sides are nearly flat and
// the step between them small, the four texels about it are laid on a ramp.
// Texels with alpha below 128 are left.
void deblock_dxt1(std::uint32_t width, std::uint32_t height, std::vector<std::uint8_t>& rgba);

// BC7 (mode 6: one pair of RGBA colours of 7 bits and a shared low bit, 16
// levels) of an RGBA8 image, rows of ceil(w / 4) 16-byte blocks; a side
// not of whole blocks repeats its last texel. On `threads` threads (0: the
// hardware's).
[[nodiscard]] std::vector<std::uint8_t> encode_bc7(std::uint32_t width, std::uint32_t height, const std::uint8_t* rgba,
                                                   unsigned threads = 0);
// One mode-6 block back to 16 RGBA8 texels (tests); false for another mode.
bool decode_bc7_mode6(const std::uint8_t* block, std::uint8_t* rgba);

// A DXT1 texture of TRAINS/TRAINSET as smoothed BC7 levels (level 0 and
// each down to 1 x 1), from its decoded level 0 and its DXT1 level 0's
// blocks (whose hash keys the disk cache): read from the cache, else made
// and kept there.
[[nodiscard]] std::optional<std::vector<std::vector<std::uint8_t>>> smoothed_bc7_levels(std::uint32_t width, std::uint32_t height,
                                                                                         const std::vector<std::uint8_t>& rgba,
                                                                                         const std::vector<std::uint8_t>& dxt1_level0);

} // namespace openrts::renderer
