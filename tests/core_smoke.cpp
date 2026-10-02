#include <filesystem>
#include <fstream>

#include <openrts/assets/assets.hpp>
#include <openrts/core/core.hpp>
#include <openrts/formats/ace_file.hpp>
#include <openrts/formats/shape_binary.hpp>
#include <openrts/formats/shape_description.hpp>
#include <openrts/formats/shape_file.hpp>
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

[[nodiscard]] std::string block(const std::uint32_t token, std::string payload) {
    std::string bytes;
    append_u32(bytes, token);
    append_u32(bytes, static_cast<std::uint32_t>(payload.size()));
    bytes += payload;
    return bytes;
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
    std::ofstream(fixture / "ROUTES" / "Demo" / "TEXTURES" / "Route.ace").put('\n');
    std::ofstream(fixture / "GLOBAL" / "TEXTURES" / "Global.ace").put('\n');
    std::ofstream(fixture / "TRAINS" / "TRAINSET" / "Loco" / "CAB" / "Panel.ace").put('\n');
    {
        const std::u16string utf16_sd = u"\uFEFFSIMISA@@@@@@@@@@JINX0t1t______\nShape ( Fixture.s )\n";
        std::ofstream output(fixture / "GLOBAL" / "TEXTURES" / "Fixture.sd", std::ios::binary);
        output.write(reinterpret_cast<const char*>(utf16_sd.data()),
                     static_cast<std::streamsize>(utf16_sd.size() * sizeof(char16_t)));
    }

    const openrts::assets::ContentResolver resolver({.installation_root = fixture});
    const openrts::assets::AssetContext route_context{.route_name = "demo"};
    const openrts::assets::AssetContext train_context{.trainset_name = "loco"};
    const auto route_texture = resolver.resolve(openrts::assets::AssetKind::texture, "route.ace", route_context);
    const auto global_texture = resolver.resolve(openrts::assets::AssetKind::texture, "global.ace", route_context);
    const auto train_texture = resolver.resolve(openrts::assets::AssetKind::train_asset, "cab/panel.ace", train_context);
    const auto rejected_path = resolver.resolve(openrts::assets::AssetKind::texture, "../outside.ace", route_context);
    const auto relative_shape = resolver.resolve_relative_to_directory(
        openrts::assets::AssetKind::shape,
        fixture / "GLOBAL" / "TEXTURES",
        "global.ace");
    const auto shape_description = openrts::formats::parse_shape_description(
        "SIMISA@@@@@@@@@@JINX0t1t______\nShape ( \"Example.s\" )\n");
    const auto utf16_shape_description = openrts::formats::load_shape_description(
        fixture / "GLOBAL" / "TEXTURES" / "Fixture.sd");
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
    append_u32(structured_ace, 220);
    structured_ace += std::string("\x10\x20\x30\x40\x50\x60", 6);
    const auto decoded_ace = openrts::formats::decode_structured_ace_rgba(structured_ace);

    const bool passed = openrts::core::module_name() == "openrts_core"
        && openrts::assets::module_name() == "openrts_assets"
        && (openrts::renderer::backend_name() == "null"
            || openrts::renderer::backend_name() == "wgpu-native")
        && route_texture.found() && route_texture.attempts.size() == 1
        && global_texture.found() && global_texture.attempts.size() == 2
        && train_texture.found() && rejected_path.attempts.empty()
        && relative_shape.found() && shape_description.ok()
        && shape_description.value->shape_file == "Example.s"
        && utf16_shape_description.ok() && utf16_shape_description.value->shape_file == "Fixture.s"
        && compressed_shape.representation == openrts::formats::ShapeRepresentation::compressed_binary
        && compressed_shape.declared_inflated_size == std::uint32_t{0x1234}
        && inflated_abc.ok() && *inflated_abc.body == "abc"
        && ace_probe.representation == openrts::formats::AceRepresentation::uncompressed
        && ace_probe.header && ace_probe.header->width == 256 && ace_probe.header->height == 128
        && decoded_ace.ok() && decoded_ace.image->pixels
        == std::vector<std::uint8_t>({0x10, 0x30, 0x50, 0xFF, 0x20, 0x40, 0x60, 0xFF})
        && images.ok() && images.filenames.size() == 1 && images.filenames[0] == "fixture.ace";
    fs::remove_all(fixture);
    return passed ? 0 : 1;
}
