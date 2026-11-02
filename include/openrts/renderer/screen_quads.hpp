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
};

struct ScreenQuadDraw {
    std::uint32_t texture{}; // from add_texture
    bool blend = true;       // alpha blending, else opaque
    std::uint32_t first{}, count{}; // vertices (triangles) in the frame's list
};

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
    // An RGBA8 texture; its id, 0 on failure.
    [[nodiscard]] std::uint32_t add_texture(Rgba8ImageView image);
    // This frame's triangles, replacing the last frame's.
    void set(std::vector<ScreenQuadVertex> vertices, std::vector<ScreenQuadDraw> draws);
    void render(WGPURenderPassEncoder pass, const glm::mat4& view_projection, const glm::vec3& eye);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openrts::renderer
#endif
