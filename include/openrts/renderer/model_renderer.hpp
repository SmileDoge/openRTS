#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <openrts/core/virtual_path.hpp>
#include <openrts/formats/ace_file.hpp>
#include <openrts/formats/shape_binary.hpp>
#include <openrts/renderer/model_assets.hpp>

#ifdef OPENRTS_HAS_WGPU
#include <webgpu/webgpu.h>
#endif

namespace openrts::renderer {

// Scene lighting in the form the RTS shape shader consumes as Light[0]
// (the sun) plus its fog block. Colours are RGBA in 0..1.
struct ModelLighting {
    glm::vec4 sun_diffuse{1.0f};
    glm::vec4 sun_ambient{0.5f, 0.5f, 0.5f, 1.0f};
    // Direction the sunlight travels, in world space; see rts_sun_direction().
    glm::vec3 sun_direction{0.0f, -1.0f, 0.0f};
    glm::vec4 fog_colour{0.0f};
    float fog_start = 0.0f;
    float fog_end = 0.0f; // <= 0 disables fog
};

// RTS sun direction from the environment's azimuth and elevation in degrees.
[[nodiscard]] glm::vec3 rts_sun_direction(float azimuth_degrees, float elevation_degrees);

// How shapes are shaded and ordered.
//  - rts_compatible reproduces the original RTS renderer, including its draw
//    order and the GL state carried between primitives (see rts_material.hpp).
//  - experimental is openRTS's own deterministic material model, the place for
//    future PBR, shadows and other features RTS never had.
enum class ShadingPath { rts_compatible, experimental };

struct ModelFrameContext {
    glm::mat4 view_projection{1.0f};
    glm::vec3 camera_position{};
    ModelLighting lighting{};
};

using ModelTransform = glm::mat4;

struct ModelBounds {
    glm::vec3 center{};
    float radius{};
};

struct ModelRendererStats {
    std::size_t cached_textures{};
    std::size_t retired_pending{}; // generations waiting for the GPU
    std::uint64_t completed_frame{};
};

#ifdef OPENRTS_HAS_WGPU
class RenderModel {
public:
    RenderModel();
    ~RenderModel();
    RenderModel(const RenderModel&) = delete;
    RenderModel& operator=(const RenderModel&) = delete;
    RenderModel(RenderModel&&) noexcept;
    RenderModel& operator=(RenderModel&&) noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] ModelBounds bounds() const noexcept;
    [[nodiscard]] const core::VirtualPath& source() const noexcept;
    // Keys of the files this generation was built from (shape, .sd, texture
    // candidates), for hot reload.
    [[nodiscard]] std::vector<std::string> dependency_keys() const;

private:
    struct Impl;
    // The current generation; frames and the retire queue share it.
    std::shared_ptr<Impl> impl_;
    friend class ModelRenderer;
};

class ModelRenderer {
public:
    ModelRenderer(WGPUDevice device, WGPUTextureFormat target_format);
    ~ModelRenderer();
    ModelRenderer(const ModelRenderer&) = delete;
    ModelRenderer& operator=(const ModelRenderer&) = delete;
    ModelRenderer(ModelRenderer&&) noexcept;
    ModelRenderer& operator=(ModelRenderer&&) noexcept;

    [[nodiscard]] bool ready() const noexcept;
    void set_shading_path(ShadingPath path) noexcept;
    [[nodiscard]] ShadingPath shading_path() const noexcept;
    // Uploads a model; textures already uploaded for the same virtual path are
    // shared.
    [[nodiscard]] RenderModel create_model(StreamedModelAsset&& asset);

    // Hot reload. replace_texture() uploads a new generation of a texture into
    // the cache (models created afterwards use it) and returns a handle that
    // keeps it alive until those models exist. replace() swaps a model slot to
    // a new generation and retires the old one; retired generations are freed
    // once the GPU has finished every frame that may use them.
    [[nodiscard]] std::shared_ptr<const void> replace_texture(const core::VirtualPath& path,
                                                              const formats::AceRgbaImage& image);
    void forget_texture(const core::VirtualPath& path);
    void replace(RenderModel& slot, RenderModel&& next);
    void retire(RenderModel&& model);
    // Call after each queue submit, then collect_retired() once per frame.
    void frame_submitted(WGPUQueue queue);
    std::size_t collect_retired();
    [[nodiscard]] ModelRendererStats stats() const;

    // A frame is assembled first and then emitted the way RTS draws statics:
    // opaque pass, then translucent pass, each in submission order (RTS does not
    // sort translucent geometry). Submitted RenderModels must outlive render().
    void begin_frame(const ModelFrameContext& frame);
    void submit(RenderModel& model, const ModelTransform& transform);
    void submit(RenderModel& model);
    void render(WGPURenderPassEncoder pass);
    void draw(WGPURenderPassEncoder pass, RenderModel& model, const ModelFrameContext& frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
#endif

} // namespace openrts::renderer
