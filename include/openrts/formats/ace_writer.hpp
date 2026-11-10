#pragma once

// Writing ACE textures (openrts-ace): the kinds RTS's content has, laid out as
// its tools write them (docs/reverse/ace.md):
//  - DXT1 (format 0x12, raw data): DXT1 blocks a level down to 4 x 4, the
//    2 x 2 and 1 x 1 levels as plain rows; a 1-bit mask channel when any
//    texel is transparent (DXT1's fourth entry of three-colour blocks);
//  - RGB (0x0E), RGB + mask (0x10), RGBA (0x11: R G B, the mask and an 8-bit
//    alpha): rows of channel planes after a table of row offsets.
// A power-of-two image gets its mipmap chain (RTS's 2 x 2 box filter, as its
// gluBuild2DMipmaps), options 5 / 21 so RTS takes a square one's levels; the
// body zlib-packed (SIMISA@F) as most of RTS's files are, or plain.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace openrts::formats {

enum class AceFormat { dxt1, rgb, rgb_mask, rgba };

struct AceWriteOptions {
    AceFormat format = AceFormat::dxt1;
    bool mipmaps = true;
    bool zlib = true;
};

struct AceEncodeResult {
    std::string bytes;
    std::string error;
    std::vector<std::string> notes; // what was done otherwise than asked (no mipmaps: not a power of two)
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

[[nodiscard]] AceEncodeResult encode_ace(std::uint32_t width, std::uint32_t height, const std::vector<std::uint8_t>& rgba,
                                         const AceWriteOptions& options);

// One 4 x 4 tile (16 RGBA8 texels, row by row) as a DXT1 block. With
// `punch_through` a texel whose alpha is under 128 is the transparent fourth
// entry (a three-colour block).
[[nodiscard]] std::array<std::uint8_t, 8> encode_dxt1_block(const std::uint8_t (&tile)[16][4], bool punch_through);

// The format a texture is best kept in: DXT1 when it has no partly
// transparent texels (alpha only 0 or 255), else RGBA.
[[nodiscard]] AceFormat suggested_ace_format(const std::vector<std::uint8_t>& rgba);

} // namespace openrts::formats
