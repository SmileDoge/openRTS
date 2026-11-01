#pragma once

#ifdef OPENRTS_HAS_WGPU
#include <cstdint>
#include <memory>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <webgpu/webgpu.h>

#include <openrts/renderer/webgpu_texture.hpp>

namespace openrts::renderer {

struct GlowSprite {
    glm::vec3 position{};
    float radius{};           // half size of the quad, m
    glm::vec4 uv{0.0f, 0.0f, 1.0f, 1.0f}; // u0 v0 u1 v1 of the texture
    std::uint32_t colour{};   // tint, 0xAABBGGRR
    std::uint32_t texture{};  // from add_texture; 0 = the built-in round glow
    // Far size per metre of distance: the sprite is max(radius, distance *
    // far_scale) (RTS 0x408574: 0.012 * LightsScale * |u1 - u0| * width / 512).
    float far_scale{};
    // The quad's half-axes directions (unit). Zero: it faces the camera.
    // Signal lights lie flat in the signal's XY plane, as in RTS 0x449818.
    glm::vec3 axis_x{};
    glm::vec3 axis_y{};
};

// Light sprites (signal lights, RTS LightTex): a textured quad tinted by the
// light colour, added to the scene, depth-tested without writing depth. A
// quad either lies in a given plane (signal lights: flat on the lens, like
// RTS) or faces the camera. Near, a sprite has its own size; far, it keeps a
// constant angular size (far_scale), and everything scales with the field of
// view as RTS does (fov / 45 degrees).
class GlowSpriteRenderer {
public:
    GlowSpriteRenderer(WGPUDevice device, WGPUTextureFormat target_format);
    ~GlowSpriteRenderer();
    GlowSpriteRenderer(const GlowSpriteRenderer&) = delete;
    GlowSpriteRenderer& operator=(const GlowSpriteRenderer&) = delete;

    [[nodiscard]] bool ready() const noexcept;
    // An RGBA8 texture for sprites; returns its id (0 on failure: the built-in glow).
    [[nodiscard]] std::uint32_t add_texture(Rgba8ImageView image);
    // Sprites for this frame.
    void set_sprites(std::vector<GlowSprite> sprites);
    void render(WGPURenderPassEncoder pass, const glm::mat4& projection, const glm::mat4& view, float viewport_height);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openrts::renderer
#endif
