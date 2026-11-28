#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <openrts/core/vfs.hpp>
#include <openrts/core/virtual_path.hpp>
#include <openrts/formats/ace_file.hpp>
#include <openrts/formats/shape_binary.hpp>
#include <openrts/formats/shape_description.hpp>
#include <openrts/renderer/fog_ring.hpp>
#include <openrts/formats/material.hpp>
#include <openrts/renderer/model_assets.hpp>

#ifdef OPENRTS_HAS_WGPU
#include <webgpu/webgpu.h>
#endif

namespace openrts::renderer {

// Scene lighting in the form the RTS shape shader consumes as Light[0]
// (the sun) plus its fog block. Colours are RGBA in 0..1.
// A further RTS light source for the shape shaders (GLLightObj of RTS's
// lights.glsl): a car's headlight or searchlight. Light space: the light at
// the origin shining along +z. Kind 1 spotlight, 2 sphere, 3 orthogonal, 4 cone;
// plus 32: positional, plus 64: it throws no shadow (ORTSNoShadow).
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
    // Its shadow priority (formats::ShapeLight::shadow_priority), 0..5: the
    // lamps' shadow maps go by it with their light where the player is.
    std::uint8_t shadow_priority = 1;
    // A photometric lamp (photometric.hpp): rgb its light at 1 m in the
    // scene's units, w 1; the experimental path's physical light takes it
    // (inverse square) in place of `diffuse` and the fall-off. w 0: none.
    glm::vec4 photometric{0.0f};
};

// A cone light's cone (kinds 1 and 4, one lamp): its apex, axis (unit),
// reach from the apex and the tangent of its half angle, as the shaders'
// physical_light and light_factor bound it. None for a light array or a
// cone wider than some 84 degrees each way. The clusters and the falling
// rain and snow (the headlights' beams) take it.
struct ExtraLightCone {
    glm::vec3 apex;
    glm::vec3 axis;
    float reach;
    float tangent;
};
[[nodiscard]] std::optional<ExtraLightCone> extra_light_cone(const ModelExtraLight& light);

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

// openRTS: a doorway the camera looks through into another room (the
// passage door of the car he is in, open into the car coupled there): its
// centre and its two half axes, across its width and up its height, all in
// world space; cross(across, up) points into the next car. Seen through it,
// by distance beyond it along that normal: the gap between the cars
// (near_cut .. far_cut: the gangway's plates and bellows), then the next
// car from its end wall (far_cut) on. An instance tagged set_portal(+k) is
// drawn only through portal k - 1 (that car's salon or cab); -k everywhere
// but through it beyond far_cut, and inside the doors' opening (door_half_*
// round the centre) beyond far_door (that car's outside: its insides, and
// its end door, part of its end wall in the shape, the ED9M's); 8 + k only
// through it between near_cut and far_cut, not inside the opening short of
// near_door, no higher than the opening (the outside of the car he is in:
// its half of the gangway, not its own end door nor its end's top; no
// shadow).
struct ModelPortal {
    glm::vec3 centre{};
    glm::vec3 across{1.0f, 0.0f, 0.0f}, up{0.0f, 1.0f, 0.0f}; // unit
    float half_across{}, half_up{};
    float near_cut{}, far_cut{};
    float near_door{}, far_door{};
    float door_half_across{}, door_half_up{};
};
inline constexpr std::size_t max_portals = 2;
// The cab's or salon's lamps lit at once (RTS LightsInt): RTS takes
// config.ini's Shader.NumLightsInt (64 without it, at most 1024; the MDD
// build's 16); openRTS's frame holds 64. Until 2026-11-15 8: the 2ES6's
// desk lamps past the eighth stayed dark.
inline constexpr std::size_t max_interior_lights = 64;

// The room the eye is in (the cab, a salon): world -> its space, and its
// box there. Its surfaces take the light indoors (SceneEffectSettings::indoor_light).
struct ModelRoom {
    glm::mat4 to_room{1.0f};
    glm::vec3 lower{}, upper{};
};

// openRTS (beyond RTS): raindrops on the glass of the cab the eye is in
// (ModelLighting::room): the blended surfaces at its box's walls facing out.
// They gather with the time since the glass was last wiped and the rain's
// amount; on the move they run up and back, standing they slide down.
inline constexpr int rain_grid_columns = 128, rain_grid_rows = 64;
struct ModelRainGlass {
    float amount{};      // 0..1, the rain's
    float time{};        // s, the clock the times below are on
    float since{};       // s on that clock the glass has been wet from (the rain's start)
    float speed{};       // m/s, the train's (without its sign)
    bool snow = false;   // snowflakes sticking, not drops running
    float flow{};        // m the drops have run along the glass's up (TrainView integrates it)
    // On the side windows (their z, y): how far the drops have run (m), and
    // which way they run now (unit): back and down on the move, down standing.
    float side_x{}, side_y{};
    float side_dir_x{}, side_dir_y{-1.0f};
    // The snow's run (m): only on the move, slower than water (it sticks),
    // up the front glass and back along the side windows.
    float snow_flow{}, snow_side_x{}, snow_side_y{};
    // The wipers' glass: the room's front (+z) or back (-z) wall, its x and
    // y range, and when each cell of it (rain_grid_columns x _rows, rows
    // up from y0) was last wiped (on `time`'s clock; very old: never).
    bool wiped_glass = false;
    float front = 1.0f;
    float x0{}, y0{}, x1{}, y1{};
    std::array<float, rain_grid_columns * rain_grid_rows> wiped{};
};

struct ModelLighting {
    glm::vec4 sun_diffuse{1.0f};
    glm::vec4 sun_ambient{0.5f, 0.5f, 0.5f, 1.0f};
    // A lightning flash's light this frame (lightning.hpp), as sun_ambient
    // is encoded: added to it on every surface, shapes, terrain and haze
    // alike; not daylight (the lamps by day stay as they are).
    glm::vec4 flash{0.0f};
    // Direction the sunlight travels, in world space; see rts_sun_direction().
    glm::vec3 sun_direction{0.0f, -1.0f, 0.0f};
    glm::vec4 fog_colour{0.0f};
    float fog_start = 0.0f;
    float fog_end = 0.0f; // <= 0 disables fog
    // openRTS's sky: the fog's colour by direction (off: fog_colour everywhere).
    FogRing fog_ring;
    LightRings light_rings; // the further lights (RTS Shader.NumLights 10, the sun first)
    // Lamps of the cab or salon the player is in (RTS LightsInt): kind 4 cone,
    // 2 sphere, 3 box; up to max_interior_lights.
    std::vector<ModelExtraLight> interior_lights;
    // The doorways he looks through into the next cars (ModelPortal), up to 2.
    std::vector<ModelPortal> portals;
    // The cab or salon he is in.
    std::optional<ModelRoom> room;
    // Raindrops on its glass (with `room`).
    std::optional<ModelRainGlass> rain;
    // How much of the snow the models' snow maps paint lies now (0: none,
    // summer; 1: all of it).
    float snow_cover = 0.0f;
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
// sun_shadow: a shadow pass of the sun's cascades (with shadows by rays the
// models the rays see stay out of it, ray_scene.hpp); shadow: any other (the
// lamps', the cab's map).
enum class DepthPassKind { shadow, camera, sun_shadow };
// Which casters a depth pass draws: all, those that stay where they are (the
// world's objects, the terrain), or those that move (ModelRenderer::set_moving:
// trains, road traffic, crossing gates, switch blades). The sun's shadow cache
// keeps the still ones' depth between frames (SceneEffects). `trains`: the
// trains alone (set_train; the temporal AA's moving models).
enum class DepthCasters : std::uint8_t { all, still, moving, trains };

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
    std::string name;           // the primitive state's label in the shape
    std::string matrix;         // the node it hangs on (its vertex state's matrix)
    // openRTS's material (PBR stage 4): as drawn, and the layers over RTS's.
    formats::Material material;
    std::vector<std::pair<formats::MaterialLayer, formats::MaterialPatch>> material_layers;
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
    std::uint64_t texture_bytes{};       // the cached textures' levels on the GPU
    std::uint64_t train_texture_bytes{}; // of them, the stock's (TRAINS/)
    std::uint64_t rgba8_texture_bytes{}; // of them, uncompressed (RGBA8: 4 bytes a texel; BC1 has a half)
    std::uint64_t geometry_bytes{};       // the models' vertex and index buffers alive
    std::uint64_t train_geometry_bytes{}; // of them, the stock's
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
    // The texture_variant_key this generation was loaded with.
    [[nodiscard]] const std::vector<std::string>& texture_key() const noexcept;
    // ESD_Light_Sources of the shape (scenery lamps), in its space.
    [[nodiscard]] const std::vector<formats::ShapeLight>& lights() const noexcept;
    // Its .sd's ESD_Pickup_Object: the shape is a pickup (docs/reverse/cargo.md).
    [[nodiscard]] const std::optional<formats::PickupDescription>& pickup() const noexcept;
    // Its lamps changed in place (the inspector's, live; till it is loaded anew).
    void set_lights(std::vector<formats::ShapeLight> lights);
    // Its primitive states, in the shape's order (the inspector).
    [[nodiscard]] const std::vector<ModelPrimitive>& primitives() const noexcept;
    // Its distance levels (m), nearest first; empty for one level.
    [[nodiscard]] const std::vector<float>& level_distances() const noexcept;
    // What was wrong with its material files (PBR stage 4), for the inspector.
    [[nodiscard]] const std::vector<std::string>& material_messages() const noexcept;

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
    // The material maps (docs/pbr-materials.md) made from any of these files
    // (VirtualPath::key()) forgotten: the models made from now on upload
    // them again. Until 2026-11-26 a reloaded model took the old map from
    // the cache, so a material's PNG changed showed nothing (testers).
    void forget_material_maps(const std::unordered_set<std::string>& files);
    // Every texture forgotten: the models made from now on upload their own
    // (texture_size_limit() changed; those loaded keep theirs till reloaded).
    void forget_textures();
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
    // After the frame's submits, before prepare(): the room the eye is in
    // as this frame placed it (ModelLighting::room), over begin_frame()'s.
    // The scene's lighting is made before the trains are submitted, so its
    // room was the last frame's: a train moving 0.7 m a frame (30 fps, 80
    // km/h) left the cab's front outside it, its light by day cut off across
    // the cab and flickering (testers, 2026-11-20).
    void set_room(const std::optional<ModelRoom>& room) noexcept;
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
    // The instance submitted last seen through a portal of the frame
    // (ModelLighting::portals): +k only through portal k - 1, -k everywhere
    // but through it beyond its far cut, 8 + k only through it between its
    // cuts (left out of the shadow passes), 0 as always.
    void set_portal(std::int32_t code);
    // Instances submitted from here on move (true) or stay (false, the
    // default): trains, road traffic, crossing gates, switch blades. The
    // sun's shadow cache redraws only those every frame.
    void set_moving(bool moving) noexcept;
    // Instances submitted from here on are trains (true) or not (the
    // default): the temporal AA takes their points as riding with the camera.
    void set_train(bool train) noexcept;
    // Without MSAA under a temporal AA or an upscaler: an alpha test's edge
    // by noise that changes every frame (`frame`), which those blend into a
    // soft edge as alpha to coverage gives under MSAA; off: the test at its
    // cut (RTS's).
    void set_stochastic_alpha(bool on, std::uint32_t frame) noexcept;
    // Added to every texture's LOD bias this frame: an upscaler drawing at a
    // smaller size than the window's wants log2(that size / the window's)
    // (DLSS's guide), the textures as sharp as at the window's size.
    void set_texture_lod_bias(float bias) noexcept;
    // After prepare(): a hash of this frame's moving instances (set_moving):
    // their models, distance levels, places and node poses, to a millimetre
    // and a ten-thousandth. The same as last frame: nothing moving moved
    // (the shadow maps may be kept, SceneEffects::set_moving_signature).
    // Without the cab (RTS pass 3: its needles and displays live every
    // frame) when `with_cab` is false.
    [[nodiscard]] std::uint64_t moving_signature(bool with_cab = true) const noexcept;
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
    // prepare()'s last step: the ray scene's TLAS (ray_scene.hpp).
    void build_ray_scene();
    // Depth only, for SceneEffects (the experimental path): every surface
    // that writes depth and is not blended, from `view_projection`, in depth
    // slot `slot` (< depth_slots); a shadow pass takes the casters out of
    // view too. `skip_owner`: the instances tagged so are left out (a lamp's
    // own model in its shadow map; 0 none). `casters`: all, the still ones or
    // the moving ones (set_moving).
    void render_depth(WGPURenderPassEncoder pass, std::uint32_t slot, const glm::mat4& view_projection, DepthPassKind kind,
                      std::uint32_t skip_owner = 0, DepthCasters casters = DepthCasters::all);
    // Depth slots: the cascades, the camera, the focus, 8 lamps, the shadow
    // cache's pieces (8 a cascade) and its check (a cascade each).
    static constexpr std::uint32_t depth_slots = 51;
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
