#include <openrts/renderer/rts_material.hpp>

#include <algorithm>
#include <cctype>
#include <string>

namespace openrts::renderer::rts {
namespace {

[[nodiscard]] std::string lowercase(const std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

[[nodiscard]] bool is_blend_shader(const NamedShader shader) noexcept {
    return shader == NamedShader::blend_a_tex || shader == NamedShader::blend_a_tex_diff;
}

// prim_state flag bit 8 disables lighting for the primitive.
constexpr std::uint32_t primitive_state_unlit_flag = 0x100;

constexpr std::int32_t light_material_half_bright = -11;

// Conditions under which RTS draws a primitive unlit (draw_elements 0x4B042C).
// Not modelled yet: the night-only model flag and per-instance unlit flag.
[[nodiscard]] bool forced_unlit(const NamedShader shader,
                                const std::int32_t light_material,
                                const std::uint32_t state_flags) noexcept {
    return light_material == light_material_half_bright || shader == NamedShader::tex
        || shader == NamedShader::blend_a_tex || shader == NamedShader::add_a_tex
        || (state_flags & primitive_state_unlit_flag) != 0;
}

} // namespace

NamedShader named_shader_from_shape_name(const std::string_view name) noexcept {
    const auto lower = lowercase(name);
    if (lower == "tex") return NamedShader::tex;
    if (lower == "texdiff") return NamedShader::tex_diff;
    if (lower == "blendatex") return NamedShader::blend_a_tex;
    if (lower == "blendatexdiff") return NamedShader::blend_a_tex_diff;
    if (lower == "gloss") return NamedShader::gloss;
    return NamedShader::unknown;
}

NamedShader named_shader_from_environment_name(const std::string_view name) noexcept {
    const auto lower = lowercase(name);
    if (lower == "addatex") return NamedShader::add_a_tex;
    if (lower == "addatexdiff") return NamedShader::add_a_tex_diff;
    return named_shader_from_shape_name(name);
}

bool is_alpha_shader(const NamedShader shader) noexcept {
    return is_blend_shader(shader) || shader == NamedShader::add_a_tex || shader == NamedShader::add_a_tex_diff;
}

TextureFilter texture_filter_from_name(const std::string_view name) noexcept {
    const auto lower = lowercase(name);
    if (lower == "linearmiplinear" || lower == "miplinear") return TextureFilter::linear_mip_linear;
    if (lower == "linear") return TextureFilter::linear;
    if (lower == "nearest") return TextureFilter::nearest;
    if (lower == "linearmipnearest" || lower == "mipnearest") return TextureFilter::linear_mip_nearest;
    if (lower == "nearestmipnearest") return TextureFilter::nearest_mip_nearest;
    if (lower == "nearestmiplinear") return TextureFilter::nearest_mip_linear;
    return TextureFilter::unspecified;
}

std::uint16_t primitive_flags(const formats::ShapePrimitiveState& state, const NamedShader shader) noexcept {
    std::uint16_t flags = 0;
    if (is_blend_shader(shader) && state.alpha_test_mode == 0) flags |= primitive_translucent;
    if (state.z_buffer_mode == 3) flags |= primitive_no_depth_write;
    return flags;
}

PrimitiveMaterial classify_primitive(const formats::ShapeModel& model, const formats::ShapePrimitiveState& state) {
    PrimitiveMaterial material;
    if (state.shader_index < model.shader_names.size()) {
        material.shader = named_shader_from_shape_name(model.shader_names[state.shader_index]);
    }
    if (state.vertex_state_index >= 0 && static_cast<std::size_t>(state.vertex_state_index) < model.vertex_states.size()) {
        material.light_material = model.vertex_states[state.vertex_state_index].light_material_index;
    }
    if (state.texture_index < model.textures.size()) {
        const auto& texture = model.textures[state.texture_index];
        if (texture.filter_mode_index < model.filter_names.size()) {
            material.filter = texture_filter_from_name(model.filter_names[texture.filter_mode_index]);
        }
        material.lod_bias = texture.mipmap_lod_bias;
    }

    material.alpha_test_mode = state.alpha_test_mode != 0;
    material.depth_write = state.z_buffer_mode != 3;
    material.flags = primitive_flags(state, material.shader);
    material.shader_uniform = forced_unlit(material.shader, material.light_material, state.flags)
        ? NamedShader::blend_a_tex
        : material.shader;
    return material;
}

std::uint16_t model_flags(const formats::ShapeModel& model) {
    std::uint16_t flags = 0;
    for (const auto& state : model.primitive_states) {
        const auto shader = state.shader_index < model.shader_names.size()
            ? named_shader_from_shape_name(model.shader_names[state.shader_index])
            : NamedShader::unknown;
        flags |= primitive_flags(state, shader);
    }
    return flags;
}

FixedFunctionState apply_material(const PrimitiveMaterial& material) noexcept {
    if (!is_blend_shader(material.shader)) return {};
    if (material.alpha_test_mode) return {.blend = false, .alpha_test = true};
    return {.blend = true, .alpha_test = false};
}

int object_pass(const std::uint16_t flags) noexcept {
    return (flags & primitive_translucent) != 0 ? translucent_pass : opaque_pass;
}

int object_pass(const std::uint16_t model_flags, const std::uint32_t static_flags) noexcept {
    int pass = object_pass(model_flags);
    if ((static_flags & static_flag_terrain) != 0) pass = terrain_pass;
    if ((static_flags & static_flag_global) != 0) pass = global_pass;
    return pass;
}

bool visits(const int pass, const int object_pass_value) noexcept {
    if (pass == opaque_pass) return object_pass_value >= global_pass && object_pass_value <= translucent_pass;
    return pass == object_pass_value;
}

bool draws_in_pass(const int pass, const int object_pass_value, const std::uint16_t flags, const std::uint16_t primitive,
                   const std::uint32_t static_flags) noexcept {
    if (!visits(pass, object_pass_value)) return false;
    switch (pass) {
    case terrain_pass:
        return (primitive & primitive_translucent) == 0;
    case global_pass:
        return (primitive & primitive_no_depth_write) != 0;
    case opaque_pass:
        // Models with any special primitive draw only their plain ones here.
        return flags == 0 || (primitive & (primitive_translucent | primitive_no_depth_write)) == 0;
    case translucent_pass:
        return (primitive & primitive_translucent) != 0
            && !((primitive & primitive_no_depth_write) != 0 && (static_flags & static_flag_global) != 0);
    default: // -2 (transfers) and 3 (the cab): every primitive
        return true;
    }
}

} // namespace openrts::renderer::rts
