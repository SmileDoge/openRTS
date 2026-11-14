#pragma once

#ifdef OPENRTS_HAS_WGPU
#include <cstdint>
#include <memory>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <webgpu/webgpu.h>

#include <openrts/renderer/webgpu_texture.hpp>

namespace openrts::renderer {

struct ScreenQuadVertex {
    glm::vec3 position{};               // scene coordinates
    glm::vec2 uv{};
    std::uint32_t colour = 0xFFFFFFFFu; // RGBA8 tint, 0xAABBGGRR
    glm::vec2 local{};                  // on its screen: 0..1 across (x) and down (y)
};

struct ScreenQuadDraw {
    std::uint32_t texture{}; // from add_texture (premultiplied for a blended draw drawn supersampled)
    bool blend = true;       // alpha blending, else opaque
    std::uint32_t first{}, count{}; // vertices (triangles) in the frame's list
    std::uint32_t surface{};        // the screen it is on (ScreenSurface in the frame's list)
};

// A screen this frame: its corners (scene coordinates), its size (m).
struct ScreenSurface {
    glm::vec3 top_left{}, top_right{}, bottom_right{};
    float width{}, height{};
};

// openRTS: each screen drawn into an image of its own at about 5000 pixels
// a metre (several times what it covers on the monitor), blended with
// premultiplied alpha in linear light, its mipmaps averaged in linear light,
// and that image laid on the panel (supersampling). Off: every object drawn
// straight onto the panel, as RTS does. Thin lines then keep their light
// (a fifth of a pixel shows a fifth of its brightness, not a twentieth),
// edges and turned needles lose their steps. Read each frame.
inline bool& cab_screens_supersampled() {
    static bool on = true;
    return on;
}

// Cab screens (RTS SCREEN displays): textured triangles on the panels, tested
// against the depth without writing it, as RTS draws them (0x46B668). They
// sit a millimetre towards the eye so the panel under them does not show
// through.
class ScreenQuadRenderer {
public:
    ScreenQuadRenderer(WGPUDevice device, WGPUTextureFormat target_format);
    ~ScreenQuadRenderer();
    ScreenQuadRenderer(const ScreenQuadRenderer&) = delete;
    ScreenQuadRenderer& operator=(const ScreenQuadRenderer&) = delete;

    [[nodiscard]] bool ready() const noexcept;
    // An RGBA8 texture; its id, 0 on failure. `premultiplied`: as the
    // supersampled screens blend it (colour times alpha in linear light, the
    // mipmaps made from it; the file's own are left).
    [[nodiscard]] std::uint32_t add_texture(Rgba8ImageView image, bool premultiplied = false);
    // This frame's triangles, replacing the last frame's.
    void set(std::vector<ScreenQuadVertex> vertices, std::vector<ScreenQuadDraw> draws, std::vector<ScreenSurface> surfaces);
    // Supersampled: draws each screen into its image (before the main pass).
    void prepare(WGPUCommandEncoder encoder);
    void render(WGPURenderPassEncoder pass, const glm::mat4& view_projection, const glm::vec3& eye);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// RGBA8 levels (display-encoded colour, straight alpha) to sRGB-encoded
// colour premultiplied by alpha in linear light, level 0 and its mipmaps
// (box filter in linear light) down to 1 x 1. For tests and add_texture.
[[nodiscard]] std::vector<std::vector<std::uint8_t>> premultiplied_linear_levels(std::uint32_t width, std::uint32_t height,
                                                                                 const std::vector<std::uint8_t>& pixels);

} // namespace openrts::renderer
#endif
