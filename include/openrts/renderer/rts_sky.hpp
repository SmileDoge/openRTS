#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <glm/vec4.hpp>

#include <openrts/formats/environment.hpp>
#include <openrts/renderer/model_renderer.hpp>

// Time-of-day sky of the original RTS renderer (rts1.exe 0x417D74), see
// docs/reverse/sky.md. Backend-independent: it turns an environment file and a
// clock into satellite positions, layer fades and the scene's Light[0].
namespace openrts::renderer::rts {

// Order of the .trk Environment block and of RTS's season variable.
enum class Season : std::uint8_t { spring = 0, summer = 1, autumn = 2, winter = 3 };

// RTS weather variable (MSTS activity order).
enum class Weather : std::uint8_t { clear = 0, snow = 1, rain = 2 };

// .trk Environment block condition for a season and weather, e.g. "SummerClear".
[[nodiscard]] const char* environment_condition(Season season, Weather weather) noexcept;

inline constexpr std::int32_t seconds_per_day = 86400;

// RTS config DefaultAmbientLight: lower bound of the sun's ambient colour when
// the environment has no world_environment_ambient_light.
inline constexpr std::uint32_t default_ambient_light = 0x151515;

struct SatelliteState {
    bool visible = false;
    float azimuth_degrees = 0.0f;
    float elevation_degrees = 0.0f;
    float scale = 0.0f;              // quad half size is 100 * scale (sun: * 1.5)
    std::uint32_t direct_colour = 0; // 0xAARRGGBB
    std::uint32_t ambient_colour = 0;
};

struct SkyLayerState {
    bool visible = false;
    bool fading = false;
    float alpha = 1.0f;
    float uv_offset[2]{};
};

struct SkyState {
    std::vector<SatelliteState> satellites;
    std::vector<SkyLayerState> layers;
    bool night = false;
    glm::vec4 clear_colour{0.0f};
    glm::vec4 fog_colour{0.0f};
    float fog_start = 300.0f;
    float fog_end = 500.0f;
    ModelLighting lighting{};
};

[[nodiscard]] glm::vec4 colour_from_argb(std::uint32_t argb);

// Keeps the per-layer UV scroll offsets, which RTS accumulates every frame.
class SkySimulation {
public:
    explicit SkySimulation(const formats::EnvironmentDefinition& environment);

    // `time_of_day` in seconds, `elapsed` frame time in seconds, `camera_tile_z`
    // the camera's MSTS tile Z (RTS derives the sun's culmination from it).
    [[nodiscard]] SkyState update(std::int32_t time_of_day, float elapsed, std::int32_t camera_tile_z, Season season);

private:
    const formats::EnvironmentDefinition* environment_;
    std::vector<std::array<float, 2>> uv_offsets_;
};

// Pure parts, exposed for tests.
[[nodiscard]] SatelliteState evaluate_satellite(const formats::SkySatellite& satellite,
                                                std::int32_t time_of_day,
                                                std::int32_t camera_tile_z,
                                                Season season);
[[nodiscard]] SkyLayerState evaluate_layer_fade(const formats::SkyLayer& layer, std::int32_t time_of_day);

} // namespace openrts::renderer::rts
