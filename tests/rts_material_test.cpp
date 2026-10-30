#include <cstdio>

#include <openrts/formats/shape_binary.hpp>
#include <openrts/renderer/rts_material.hpp>

namespace {

int failures = 0;

void check(const bool condition, const char* what) {
    if (!condition) {
        std::printf("FAILED: %s\n", what);
        ++failures;
    }
}

namespace rts = openrts::renderer::rts;
using openrts::formats::ShapeModel;
using openrts::formats::ShapePrimitiveState;

[[nodiscard]] ShapeModel model_with(const char* shader, const std::int32_t light_material) {
    ShapeModel model;
    model.shader_names = {shader};
    model.vertex_states = {{.flags = 0, .matrix_index = 0, .light_material_index = light_material}};
    model.filter_names = {"MipLinear"};
    model.textures = {{.image_index = 0, .filter_mode_index = 0, .mipmap_lod_bias = -3.0f}};
    return model;
}

[[nodiscard]] ShapePrimitiveState state(const std::uint32_t alpha_test, const std::uint32_t z_mode, const std::uint32_t flags = 0) {
    return {.flags = flags, .texture_index = 0, .texture_count = 1, .shader_index = 0, .vertex_state_index = 0,
            .alpha_test_mode = alpha_test, .z_buffer_mode = z_mode};
}

} // namespace

int main() {
    // Name tables (case-insensitive, shape loader knows tex..gloss only).
    check(rts::named_shader_from_shape_name("TexDiff") == rts::NamedShader::tex_diff, "TexDiff");
    check(rts::named_shader_from_shape_name("BlendATexDiff") == rts::NamedShader::blend_a_tex_diff, "BlendATexDiff");
    check(rts::named_shader_from_shape_name("AddATex") == rts::NamedShader::unknown, "AddATex in shapes");
    check(rts::texture_filter_from_name("MipLinear") == rts::TextureFilter::linear_mip_linear, "MipLinear");
    check(rts::texture_filter_from_name("mipnearest") == rts::TextureFilter::linear_mip_nearest, "mipnearest");

    // Lit TexDiff keeps its shader; HalfBright, Tex and prim flag 0x100 go unlit.
    const auto lit = model_with("TexDiff", -5);
    const auto lit_material = rts::classify_primitive(lit, state(0, 1));
    check(lit_material.shader_uniform == rts::NamedShader::tex_diff, "lit texdiff uniform");
    check(lit_material.filter == rts::TextureFilter::linear_mip_linear && lit_material.lod_bias == -3.0f, "texture filter");
    check(rts::classify_primitive(model_with("TexDiff", -11), state(0, 1)).shader_uniform == rts::NamedShader::blend_a_tex,
          "half bright forced unlit");
    check(rts::classify_primitive(model_with("Tex", -5), state(0, 1)).shader_uniform == rts::NamedShader::blend_a_tex,
          "tex forced unlit");
    check(rts::classify_primitive(lit, state(0, 1, 0x100)).shader_uniform == rts::NamedShader::blend_a_tex,
          "prim flag 0x100 forced unlit");

    // Pass flags: blend without alpha test is translucent, z_buf_mode 3 has no depth write.
    const auto blend = model_with("BlendATexDiff", -5);
    const auto translucent = rts::classify_primitive(blend, state(0, 1));
    const auto masked = rts::classify_primitive(blend, state(1, 1));
    const auto no_depth = rts::classify_primitive(lit, state(0, 3));
    check(translucent.flags == rts::primitive_translucent, "translucent flag");
    check(masked.flags == 0, "alpha-tested blend is opaque-pass");
    check(no_depth.flags == rts::primitive_no_depth_write && !no_depth.depth_write, "no depth write flag");

    // Fixed-function state: blend shaders set it, everything else inherits it.
    const auto after_blend = rts::apply_material(translucent, rts::object_pass_entry_state);
    check(after_blend == rts::FixedFunctionState{.blend = true, .alpha_test = false}, "blend state");
    check(rts::apply_material(lit_material, after_blend) == after_blend, "texdiff inherits blend");
    const auto after_mask = rts::apply_material(masked, after_blend);
    check(after_mask == rts::FixedFunctionState{.blend = false, .alpha_test = true}, "alpha test state");

    // Passes: translucent primitives only in pass 2 and only for pass-2 objects;
    // a z_buf_mode 3 opaque primitive of a static is never drawn.
    const std::uint16_t mixed = rts::primitive_translucent;
    check(rts::object_pass(mixed) == rts::translucent_pass, "translucent model object pass");
    check(rts::object_pass(0) == rts::opaque_pass, "opaque model object pass");
    check(rts::draws_in_pass(rts::opaque_pass, rts::translucent_pass, mixed, 0), "opaque prim in pass 1");
    check(!rts::draws_in_pass(rts::opaque_pass, rts::translucent_pass, mixed, rts::primitive_translucent), "translucent not in pass 1");
    check(rts::draws_in_pass(rts::translucent_pass, rts::translucent_pass, mixed, rts::primitive_translucent), "translucent in pass 2");
    check(!rts::draws_in_pass(rts::translucent_pass, rts::opaque_pass, 0, 0), "opaque object skips pass 2");
    const std::uint16_t depthless = rts::primitive_no_depth_write;
    check(!rts::draws_in_pass(rts::opaque_pass, rts::opaque_pass, depthless, rts::primitive_no_depth_write)
              && !rts::draws_in_pass(rts::translucent_pass, rts::opaque_pass, depthless, rts::primitive_no_depth_write),
          "z_buf_mode 3 opaque prim dropped");

    if (failures == 0) std::printf("rts material rules: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
