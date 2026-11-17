#pragma once

// Block compression (the GPUs' BC formats, D3D's names; openRTS's own
// encoders): each 4 x 4 texels in a block of fixed size, sampled by a GPU as
// they are.
//  - BC1 (DXT1): RGB on a line of two 5:6:5 colours, 4 levels, or 3 and a
//    transparent black; 8 bytes.
//  - BC2 (DXT3): BC1's colour (always 4 levels) and a 4-bit alpha a texel; 16.
//  - BC3 (DXT5): BC1's colour (always 4 levels) and BC4's alpha; 16.
//  - BC4: one channel between two 8-bit ends, 8 or 6 levels and 0 / 255; 8.
//  - BC5: two BC4 channels (red, green: a normal map's x and y); 16.
//  - BC7: RGB(A) in 8 modes, 1-3 parts a block, 7-8 bits a channel; 16.
// The decoders read a block as a GPU's UNORM view samples it: BC4
// (r, 0, 0, 255), BC5 (r, g, 0, 255), a BC7 block of no mode transparent
// black. BC7 must decode to the bit (the GPU check: tests/bc_gpu_test.cpp);
// BC1-BC5's levels between the ends may differ by a step from a GPU's.

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace openrts::formats {

enum class BlockFormat : std::uint8_t { bc1, bc2, bc3, bc4, bc5, bc7 };

[[nodiscard]] constexpr std::uint32_t block_bytes(const BlockFormat format) noexcept {
    return format == BlockFormat::bc1 || format == BlockFormat::bc4 ? 8u : 16u;
}
[[nodiscard]] std::string_view block_format_name(BlockFormat format) noexcept; // "BC7", "BC3 (DXT5)", ...

// One block to 16 RGBA8 texels, row by row.
void decode_block(BlockFormat format, const std::uint8_t* block, std::uint8_t* rgba);
// A level's blocks (rows of ceil(w / 4) blocks) to its RGBA8 texels; empty
// when `size` is not the level's blocks.
[[nodiscard]] std::vector<std::uint8_t> decode_blocks(BlockFormat format, std::uint32_t width, std::uint32_t height, const std::uint8_t* blocks,
                                                      std::size_t size);
// An RGBA8 level as blocks (a side not of whole blocks repeats its last
// texel), on `threads` threads (0: the hardware's).
//  - BC1: a block with a texel's alpha under 128 takes the transparent entry.
//  - BC4 / BC5: red / red and green.
//  - BC7: mode 6 (one RGBA line, 16 levels), or mode 5 (colour and alpha on
//    lines of their own, 4 levels each, the alpha's ends in 8 bits) where a
//    block's alpha varies and it comes nearer, a clear texel's colour by how
//    opaque it is.
[[nodiscard]] std::vector<std::uint8_t> encode_blocks(BlockFormat format, std::uint32_t width, std::uint32_t height, const std::uint8_t* rgba,
                                                      unsigned threads = 0);

// The size of a level's blocks.
[[nodiscard]] constexpr std::size_t level_block_bytes(const BlockFormat format, const std::uint32_t width, const std::uint32_t height) noexcept {
    return static_cast<std::size_t>((width + 3) / 4) * ((height + 3) / 4) * block_bytes(format);
}

// A BC7 block's mode (0..7; 8: none, the first byte zero).
[[nodiscard]] int bc7_mode(const std::uint8_t* block) noexcept;

// BC7's partition tables (tests): the part (0..2) of texel i in partition p
// of a block of 2 or 3 parts, and the parts' anchor texels (their index's
// top bit left out).
[[nodiscard]] int bc7_part(int parts, int partition, int texel) noexcept;
[[nodiscard]] bool bc7_anchor(int parts, int partition, int texel) noexcept;

} // namespace openrts::formats
