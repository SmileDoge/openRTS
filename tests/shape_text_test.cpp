// Text shapes are transcoded into the binary token stream and parsed by the
// binary reader (formats/src/shape_text.cpp).
#include <cmath>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <openrts/formats/cab_description.hpp>
#include <openrts/formats/shape_binary.hpp>
#include <openrts/formats/shape_file.hpp>
#include <openrts/formats/shape_override.hpp>
#include <openrts/formats/shape_description.hpp>

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
		matrix MAIN ( 1 0 0 0 1 0 0 0 1 0 13.5 0 )
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

// The inspector's override (formats/shape_override.hpp): written and read
// back; applied, a primitive state gets a vertex state of its own, the one
// sharing the old keeps it; a shape whose image changed is left alone.
void override_checks() {
    using namespace openrts::formats;
    ShapeOverride data;
    data.set_light_material(1, "TEXTURES/Platform.ACE", -5);
    data.set_light_material(0, "sign.ace", -5);
    data.set_light_material(0, "sign.ace", std::nullopt); // taken back
    check(data.prim_states.size() == 1 && data.prim_states[0].index == 1 && data.prim_states[0].image == "platform.ace",
          "an override keeps the changed state, its image's name in lower case");
    const auto again = parse_shape_override(write_shape_override(data));
    check(again.prim_states.size() == 1 && again.prim_states[0].index == 1 && again.prim_states[0].image == "platform.ace"
              && again.prim_states[0].light_material == -5,
          "an override written and read back");

    ShapeModel model;
    model.images = {"sign.ace", "platform.ace"};
    model.textures = {{.image_index = 0}, {.image_index = 1}};
    model.vertex_states = {{.light_material_index = -8}};
    model.primitive_states = {{.texture_index = 0, .texture_count = 1, .vertex_state_index = 0},
                              {.texture_index = 1, .texture_count = 1, .vertex_state_index = 0}};
    const auto before = apply_shape_override(model, data);
    check(before.size() == 2 && before[0] == -8 && before[1] == -8, "the light materials before the override");
    check(model.vertex_states.size() == 2 && model.primitive_states[1].vertex_state_index == 1
              && model.vertex_states[1].light_material_index == -5 && model.primitive_states[0].vertex_state_index == 0
              && model.vertex_states[0].light_material_index == -8,
          "the overridden state lit as ordinary, the other still FullBright");

    ShapeModel changed;
    changed.images = {"sign.ace", "other.ace"};
    changed.textures = {{.image_index = 0}, {.image_index = 1}};
    changed.vertex_states = {{.light_material_index = -8}};
    changed.primitive_states = {{.texture_index = 0, .texture_count = 1, .vertex_state_index = 0},
                                {.texture_index = 1, .texture_count = 1, .vertex_state_index = 0}};
    apply_shape_override(changed, data);
    check(changed.vertex_states.size() == 1 && changed.primitive_states[1].vertex_state_index == 0,
          "an override whose image no longer matches is skipped");
}

// LightArray ( n dx dy dz ) of an ESD_LightSources lamp: the Novokuznetsk
// platform's platf_fonar_9.sd has two rows, 4 and 5 lamps 50.118 m apart.
void light_array_checks() {
    using namespace openrts::formats;
    const auto result = parse_shape_description(
        "SIMISA@@@@@@@@@@JINX0t1t______\nShape ( platf_fonar_9.s\n ESD_LightSources ( 2\n"
        "  LightSource ( Position ( -75.323 7.339 -0.903 ) PointsTo ( -75.323 -1.428 -3.207 ) Radius ( 20 20 )\n"
        "   Flags ( NIGHT_AUTO LIGHT_CONE ) LightArray ( 4 50.118 0.0 0.0 ) )\n"
        "  LightSource ( Position ( -100.238 7.339 0.89 ) PointsTo ( -100.238 -1.428 3.207 ) Radius ( 20 20 ) ) ) )");
    check(result.ok() && result.value->lights.size() == 2, "two light sources");
    if (!result.ok() || result.value->lights.size() != 2) return;
    const auto& row = result.value->lights[0];
    check(row.array_count == 4 && std::abs(row.array_step[0] - 50.118f) < 1e-4f && row.array_step[1] == 0.0f,
          "LightArray: four lamps 50.118 m apart along x");
    check(result.value->lights[1].array_count == 0, "a lamp without LightArray is one lamp");
}

int main() {
    override_checks();
    light_array_checks();
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
    check(model.matrices.size() == 2 && model.matrices[0].name == "MAIN" && near(model.matrices[0].values[10], 0.0)
              && near(model.matrices[0].values[0], 1.0) && near(model.matrices[0].values[4], 1.0),
          "matrix 0 is the identity, as RTS never applies it (its 13.5 m lift dropped)");
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

    // A cab .sd's ESD_Extended_Animation (the ChS7's controller): Shortcut
    // ( n frames ) sets, sorted as RTS keeps them (0x4BB7BC).
    {
        const auto cab = parse_cab_description(utf16_file(
            "SIMISA@@@@@@@@@@JINX0t1t______\nShape ( cab.s\n ESD_Extended_Animation ( AnimationNode ( Name ( \"C_MAIN\" )\n"
            " Shortcut ( 2 2 12 ) Shortcut ( 2 10 4 ) Shortcut ( 0 ) ) )\n"
            " ESD_Interior ( Switches ( 1 Switch ( ID ( 0 ) Name ( C_MAIN ) States ( 15 12 ) Flags ( CONTROLLERX ) ) ) ) )"));
        check(cab.ok() && cab.switches.size() == 1, "cab with an extended animation parses");
        check(cab.animation_nodes.size() == 1 && cab.animation_nodes[0].name == "C_MAIN", "AnimationNode by name");
        if (cab.animation_nodes.size() == 1) {
            const auto& sets = cab.animation_nodes[0].shortcuts;
            check(sets.size() == 2, "two Shortcut sets, the empty one skipped");
            check(sets.size() == 2 && sets[0] == std::vector<std::uint16_t>{2, 12} && sets[1] == std::vector<std::uint16_t>{4, 10},
                  "Shortcut frames sorted");
        }
    }

    // A block closed with `}` for `)` (VL80T's CabTEPT.sd Animations): RTS
    // counts only ( and ), and finds InteriorGeometryInfo inside the block
    // left open, at any depth; so does openRTS (its chairs).
    {
        const auto cab = parse_cab_description(utf16_file(
            "SIMISA@@@@@@@@@@JINX0t1t______\nShape ( cab.s\n ESD_Interior ( Switches ( 1 Switch ( ID ( 0 ) Name ( C_MAIN ) ) )\n"
            " Animations ( 1 AnimNode ( Name ( \"WIPERS\" ) ) }\n"
            " InteriorGeometryInfo ( InteriorChairs ( 1 Chair ( Position ( 0.8 2.6 6.2 ) Pos1 ( 0.8 2.4 6.2 ) ) ) ) ) )"));
        check(cab.ok() && cab.switches.size() == 1, "a cab with a } for a ) parses");
        check(cab.chairs.size() == 1 && cab.chairs[0].seated[2] > 6.0f, "its chair after the } is read");
    }

    // A screen label's Alphabet with brackets in it (the EP2D's "...()+/Ё"):
    // RTS takes the block's first word, brackets and all (0x4578C0,
    // 0x458688); read as a nested block, the alphabet was lost and the
    // label drew nothing.
    {
        const auto cab = parse_cab_description(utf16_file(
            "SIMISA@@@@@@@@@@JINX0t1t______\nShape ( cab.s\n ESD_Interior ( Displays ( 1 Display ( ID ( 0 ) DisplayType ( SCREEN )\n"
            " DisplayScreen ( TopLeft ( 0 1 0 ) TopRight ( 1 1 0 ) BottomRight ( 1 0 0 ) SubObjects ( 2\n"
            " SubObject ( SubID ( 0 ) Type ( LABEL ) Area ( 0 0 1 1 ) UV ( 0 0 1 1 ) Textures ( 1 \"f.ace\" )\n"
            "  CharacterWidth ( 0.1 0.1 ) CharacterHeight ( 0.1 0.1 ) Alphabet ( 01.AB-()+/Z ) MaxStringLength ( 4 ) )\n"
            " SubObject ( SubID ( 1 ) Type ( LABEL ) Area ( 0 0 1 1 ) UV ( 0 0 1 1 ) Textures ( 1 \"f.ace\" )\n"
            "  CharacterWidth ( 0.1 0.1 ) CharacterHeight ( 0.1 0.1 ) Alphabet ( \"0 1\" ) MaxStringLength ( 2 ) ) ) ) ) ) ) )"));
        check(cab.ok() && cab.displays.size() == 1 && cab.displays[0].screen.objects.size() == 2, "a screen with two labels");
        if (cab.displays.size() == 1 && cab.displays[0].screen.objects.size() == 2) {
            const auto& labels = cab.displays[0].screen.objects;
            check(labels[0].alphabet == U"01.AB-()+/Z", "an Alphabet with brackets: one word, brackets and all");
            check(labels[0].max_length == 4, "the keys after it still read");
            check(labels[1].alphabet == U"0 1", "a quoted Alphabet keeps its space");
        }
    }

    // A car's WagonObjects (the VL60's .sd): the wheel chocks shown by their
    // state, the end cocks by key frames.
    {
        const auto details = parse_exterior_details(utf16_file(
            "SIMISA@@@@@@@@@@JINX0t1t______\nShape ( vl60.s\n ESD_Exterior_Details ( WagonObjects (\n"
            " DragB ( ChildNodes ( 1 BASH1 ) FirstStateInvisible ( 1 ) NumStates ( 2 ) AnimTime ( 0.001 ) )\n"
            " DragA ( ChildNodes ( 1 BASH2 ) FirstStateInvisible ( 1 ) NumStates ( 2 ) )\n"
            " EndValveA ( ChildNodes ( 2 KR_PMA KR_TMA ) NumStates ( 2 ) StateKeyFrames ( 1 0 ) ) ) ) )"));
        check(details.wagon_objects.size() == 3, "three wagon objects");
        if (details.wagon_objects.size() == 3) {
            const auto& b = details.wagon_objects[0];
            check(b.kind == "dragb" && b.nodes == std::vector<std::string>{"BASH1"} && b.first_state_invisible && b.num_states == 2,
                  "DragB: its node, hidden in state 0");
            const auto& valve = details.wagon_objects[2];
            check(valve.kind == "endvalvea" && valve.nodes.size() == 2 && !valve.first_state_invisible && valve.state_key_frames.size() == 1,
                  "EndValveA: two nodes, a key frame");
        }
    }

    // A block left open (the 2TE116's InteriorDoors, a ')' missing): the
    // windows and chairs after it are still found.
    {
        const auto cab = parse_cab_description(utf16_file(
            "SIMISA@@@@@@@@@@JINX0t1t______\nShape ( cab.s\n ESD_Interior ( Switches ( 1 Switch ( ID ( 0 ) Name ( C_MAIN ) ) )\n"
            " InteriorGeometryInfo ( InteriorDoors ( 1 InteriorDoor ( Name ( D1 ) Connects ( CABIN EXTERIOR ) )\n"
            " InteriorWindows ( 1 InteriorWindow ( Name ( W1 ) CanBeOpened ( 1 ) ) )\n"
            " InteriorChairs ( 1 Chair ( Position ( 0.7 2.5 9.6 ) Pos1 ( 0.7 2.4 9.8 ) ) ) ) ) )"));
        check(cab.doors.size() == 1, "the open block's door");
        check(cab.windows.size() == 1 && cab.chairs.size() == 1 && cab.chairs[0].seated[2] > 9.0f,
              "the window and the chair inside the open block");
    }

    // SMSTriggers of a door and a window (the VL80T's DOOR_A: 48 49): the
    // codes for the car's sounds as it opens and shuts.
    {
        const auto cab = parse_cab_description(utf16_file(
            "SIMISA@@@@@@@@@@JINX0t1t______\nShape ( cab.s\n ESD_Interior ( Switches ( 1 Switch ( ID ( 0 ) Name ( C_MAIN ) ) )\n"
            " InteriorGeometryInfo ( InteriorDoors ( 2 InteriorDoor ( Name ( DOOR_A ) Connects ( MACHINE EXTERIOR ) SMSTriggers ( 48 49 ) )\n"
            " InteriorDoor ( Name ( DOOR_A1 ) ParentObject ( 0 ) ) )\n"
            " InteriorWindows ( 1 InteriorWindow ( Name ( W1 ) CanBeOpened ( 1 ) SMSTriggers ( 200 201 ) ) ) ) ) )"));
        check(cab.doors.size() == 2 && cab.doors[0].sms_triggers[0] == 48 && cab.doors[0].sms_triggers[1] == 49
                  && cab.doors[1].sms_triggers[0] == 0,
              "a door's SMSTriggers, none on its child");
        check(cab.doors.size() == 2 && cab.doors[0].parent == -1 && cab.doors[1].parent == 0,
              "a door following another (ParentObject)");
        check(cab.windows.size() == 1 && cab.windows[0].sms_triggers[0] == 200 && cab.windows[0].sms_triggers[1] == 201,
              "a window's SMSTriggers");
    }

    // ParentObject: the EP1M's blind pieces follow shtora8_1 (and through it
    // a root further up).
    {
        const auto cab = parse_cab_description(utf16_file(
            "SIMISA@@@@@@@@@@JINX0t1t______\nShape ( cab.s\n ESD_Interior ( Switches ( 4\n"
            " Switch ( ID ( 600 ) Name ( shtora_root ) )\n"
            " Switch ( ID ( 601 ) Name ( shtora8_1 ) ParentObject ( 600 ) States ( 100 0 ) )\n"
            " Switch ( ID ( 630 ) Name ( shtora8_2 ) ParentObject ( 601 ) States ( 100 0 ) )\n"
            " Switch ( ID ( 5 ) Name ( other ) ) ) ) )"));
        const auto* piece = cab.find_switch(630);
        check(piece && piece->parent == 601, "ParentObject read");
        check(cab.root_switch(630) == 600 && cab.root_switch(601) == 600 && cab.root_switch(5) == 5, "the root through the chain");
    }

    if (failures == 0) std::cout << "shape text: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
