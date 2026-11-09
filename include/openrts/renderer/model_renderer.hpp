#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <openrts/core/vfs.hpp>
#include <openrts/core/virtual_path.hpp>
#include <openrts/formats/ace_file.hpp>
#include <openrts/formats/shape_binary.hpp>
#include <openrts/formats/shape_description.hpp>
#include <openrts/renderer/fog_ring.hpp>
#include <openrts/renderer/model_assets.hpp>

#ifdef OPENRTS_HAS_WGPU
#include <webgpu/webgpu.h>
#endif

namespace openrts::renderer {

// Scene lighting in the form the RTS shape shader consumes as Light[0]
// (the sun) plus its fog block. Colours are RGBA in 0..1.
// A further RTS light source for the shape shaders (GLLightObj of RTS's
// lights.glsl): a car's headlight or searchlight. Light space: the light at
// the origin shining along +z. Kind 1 spotlight, 2 sphere, 3 orthogonal, 4 cone.
struct ModelExtraLight {
    glm::vec4 diffuse{1.0f};
    glm::mat4 to_light{1.0f};
    glm::vec3 direction{0.0f, 0.0f, 1.0f}; // the way it shines, world
    glm::vec4 size{0.0f};                  // x, y: spread (tan of the half angle); z: reach; w: source radius
    float kind = 1.0f;
    float spot_exponent = 64.0f;           // x 128, as RTS keeps it
    glm::vec4 attenuation{2.0f, 0.6f, 0.4f, 0.0f};
    // A light array (LightArray): `array_count` lamps (0: one), each the one
    // before moved by array_step.xyz in the light's space; array_step.w its
    // length squared. The shaders light each point by the nearest lamp.
    std::uint32_t array_count{};
    glm::vec4 array_step{0.0f};
    // The instance it belongs to (ModelRenderer::set_owner), 0 none: with
    // lamp_skips_own_model() that instance is left out of its shadow.
    std::uint32_t owner{};
};

// RTS's further lights, in rings around the camera (rts1.exe, CONFIRMED).
// Ring k is centred (k + 1) tenths of MaxLightDistance from the camera and
// reaches a tenth of it to each side; a light goes into every ring its reach
// overlaps (0x4AF7D4 for the trains' lights, then 0x4B009C for the
// scenery's), nine a ring (Shader.NumLights 10, the sun first), a full ring
// giving its farthest light's place to a nearer one. An object takes the
// ring of its distance less its radius in tenths of ViewDistance, ring 0 when
// that is not positive or ring 0 is empty (0x4B042C; the terrain's patches
// alike, 0x4A2AD4). The two steps differ (RTS's own: 40 and 130 m with the
// installed config), so an object only sees lights near the camera.
// A light with the sphere it was added with (LightRings::all).
struct SceneLight {
    ModelExtraLight light;
    glm::vec3 centre{};
    float reach{};
};

struct LightRings {
    static constexpr std::size_t count = 10;
    static constexpr std::size_t capacity = 9;

    std::array<std::vector<ModelExtraLight>, count> lights;
    std::array<std::vector<float>, count> distances; // of each light, for the replacement
    // Every light added, ring or not: the experimental path clusters them all
    // (SceneEffects) instead of keeping nine a ring.
    std::vector<SceneLight> all;
    glm::vec3 eye{};
    float light_step = 100.0f;  // MaxLightDistance / 10 (ViewDistance / 10 without it)
    float object_step = 100.0f; // ViewDistance / 10

    // Empties the rings around `eye`.
    void reset(const glm::vec3& camera, float max_light_distance, float view_distance);
    // Adds a light whose reach is a sphere at `centre`.
    void add(const ModelExtraLight& light, const glm::vec3& centre, float reach);
    // The ring an object (bounding sphere) takes.
    [[nodiscard]] std::uint32_t ring_for(const glm::vec3& centre, float radius) const noexcept;
    // Distinct lights placed (a light in several rings counts once).
    [[nodiscard]] std::size_t placed() const noexcept { return placed_; }

private:
    std::size_t placed_ = 0;
};

struct ModelLighting {
    glm::vec4 sun_diffuse{1.0f};
    glm::vec4 sun_ambient{0.5f, 0.5f, 0.5f, 1.0f};
    // Direction the sunlight travels, in world space; see rts_sun_direction().
    glm::vec3 sun_direction{0.0f, -1.0f, 0.0f};
    glm::vec4 fog_colour{0.0f};
    float fog_start = 0.0f;
    float fog_end = 0.0f; // <= 0 disables fog
    // openRTS's sky: the fog's colour by direction (off: fog_colour everywhere).
    FogRing fog_ring;
    LightRings light_rings; // the further lights (RTS Shader.NumLights 10, the sun first)
    // Lamps of the cab or salon the player is in (RTS LightsInt): kind 4 cone,
    // 2 sphere, 3 box; up to 8.
    std::vector<ModelExtraLight> interior_lights;
};

// RTS sun direction from the environment's azimuth and elevation in degrees.
[[nodiscard]] glm::vec3 rts_sun_direction(float azimuth_degrees, float elevation_degrees);

// How shapes are shaded and ordered.
//  - rts_compatible reproduces the original RTS renderer, including its draw
//    order and the GL state carried between primitives (see rts_material.hpp).
//  - experimental is openRTS's own deterministic material model, the place for
//    future PBR, shadows and other features RTS never had.
enum class ShadingPath { rts_compatible, experimental };

class TaskPool;

// What a depth-only pass is for (SceneEffects): a shadow cascade, or the
// camera's pre-pass.
enum class DepthPassKind { shadow, camera };

struct ModelFrameContext {
    glm::mat4 view_projection{1.0f};
    glm::vec3 camera_position{};
    ModelLighting lighting{};
};

using ModelTransform = glm::mat4;

struct ModelBounds {
    glm::vec3 center{};
    float radius{};
    glm::vec3 min{}, max{}; // the box round every drawn vertex, in the shape's space
};

// A primitive state of a model as the inspector shows it.
struct ModelPrimitive {
    std::uint32_t index{};      // in the shape
    std::string image;          // its texture's image (file name)
    std::string shader;         // the shape's shader name
    std::int32_t light_material = -5;
    std::int32_t shape_light_material = -5; // before the override (openRTS's <shape>.openrts)
    std::uint32_t draws{};      // primitives drawing it, over every distance level
};

// A pose change of one shape node: `local` is applied in the node's own frame
// (after its matrix), e.g. a wheel turning about its axle.
struct GpuTexture;

// One of the shape's images (by its index in the .s) drawn as another
// texture for one instance: a cab's TextureChanges, seasonal or night
// textures of a model.
struct TextureOverride {
    std::uint32_t image{};
    std::shared_ptr<const GpuTexture> texture;
};

struct NodeAdjustment {
    std::int32_t matrix{};
    glm::mat4 local{1.0f};
    // >= 0: the node first takes its pose at this frame of the shape's first
    // animation (pantographs, doors), then `local` applies on top.
    float frame = -1.0f;
    // The node's own primitives drawn unlit (RTS node state flag 0x40: a lit
    // SOURCE_VISIBLE lamp's node, 0x514630).
    bool unlit = false;
    // The node's own primitives not drawn (RTS node state flag 0x01: an
    // unlit lamp's node, 0x514630, a cab signal light that is out, 0x525DB0).
    // RTS looks the state up by the primitive's own matrix, so the node's
    // children still show (a zero `local` would hide them too).
    bool hidden = false;
};

// RTS object pass of an instance (the object's pass byte, 0x408574).
enum class ObjectPass : std::uint8_t {
    // Statics and trains: pass 1, and pass 2 for translucent primitives.
    by_model,
    // Pass 3, the cab (0x438D50): after every other object, all of its
    // primitives in the shape's order in one pass, none left out. The
    // experimental path keeps that order too (it does not sort them).
    last,
};

struct ModelAnimationInfo {
    std::uint32_t frame_count{}; // 0: not animated
    std::uint32_t frame_rate{};
};

struct ModelRendererStats {
    std::size_t cached_textures{};
    std::size_t retired_pending{}; // generations waiting for the GPU
    std::uint64_t completed_frame{};
    // Last rendered frame.
    std::size_t instances{};  // submitted model instances
    std::size_t primitives{}; // instance primitives drawn
    std::size_t draw_calls{};    // primitives drawn, each with its instances
    std::size_t draw_commands{}; // calls issued in the main pass (a multi-draw counts once)
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
    // The shape's first animation (switch blades, doors, ...).
    [[nodiscard]] ModelAnimationInfo animation() const noexcept;
    // Matrices (with names), hierarchy and animations of the shape.
    [[nodiscard]] const formats::ShapeModel* rig() const noexcept;
    // ESD_Alternative_Texture bits and the subfolders this generation's
    // images came from (alternative_texture_folders).
    [[nodiscard]] std::uint32_t alternative_texture() const noexcept;
    [[nodiscard]] const std::vector<std::string>& texture_folders() const noexcept;
    // Whether some image is drawn only on FullBright surfaces, and the
    // texture_variant_key this generation was loaded with.
    [[nodiscard]] bool fullbright_images() const noexcept;
    [[nodiscard]] const std::vector<std::string>& texture_key() const noexcept;
    // ESD_Light_Sources of the shape (scenery lamps), in its space.
    [[nodiscard]] const std::vector<formats::ShapeLight>& lights() const noexcept;
    // Its primitive states, in the shape's order (the inspector).
    [[nodiscard]] const std::vector<ModelPrimitive>& primitives() const noexcept;
    // Its distance levels (m), nearest first; empty for one level.
    [[nodiscard]] const std::vector<float>& level_distances() const noexcept;

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
    // Back faces culled as RTS draws shapes (on by default); off draws both
    // sides, for comparison.
    void set_face_culling(bool enabled) noexcept;
    [[nodiscard]] bool face_culling() const noexcept;
    // Merging identical primitives into instanced draw calls (on by default).
    // Off draws every instance primitive separately in submission order, for
    // comparison.
    void set_batching(bool enabled) noexcept;
    [[nodiscard]] bool batching() const noexcept;
    // Multi-draw indirect (on by default where the device takes a first
    // instance in indirect arguments, IndirectFirstInstance): a model's
    // consecutive primitives with the same texture are drawn by one call.
    // Off: a call per primitive, as before.
    void set_multi_draw(bool enabled) noexcept;
    [[nodiscard]] bool multi_draw() const noexcept;
    [[nodiscard]] bool multi_draw_supported() const noexcept;
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
    // The last frame's draw calls by model, the most first (`top` lines):
    // calls, instances drawn, calls of one instance (not merged), the shape.
    [[nodiscard]] std::string draw_report(std::size_t top) const;

    // A frame is assembled first and then emitted the way RTS draws statics:
    // opaque pass, then translucent pass, each in submission order (RTS does not
    // sort translucent geometry), then the pass-3 instances (the cab).
    // Submitted RenderModels must outlive render().
    void begin_frame(const ModelFrameContext& frame);
    void submit(RenderModel& model, const ModelTransform& transform);
    // Draws an animated model at `frame` of its first animation (rest pose
    // when the shape has none).
    void submit(RenderModel& model, const ModelTransform& transform, float frame);
    // Draws a model with some nodes posed (wheels, bogies).
    void submit(RenderModel& model, const ModelTransform& transform, std::span<const NodeAdjustment> adjustments);
    // ... with some of its images drawn as other textures, in an object pass.
    void submit(RenderModel& model, const ModelTransform& transform, std::span<const NodeAdjustment> adjustments,
                std::span<const TextureOverride> textures, ObjectPass object_pass = ObjectPass::by_model);
    // A texture for overrides: the ACE decoded and uploaded the first time
    // (null when it does not read); kept while something holds it.
    [[nodiscard]] std::shared_ptr<const GpuTexture> texture(const core::Vfs& vfs, const core::VirtualPath& path);
    void submit(RenderModel& model);
    // The world object behind the instance submitted last: its StaticFlags
    // (.w) and whether it is a transfer, which decide its RTS object pass
    // (rts::object_pass; Terrain -1, Global 0, a transfer -2).
    void set_world_object(std::uint32_t static_flags, bool transfer);
    // A tag of the instance submitted last (0: none), the one its lamps carry
    // as ModelExtraLight::owner.
    void set_owner(std::uint32_t owner);
    // Records the frame's draws. `before_translucent` is called once in the
    // pass after the opaque and alpha-tested geometry and before the
    // translucent pass (glass), for helpers that must show through glass but
    // hide behind solid models; `before_last` once after the translucent pass
    // and before the cab (RTS pass 3: its light loop sits there, 0x408574).
    // Each is called at the end when there is nothing after its place.
    void render(WGPURenderPassEncoder pass, const std::function<void()>& before_translucent = {},
                const std::function<void()>& before_last = {});
    // Builds the frame's draws and uploads its data once a frame (render()
    // and render_depth() call it when it was not called).
    void prepare();
    // Depth only, for SceneEffects (the experimental path): every surface
    // that writes depth and is not blended, from `view_projection`, in depth
    // slot `slot` (< 12); a shadow pass takes the casters out of view too.
    // `skip_owner`: the instances tagged so are left out (a lamp's own model
    // in its shadow map; 0 none).
    void render_depth(WGPURenderPassEncoder pass, std::uint32_t slot, const glm::mat4& view_projection, DepthPassKind kind,
                      std::uint32_t skip_owner = 0);
    // An instance out of view that casts a shadow into it: shadow passes only.
    void submit_shadow_caster(RenderModel& model, const ModelTransform& transform);
    // Group 2 of the experimental path (SceneEffects::bind_group); nullptr: a
    // built-in one with every effect off.
    void set_effects(WGPUBindGroup effects);
    // Whether this frame has depth passes (render_depth): without them
    // prepare() does not build their draws.
    void set_depth_passes(bool wanted);
    // Workers prepare() may spread the per-instance data over (nullptr: none).
    void set_task_pool(TaskPool* pool);
    void draw(WGPURenderPassEncoder pass, RenderModel& model, const ModelFrameContext& frame);

private:
    void submit_posed(RenderModel& model, const ModelTransform& transform, std::span<const NodeAdjustment> adjustments);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};
#endif

} // namespace openrts::renderer
