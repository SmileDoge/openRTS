// Text shapes are transcoded into the binary token stream and parsed by the
// binary reader (formats/src/shape_text.cpp).
#include <cmath>
#include <iostream>
#include <string>
#include <string_view>

#include <openrts/formats/shape_binary.hpp>
#include <openrts/formats/shape_file.hpp>

namespace {

int failures = 0;

void check(const bool condition, const char* what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

// UTF-16LE with a BOM, as MSTS tools write text shapes.
std::string utf16_file(const std::string& text) {
    std::string bytes = "\xFF\xFE";
    for (const char character : text) {
        bytes.push_back(character);
        bytes.push_back('\0');
    }
    return bytes;
}

// One quad textured with image 0, one triangle without UVs, a labelled child
// matrix with a 2-key animation, comments and blocks the reader ignores, and a
// wrong `points` count (counts are recomputed).
const char* const quad_shape = R"(SIMISA@@@@@@@@@@JINX0s1t______

shape (
	shape_header ( 00000000 00000000 )
	volumes ( 1 vol_sphere ( vector ( 0 0 0 ) 2 ) )
	shader_names ( 1 named_shader ( TexDiff ) )
	texture_filter_names ( 1 named_filter_mode ( MipLinear ) )
	points ( 5
		point ( -1 0 -1 ) point ( 1 0 -1 ) point ( 1 0 1 ) point ( -1 0 1 )
		point ( 0 2 0 ) point ( 0 3 0 )
	)
	uv_points ( 4 uv_point ( 0 0 ) uv_point ( 1 0 ) uv_point ( 1 1 ) uv_point ( 0 1 ) )
	normals ( 1 vector ( 0 1 0 ) )
	sort_vectors ( 0 )
	colours ( 0 )
	matrices ( 2
		matrix MAIN ( 1 0 0 0 1 0 0 0 1 0 0 0 )
		matrix DOOR_A ( 1 0 0 0 1 0 0 0 1 0.5 0 0 )
	)
	images ( 1 image ( "brick wall.ace" ) )
	textures ( 1 texture ( 0 0 -3 ff000000 ) )
	light_materials ( 0 )
	light_model_cfgs ( 1 light_model_cfg ( 00000000 uv_ops ( 1 uv_op_copy ( 1 0 ) ) ) )
	vtx_states ( 2
		vtx_state ( 00000000 0 -5 0 00000002 )
		vtx_state ( 00000000 1 -5 0 00000002 )
	)
	prim_states ( 2
		prim_state Textured ( 00000000 0 tex_idxs ( 1 0 ) 0 0 0 0 1 )
		prim_state Plain ( 00000000 0 tex_idxs ( 0 ) 0 1 0 0 1 )
	)
	lod_controls ( 1
		lod_control (
			distance_levels_header ( 100 )
			distance_levels ( 1
				distance_level (
					distance_level_header (
						dlevel_selection ( 2000 )
						hierarchy ( 2 -1 0 )
					)
					sub_objects ( 1
						sub_object (
							sub_object_header ( 00000400 -1 -1 000001d2 000001c4
								geometry_info ( 1 1 0 3 0 0 1 0 0 0
									geometry_nodes ( 1 geometry_node ( 1 0 0 0 0 cullable_prims ( 1 1 3 ) ) )
									geometry_node_map ( 1 0 )
								)
								subobject_shaders ( 1 0 )
								subobject_light_cfgs ( 1 0 ) 0
							)
							vertices ( 7
								vertex ( 00000000 0 0 ffffffff ff000000 vertex_uvs ( 1 0 ) )
								vertex ( 00000000 1 0 ffffffff ff000000 vertex_uvs ( 1 1 ) )
								vertex ( 00000000 2 0 ffffffff ff000000 vertex_uvs ( 1 2 ) )
								vertex ( 00000000 3 0 ffffffff ff000000 vertex_uvs ( 1 3 ) )
								vertex ( 00000000 4 0 ffffffff ff000000 vertex_uvs ( 0 ) )
								vertex ( 00000000 5 0 ffffffff ff000000 vertex_uvs ( 0 ) )
								vertex ( 00000000 2 0 ffffffff ff000000 vertex_uvs ( 0 ) )
							)
							vertex_sets ( 1 vertex_set ( 0 0 7 ) )
							primitives ( 4
								prim_state_idx ( 0 )
								indexed_trilist (
									vertex_idxs ( 6 0 1 2 0 2 3 )
									normal_idxs ( 2 0 3 0 3 )
									flags ( 2 00000000 00000000 )
								)
								prim_state_idx ( 1 )
								indexed_trilist (
									vertex_idxs ( 3 4 5 6 )
									normal_idxs ( 1 0 3 )
									flags ( 1 00000000 )
								)
							)
						)
					)
				)
			)
		)
	)
	animations ( 1
		animation ( 2 30
			anim_nodes ( 2
				anim_node MAIN ( controllers ( 0 ) )
				anim_node DOOR_A (
					controllers ( 2
						tcb_rot ( 2
							tcb_key ( 0 0 0 0 1 0 0 0 0 0 )
							slerp_rot ( 2 0 0.7071068 0 0.7071068 )
						)
						linear_pos ( 2 linear_key ( 0 0.5 0 0 ) linear_key ( 2 0.5 1 0 ) )
					)
				)
			)
		)
	)
)
)";

bool near(const double a, const double b) { return std::abs(a - b) < 1e-5; }

} // namespace

int main() {
    using namespace openrts::formats;
    const auto bytes = utf16_file(quad_shape);
    const std::string_view file = bytes;
    check(probe_shape_file(file).representation == ShapeRepresentation::text, "probe sees a text shape");

    const auto body = load_binary_shape_body(file);
    check(body.ok(), "text shape transcodes");
    if (!body.ok()) {
        std::cerr << body.error << '\n';
        return 1;
    }
    const auto model = parse_binary_shape_model(*body.body);
    check(model.ok(), "transcoded shape parses");
    if (!model.ok()) {
        std::cerr << model.error << '\n';
        return 1;
    }
    check(model.vertices.size() == 7 && model.indices.size() == 9, "all vertices and triangles");
    check(model.draws.size() == 2, "one draw per primitive state");
    check(model.images.size() == 1 && model.images[0] == "brick wall.ace", "quoted image name with a space");
    check(model.shader_names.size() == 1 && model.shader_names[0] == "TexDiff", "shader name");
    check(model.textures.size() == 1 && near(model.textures[0].mipmap_lod_bias, -3.0), "texture lod bias is a float");
    check(model.vertex_states.size() == 2 && model.vertex_states[1].matrix_index == 1
              && model.vertex_states[0].light_material_index == -5,
          "vertex states, negative light material");
    check(model.primitive_states.size() == 2 && model.primitive_states[1].vertex_state_index == 1,
          "primitive state after the tex_idxs block");
    check(near(model.vertices[2].uv[0], 1.0) && near(model.vertices[2].uv[1], 1.0), "textured vertex UV");
    check(near(model.vertices[4].position[1], 2.0) && near(model.vertices[4].uv[0], 0.0),
          "vertex without UVs gets (0, 0)");
    check(model.matrices.size() == 2 && model.matrices[1].name == "DOOR_A" && near(model.matrices[1].values[9], 0.5),
          "labelled matrices");
    check(model.hierarchy.size() == 2 && model.hierarchy[1] == 0, "hierarchy");
    check(model.animations.size() == 1 && model.animations[0].frame_count == 2, "animation");
    if (!model.animations.empty()) {
        const auto& nodes = model.animations[0].nodes;
        // Nodes without keys (MAIN) are not kept.
        const auto door = nodes.size() == 1 && nodes[0].name == "DOOR_A" ? &nodes[0] : nullptr;
        check(door && door->matrix_index == 1 && door->rotations.size() == 2 && door->positions.size() == 2,
              "animated node bound by name, tcb and slerp keys");
        if (door && door->rotations.size() == 2) check(near(door->rotations[1].value[1], 0.7071068), "slerp key");
    }

    // Plain 8-bit text works too.
    check(load_binary_shape_body(std::string_view(quad_shape)).ok(), "8-bit text shape");
    const auto truncated = utf16_file("SIMISA@@@@@@@@@@JINX0s1t______\nshape ( points ( 1");
    check(!load_binary_shape_body(std::string_view(truncated)).ok(),
          "truncated text is an error");

    if (failures == 0) std::cout << "shape text: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
