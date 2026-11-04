#include <openrts/formats/terrain.hpp>

#include <bit>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <string_view>

namespace openrts::formats {
namespace {

// Binary terrain tokens (JINX0t6b).
constexpr std::uint32_t token_terrain = 136;
constexpr std::uint32_t token_samples = 139;
constexpr std::uint32_t token_sample_count = 140;
constexpr std::uint32_t token_sample_floor = 142;
constexpr std::uint32_t token_sample_scale = 143;
constexpr std::uint32_t token_sample_size = 144;
constexpr std::uint32_t token_sample_ybuffer = 146;
constexpr std::uint32_t token_shaders = 151;
constexpr std::uint32_t token_shader = 152;
constexpr std::uint32_t token_texslots = 153;
constexpr std::uint32_t token_texslot = 154;
constexpr std::uint32_t token_patches = 157;
constexpr std::uint32_t token_patchsets = 158;
constexpr std::uint32_t token_patchset = 159;
constexpr std::uint32_t token_patchset_npatches = 161;
constexpr std::uint32_t token_patchset_patches = 163;
constexpr std::uint32_t token_patchset_patch = 164;
constexpr std::uint32_t token_water_height_offset = 251;

constexpr std::size_t patch_record_size = 60;

[[nodiscard]] std::string utf16_to_utf8(const std::u16string_view text) {
    std::string result;
    for (std::size_t i = 0; i < text.size(); ++i) {
        std::uint32_t code = text[i];
        if (code >= 0xD800 && code <= 0xDBFF && i + 1 < text.size() && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
            code = 0x10000 + ((code - 0xD800) << 10) + (text[++i] - 0xDC00u);
        }
        if (code < 0x80) {
            result.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            result.push_back(static_cast<char>(0xC0 | (code >> 6)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            result.push_back(static_cast<char>(0xE0 | (code >> 12)));
            result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            result.push_back(static_cast<char>(0xF0 | (code >> 18)));
            result.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }
    return result;
}

[[nodiscard]] std::optional<std::string> read_file(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(input), {});
}

// Bounded reader over one block payload.
class Reader {
public:
    Reader(const std::string_view bytes, const std::size_t begin, const std::size_t end)
        : bytes_(bytes), at_(begin), end_(end) {}

    [[nodiscard]] bool done() const noexcept { return at_ >= end_; }
    [[nodiscard]] std::size_t position() const noexcept { return at_; }

    [[nodiscard]] std::optional<std::uint32_t> u32() {
        if (at_ + 4 > end_) return std::nullopt;
        const auto value = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes_[at_]))
            | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes_[at_ + 1])) << 8)
            | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes_[at_ + 2])) << 16)
            | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes_[at_ + 3])) << 24);
        at_ += 4;
        return value;
    }

    [[nodiscard]] std::optional<float> f32() {
        const auto bits = u32();
        return bits ? std::optional<float>(std::bit_cast<float>(*bits)) : std::nullopt;
    }

    // UTF-16LE string with a u16 length, returned as UTF-8 (texture names may
    // be Cyrillic).
    [[nodiscard]] std::optional<std::string> string() {
        if (at_ + 2 > end_) return std::nullopt;
        const auto units = static_cast<std::size_t>(static_cast<unsigned char>(bytes_[at_])
                                                    | (static_cast<unsigned char>(bytes_[at_ + 1]) << 8));
        at_ += 2;
        if (units > (end_ - at_) / 2) return std::nullopt;
        std::u16string text;
        for (std::size_t i = 0; i < units; ++i, at_ += 2) {
            text.push_back(static_cast<char16_t>(static_cast<unsigned char>(bytes_[at_])
                                                 | (static_cast<unsigned char>(bytes_[at_ + 1]) << 8)));
        }
        return utf16_to_utf8(text);
    }

    // Next child block; its contents start after the optional label.
    [[nodiscard]] std::optional<struct Block> block();

private:
    std::string_view bytes_;
    std::size_t at_;
    std::size_t end_;
};

struct Block {
    std::uint32_t token;
    Reader contents;
};

std::optional<Block> Reader::block() {
    const auto token = u32();
    const auto size = u32();
    if (!token || !size || *size > end_ - at_ || *size == 0) return std::nullopt;
    const auto block_end = at_ + *size;
    const auto label_units = static_cast<unsigned char>(bytes_[at_]);
    const auto contents_begin = at_ + 1 + static_cast<std::size_t>(label_units) * 2;
    at_ = block_end;
    if (contents_begin > block_end) return std::nullopt;
    return Block{*token, Reader(bytes_, contents_begin, block_end)};
}

void read_samples(Reader reader, TerrainTile& tile, std::string& ybuffer) {
    while (!reader.done()) {
        auto child = reader.block();
        if (!child) return;
        auto& contents = child->contents;
        switch (child->token) {
        case token_sample_count:
            tile.sample_count = contents.u32().value_or(0);
            break;
        case token_sample_floor:
            tile.sample_floor = contents.f32().value_or(0.0f);
            break;
        case token_sample_scale:
            tile.sample_scale = contents.f32().value_or(0.0f);
            break;
        case token_sample_size:
            tile.sample_size = contents.f32().value_or(0.0f);
            break;
        case token_sample_ybuffer:
            ybuffer = contents.string().value_or(std::string{});
            break;
        default:
            break;
        }
    }
}

[[nodiscard]] TerrainShader read_shader(Reader reader) {
    TerrainShader shader;
    shader.name = reader.string().value_or(std::string{});
    while (!reader.done()) {
        auto child = reader.block();
        if (!child) break;
        if (child->token != token_texslots) continue;
        auto& slots = child->contents;
        const auto count = slots.u32().value_or(0);
        for (std::uint32_t i = 0; i < count; ++i) {
            auto slot = slots.block();
            if (!slot || slot->token != token_texslot) break;
            shader.textures.push_back(slot->contents.string().value_or(std::string{}));
        }
    }
    return shader;
}

void read_shaders(Reader reader, TerrainTile& tile) {
    const auto count = reader.u32().value_or(0);
    for (std::uint32_t i = 0; i < count; ++i) {
        auto shader = reader.block();
        if (!shader || shader->token != token_shader) return;
        tile.shaders.push_back(read_shader(shader->contents));
    }
}

[[nodiscard]] std::optional<TerrainPatch> read_patch(Reader reader) {
    TerrainPatch patch;
    const auto flags = reader.u32();
    float values[6]{};
    for (auto& value : values) value = reader.f32().value_or(0.0f);
    const auto shader = reader.u32();
    float uv[7]{};
    for (auto& value : uv) value = reader.f32().value_or(0.0f);
    if (!flags || !shader) return std::nullopt;

    patch.flags = *flags;
    patch.centre[0] = values[0];
    patch.centre[1] = values[1];
    patch.centre[2] = values[2];
    patch.factor_y = values[3];
    patch.radius = values[4];
    patch.shader_index = *shader;
    // values[5] is a second radius-like value; uv[] follows the MSTS layout X Y W B C H ErrorBias.
    patch.u0 = uv[0];
    patch.v0 = uv[1];
    patch.du_dx = uv[2];
    patch.du_dz = uv[3];
    patch.dv_dx = uv[4];
    patch.dv_dz = uv[5];
    patch.error_bias = uv[6];
    return patch;
}

void read_patchset(Reader reader, TerrainTile& tile) {
    while (!reader.done()) {
        auto child = reader.block();
        if (!child) return;
        if (child->token == token_patchset_npatches) {
            tile.patches_per_side = child->contents.u32().value_or(0);
        } else if (child->token == token_patchset_patches) {
            auto& patches = child->contents;
            while (!patches.done()) {
                auto patch = patches.block();
                if (!patch || patch->token != token_patchset_patch) break;
                if (auto parsed = read_patch(patch->contents)) tile.patches.push_back(*parsed);
            }
        }
    }
}

void read_patches(Reader reader, TerrainTile& tile) {
    while (!reader.done()) {
        auto child = reader.block();
        if (!child) return;
        if (child->token != token_patchsets) continue;
        auto& patchsets = child->contents;
        const auto count = patchsets.u32().value_or(0);
        // The first patch set is the full-detail one.
        if (count == 0) return;
        if (auto patchset = patchsets.block(); patchset && patchset->token == token_patchset) {
            read_patchset(patchset->contents, tile);
        }
        return;
    }
}

} // namespace

std::string terrain_tile_name(const std::int32_t tile_x, const std::int32_t tile_z) {
    // 15 quadtree levels over [-16384, 16384); two levels per hex digit, the
    // odd last level padded. Quadrant code: north 0, south 2; +1 unless the
    // tile is in the north-east or south-west quadrant.
    constexpr int levels = 15;
    std::int32_t rect_x = -16384;
    std::int32_t rect_z = -16384;
    std::int32_t half = 16384;
    std::string name = "-";
    unsigned partial = 0;
    for (int level = 0; level < levels; ++level) {
        const bool east = tile_x >= rect_x + half;
        const bool north = tile_z >= rect_z + half;
        partial = (partial << 2) + (north ? 0u : 2u) + ((east != north) ? 0u : 1u);
        if (level % 2 == 1) {
            char digit[2];
            std::snprintf(digit, sizeof digit, "%x", partial);
            name += digit;
            partial = 0;
        }
        if (east) rect_x += half;
        if (north) rect_z += half;
        half /= 2;
    }
    char digit[2];
    std::snprintf(digit, sizeof digit, "%x", partial << 2);
    name += digit;
    return name;
}

namespace {

// Reads a tile; `read_tile_file(name)` returns a file of the TILES folder.
template <typename ReadFile>
[[nodiscard]] TerrainTile load_tile_with(const ReadFile& read_tile_file, const std::int32_t tile_x, const std::int32_t tile_z) {
    TerrainTile tile{.tile_x = tile_x, .tile_z = tile_z};
    const auto stem = terrain_tile_name(tile_x, tile_z);
    const auto bytes = read_tile_file(stem + ".t");
    if (!bytes) {
        tile.error = "No terrain tile " + stem + ".t";
        return tile;
    }
    if (bytes->size() < 32 || bytes->compare(16, 8, "JINX0t6b") != 0) {
        tile.error = "Unsupported terrain file representation (binary JINX0t6b expected).";
        return tile;
    }

    std::string ybuffer = stem + "_y.raw";
    Reader file(*bytes, 32, bytes->size());
    auto terrain = file.block();
    if (!terrain || terrain->token != token_terrain) {
        tile.error = "Terrain file has no terrain block.";
        return tile;
    }
    while (!terrain->contents.done()) {
        auto child = terrain->contents.block();
        if (!child) break;
        if (child->token == token_samples) {
            read_samples(child->contents, tile, ybuffer);
        } else if (child->token == token_shaders) {
            read_shaders(child->contents, tile);
        } else if (child->token == token_patches) {
            read_patches(child->contents, tile);
        } else if (child->token == token_water_height_offset) {
            std::array<float, 4> heights{};
            bool all = true;
            for (auto& height : heights) {
                const auto value = child->contents.f32();
                if (!value) all = false;
                height = value.value_or(0.0f);
            }
            if (all) tile.water_heights = heights;
        }
    }

    if (tile.sample_count == 0 || tile.sample_count > 4096 || tile.sample_size <= 0.0f) {
        tile.error = "Terrain tile has no valid sample grid.";
        return tile;
    }
    const auto heights = read_tile_file(ybuffer);
    const auto expected = static_cast<std::size_t>(tile.sample_count) * tile.sample_count * 2;
    if (!heights || heights->size() < expected) {
        tile.error = "Terrain height buffer " + ybuffer + " is missing or truncated.";
        return tile;
    }
    tile.heights.resize(static_cast<std::size_t>(tile.sample_count) * tile.sample_count);
    for (std::size_t i = 0; i < tile.heights.size(); ++i) {
        tile.heights[i] = static_cast<std::uint16_t>(static_cast<unsigned char>((*heights)[2 * i])
                                                     | (static_cast<unsigned char>((*heights)[2 * i + 1]) << 8));
    }
    if (tile.patches.size() != static_cast<std::size_t>(tile.patches_per_side) * tile.patches_per_side) {
        tile.error = "Terrain patch count does not match the patch grid.";
    }
    return tile;
}

} // namespace

TerrainTile load_terrain_tile(const std::filesystem::path& tiles_directory,
                              const std::int32_t tile_x,
                              const std::int32_t tile_z) {
    return load_tile_with([&](const std::string& name) { return read_file(tiles_directory / name); }, tile_x, tile_z);
}

TerrainTile load_terrain_tile(const core::Vfs& vfs,
                              const core::VirtualPath& tiles,
                              const std::int32_t tile_x,
                              const std::int32_t tile_z) {
    return load_tile_with(
        [&](const std::string& name) -> std::optional<std::string> {
            const auto path = tiles.resolve(std::string_view(name));
            return path ? vfs.read(*path) : std::nullopt;
        },
        tile_x, tile_z);
}

} // namespace openrts::formats
