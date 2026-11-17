#pragma once

// Rolling stock textures without DXT1's blocks (openRTS's own; off: as RTS).
// MSTS / RTS content keeps its textures in DXT1: every 4 x 4 texels have two
// colours of 5:6:5 and two between them, so a dark, grimy body or a cab seen
// close shows the blocks as squares where they meet (testers 2026-11-15, the
// 2ES6 under Khronos PBR Neutral, which shows shadows as drawn). Here the
// steps where blocks meet are smoothed when they are small (a real edge is
// left), and the result kept in BC7 (16 levels between two colours of 7
// bits and more a block, 1 byte a texel: twice DXT1, an eighth of RGBA8).

#include <array>
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
// RTS's uncompressed textures (raw ACE, RGBA8: 4 bytes a texel) and the
// materials' maps kept compressed, a quarter of the memory (openRTS's own;
// off: as RTS, RGBA8). Testers 2026-11-17: an RTX 4060 with 8 GB over its
// budget with PBR cars at Kuban, 1.6 GB of the maps alone, 14 fps.
// Encoded each load (memory), or kept in texture_cache_directory() and
// read from there the next time (disk; the player's choice).
enum class TextureCompression : std::uint8_t { off, memory, disk };
inline TextureCompression& texture_compression() {
    static TextureCompression chosen = TextureCompression::disk;
    return chosen;
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
// A mode 5 or mode 6 block back to 16 RGBA8 texels; false for another mode.
// encode_bc7 takes mode 5 (colour and alpha on lines of their own, 4 levels
// each, the alpha's ends in 8 bits) for a block whose alpha varies when it
// comes nearer: a leaf's edge, clear texels by opaque ones.
bool decode_bc7_block(const std::uint8_t* block, std::uint8_t* rgba);

// BC5 of an RGBA8 image's red and green (a normal map's x and y; its z
// the shader's), two 8-byte BC4 blocks a 4 x 4 block, 1 byte a texel.
[[nodiscard]] std::vector<std::uint8_t> encode_bc5(std::uint32_t width, std::uint32_t height, const std::uint8_t* rgba,
                                                   unsigned threads = 0);
// One BC5 block back to 16 RGBA8 texels (blue 0, alpha 255).
void decode_bc5_block(const std::uint8_t* block, std::uint8_t* rgba);

// The disk cache's files in a folder (the smoothed trains' .bc7, the
// compressed textures' .r7 / .n5, half-written ones): how many, how big.
struct TextureCacheSize {
    std::uint64_t files = 0;
    std::uint64_t bytes = 0;
};
[[nodiscard]] TextureCacheSize texture_cache_size(const std::filesystem::path& folder);
// Removes them (one open elsewhere is left); what was removed. Textures
// already on the GPU stay; the next loads encode theirs again.
TextureCacheSize clear_texture_cache(const std::filesystem::path& folder);

enum class BlockFormat : std::uint8_t { bc7, bc5 };
// An uncompressed texture (level 0 and its levels 1.., RGBA8; none: built
// as RTS builds them) as BC7 or BC5 levels. With `disk`: from the disk
// cache by a hash of its texels, else encoded and kept there; without:
// encoded. Level 0's sides must be whole blocks (else nullopt).
[[nodiscard]] std::optional<std::vector<std::vector<std::uint8_t>>> compressed_levels(
    std::uint32_t width, std::uint32_t height, const std::vector<std::uint8_t>& rgba, const std::vector<std::vector<std::uint8_t>>& mips,
    BlockFormat format, bool disk = true);

// A DXT1 texture of TRAINS/TRAINSET as smoothed BC7 levels (level 0 and
// each down to 1 x 1), from its decoded level 0 and its DXT1 level 0's
// blocks (whose hash keys the disk cache): read from the cache, else made
// and kept there.
[[nodiscard]] std::optional<std::vector<std::vector<std::uint8_t>>> smoothed_bc7_levels(std::uint32_t width, std::uint32_t height,
                                                                                         const std::vector<std::uint8_t>& rgba,
                                                                                         const std::vector<std::uint8_t>& dxt1_level0);

} // namespace openrts::renderer
