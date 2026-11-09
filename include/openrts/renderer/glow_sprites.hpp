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

// Signal lights at the lens's own size however far (not as in RTS): far
// off they shrink with the signal, down to a dot of GlowSprite::min_pixels,
// and no longer keep RTS's angle (far_scale), which made a far signal's
// light a big blob. Read each frame (SignalView).
inline bool& signal_lights_real_size() {
    static bool on = true;
    return on;
}

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
    // At least this many pixels (half its size) on the screen, whatever the
    // distance (0: no least size): a far light stays a dot.
    float min_pixels{};
    // 1: a quad min_pixels made bigger keeps its light, spread over it
    // (dimmed by the sizes' ratio squared), as MSAA's samples would average
    // a light smaller than a pixel; 0: as bright as near (signal lights).
    // Without it a train's lamp far off fell between the pixels' centres
    // and went out with MSAA off.
    float spread_light{};
};

// How sprites meet the frame: added (rgb x alpha, an emissive light in the
// HDR experimental path) or laid over by their alpha as RTS does (its light
// loop keeps GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA).
enum class GlowBlend { added, over };

// Light sprites (signal lights, RTS LightTex): a textured quad tinted by the
// light colour, added to the scene or laid over it (GlowBlend), depth-tested without writing depth. A
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
    // Sprites for this frame. Their size is fixed: RTS multiplies a light's
    // size by its camera's field of view / 45 degrees, so its lights grow and
    // shrink as the FOV changes; here it is the factor at RTS's installed
    // FOV (65 degrees), whatever the field of view, as content was made for.
    void set_sprites(std::vector<GlowSprite> sprites);
    // The size factor (default RTS's lights: 65 / 45); smoke and rain use 1.
    void set_size_factor(float factor) noexcept;
    void render(WGPURenderPassEncoder pass, const glm::mat4& projection, const glm::mat4& view, float viewport_height,
                GlowBlend blend = GlowBlend::added);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openrts::renderer
#endif
