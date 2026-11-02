#include <array>
#include <bit>
#include <filesystem>
#include <fstream>

#include <openrts/assets/assets.hpp>
#include <openrts/core/core.hpp>
#include <openrts/core/virtual_path.hpp>
#include <openrts/formats/ace_file.hpp>
#include <openrts/formats/shape_binary.hpp>
#include <openrts/formats/shape_description.hpp>
#include <openrts/formats/shape_file.hpp>
#include <openrts/formats/route_world.hpp>
#include <openrts/renderer/renderer.hpp>

namespace {

void append_u16(std::string& bytes, const std::uint16_t value) {
    bytes.push_back(static_cast<char>(value & 0xFF));
    bytes.push_back(static_cast<char>(value >> 8));
}

void append_u32(std::string& bytes, const std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<char>(value >> shift));
}

void append_u64(std::string& bytes, const std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) bytes.push_back(static_cast<char>(value >> shift));
}

void append_f32(std::string& bytes, const float value) {
    append_u32(bytes, std::bit_cast<std::uint32_t>(value));
}

[[nodiscard]] std::string block(const std::uint32_t token, std::string payload) {
    std::string bytes;
    append_u32(bytes, token);
    append_u32(bytes, static_cast<std::uint32_t>(payload.size()));
    bytes += payload;
    return bytes;
}

[[nodiscard]] std::string binary_node(const std::uint32_t token, std::string body) {
    return block(token, std::string(1, '\0') + std::move(body));
}

[[nodiscard]] std::string ascii_utf16(const std::string_view text) {
    std::string bytes;
    append_u16(bytes, static_cast<std::uint16_t>(text.size()));
    for (const char character : text) append_u16(bytes, static_cast<unsigned char>(character));
    return bytes;
}

} // namespace

int main() {
    namespace fs = std::filesystem;
    const auto fixture = fs::temp_directory_path() / "openrts-content-resolver-smoke";
    fs::remove_all(fixture);
    fs::create_directories(fixture / "ROUTES" / "Demo" / "TEXTURES");
    fs::create_directories(fixture / "GLOBAL" / "TEXTURES");
    fs::create_directories(fixture / "TRAINS" / "TRAINSET" / "Loco" / "CAB");
    fs::create_directories(fixture / "TRAINS" / "TRAINSET" / "Alias" / "TEXTURES");
    fs::create_directories(fixture / "ROUTES" / "Demo" / "WORLD");
    std::ofstream(fixture / "ROUTES" / "Demo" / "TEXTURES" / "Route.ace").put('\n');
    std::ofstream(fixture / "GLOBAL" / "TEXTURES" / "Global.ace").put('\n');
    std::ofstream(fixture / "ROUTES" / "Demo" / "Outside.ace").put('\n');
    std::ofstream(fixture / "TRAINS" / "TRAINSET" / "Loco" / "CAB" / "Panel.ace").put('\n');
    std::ofstream(fixture / "TRAINS" / "TRAINSET" / "Alias" / "TEXTURES" / "Shared.ace").put('\n');
    std::ofstream(fixture / "ROUTES" / "Demo" / "Demo.trk")
        << "SIMISA@@@@@@@@@@JINX0r0t______\nTr_RouteFile ( RouteID ( Demo ) RouteStart ( -1 2 3.5 4.5 ) )\n";
    std::ofstream(fixture / "ROUTES" / "Demo" / "WORLD" / "w-000001+000002.w")
        << "SIMISA@@@@@@@@@@JINX0w0t______\nTr_Worldfile ( Static ( UiD ( 17 ) FileName ( tree.s ) Position ( 1 2 3 ) QDirection ( 0 0 0 1 ) ) )\n";
    {
        const std::u16string utf16_sd = u"\uFEFFSIMISA@@@@@@@@@@JINX0t1t______\nShape ( Fixture.s )\n";
        std::ofstream output(fixture / "GLOBAL" / "TEXTURES" / "Fixture.sd", std::ios::binary);
        output.write(reinterpret_cast<const char*>(utf16_sd.data()),
                     static_cast<std::streamsize>(utf16_sd.size() * sizeof(char16_t)));
    }

    const openrts::assets::ContentResolver resolver(openrts::assets::ContentRoots{.installation_root = fixture});
    const openrts::assets::AssetContext route_context{.route_name = "demo"};
    const openrts::assets::AssetContext train_context{.trainset_name = "loco"};
    const auto route_texture = resolver.resolve(openrts::assets::AssetKind::texture, "route.ace", route_context);
    const auto global_texture = resolver.resolve(openrts::assets::AssetKind::texture, "global.ace", route_context);
    const auto train_texture = resolver.resolve(openrts::assets::AssetKind::train_asset, "cab/panel.ace", train_context);
    // RTS lets content leave its search folder with `..`; only rooted paths are refused.
    const auto parent_path = resolver.resolve(openrts::assets::AssetKind::texture, "../outside.ace", route_context);
    const auto backslash_parent_path =
        resolver.resolve(openrts::assets::AssetKind::texture, "..\\Outside.ace", route_context);
    const auto rejected_path = resolver.resolve(
        openrts::assets::AssetKind::texture, (fixture / "GLOBAL" / "TEXTURES" / "Global.ace").string(), route_context);
    const auto relative_shape = resolver.resolve_relative_to_directory(
        openrts::assets::AssetKind::shape,
        *openrts::core::VirtualPath::parse("GLOBAL/TEXTURES"),
        "global.ace");
    const auto relative_alias_texture = resolver.resolve_relative_to_directory(
        openrts::assets::AssetKind::texture,
        *openrts::core::VirtualPath::parse("TRAINS/TRAINSET/Loco"),
        "../Alias/TEXTURES/shared.ace");
    const auto shape_description = openrts::formats::parse_shape_description(
        "SIMISA@@@@@@@@@@JINX0t1t______\nShape ( \"Example.s\" )\n");
    const auto utf16_shape_description = openrts::formats::load_shape_description(
        fixture / "GLOBAL" / "TEXTURES" / "Fixture.sd");
    const auto route_definition = openrts::formats::load_route_definition(fixture / "ROUTES" / "Demo");
    const auto world_tile = openrts::formats::load_world_tile(
        fixture / "ROUTES" / "Demo" / "WORLD" / "w-000001+000002.w");
    const std::string compressed_bytes = std::string("SIMISA@F", 8)
        + std::string("\x34\x12\x00\x00", 4) + std::string(4, '\0');
    const auto compressed_shape = openrts::formats::probe_shape_file(std::string_view(compressed_bytes));
    const std::string compressed_abc = std::string("SIMISA@F", 8)
        + std::string("\x03\x00\x00\x00", 4) + std::string(4, '\0')
        + std::string("\x78\x9C\x4B\x4C\x4A\x06\x00\x02\x4D\x01\x27", 11);
    const auto inflated_abc = openrts::formats::inflate_narrow_compressed_shape(std::string_view(compressed_abc));
    std::string image_payload(1, '\0');
    image_payload += ascii_utf16("fixture.ace");
    std::string images_payload(1, '\0');
    append_u32(images_payload, 1);
    images_payload += block(13, std::move(image_payload));
    std::string root_payload(1, '\0');
    root_payload += block(14, std::move(images_payload));
    const std::string binary_shape = "JINX0s1b________" + block(71, std::move(root_payload));
    const auto images = openrts::formats::extract_image_filenames_from_binary_shape(binary_shape);
    std::string ace_fixture = "SIMISA@@@@@@@@@@";
    append_u32(ace_fixture, 1);
    append_u32(ace_fixture, 5);
    append_u32(ace_fixture, 256);
    append_u32(ace_fixture, 128);
    append_u32(ace_fixture, 14);
    append_u32(ace_fixture, 3);
    const auto ace_probe = openrts::formats::probe_ace_file(ace_fixture);
    std::string structured_ace = "SIMISA@@@@@@@@@@";
    append_u32(structured_ace, 1);
    append_u32(structured_ace, 0);
    append_u32(structured_ace, 2);
    append_u32(structured_ace, 1);
    append_u32(structured_ace, 0);
    append_u32(structured_ace, 3);
    structured_ace.append(128, '\0');
    append_u64(structured_ace, 8); append_u64(structured_ace, 3);
    append_u64(structured_ace, 8); append_u64(structured_ace, 4);
    append_u64(structured_ace, 8); append_u64(structured_ace, 5);
    // Scanline offsets count from the body after the 16-byte SIMISA envelope.
    append_u32(structured_ace, 204);
    structured_ace += std::string("\x10\x20\x30\x40\x50\x60", 6);
    const auto decoded_ace = openrts::formats::decode_structured_ace_rgba(structured_ace);
    // A broken scanline table (Pavlovsk's terrain.ace): the rows are read in
    // order right after it.
    std::string broken_ace = "SIMISA@@@@@@@@@@";
    for (const std::uint32_t value : {1u, 0u, 2u, 2u, 0u, 3u}) append_u32(broken_ace, value);
    broken_ace.append(128, '\0');
    append_u64(broken_ace, 8); append_u64(broken_ace, 3);
    append_u64(broken_ace, 8); append_u64(broken_ace, 4);
    append_u64(broken_ace, 8); append_u64(broken_ace, 5);
    append_u32(broken_ace, 208);
    append_u32(broken_ace, 99999);
    broken_ace += std::string("\x10\x20\x30\x40\x50\x60\x11\x21\x31\x41\x51\x61", 12);
    const auto decoded_broken = openrts::formats::decode_structured_ace_rgba(broken_ace);
    std::string point_table;
    append_u32(point_table, 3);
    for (const auto& point : {std::array<float, 3>{0.0f, 0.0f, 0.0f},
                              std::array<float, 3>{1.0f, 0.0f, 0.0f},
                              std::array<float, 3>{0.0f, 1.0f, 0.0f}}) {
        std::string point_body;
        for (const float component : point) append_f32(point_body, component);
        point_table += binary_node(2, std::move(point_body));
    }
    std::string uv_table;
    append_u32(uv_table, 3);
    for (const auto& uv : {std::array<float, 2>{0.0f, 0.0f},
                           std::array<float, 2>{1.0f, 0.0f},
                           std::array<float, 2>{0.0f, 1.0f}}) {
        std::string uv_body;
        for (const float component : uv) append_f32(uv_body, component);
        uv_table += binary_node(8, std::move(uv_body));
    }
    std::string vertex_table;
    append_u32(vertex_table, 3);
    for (std::uint32_t index = 0; index < 3; ++index) {
        std::string vertex_body;
        append_u32(vertex_body, 0); // flags
        append_u32(vertex_body, index); // point index
        append_u32(vertex_body, 0); // normal index
        append_u32(vertex_body, 0); // colour 1
        append_u32(vertex_body, 0); // colour 2
        std::string vertex_uvs;
        append_u32(vertex_uvs, 1);
        append_u32(vertex_uvs, index);
        vertex_body += binary_node(49, std::move(vertex_uvs));
        vertex_table += binary_node(48, std::move(vertex_body));
    }
    std::string triangle_indices;
    append_u32(triangle_indices, 3);
    append_u32(triangle_indices, 0);
    append_u32(triangle_indices, 1);
    append_u32(triangle_indices, 2);
    std::string triangle_list = binary_node(63, std::move(triangle_indices));
    std::string primitives;
    append_u32(primitives, 1);
    primitives += binary_node(60, std::move(triangle_list));
    std::string sub_object = binary_node(50, std::move(vertex_table));
    sub_object += binary_node(53, std::move(primitives));
    std::string sub_objects;
    append_u32(sub_objects, 1);
    sub_objects += binary_node(39, std::move(sub_object));
    std::string distance_level = binary_node(38, std::move(sub_objects));
    std::string distance_levels;
    append_u32(distance_levels, 1);
    distance_levels += binary_node(37, std::move(distance_level));
    std::string lod_control = binary_node(36, std::move(distance_levels));
    std::string lod_controls;
    append_u32(lod_controls, 1);
    lod_controls += binary_node(32, std::move(lod_control));
    std::string mesh_root;
    mesh_root += binary_node(7, std::move(point_table));
    mesh_root += binary_node(9, std::move(uv_table));
    mesh_root += binary_node(31, std::move(lod_controls));
    const std::string mesh_shape = "JINX0s1b________" + binary_node(71, std::move(mesh_root));
    const auto mesh = openrts::formats::extract_first_mesh_from_binary_shape(mesh_shape);
    const auto model = openrts::formats::parse_binary_shape_model(mesh_shape);
    const auto malformed_mesh = openrts::formats::extract_first_mesh_from_binary_shape(
        std::string_view(mesh_shape).substr(0, mesh_shape.size() - 1));

    const bool passed = openrts::core::module_name() == "openrts_core"
        && openrts::assets::module_name() == "openrts_assets"
        && (openrts::renderer::backend_name() == "null"
            || openrts::renderer::backend_name() == "wgpu-native")
        && route_texture.found() && route_texture.attempts.size() == 1
        && global_texture.found() && global_texture.attempts.size() == 2
        && train_texture.found() && parent_path.found() && backslash_parent_path.found()
        && rejected_path.attempts.empty()
        && relative_shape.found() && shape_description.ok()
        && relative_alias_texture.found()
        && shape_description.value->shape_file == "Example.s"
        && utf16_shape_description.ok() && utf16_shape_description.value->shape_file == "Fixture.s"
        && route_definition.ok() && route_definition.start && route_definition.start->tile_x == -1
        && route_definition.start->tile_z == 2 && world_tile.ok() && world_tile.objects.size() == 1
        && world_tile.objects[0].uid == 17 && world_tile.objects[0].position.tile_x == -1
        && world_tile.objects[0].position.tile_z == 2
        && compressed_shape.representation == openrts::formats::ShapeRepresentation::compressed_binary
        && compressed_shape.declared_inflated_size == std::uint32_t{0x1234}
        && inflated_abc.ok() && *inflated_abc.body == "abc"
        && ace_probe.representation == openrts::formats::AceRepresentation::uncompressed
        && ace_probe.header && ace_probe.header->width == 256 && ace_probe.header->height == 128
        && decoded_ace.ok() && decoded_ace.image->pixels
        == std::vector<std::uint8_t>({0x10, 0x30, 0x50, 0xFF, 0x20, 0x40, 0x60, 0xFF})
        && decoded_broken.ok() && decoded_broken.image->pixels
        == std::vector<std::uint8_t>({0x10, 0x30, 0x50, 0xFF, 0x20, 0x40, 0x60, 0xFF,
                                      0x11, 0x31, 0x51, 0xFF, 0x21, 0x41, 0x61, 0xFF})
        && mesh.ok() && mesh.vertices.size() == 3 && mesh.indices == std::vector<std::uint32_t>({0, 1, 2})
        && mesh.vertices[1].position[0] == 1.0f && mesh.vertices[2].uv[1] == 1.0f
        && model.ok() && model.draws.size() == 1 && model.indices == std::vector<std::uint32_t>({0, 1, 2})
        && model.matrices.size() == 1 && model.draws.front().matrix_index == 0
        && !malformed_mesh.ok()
        && images.ok() && images.filenames.size() == 1 && images.filenames[0] == "fixture.ace";
    fs::remove_all(fixture);
    return passed ? 0 : 1;
}
