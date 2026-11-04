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

namespace openrts::viewer {

class PrecipitationView {
public:
    PrecipitationView(WGPUDevice device, WGPUTextureFormat target_format, std::shared_ptr<const core::Vfs> vfs);
    ~PrecipitationView();
    [[nodiscard]] bool ready() const noexcept { return sprites_ && sprites_->ready(); }
    // The environment's precipitation (nullptr: none) and wind; textures from `folders`.
    void set_environment(const formats::EnvPrecipitation* precipitation, std::vector<formats::EnvWindLayer> wind,
                         const std::vector<core::VirtualPath>& folders);
    // `amount` 0..1 (RTS's precipitation density); `light` the scene's
    // light on a surface facing up (RGBA).
    void update(float elapsed, const glm::vec3& eye, const glm::vec3& right, float amount, const glm::vec4& light);
    void render(WGPURenderPassEncoder pass, const glm::mat4& projection, const glm::mat4& view, float viewport_height);
    [[nodiscard]] std::size_t drops() const noexcept { return drops_.size(); }
    [[nodiscard]] bool snow() const noexcept { return precipitation_ && precipitation_->snow; }

private:
    std::unique_ptr<renderer::GlowSpriteRenderer> sprites_;
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
    core::VirtualPath texture;
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
        bool started = false;
    };
    struct Emitter {
        std::vector<Particle> particles;
        float clock{};
        glm::vec3 nozzle{};
        glm::vec3 direction{};
        glm::vec3 velocity{};
        float nozzle_size = 0.1f;
        std::uint32_t texture = 0;
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
