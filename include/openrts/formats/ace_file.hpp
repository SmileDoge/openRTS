#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <openrts/core/vfs.hpp>
#include <openrts/formats/block_compression.hpp>

namespace openrts::formats {

enum class AceRepresentation { invalid, uncompressed, compressed };

[[nodiscard]] std::string_view to_string(AceRepresentation representation) noexcept;

// These fields remain raw: no renderer pixel format is inferred from them yet.
struct AceHeader {
    std::uint32_t version = 0;
    std::uint32_t options = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t format_code = 0;
    std::uint32_t channel_count = 0;
};

struct AceFileProbe {
    AceRepresentation representation = AceRepresentation::invalid;
    std::optional<std::uint32_t> declared_inflated_size;
    std::optional<AceHeader> header;
    std::string diagnostic;

    [[nodiscard]] bool valid() const noexcept { return representation != AceRepresentation::invalid; }
};

struct AceRgbaImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;
    // Mipmap levels 1.. (level k: max(1, width >> k) x max(1, height >> k)),
    // RGBA8 like `pixels`. As RTS (0x44F554, 0x44150C): the file's own levels
    // for a square raw ACE with options 5 or a square DXT1 one with options
    // 17 / 21, else built from level 0 (build_rgba_mipmaps, as RTS's
    // gluBuild2DMipmaps). Empty only for images not decoded from an ACE.
    std::vector<std::vector<std::uint8_t>> mips;
    bool mips_from_file = false;
    // A DXT1 ACE with its own levels: the file's 8-byte BC1 blocks of level 0
    // and of each level down to 4x4 (rows of ceil(w/4) blocks), for a GPU to
    // take as they are (as RTS gives OpenGL its DXT1, glCompressedTexImage2D). Its three-colour
    // blocks' fourth colour is transparent black there, as `pixels` has it
    // for a texture with a mask; a maskless one using it (none in the 2026-11
    // content) is not kept. Empty otherwise.
    std::vector<std::vector<std::uint8_t>> bc1;
    // An openRTS ACE of block-compressed levels (raw data, format codes
    // 0x20-0x24, docs/reverse/ace.md; RTS cannot read them): the file's
    // blocks of level 0 and of each level down to 1 x 1 (rows of ceil(w/4)
    // blocks of `block_format`), for a GPU to take as they are; `pixels` and
    // `mips` decoded from them. Empty otherwise.
    std::vector<std::vector<std::uint8_t>> blocks;
    BlockFormat block_format = BlockFormat::bc7;
};

// The raw-data ACE format codes: DXT1 (RTS's) and openRTS's block formats.
inline constexpr std::uint32_t ace_format_dxt1 = 0x12;
inline constexpr std::uint32_t ace_format_bc2 = 0x20; // DXT3
inline constexpr std::uint32_t ace_format_bc3 = 0x21; // DXT5
inline constexpr std::uint32_t ace_format_bc5 = 0x22;
inline constexpr std::uint32_t ace_format_bc7 = 0x23;
inline constexpr std::uint32_t ace_format_bc4 = 0x24;
// The block format of an openRTS code (nullopt: none of them).
[[nodiscard]] std::optional<BlockFormat> ace_block_format(std::uint32_t format_code) noexcept;
[[nodiscard]] std::uint32_t ace_format_code(BlockFormat format) noexcept;

// Mipmap levels 1.. of an RGBA8 image: each a 2x2 box filter of the level
// before, rounded, the way gluBuild2DMipmaps makes them (a side already 1
// averages two texels). Odd sides drop their last row or column.
[[nodiscard]] std::vector<std::vector<std::uint8_t>> build_rgba_mipmaps(std::uint32_t width, std::uint32_t height,
                                                                        const std::vector<std::uint8_t>& pixels);

struct AceDecodeResult {
    std::optional<AceRgbaImage> image;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return image.has_value(); }
};

// Inspects the envelope and fixed uncompressed header. Compressed ACE files
// are identified but their payload header is not inspected until decoding.
[[nodiscard]] AceFileProbe probe_ace_file(std::string_view bytes);
[[nodiscard]] AceFileProbe probe_ace_file_path(const std::filesystem::path& file);

// Decodes a narrow compressed or uncompressed structured ACE into RGBA8, with
// its mipmap chain (AceRgbaImage::mips). Raw ACE files are decoded when they
// hold DXT1 blocks or openRTS's (BC2, BC3, BC4, BC5, BC7).
[[nodiscard]] AceDecodeResult decode_structured_ace_rgba(std::string_view bytes);
[[nodiscard]] AceDecodeResult decode_structured_ace_rgba_path(const std::filesystem::path& file);
[[nodiscard]] AceDecodeResult decode_ace_rgba(const core::Vfs& vfs, const core::VirtualPath& file);

} // namespace openrts::formats
