#pragma once

#include <cstdint>
#include <string_view>

#include <openrts/formats/shape_binary.hpp>

// Shape material rules of the original RTS renderer (rts1.exe 2021-08), see
// docs/reverse/shape-rendering.md. Everything here is backend-independent: it
// decides *what* fixed-function state and shader inputs a primitive gets, and
// in which pass it is drawn; the WebGPU renderer only executes the decision.
namespace openrts::renderer::rts {

// Value of the `NamedShader` uniform; the order is RTS's own table.
enum class NamedShader : std::int32_t {
    unknown = 0,
    tex = 1,
    tex_diff = 2,
    blend_a_tex = 3,
    blend_a_tex_diff = 4,
    gloss = 5,
    add_a_tex = 6,
    add_a_tex_diff = 7,
};

// Shape files only know tex..gloss; every other name maps to `unknown`.
[[nodiscard]] NamedShader named_shader_from_shape_name(std::string_view name) noexcept;

// Environment files (sky, water) use the full table including addatex*.
[[nodiscard]] NamedShader named_shader_from_environment_name(std::string_view name) noexcept;

// Blend shaders: blendatex*, and addatex*, which RTS also draws with alpha blending.
[[nodiscard]] bool is_alpha_shader(NamedShader shader) noexcept;

// GL filter that RTS applies to both minification and magnification.
enum class TextureFilter {
    unspecified, // filter left as created by the texture loader
    nearest,
    linear,
    nearest_mip_nearest,
    linear_mip_nearest,
    nearest_mip_linear,
    linear_mip_linear,
};

[[nodiscard]] TextureFilter texture_filter_from_name(std::string_view name) noexcept;

// Primitive classification computed once per primitive state at shape load.
inline constexpr std::uint16_t primitive_translucent = 1;     // blend shader without alpha test
inline constexpr std::uint16_t primitive_no_depth_write = 2;  // z_buf_mode 3

[[nodiscard]] std::uint16_t primitive_flags(const formats::ShapePrimitiveState& state, NamedShader shader) noexcept;

struct PrimitiveMaterial {
    NamedShader shader = NamedShader::unknown;
    // Uniform actually sent to the shader: unlit primitives are sent as blend_a_tex.
    NamedShader shader_uniform = NamedShader::unknown;
    std::int32_t light_material = -5;
    bool alpha_test_mode = false;
    bool depth_write = true;
    std::uint16_t flags = 0;
    TextureFilter filter = TextureFilter::unspecified;
    float lod_bias = 0.0f;
};

[[nodiscard]] PrimitiveMaterial classify_primitive(const formats::ShapeModel& model,
                                                   const formats::ShapePrimitiveState& state);

// OR of primitive_flags over all primitive states of a model.
[[nodiscard]] std::uint16_t model_flags(const formats::ShapeModel& model);

// Blend and alpha-test enables of a primitive in RTS's object passes. Every
// primitive sets its own (draw_elements 0x4B042C, called with flags 0): blend
// shaders as their alpha test mode says, every other shader draws with both
// off (CONFIRMED, 0x4B0EDC). Nothing carries over between primitives.
struct FixedFunctionState {
    bool blend = false;
    bool alpha_test = false;

    friend bool operator==(const FixedFunctionState&, const FixedFunctionState&) = default;
};

// GREATER reference used by RTS for alpha-tested blend primitives.
inline constexpr float alpha_test_reference = 0.3f;

[[nodiscard]] FixedFunctionState apply_material(const PrimitiveMaterial& material) noexcept;

// Object passes (rts1.exe 0x408574 visits -2..3; 0x4E0BFC sets an object's
// pass byte). A static object is in pass 1, or in pass 2 once its model
// contains translucent primitives; a transfer (ground decal) is in pass -2;
// StaticFlags 0x40000 (Terrain) puts an object in -1 and 0x200000 (Global:
// the track shapes) in 0, whatever its model.
inline constexpr int transfer_pass = -2;
inline constexpr int terrain_pass = -1;
inline constexpr int global_pass = 0;
inline constexpr int opaque_pass = 1;
inline constexpr int translucent_pass = 2;
// The cab's pass (its object pass byte is 3, 0x438D50): every primitive, in
// the shape's order, after all other objects.
inline constexpr int last_pass = 3;

[[nodiscard]] int object_pass(std::uint16_t model_flags) noexcept;
// An instance's pass from its world StaticFlags (0x4E0BFC): the model's,
// then Terrain (0x40000) -1, then Global (0x200000) 0.
inline constexpr std::uint32_t static_flag_terrain = 0x40000;
inline constexpr std::uint32_t static_flag_global = 0x200000;
[[nodiscard]] int object_pass(std::uint16_t model_flags, std::uint32_t static_flags) noexcept;

// Whether pass `pass` visits an object of pass `object_pass` (0x408574):
// -2, -1, 0 and 3 their own objects, 1 those of 0, 1 and 2, 2 those of 2.
[[nodiscard]] bool visits(int pass, int object_pass) noexcept;

// Whether a primitive is drawn when its object is visited in `pass`
// (0x4B042C): -1 all but the translucent ones, 0 only those not writing
// depth, 1 a model with any flagged primitive only its plain ones, 2 only
// the translucent ones (not those without depth writes of a Global object),
// -2 and 3 all. Note that non-blended z_buf_mode 3 primitives of ordinary
// statics are never drawn.
[[nodiscard]] bool draws_in_pass(int pass, int object_pass, std::uint16_t model_flags, std::uint16_t primitive_flags,
                                 std::uint32_t static_flags = 0) noexcept;

} // namespace openrts::renderer::rts
