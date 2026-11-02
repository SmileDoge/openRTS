// Night and seasonal textures (ESD_Alternative_Texture) and scenery lamps
// (ESD_LightSources): the folder rule, the .sd reader, and with an installed
// Kuban_RTS (argument: the installation) a lamp's images by night and day.
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <openrts/core/installation.hpp>
#include <openrts/core/vfs.hpp>
#include <openrts/formats/shape_description.hpp>
#include <openrts/formats/route_world.hpp>
#include <openrts/renderer/model_assets.hpp>
#include "../renderer/src/transfer_shape.hpp"

namespace {

using namespace openrts;
using renderer::TextureVariant;
using renderer::rts::Season;
using renderer::rts::Weather;

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

void test_folders() {
    using V = std::vector<std::string>;
    const auto folders = [](const std::uint32_t flags, const TextureVariant variant) { return renderer::alternative_texture_folders(flags, variant); };
    check(folders(0, {.night = true}).empty(), "no bits: no folder");
    check(folders(256, {.night = true}) == V{"NIGHT"}, "night bit at night: NIGHT");
    check(folders(256, {.night = false}).empty(), "night bit by day: none");
    // Bit 1 (snow): SNOW in winter whatever the weather, outside summer when it snows.
    check(folders(1, {.season = Season::winter}) == V{"SNOW"}, "snow bit in winter");
    check(folders(1, {.season = Season::autumn, .weather = Weather::snow}) == V{"SNOW"}, "snow bit when it snows in autumn");
    check(folders(1, {.season = Season::summer, .weather = Weather::snow}).empty(), "snow bit: never in summer");
    check(folders(257, {.night = true, .season = Season::winter}) == V{"NIGHT"}, "257 at night: NIGHT only (RTS 0x4224F8)");
    check(folders(257, {.night = false, .season = Season::winter}) == V{"SNOW"}, "257 by a winter day: SNOW");
    check(folders(4, {.season = Season::spring}).empty(), "4 alone: the snow rule, not SPRING (RTS compares the value)");
    check(folders(4, {.season = Season::winter}) == V{"SNOW"}, "4 in winter: SNOW");
    // 252: the six season folders.
    check(folders(252, {.season = Season::winter}) == V{"WINTER"}, "252 in a clear winter: WINTER");
    check(folders(252, {.season = Season::winter, .weather = Weather::snow}) == V{"WINTERSNOW"}, "252 in a snowy winter: WINTERSNOW");
    check(folders(252, {.season = Season::spring}) == V{"SPRING"}, "252 in spring: SPRING");
    check(folders(252, {.season = Season::summer}).empty(), "252 in summer: none");
}

void test_description() {
    const auto parsed = formats::parse_shape_description(R"(SIMISA@@@@@@@@@@JINX0t1t______
shape( JKU15_1Lamp.s
    ESD_Detail_Level (0)
    ESD_Alternative_Texture ( 257 )
    ESD_LightSources ( 1
        LightSource (
            Position ( 2.0 1.2 0.0 )
            PointsTo ( 2.0 -12.0 0.0 )
            Radius ( 20 20 )
            Colour ( FFffae29 )
            Flags ( NIGHT_AUTO LIGHT_CONE )
            Distance ( 250 )
            Attenuation ( 1.0 3.4 2 )
            SpotExponent ( 0.7000 )
        )
    )
))");
    check(parsed.ok(), "lamp .sd parses");
    if (!parsed.ok()) return;
    const auto& sd = *parsed.value;
    check(sd.shape_file == "JKU15_1Lamp.s", "shape file name");
    check(sd.alternative_texture == 257, "ESD_Alternative_Texture 257");
    check(sd.lights.size() == 1, "one light source");
    if (sd.lights.size() != 1) return;
    const auto& lamp = sd.lights[0];
    check(lamp.position[1] == 1.2f && lamp.points_to[1] == -12.0f, "position and PointsTo");
    check(lamp.radius[0] == 20.0f && lamp.radius[1] == 20.0f, "radius");
    check(lamp.colour == 0xFFFFAE29u, "colour");
    check(lamp.flags == "night_auto light_cone", "flags, lower case");
    check(lamp.distance == 250.0f, "distance");
    check(lamp.attenuation[0] == 2.0f && lamp.attenuation[1] == 1.0f && lamp.attenuation[2] == 3.4f, "attenuation as kind, start, length");
    check(lamp.spot_exponent > 0.69f && lamp.spot_exponent < 0.71f, "spot exponent");
}

void test_content(const std::filesystem::path& root) {
    core::Vfs vfs;
    core::mount_installation(vfs, root, core::load_installation_config(root));
    const auto shape = core::VirtualPath::parse("ROUTES/Kuban_RTS/SHAPES/JKU15_1Lamp.s");
    const auto textures = core::VirtualPath::parse("ROUTES/Kuban_RTS/TEXTURES");
    if (!shape || !textures || !vfs.exists(*shape)) {
        std::cout << "Kuban_RTS lamp: not installed, skipped\n";
        return;
    }
    const auto source = [](const renderer::StreamedModelAsset& asset, const std::string& image) -> std::string {
        for (const auto& texture : asset.textures) {
            if (texture.image_name == image && texture.source_path) return std::string(texture.source_path->str());
        }
        return {};
    };
    const auto day = renderer::load_model_asset(vfs, *shape, {*textures}, nullptr, {.night = false});
    const auto night = renderer::load_model_asset(vfs, *shape, {*textures}, nullptr, {.night = true});
    check(day.ok() && night.ok(), "lamp loads");
    check(day.alternative_texture == 257 && day.lights.size() == 1, "lamp's .sd read by the loader");
    const auto day_path = source(day, "JKU15_2.ace");
    const auto night_path = source(night, "JKU15_2.ace");
    check(!day_path.empty() && day_path.find("NIGHT") == std::string::npos, "by day the lamp's image is the plain one: " + day_path);
    check(night_path.find("NIGHT") != std::string::npos, "at night it comes from TEXTURES/NIGHT: " + night_path);
    check(night.texture_folders == std::vector<std::string>{"NIGHT"}, "night folders recorded");

    // The Rostov backdrop (ESD_Alternative_Texture 256, TEXTURES\NIGHT\Rostov.ace).
    if (const auto backdrop = core::VirtualPath::parse("ROUTES/Kuban_RTS/SHAPES/Fon_Rostov.s"); backdrop && vfs.exists(*backdrop)) {
        const auto by_day = renderer::load_model_asset(vfs, *backdrop, {*textures}, nullptr, {.night = false});
        const auto by_night = renderer::load_model_asset(vfs, *backdrop, {*textures}, nullptr, {.night = true});
        check(by_night.alternative_texture == 256, "Fon_Rostov: flag 256 read");
        const auto night_rostov = source(by_night, "Rostov.ace");
        check(night_rostov.find("NIGHT") != std::string::npos, "Fon_Rostov at night from TEXTURES/NIGHT: " + night_rostov);
        check(source(by_day, "Rostov.ace").find("NIGHT") == std::string::npos, "Fon_Rostov by day: the plain texture");
        bool decoded = false;
        for (const auto& texture : by_night.textures) decoded |= texture.image_name == "Rostov.ace" && texture.image.has_value();
        check(decoded, "Fon_Rostov's night texture decodes");
    }

    // A transfer (rts1.exe 0x4B4E60): Kuban's UiD 325, 80 x 80 m of
    // zemlya5.ace: a 20 x 20 grid of 4 m cells on the terrain, the far level
    // a quad, BlendATexDiff with the mip bias -3, its texture from TEXTURES
    // (SNOW in winter).
    if (const auto world = core::VirtualPath::parse("ROUTES/Kuban_RTS/WORLD/w-004493+014583.w"); world && vfs.exists(*world)) {
        const auto tile = formats::load_world_tile(vfs, *world);
        const formats::WorldObjectPlacement* transfer = nullptr;
        for (const auto& object : tile.objects) {
            if (object.kind == formats::WorldObjectKind::transfer && object.uid == 325) transfer = &object;
        }
        check(transfer != nullptr, "Kuban transfer 325 parsed");
        if (transfer) {
            check(transfer->transfer_width == 80.0f && transfer->transfer_height == 80.0f, "transfer width and height");
            check(transfer->shape_file.generic_string() == "zemlya5.ace", "transfer texture name");
            check(transfer->static_flags == 0x00100000u, "transfer StaticFlags");
        }
        const auto path = renderer::transfer_shape_path(*world, 325);
        check(path.has_value() && path->extension() == ".transfer", "transfer shape path");
        if (path) {
            const auto asset = renderer::load_model_asset(vfs, *path, {*textures}, nullptr, {});
            check(asset.ok(), "transfer builds: " + asset.error);
            check(asset.model.vertices.size() == 21u * 21u, "transfer grid: 21 x 21 vertices");
            check(asset.model.draws.size() == 2 && asset.model.draws[0].index_count == 20u * 20u * 6u
                      && asset.model.draws[1].index_count == 6u && asset.model.level_distances.size() == 2,
                  "transfer levels: the grid, then a quad");
            check(asset.model.textures.size() == 1 && asset.model.textures[0].mipmap_lod_bias == -3.0f, "transfer mip bias -3");
            check(asset.textures.size() == 1 && asset.textures[0].image.has_value(), "transfer texture decodes");
            bool on_ground = !asset.model.vertices.empty();
            for (const auto& vertex : asset.model.vertices) on_ground = on_ground && std::abs(vertex.position[1]) < 30.0f;
            check(on_ground, "transfer vertices near the object's height");
            const auto winter = renderer::load_model_asset(vfs, *path, {*textures}, nullptr, {.season = Season::winter});
            check(winter.texture_folders == std::vector<std::string>{"SNOW"}, "transfer texture from SNOW in winter");
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    test_folders();
    test_description();
    if (argc > 1) test_content(argv[1]);
    std::cout << (failures ? "FAILED" : "passed") << '\n';
    return failures ? 1 : 0;
}
