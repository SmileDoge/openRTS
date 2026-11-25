#pragma once

// Rain and snow (the environment's world_precipitation, rts1.exe 0x41A318)
// and diesel smoke (DieselSpecialEffects, 0x50D3B4 / 0x50E4F8), drawn as
// sprites over the scene. See docs/world-objects.md.

#ifdef OPENRTS_HAS_WGPU
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <webgpu/webgpu.h>

#include <openrts/core/vfs.hpp>
#include <openrts/formats/environment.hpp>
#include <openrts/renderer/glow_sprites.hpp>
#include <openrts/renderer/light_shafts.hpp>
#include <openrts/renderer/weather_particles.hpp>

namespace openrts::viewer {

class PrecipitationView {
public:
    PrecipitationView(WGPUDevice device, WGPUTextureFormat target_format, std::shared_ptr<const core::Vfs> vfs);
    ~PrecipitationView();
    [[nodiscard]] bool ready() const noexcept { return sprites_ && sprites_->ready(); }
    // The environment's precipitation (nullptr: none) and wind; textures from `folders`.
    void set_environment(const formats::EnvPrecipitation* precipitation, std::vector<formats::EnvWindLayer> wind,
                         const std::vector<core::VirtualPath>& folders);
    // `amount` 0..2 (RTS's precipitation density 1; a thunderstorm's more); `light` the scene's
    // light on a surface facing up (RGBA).
    void update(float elapsed, const glm::vec3& eye, const glm::vec3& right, float amount, const glm::vec4& light);
    void render(WGPURenderPassEncoder pass, const glm::mat4& projection, const glm::mat4& view, float viewport_height);
    [[nodiscard]] std::size_t drops() const noexcept { return drops_.size(); }
    // The scene's origin moved by `shift` (the drops stay in the world).
    void set_origin_shift(const glm::vec3& shift) {
        weather_.origin_shift += glm::dvec3{shift};
        last_eye_ -= shift;
    }
    // The cab he is in (none fall inside it; openRTS's look).
    void set_room(const std::optional<renderer::LightShaftRoom>& room) {
        weather_.has_room = room.has_value();
        if (room) {
            weather_.to_room = room->to_room;
            weather_.room_lower = room->lower;
            weather_.room_upper = room->upper;
        }
    }
    [[nodiscard]] bool snow() const noexcept { return precipitation_ && precipitation_->snow; }
    // This frame's particles, the beams that light them (set_lamps) among them.
    [[nodiscard]] const renderer::WeatherParticles& weather() const noexcept { return weather_; }
    // The scene's lights: the nearest beams (cones: headlights, searchlights)
    // that reach the box of falling drops around `eye` light them.
    void set_lamps(const std::vector<renderer::SceneLight>& lights, const glm::vec3& eye);

private:
    std::unique_ptr<renderer::GlowSpriteRenderer> sprites_;
    // openRTS's look (renderer::own_precipitation): particles made on the
    // GPU; the eye's velocity (smoothed) and the clock they run on.
    std::unique_ptr<renderer::WeatherParticleRenderer> particles_;
    renderer::WeatherParticles weather_;
    glm::vec3 last_eye_{};
    bool has_eye_ = false;
    bool own_drawn_ = false;
    std::shared_ptr<const core::Vfs> vfs_;
    std::optional<formats::EnvPrecipitation> precipitation_;
    std::vector<formats::EnvWindLayer> wind_;
    std::uint32_t texture_ = 0;
    std::vector<glm::vec3> drops_;
    std::minstd_rand random_{20261103u};
    bool drawn_ = false;
};

// One exhaust nozzle this frame (TrainView::collect_exhausts).
struct ExhaustSource {
    std::uint64_t key{};       // the car and its exhaust
    glm::vec3 position{};      // the nozzle, scene coordinates
    glm::vec3 direction{};     // where it blows, scene coordinates
    glm::vec3 velocity{};      // the car's, m/s
    float acceleration{};      // the car's, m/s^2
    float nozzle = 0.1f;       // its width, m
    float rate{};              // puffs a second (0: none)
    float magnitude{};
    float dispersion{};
    float mass = 1.0f;
    std::uint32_t colour = 0xCF656577u; // ARGB
    float brightness = 1.0f;   // over the light (snow: whiter than smoke)
    float life = 0.0f;         // seconds a puff lives, fading (0: RTS's smoke)
    core::VirtualPath texture; // none: the round glow
};

class SmokeView {
public:
    SmokeView(WGPUDevice device, WGPUTextureFormat target_format, std::shared_ptr<const core::Vfs> vfs);
    ~SmokeView();
    [[nodiscard]] bool ready() const noexcept { return sprites_ && sprites_->ready(); }
    void set_wind(std::vector<formats::EnvWindLayer> wind) { wind_ = std::move(wind); }
    void set_origin_shift(const glm::vec3& shift);
    void update(float elapsed, const std::vector<ExhaustSource>& sources, const glm::vec4& light);
    void render(WGPURenderPassEncoder pass, const glm::mat4& projection, const glm::mat4& view, float viewport_height);
    [[nodiscard]] std::size_t particles() const noexcept;

private:
    struct Particle {
        glm::vec3 position{};
        float size = 0.0f;      // [3]: grows from 1 by the magnitude a second
        float scale = 1.0f;     // [4]
        float alpha = 1.0f;     // [5]
        float growth{};         // [6] the magnitude it was sent with
        float dispersion{};     // [7]
        float rate{};           // [8]
        float mass{};           // [9]
        glm::vec4 colour{};     // [10..13]
        glm::vec3 drift{};      // [14..16]
        float lift{};           // [17]
        glm::vec3 push{};       // [18..20]
        float head_start{};     // [5] with flag 8: sent part of a frame ago
        float age{};            // seconds (life)
        bool started = false;
    };
    struct Emitter {
        std::vector<Particle> particles;
        float clock{};
        glm::vec3 nozzle{};
        glm::vec3 direction{};
        glm::vec3 velocity{};
        float nozzle_size = 0.1f;
        float brightness = 1.0f;
        float life = 0.0f;
        std::uint32_t texture = 0;
        bool textured = false;
        bool seen = false;
    };

    [[nodiscard]] std::uint32_t texture(const core::VirtualPath& path);

    std::unique_ptr<renderer::GlowSpriteRenderer> sprites_;
    std::shared_ptr<const core::Vfs> vfs_;
    std::vector<formats::EnvWindLayer> wind_;
    std::unordered_map<std::uint64_t, Emitter> emitters_;
    std::unordered_map<std::string, std::uint32_t> textures_;
    std::minstd_rand random_{19750331u};
};

} // namespace openrts::viewer
#endif
