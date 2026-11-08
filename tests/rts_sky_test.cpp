#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include <openrts/formats/environment.hpp>
#include <glm/geometric.hpp>

#include <openrts/renderer/native_sky.hpp>
#include <openrts/renderer/rts_sky.hpp>

namespace {

int failures = 0;

void check(const bool condition, const char* what) {
    if (!condition) {
        std::printf("FAILED: %s\n", what);
        ++failures;
    }
}

constexpr char environment_text[] = R"(SIMISA@@@@@@@@@@JINX0w0t______
world (
    world_fog_distance ( 200 )
    world_fog_day_colour ( ff888888 )
    world_sky (
        world_sky_nlayers_behind_satellites ( 3 )
        world_sky_layers ( 2
            world_sky_layer (
                world_sky_layer_top ( world_sky_layer_top_nfaces ( 8 ) world_sky_layer_top_radius ( 800 ) world_sky_layer_top_height ( 250 ) )
                world_sky_layer_edge ( world_sky_layer_edge_steps ( 1 world_sky_layer_edge_step_height ( 0 ) world_sky_layer_edge_step_radius ( 1800 ) ) )
                world_anim_shader (
                    world_anim_shader_frames ( 1 world_anim_shader_frame ( world_anim_shader_frame_uvscroll ( 0 0.004 ) ) )
                    world_shader ( TexDiff terrain_texslots ( 1 terrain_texslot ( sky.ace 1 0 ) ) )
                )
            )
            world_sky_layer (
                world_sky_layer_fadein ( 16:00:00 18:00:00 )
                world_sky_layer_fadeout ( 06:00:00 08:00:00 )
                world_sky_layer_top ( world_sky_layer_top_nfaces ( 8 ) world_sky_layer_top_radius ( 800 ) world_sky_layer_top_height ( 550 ) )
                world_sky_layer_edge ( world_sky_layer_edge_steps ( 2
                    world_sky_layer_edge_step_height ( 500 ) world_sky_layer_edge_step_radius ( 1400 )
                    world_sky_layer_edge_step_height ( 240 ) world_sky_layer_edge_step_radius ( 1650 ) ) )
                world_anim_shader (
                    world_anim_shader_frames ( 1 world_anim_shader_frame ( world_anim_shader_frame_uvtiles ( 2 2 ) ) )
                    world_shader ( BlendATex terrain_texslots ( 1 terrain_texslot ( night.ace 2 0 ) ) )
                )
            )
        )
        world_sky_satellites ( 1
            world_sky_satellite (
                world_sky_satellite_low_scale ( 200 )
                world_sky_satellite_high_scale ( 150 )
                world_sky_satellite_rise_position ( 0 )
                world_sky_satellite_rise_time ( 07:13:00 )
                world_sky_satellite_set_time ( 17:02:00 )
                world_sky_satellite_dir_rise_colour ( ff776755 )
                world_sky_satellite_dir_high_colour ( ff999999 )
                world_sky_satellite_dir_set_colour ( ff776755 )
                world_sky_satellite_amb_rise_colour ( ff202020 )
                world_sky_satellite_amb_high_colour ( ff404040 )
                world_sky_satellite_amb_set_colour ( ff202020 )
                world_anim_shader ( world_shader ( AddATexDiff terrain_texslots ( 1 terrain_texslot ( sun.ace 1 0 ) ) ) )
            )
        )
    )
    world_water (
        world_water_layers ( 2
            world_water_layer (
                world_water_layer_height ( -.3 )
                world_anim_shader (
                    world_anim_shader_frames ( 1 world_anim_shader_frame ( world_anim_shader_frame_uvtiles ( 8 8 ) ) )
                    world_shader ( TexDiff terrain_texslots ( 1 terrain_texslot ( watertop.ace 1 0 ) ) )
                )
            )
            world_water_layer (
                world_water_layer_height ( 0 )
                world_anim_shader (
                    world_anim_shader_frames ( 1
                        world_anim_shader_frame ( world_anim_shader_frame_uvscroll ( 0 0.005 ) )
                        world_shader ( BlendATex terrain_texslots ( 1 terrain_texslot ( dots14.ace 1 0 ) ) )
                    )
                )
            )
        )
    )
)
)";

[[nodiscard]] bool near(const float a, const float b, const float tolerance = 1e-3f) {
    return std::fabs(a - b) <= tolerance;
}

} // namespace

int main() {
    namespace rts = openrts::renderer::rts;
    const auto file = std::filesystem::temp_directory_path() / "openrts-sky-test.env";
    {
        std::ofstream output(file, std::ios::binary);
        output << environment_text;
    }
    const auto environment = openrts::formats::load_environment(file);
    std::filesystem::remove(file);

    check(environment.ok(), "environment parsed");
    check(environment.layers_behind_satellites == 3, "layers behind satellites");
    check(environment.sky_layers.size() == 2 && environment.satellites.size() == 1, "layer and satellite counts");
    if (failures) return 1;

    const auto& base = environment.sky_layers[0];
    const auto& night = environment.sky_layers[1];
    check(base.top_faces == 8 && base.top_radius == 800 && base.top_height == 250, "top cap");
    check(base.edge_steps.size() == 1 && base.edge_steps[0].radius == 1800, "edge step");
    check(base.shader.shader_name == "TexDiff" && base.shader.texture == "sky.ace", "layer shader");
    check(base.shader.frames.size() == 1 && base.shader.frames[0].has_uv_scroll
              && near(base.shader.frames[0].uv_scroll[1], 0.004f),
          "uv scroll");
    check(night.shader.texture_addressing == 2 && near(night.shader.frames[0].uv_tiles[0], 2.0f), "night tiles");
    // RTS normalisation: fade-in moved before midnight.
    check(night.fade_in_start == 16 * 3600 - 86400 && night.fade_in_end == 18 * 3600 - 86400, "fade-in normalised");
    check(night.fade_out_start == 6 * 3600 && night.fade_out_end == 8 * 3600, "fade-out");

    // Water layers; the second's world_shader sits inside its frames block
    // (Novokuznetsk), where RTS's text search still finds it.
    check(environment.water && environment.water->layers.size() == 2, "water layers");
    if (environment.water && environment.water->layers.size() == 2) {
        const auto& bottom = environment.water->layers[0].shader;
        const auto& top = environment.water->layers[1].shader;
        check(bottom.shader_name == "TexDiff" && bottom.texture == "watertop.ace", "water layer shader");
        check(top.shader_name == "BlendATex" && top.texture == "dots14.ace", "shader inside the frames block");
        check(top.frames.size() == 1 && top.frames[0].has_uv_scroll, "frames before the misplaced shader");
    }

    // Night layer: invisible at noon, opaque at 3:00, half faded in at 17:00.
    check(!rts::evaluate_layer_fade(night, 12 * 3600).visible, "night layer hidden at noon");
    const auto night_3am = rts::evaluate_layer_fade(night, 3 * 3600);
    check(night_3am.visible && !night_3am.fading && near(night_3am.alpha, 1.0f), "night layer at 3:00");
    check(near(rts::evaluate_layer_fade(night, 17 * 3600).alpha, 0.5f), "night layer at 17:00");
    check(near(rts::evaluate_layer_fade(night, 7 * 3600).alpha, 0.5f), "night layer at 7:00");

    // Sun on a northern tile in summer: culminates mid-way between rise and set.
    const auto& sun = environment.satellites[0];
    const auto noon = (sun.rise_time + sun.set_time) / 2;
    const auto culminating = rts::evaluate_satellite(sun, noon, 14554, rts::Season::summer);
    const auto expected_elevation = static_cast<float>((0x4000 - 14554) * 0.000125 * 180.0 + 22.5);
    check(culminating.visible, "sun visible at noon");
    check(near(culminating.elevation_degrees, expected_elevation, 0.05f), "culmination elevation");
    check(near(culminating.azimuth_degrees, 180.0f, 0.5f), "culmination azimuth");
    check(culminating.direct_colour == 0xFF999999u && culminating.ambient_colour == 0xFF404040u, "noon colours");
    check(!rts::evaluate_satellite(sun, 2 * 3600, 14554, rts::Season::summer).visible, "sun hidden at night");

    // Simulation: noon light comes from the sun, night falls back to the ambient floor.
    rts::SkySimulation simulation(environment);
    const auto day = simulation.update(noon, 0.0f, 14554, rts::Season::summer);
    check(!day.night && near(day.lighting.sun_diffuse.x, 0x99 / 255.0f), "day lighting");
    check(day.lighting.sun_direction.y < 0.0f, "sun shines downwards");
    const auto midnight = simulation.update(0, 1.0f, 14554, rts::Season::summer);
    check(midnight.night && near(midnight.lighting.sun_ambient.x, 0x15 / 255.0f), "night ambient floor");
    check(near(midnight.layers[0].uv_offset[1], 0.004f), "uv scroll accumulates");
    check(near(midnight.fog_end, 1700.0f), "fog end from visible layers");

    // openRTS's own sky (native_sky.hpp): a blue zenith at noon, the
    // horizon under a low sun red, the night dark, the air letting through
    // less of the low sun's blue than of its red.
    {
        const glm::vec3 high_sun = glm::normalize(glm::vec3{0.0f, 0.77f, 0.64f});
        const auto zenith = openrts::renderer::atmosphere_radiance(300.0f, {0.0f, 1.0f, 0.0f}, high_sun);
        check(zenith.z > zenith.y && zenith.y > zenith.x && zenith.z > 0.1f && zenith.z < 2.0f, "native sky: blue zenith at noon");
        const glm::vec3 low_sun = glm::normalize(glm::vec3{0.0f, 0.05f, 1.0f});
        const auto towards = openrts::renderer::atmosphere_radiance(300.0f, glm::normalize(glm::vec3{0.0f, 0.03f, 1.0f}), low_sun);
        check(towards.x > towards.z, "native sky: the horizon red under a low sun");
        const auto dark = openrts::renderer::atmosphere_radiance(300.0f, {0.0f, 1.0f, 0.0f}, glm::normalize(glm::vec3{0.0f, -0.5f, 1.0f}));
        check(dark.x + dark.y + dark.z < 0.001f, "native sky: dark at night");
        const auto through = openrts::renderer::atmosphere_transmittance(300.0f, low_sun);
        check(through.x > through.z && through.x <= 1.0f && through.z >= 0.0f, "native sky: the low sun reddened");
    }

    if (failures == 0) std::printf("rts sky: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
