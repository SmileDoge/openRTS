#pragma once

#include <cstdint>
#include <optional>
#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include <openrts/core/vfs.hpp>

// MSTS/RTS terrain tiles (ROUTES/<route>/TILES/<name>.t plus <name>_y.raw).
// A tile covers 2048 m with a grid of height samples; terrain patches assign
// a terrain shader (textures) and UV mapping to 128 m squares.
namespace openrts::formats {

// File name stem of the 2048 m terrain tile at MSTS tile coordinates, e.g.
// "-11ac64cc" (quadtree path through the 32768 x 32768 tile world).
[[nodiscard]] std::string terrain_tile_name(std::int32_t tile_x, std::int32_t tile_z);

struct TerrainShader {
    std::string name;                  // DetailTerrain, AlphaTerrain, ...
    std::vector<std::string> textures; // terrain_texslot file names (UTF-8), in slot order
};

struct TerrainPatch {
    std::uint32_t flags{};
    float centre[3]{}; // relative to the tile's north-west corner, z pointing south as negative
    float factor_y{};
    float radius{};
    std::uint32_t shader_index{};
    // Texture coordinates: u = u0 + du_dx * column + du_dz * row,
    //                      v = v0 + dv_dx * column + dv_dz * row (tile sample units).
    float u0{};
    float v0{};
    float du_dx{};
    float du_dz{};
    float dv_dx{};
    float dv_dz{};
    float error_bias{};
};

struct TerrainTile {
    std::int32_t tile_x{};
    std::int32_t tile_z{};
    std::uint32_t sample_count{};  // samples per side
    float sample_floor{};
    float sample_scale{};
    float sample_size{};           // metres between samples
    std::vector<std::uint16_t> heights; // row-major, row 0 at the north edge
    std::vector<TerrainShader> shaders;
    std::uint32_t patches_per_side{};
    std::vector<TerrainPatch> patches; // row-major, row 0 at the north edge
    // terrain_water_height_offset (token 251): the water's height at the
    // tile's corners SW, SE, NE, NW as stored (missing ones 0); water_height
    // spreads them. Patches with flag bits 0xC0 are water (0x49B088,
    // 0x4A2144).
    std::optional<std::array<float, 4>> water_heights;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
    // Height in metres of sample (row, column).
    [[nodiscard]] float height(std::uint32_t row, std::uint32_t column) const {
        return sample_floor + sample_scale * heights[static_cast<std::size_t>(row) * sample_count + column];
    }
    // The water's height (m) at a point of the tile: `east` 0 at its west
    // edge .. 1 at its east edge, `south` 0 at its north edge .. 1 at its
    // south edge. The corners' heights spread bilinearly from corner to
    // corner; a corner given as 0 takes the SW one, so a tile with only its
    // first height set is level. As Open Rails reads and lays them
    // (Orts.Formats.Msts/TerrainFile.cs, RunActivity/Viewer3D/Tiles.cs and
    // Water.cs, the Open Rails project, GPL-3.0). rts1.exe (0x4A2A5C) spreads
    // them away from the first values instead of towards the others: equal
    // corners (every water tile of the installed routes) come out the same,
    // sloped water did not (testers 2026-11-20).
    [[nodiscard]] float water_height(float east, float south) const;
};

[[nodiscard]] TerrainTile load_terrain_tile(const std::filesystem::path& tiles_directory,
                                            std::int32_t tile_x,
                                            std::int32_t tile_z);
// `tiles` is the route's TILES folder in the content VFS.
[[nodiscard]] TerrainTile load_terrain_tile(const core::Vfs& vfs,
                                            const core::VirtualPath& tiles,
                                            std::int32_t tile_x,
                                            std::int32_t tile_z);

} // namespace openrts::formats
