// Night and seasonal textures (ESD_Alternative_Texture) and scenery lamps
// (ESD_LightSources): the folder rule, the .sd reader, and with an installed
// Kuban_RTS (argument: the installation) a lamp's images by night and day.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <openrts/core/installation.hpp>
#include <openrts/core/vfs.hpp>
#include <openrts/formats/cab_description.hpp>
#include <openrts/formats/shape_description.hpp>
#include <openrts/formats/route_world.hpp>
#include <openrts/formats/terrain.hpp>
#include <openrts/formats/rolling_stock.hpp>
#include <openrts/formats/environment.hpp>
#include <openrts/formats/car_spawn.hpp>
#include <openrts/renderer/forest_clearance.hpp>
#include <openrts/renderer/model_assets.hpp>
#include <openrts/simulation/track_geometry.hpp>
#include <openrts/simulation/track_network.hpp>

#include <glm/gtc/quaternion.hpp>
#include "../renderer/src/forest_shape.hpp"
#include "../renderer/src/terrain_heights.hpp"
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

// The clearance's axes: the distance to a piece's edge, the grid's reach.
void test_clearance() {
    renderer::ForestClearance axes;
    axes.add(0.0, 0.0, 10.0, 0.0, 1.5f);
    check(std::fabs(axes.nearest(5.0, 4.0, 20.0) - 2.5) < 1e-9, "clearance: 4 m beside a 1.5 m half-wide way is 2.5 m from its edge");
    check(std::fabs(axes.nearest(13.0, 4.0, 20.0) - 3.5) < 1e-9, "clearance: past the end, from the end");
    check(axes.nearest(100.0, 100.0, 20.0) == 20.0, "clearance: nothing within reach");
    axes.add(-40.0, -70.0, -33.0, -70.0, 0.0f);
    check(std::fabs(axes.nearest(-35.0, -66.0, 10.0) - 4.0) < 1e-9, "clearance: negative cells");
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

    // An RTS .repaint: the shape and .sd it names, TextureReplace images
    // from their new files, the model under the repaint's own path.
    if (const auto repaint = core::VirtualPath::parse("TRAINS/TRAINSET/ev_CHS4t-Alias/Cabview/cab1.repaint"); repaint && vfs.exists(*repaint)) {
        const auto asset = renderer::load_model_asset(vfs, *repaint, {});
        check(asset.ok(), "cab1.repaint loads: " + asset.error);
        check(asset.source_path == *repaint, "the model keeps the repaint's path");
        const auto from = [&](const std::string& image) -> std::string {
            for (const auto& texture : asset.textures) {
                if (core::fold_case(texture.image_name) == image && texture.source_path) return core::fold_case(texture.source_path->filename());
            }
            return {};
        };
        check(from("cab_16.ace") == "cab_16_2.ace", "TextureReplace cab_16 -> cab_16_2: " + from("cab_16.ace"));
        check(from("stekla.ace") == "stekla_1.ace", "TextureReplace stekla -> stekla_1: " + from("stekla.ace"));
        const auto keys = asset.dependency_keys();
        const auto depends = [&](const char* path) {
            const auto key = core::VirtualPath::parse(path)->key();
            return std::find(keys.begin(), keys.end(), key) != keys.end();
        };
        check(depends("TRAINS/TRAINSET/ev_CHS4t-Alias/Cabview/cab.s") && depends("TRAINS/TRAINSET/ev_CHS4t-Alias/Cabview/cab1.sd"),
              "a repaint depends on its shape and .sd");
        check(core::fold_case(formats::shape_description(vfs, *repaint).filename()) == "cab1.sd", "a repaint's .sd: its ShapeDescription");
    } else {
        std::cout << "ChS4t cab repaint: not installed, skipped" << std::endl;
    }
    if (const auto chs2 = core::VirtualPath::parse("TRAINS/TRAINSET/tsrloco_chs2-010/tsrloco_chs2-010.repaint"); chs2 && vfs.exists(*chs2)) {
        const auto asset = renderer::load_model_asset(vfs, *chs2, {});
        check(asset.ok() && !asset.textures.empty(), "TSR ChS2's body (a repaint in its .rtseng) loads: " + asset.error);
    }

    // A transfer (rts1.exe 0x4B4E60): Kuban's UiD 325, 80 x 80 m of
    // zemlya5.ace, BlendATexDiff with the mip bias -3, its texture from
    // TEXTURES (SNOW in winter). openRTS cuts it by the terrain's own
    // triangles (RTS: a 4 m grid of its own, which flickers against the
    // ground): every corner on the ground, the whole 80 x 80 m covered, the
    // faces clockwise from above, one level to 2000 m.
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
            check(asset.model.draws.size() == 1 && asset.model.level_distances == std::vector<float>{2000.0f},
                  "transfer: one level to 2000 m");
            if (transfer && asset.ok()) {
                const auto& q = transfer->rotation;
                const glm::quat rotation{q[3], -q[0], -q[1], -q[2]};
                renderer::TerrainHeights terrain(vfs, *core::VirtualPath::parse("ROUTES/Kuban_RTS/TILES"));
                double area = 0.0;
                bool clockwise = true, on_terrain = true, in_texture = true;
                const auto& vertices = asset.model.vertices;
                const auto& indices = asset.model.indices;
                for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
                    const auto& a = vertices[indices[i]].position;
                    const auto& b = vertices[indices[i + 1]].position;
                    const auto& c = vertices[indices[i + 2]].position;
                    const double cross = (b[0] - a[0]) * (c[2] - a[2]) - (c[0] - a[0]) * (b[2] - a[2]);
                    clockwise = clockwise && cross < 0.0;
                    area += -cross * 0.5;
                }
                for (const auto& vertex : vertices) {
                    const glm::vec3 offset = rotation * glm::vec3{vertex.position[0], 0.0f, vertex.position[2]};
                    const auto ground = terrain.at(tile.tile_x, tile.tile_z, transfer->position.x + offset.x, transfer->position.z + offset.z);
                    on_terrain = on_terrain && ground
                                 && std::abs(static_cast<double>(*ground) - (vertex.position[1] + transfer->position.y)) < 0.005;
                    in_texture = in_texture && vertex.uv[0] > -1e-4f && vertex.uv[0] < 1.0001f && vertex.uv[1] > -1e-4f && vertex.uv[1] < 1.0001f;
                }
                check(on_terrain, "transfer: every corner on the terrain's surface");
                check(std::abs(area - 6400.0) < 6400.0 * 0.001, "transfer covers 80 x 80 m, got " + std::to_string(area));
                check(clockwise, "transfer faces clockwise from above");
                check(in_texture, "transfer UVs within the texture");
            }
            check(asset.model.textures.size() == 1 && asset.model.textures[0].mipmap_lod_bias == -3.0f, "transfer mip bias -3");
            check(asset.textures.size() == 1 && asset.textures[0].image.has_value(), "transfer texture decodes");
            bool on_ground = !asset.model.vertices.empty();
            for (const auto& vertex : asset.model.vertices) on_ground = on_ground && std::abs(vertex.position[1]) < 30.0f;
            check(on_ground, "transfer vertices near the object's height");
            const auto winter = renderer::load_model_asset(vfs, *path, {*textures}, nullptr, {.season = Season::winter});
            check(winter.texture_folders == std::vector<std::string>{"SNOW"}, "transfer texture from SNOW in winter");
        }
    }

    // World objects RTS builds or runs itself (docs/world-objects.md).
    const auto world = [&](const char* name) { return formats::load_world_tile(vfs, *core::VirtualPath::parse(std::string("ROUTES/Kuban_RTS/WORLD/") + name)); };
    if (vfs.exists(*core::VirtualPath::parse("ROUTES/Kuban_RTS/WORLD/w-004493+014580.w"))) {
        const auto tile = world("w-004493+014580.w");
        const formats::WorldObjectPlacement* forest = nullptr;
        for (const auto& object : tile.objects) {
            if (object.kind == formats::WorldObjectKind::forest && object.uid == 2760) forest = &object;
        }
        check(forest != nullptr, "Kuban forest 2760 parsed");
        if (forest) {
            check(forest->forest_complete && forest->population == 77 && forest->tree_texture == "grass-green.ace", "forest fields");
            check(std::fabs(forest->forest_area[0] - 214.25f) < 0.01f && forest->tree_size[0] == 5.0f && forest->tree_size[1] == 4.0f,
                  "forest area and tree size");
        }
        const auto path = renderer::forest_shape_path(*core::VirtualPath::parse("ROUTES/Kuban_RTS/WORLD/w-004493+014580.w"), 2760);
        const auto trees = renderer::build_forest_shape(vfs, *path);
        check(trees.error.empty() && trees.model.vertices.size() == 77 * 8 && trees.model.indices.size() == 77 * 24,
              "forest 2760: 77 trees, two crossed quads both sides each: " + trees.error);
        // 0x4B3C40: the first tree at x = W / 4 - W / 2, z wrapped into the 17 m deep area.
        if (!trees.model.vertices.empty()) {
            const auto& first = trees.model.vertices.front();
            check(std::fabs(first.position[0] - (-53.5625f)) < 0.001f, "first tree x as RTS places it");
            check(std::fabs(first.position[2] - (2.14062f - 8.5703f + 2.5f)) < 0.01f, "first tree z as RTS places it");
        }
    }
    // Novokuznetsk's forests turned as placed (their QDirection, lost until
    // 2026-11-09): forest 4054, 300 x 600 m turned 113 degrees, lay across
    // the line then, and lies along it now.
    if (const auto file = core::VirtualPath::parse("ROUTES/Novokuznetsk/WORLD/w-002971+015037.w"); vfs.exists(*file)) {
        const auto tile = formats::load_world_tile(vfs, *file);
        bool turned = false;
        for (const auto& object : tile.objects)
            if (object.kind == formats::WorldObjectKind::forest && object.uid == 4054) turned = std::abs(object.rotation[1]) > 0.5f;
        check(turned, "forest 4054 keeps its QDirection (113 degrees)");
    }
    // openRTS's clearance (renderer/forest_clearance.hpp) on Novokuznetsk's
    // tile w-002941+015035, whose forests 20 and 34 reach a track: some
    // trees left out, none of the kept ones near it.
    if (const auto file = core::VirtualPath::parse("ROUTES/Novokuznetsk/WORLD/w-002941+015035.w"); vfs.exists(*file)) {
        auto network = simulation::TrackNetwork::load(vfs, *core::VirtualPath::parse("ROUTES/Novokuznetsk"), nullptr);
        check(network != nullptr, "Novokuznetsk track network");
        const auto tile = formats::load_world_tile(vfs, *file);
        if (network) {
            // The tracks' axes as the viewer's route_way_axes makes them.
            auto axes = std::make_shared<renderer::ForestClearance>();
            for (const auto& node : network->database().nodes) {
                if (node.kind != formats::TrackNodeKind::vector) continue;
                for (const auto& placement : node.sections) {
                    const auto* section = network->sections().section(placement.section);
                    if (!section || section->length <= 0.0f) continue;
                    const int pieces = section->curved || section->skewed ? (std::max)(1, static_cast<int>(std::ceil(section->length / 4.0f))) : 1;
                    auto from = simulation::evaluate_section(placement, *section, 0.0);
                    for (int k = 1; k <= pieces; ++k) {
                        const auto to = simulation::evaluate_section(placement, *section, section->length * k / pieces);
                        axes->add(from.tile_x * simulation::tile_size + from.x, from.tile_z * simulation::tile_size + from.z,
                                  to.tile_x * simulation::tile_size + to.x, to.tile_z * simulation::tile_size + to.z, 1.5f);
                        from = to;
                    }
                }
            }
            bool tested = false;
            for (const auto& object : tile.objects) {
                if (object.kind != formats::WorldObjectKind::forest || tested) continue;
                const auto path = renderer::forest_shape_path(*file, object.uid);
                const auto all = renderer::build_forest_shape(vfs, *path);
                const auto planted = all.model.vertices.size() / 8;
                if (!all.error.empty() || planted == 0) continue;
                renderer::set_forest_clearance_axes(axes);
                renderer::set_forest_clearance_distance(3.0f);
                const auto before = renderer::forest_tree_counts();
                const auto kept = renderer::build_forest_shape(vfs, *path);
                const auto counts = renderer::forest_tree_counts();
                renderer::set_forest_clearance_distance(0.0f);
                renderer::set_forest_clearance_axes(nullptr);
                const auto left = kept.model.vertices.size() / 8;
                if (left == planted || left == 0) continue; // clear of the tracks (or a collapsed one)
                tested = true;
                std::cout << "forest " << object.uid << ": " << left << " of " << planted << " trees kept off the track" << std::endl;
                check(counts.planted - before.planted == left && counts.cleared - before.cleared == planted - left, "forest tree counts");
                // Every kept tree's crown (5 m round its middle) is 3 m or more
                // from a track's edge.
                const auto& q = object.rotation;
                const glm::quat rotation{q[3], -q[0], -q[1], -q[2]};
                std::size_t near = 0;
                // The crown: half the tree's width (its first quad's two edges).
                const float crown = std::abs(all.model.vertices[0].position[2] - all.model.vertices[1].position[2]) * 0.5f;
                for (std::size_t t = 0; t < left; ++t) {
                    const auto& v = kept.model.vertices;
                    const glm::vec3 local{v[t * 8].position[0], 0.0f, v[t * 8 + 4].position[2]};
                    const auto offset = rotation * local;
                    const double x = tile.tile_x * simulation::tile_size + object.position.x + offset.x;
                    const double z = tile.tile_z * simulation::tile_size + object.position.z + offset.z;
                    if (axes->nearest(x, z, 40.0) < crown + 3.0f - 1e-3) ++near;
                }
                check(near == 0, "a forest reaching a track: no kept tree's crown within 3 m of the track's edge");
            }
            check(tested, "a forest on the tile reaches a track (some trees left out)");
        }
    }
    if (vfs.exists(*core::VirtualPath::parse("ROUTES/Kuban_RTS/WORLD/w-004494+014579.w"))) {
        const auto tile = world("w-004494+014579.w");
        std::size_t crossings = 0;
        for (const auto& object : tile.objects) {
            if (object.kind != formats::WorldObjectKind::level_crossing) continue;
            ++crossings;
            if (object.rail_items == std::vector<std::uint32_t>{435, 436}) {
                check(object.road_items == std::vector<std::uint32_t>{59, 60}, "level crossing road items");
            }
        }
        check(crossings >= 2, "Kuban level crossings parsed");
    }
    if (vfs.exists(*core::VirtualPath::parse("ROUTES/Kuban_RTS/WORLD/w-004493+014584.w"))) {
        const auto tile = world("w-004493+014584.w");
        bool found = false;
        for (const auto& object : tile.objects) {
            if (object.kind != formats::WorldObjectKind::car_spawner || object.uid != 252) continue;
            found = true;
            check(object.car_frequency == 5.0f && object.car_average_speed == 20.0f, "car spawner frequency and speed");
            check(object.road_items == std::vector<std::uint32_t>{90, 103}, "car spawner road items");
        }
        check(found, "Kuban car spawner 252 parsed");
        const auto cars = formats::load_car_spawn_list(vfs, *core::VirtualPath::parse("ROUTES/Kuban_RTS"));
        check(cars.ok() && cars.types.size() > 10 && cars.types.front().shape == "VAZ2106.s" && cars.types.front().distance == 2.0f,
              "carspawn.dat read");
    }
    if (const auto rain = core::VirtualPath::parse("ROUTES/Kuban_RTS/ENVFILES/rain.env"); rain && vfs.exists(*rain)) {
        const auto environment = formats::load_environment(vfs, *rain);
        check(environment.precipitation && !environment.precipitation->snow && environment.precipitation->density == 2048
                  && environment.precipitation->speed == 40.0f && environment.precipitation->shader.frames.size() == 4
                  && environment.precipitation->shader.frames[3].has_uv_stamp,
              "rain.env precipitation");
        check(environment.water && environment.water->layers.size() >= 3 && environment.water->layers[0].height == -1.0f,
              "rain.env water layers");
        check(environment.wind.size() == 1 && environment.wind[0].speed == 3.0f,
              "rain.env wind (the _world_wind_layers block left out): " + std::to_string(environment.wind.size()) + " layers, "
                  + (environment.wind.empty() ? std::string() : std::to_string(environment.wind[0].speed)));
    }
    if (const auto tiles = core::VirtualPath::parse("ROUTES/Kuban_RTS/TILES"); tiles && vfs.exists(*core::VirtualPath::parse("ROUTES/Kuban_RTS/Kuban_RTS.trk"))) {
        const auto tile = formats::load_terrain_tile(vfs, *tiles, -4494, 14578);
        std::size_t water = 0;
        for (const auto& patch : tile.patches) water += (patch.flags & 0xC0u) != 0 ? 1 : 0;
        check(tile.ok() && water == 7, "Kuban tile -4494 14578: 7 water patches, got " + std::to_string(water));
    }
    if (const auto eng = core::VirtualPath::parse("TRAINS/TRAINSET/mdd_2TE116-773/mdd_2te116-773a.eng"); eng && vfs.exists(*eng)) {
        const auto wagon = formats::load_wagon(vfs, "mdd_2TE116-773", "mdd_2te116-773a", true);
        check(wagon.ok() && wagon.engine && !wagon.engine->exhausts.empty() && wagon.engine->exhausts.front().size > 0.0f,
              "2TE116 DieselSpecialEffects read: " + wagon.error);
    }

}

} // namespace

int main(int argc, char** argv) {
    test_folders();
    test_clearance();
    test_description();
    if (argc > 1) test_content(argv[1]);
    std::cout << (failures ? "FAILED" : "passed") << '\n';
    return failures ? 1 : 0;
}
