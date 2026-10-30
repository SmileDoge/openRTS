#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <openrts/core/vfs.hpp>

// MSTS/RTS environment (.env) files: sky layers and satellites. Times are
// seconds of the day; colours are raw 0xAARRGGBB values. Satellites are stored
// in RTS order: the light-emitting one is moved to index 0 (the sun).
namespace openrts::formats {

struct EnvAnimFrame {
    float uv_tiles[2]{1.0f, 1.0f};
    float uv_scroll[2]{};
    bool has_uv_scroll = false;
};

struct EnvShader {
    std::string shader_name; // e.g. TexDiff, BlendATex, AddATexDiff
    std::string texture;
    // terrain_texslot addressing value: 2 clamps, anything else repeats.
    std::uint32_t texture_addressing = 1;
    std::vector<EnvAnimFrame> frames;
};

struct SkyEdgeStep {
    std::int32_t height{};
    std::int32_t radius{};
};

struct SkyLayer {
    // Fade window as normalised by RTS: fade-in may start before midnight
    // (negative), fade-out may end after it (> 86400). All zero: no fading.
    std::int32_t fade_in_start{};
    std::int32_t fade_in_end{};
    std::int32_t fade_out_start{};
    std::int32_t fade_out_end{};
    std::uint16_t top_faces{};
    std::int32_t top_radius{};
    std::int32_t top_height{};
    std::vector<SkyEdgeStep> edge_steps;
    EnvShader shader;
};

struct SkySatellite {
    std::uint16_t low_scale{};
    std::uint16_t high_scale{};
    std::uint16_t rise_position{}; // degrees
    std::int32_t rise_time{};      // made negative when rising before the previous midnight
    std::int32_t set_time{};
    std::uint32_t direct_colours[3]{};  // rise, high, set
    std::uint32_t ambient_colours[3]{}; // rise, high, set
    bool light = false;                 // world_sky_satellite_light: lights the scene
    EnvShader shader;
};

struct EnvironmentDefinition {
    float fog_distance{};
    std::uint32_t fog_day_colour{};
    std::uint32_t fog_night_colour{};
    // world_environment_ambient_light: lower bound of the scene ambient colour.
    std::optional<std::uint32_t> ambient_light;
    std::uint16_t layers_behind_satellites{};
    std::vector<SkyLayer> sky_layers;
    std::vector<SkySatellite> satellites;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

[[nodiscard]] EnvironmentDefinition load_environment(const std::filesystem::path& file);
[[nodiscard]] EnvironmentDefinition parse_environment(std::string_view bytes);
[[nodiscard]] EnvironmentDefinition load_environment(const core::Vfs& vfs, const core::VirtualPath& file);

// Parses "hh:mm" or "hh:mm:ss" into seconds of the day.
[[nodiscard]] std::int32_t parse_time_of_day(const std::string& value);

} // namespace openrts::formats
