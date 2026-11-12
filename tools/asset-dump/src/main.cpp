#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <openrts/assets/assets.hpp>
#include <openrts/assets/content_resolver.hpp>
#include <openrts/core/virtual_path.hpp>
#include <openrts/formats/ace_file.hpp>
#include <openrts/formats/shape_binary.hpp>
#include <openrts/formats/shape_description.hpp>
#include <openrts/formats/shape_file.hpp>
#include <openrts/formats/route_world.hpp>

namespace {

using openrts::assets::AssetKind;

[[nodiscard]] std::optional<AssetKind> parse_kind(const std::string_view value) {
    if (value == "shape") return AssetKind::shape;
    if (value == "texture") return AssetKind::texture;
    if (value == "library") return AssetKind::locomotive_library;
    if (value == "train") return AssetKind::train_asset;
    return std::nullopt;
}

void print_usage() {
    std::cout << "Usage: openrts_asset_dump --root <installation> --kind <shape|texture|library|train> "
                 "--asset <relative-path> [--route <name>] [--trainset <name>]\n"
                 "   or: openrts_asset_dump --root <installation> --sd <relative-sd-path> [--route <name>]\n"
                 "   or: openrts_asset_dump --root <installation> --route <name> --world <relative-world-path>\n"
                 "   or: openrts_asset_dump --root <installation> --lights <route name | all>\n";
}

// --lights: the lamps (ESD_LightSources of the .sd beside each shape) of
// everything placed on a route, counted over all its world files, by shape
// (cone, sphere, box) and by when they burn.
struct ShapeLamps {
    int cone = 0, sphere = 0, box = 0;
    int night = 0; // NIGHT_AUTO or NIGHT_ONLY
    float sphere_reach = 0.0f;
};

int print_route_lights(const openrts::assets::ContentResolver& resolver, const std::string& route_name) {
    using openrts::core::VirtualPath;
    const auto& vfs = resolver.vfs();
    const auto route = VirtualPath::parse("ROUTES/" + route_name);
    if (!route) return 64;
    const auto world_dir = route->resolve_filesystem("WORLD");
    if (!world_dir || !vfs.is_directory(*world_dir)) {
        std::cout << route_name << ": no WORLD folder\n";
        return 2;
    }
    openrts::assets::AssetContext context;
    context.route_name = route_name;
    std::unordered_map<std::string, ShapeLamps> shapes;
    std::map<std::string, int> sphere_placed; // shape -> placements with sphere lamps
    std::map<std::string, ShapeLamps> sphere_shapes;
    long placements = 0, lit_placements = 0, cone = 0, sphere = 0, box = 0, night = 0, files = 0;
    for (const auto& entry : vfs.list(*world_dir)) {
        if (entry.directory || (entry.path.extension() != ".w" && entry.path.extension() != ".W")) continue;
        const auto tile = openrts::formats::load_world_tile(vfs, entry.path);
        if (!tile.ok()) continue;
        ++files;
        for (const auto& object : tile.objects) {
            ++placements;
            const auto name = object.shape_file.generic_string();
            auto found = shapes.find(name);
            if (found == shapes.end()) {
                ShapeLamps lamps;
                const auto resolved = resolver.resolve(AssetKind::shape, name, context);
                if (resolved.found()) {
                    const auto description = openrts::formats::load_shape_description(vfs, resolved.resolved_path->with_extension(".sd"));
                    if (description.ok()) {
                        for (const auto& light : description.value->lights) {
                            const bool is_cone = light.flags.find("light_cone") != std::string::npos;
                            const bool is_sphere = light.flags.find("light_sphere") != std::string::npos;
                            if (is_cone) ++lamps.cone;
                            else if (is_sphere) ++lamps.sphere;
                            else ++lamps.box;
                            if (light.flags.find("night_auto") != std::string::npos || light.flags.find("night_only") != std::string::npos)
                                ++lamps.night;
                            if (is_sphere) {
                                const float dx = light.points_to[0] - light.position[0], dy = light.points_to[1] - light.position[1],
                                            dz = light.points_to[2] - light.position[2];
                                lamps.sphere_reach = (std::max)(lamps.sphere_reach, std::sqrt(dx * dx + dy * dy + dz * dz)
                                                                                         * (light.attenuation[1] + light.attenuation[2]));
                            }
                        }
                    }
                }
                found = shapes.emplace(name, lamps).first;
            }
            const auto& lamps = found->second;
            if (lamps.cone + lamps.sphere + lamps.box == 0) continue;
            ++lit_placements;
            cone += lamps.cone;
            sphere += lamps.sphere;
            box += lamps.box;
            night += lamps.night;
            if (lamps.sphere > 0) {
                ++sphere_placed[name];
                sphere_shapes[name] = lamps;
            }
        }
    }
    std::cout << route_name << ": " << files << " world files, " << placements << " objects, " << lit_placements
              << " with lamps; lamps: " << cone << " cone, " << sphere << " sphere, " << box << " box; " << night
              << " burn only at night\n";
    std::vector<std::pair<std::string, int>> top(sphere_placed.begin(), sphere_placed.end());
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (std::size_t i = 0; i < top.size() && i < 8; ++i) {
        const auto& lamps = sphere_shapes[top[i].first];
        std::cout << "  sphere: " << top[i].first << " placed " << top[i].second << " times, " << lamps.sphere
                  << " sphere lamp(s) each, reach about " << lamps.sphere_reach << " m\n";
    }
    return 0;
}

void print_resolution(const std::string_view label, const openrts::assets::ResolutionResult& result) {
    std::cout << label << " requested: " << result.requested_path << '\n';
    for (const auto& attempt : result.attempts) {
        std::cout << "tried: " << attempt.candidate.str() << (attempt.found ? " [found]" : " [missing]") << '\n';
    }
    std::cout << label << " resolved: " << (result.found() ? result.resolved_path->str() : "<not found>") << '\n';
}

void print_ace_probe(const openrts::formats::AceFileProbe& probe) {
    std::cout << "ace representation: " << openrts::formats::to_string(probe.representation) << '\n'
              << "ace probe: " << probe.diagnostic << '\n';
    if (probe.declared_inflated_size) {
        std::cout << "ace declared inflated size: " << *probe.declared_inflated_size << " bytes\n";
    }
    if (probe.header) {
        const auto& header = *probe.header;
        std::cout << "ace header: version=" << header.version << " options=" << header.options
                  << " size=" << header.width << 'x' << header.height
                  << " format-code=" << header.format_code << " channels=" << header.channel_count << '\n';
    }
}

} // namespace

int main(const int argc, char* argv[]) {
    std::optional<std::filesystem::path> root;
    std::optional<std::filesystem::path> asset;
    std::optional<std::filesystem::path> sd;
    std::optional<std::filesystem::path> world;
    std::optional<std::string> lights;
    std::optional<AssetKind> kind;
    openrts::assets::AssetContext context;

    for (int index = 1; index < argc; index += 2) {
        if (index + 1 >= argc) {
            print_usage();
            return 64;
        }

        const std::string_view option = argv[index];
        const std::string_view value = argv[index + 1];
        if (option == "--root") root = value;
        else if (option == "--asset") asset = value;
        else if (option == "--sd") sd = value;
        else if (option == "--world") world = value;
        else if (option == "--lights") lights = value;
        else if (option == "--kind") kind = parse_kind(value);
        else if (option == "--route") context.route_name = value;
        else if (option == "--trainset") context.trainset_name = value;
        else {
            print_usage();
            return 64;
        }
    }

    if (root && lights) {
        const openrts::assets::ContentResolver resolver(openrts::assets::ContentRoots{.installation_root = *root});
        if (*lights != "all") return print_route_lights(resolver, *lights);
        int result = 0;
        for (const auto& entry : resolver.vfs().list(*openrts::core::VirtualPath::parse("ROUTES"))) {
            if (entry.directory) result = (std::max)(result, print_route_lights(resolver, std::string(entry.path.filename())));
        }
        return result;
    }
    if (!root || (world && (sd || asset || kind || !context.route_name))
        || (sd && (asset || kind)) || (!world && !sd && (!asset || !kind))) {
        print_usage();
        return 64;
    }

    const openrts::assets::ContentResolver resolver(openrts::assets::ContentRoots{.installation_root = *root});
    const auto& vfs = resolver.vfs();
    if (world) {
        const auto route = *openrts::core::VirtualPath::parse("ROUTES/" + *context.route_name);
        const auto definition = openrts::formats::load_route_definition(vfs, route);
        if (!definition.ok()) {
            std::cout << "route parse error: " << definition.error << '\n';
            return 3;
        }
        std::cout << "route: " << definition.route_id << '\n';
        if (definition.start) {
            std::cout << "route start: tile=" << definition.start->tile_x << ',' << definition.start->tile_z
                      << " local=" << definition.start->x << ',' << definition.start->z << '\n';
        }
        const auto world_path = route.resolve_filesystem(*world);
        if (!world_path) {
            print_usage();
            return 64;
        }
        const auto tile = openrts::formats::load_world_tile(vfs, *world_path);
        if (!tile.ok()) {
            std::cout << "world parse error: " << tile.error << '\n';
            return 3;
        }
        std::cout << "world tile: " << tile.tile_x << ',' << tile.tile_z
                  << " objects: " << tile.objects.size() << '\n';
        for (const auto& object : tile.objects) {
            std::cout << openrts::formats::to_string(object.kind) << " uid=" << object.uid << " shape=" << object.shape_file.string()
                      << " local=" << object.position.x << ',' << object.position.y << ',' << object.position.z
                      << " rotation=" << object.rotation[0] << ',' << object.rotation[1] << ','
                      << object.rotation[2] << ',' << object.rotation[3] << '\n';
        }
        return 0;
    }
    if (sd) {
        const auto sd_result = resolver.resolve(AssetKind::shape, sd->generic_string(), context);
        print_resolution("sd", sd_result);
        if (!sd_result.found()) return 2;

        const auto description = openrts::formats::load_shape_description(vfs, *sd_result.resolved_path);
        if (!description.ok()) {
            std::cout << "sd parse error: " << description.error << '\n';
            return 3;
        }
        std::cout << "shape reference: " << description.value->shape_file.string() << '\n';
        const auto shape_result = resolver.resolve_relative_to_directory(
            AssetKind::shape, sd_result.resolved_path->parent(), description.value->shape_file.generic_string());
        print_resolution("shape", shape_result);
        if (!shape_result.found()) return 2;

        const auto shape_bytes = vfs.read(*shape_result.resolved_path).value_or(std::string{});
        const auto probe = openrts::formats::probe_shape_file(std::string_view(shape_bytes));
        std::cout << "shape representation: " << openrts::formats::to_string(probe.representation) << '\n'
                  << "shape probe: " << probe.diagnostic << '\n';
        if (probe.declared_inflated_size) {
            std::cout << "shape declared inflated size: " << *probe.declared_inflated_size << " bytes\n";
        }
        if (probe.representation == openrts::formats::ShapeRepresentation::compressed_binary) {
            const auto inflated = openrts::formats::inflate_narrow_compressed_shape(std::string_view(shape_bytes));
            if (!inflated.ok()) {
                std::cout << "shape inflate error: " << inflated.error << '\n';
                return 3;
            }
            std::cout << "shape inflated body: " << inflated.body->size() << " bytes\n";
            const auto images = openrts::formats::extract_image_filenames_from_binary_shape(*inflated.body);
            if (!images.ok()) {
                std::cout << "texture references: unavailable; " << images.error << '\n';
            } else {
                std::cout << "texture references: " << images.filenames.size() << '\n';
                for (const auto& filename : images.filenames) {
                    std::cout << "texture reference: " << filename.string() << '\n';
                    const auto texture_result = resolver.resolve(AssetKind::texture, filename.generic_string(), context);
                    print_resolution("texture", texture_result);
                    if (texture_result.found()) {
                        const auto texture_bytes = vfs.read(*texture_result.resolved_path).value_or(std::string{});
                        print_ace_probe(openrts::formats::probe_ace_file(texture_bytes));
                        const auto decoded = openrts::formats::decode_structured_ace_rgba(texture_bytes);
                        if (decoded.ok()) {
                            std::cout << "ace decoded RGBA8: " << decoded.image->width << 'x'
                                      << decoded.image->height << " (" << decoded.image->pixels.size() << " bytes)\n";
                        } else {
                            std::cout << "ace decode: unavailable; " << decoded.error << '\n';
                        }
                    }
                }
            }
        }
        return probe.valid() ? 0 : 3;
    }

    const auto result = resolver.resolve(*kind, asset->generic_string(), context);

    std::cout << "requested: " << result.requested_path << '\n'
              << "kind: " << openrts::assets::to_string(result.kind) << '\n';
    print_resolution("asset", result);
    if (!result.found()) return 2;
    if (*kind == AssetKind::shape) {
        // The nodes (matrices: name, translation, the basis rows), their
        // parents and the animations' keys: what a node's rest pose and its
        // frames are (a crossing's gates, a turntable, a door).
        // Compressed, binary or text: the binary body the game reads.
        const auto bytes = vfs.read(*result.resolved_path).value_or(std::string{});
        const auto loaded = openrts::formats::load_binary_shape_body(std::string_view(bytes));
        if (!loaded.ok()) {
            std::cout << "shape read error: " << loaded.error << '\n';
            return 3;
        }
        const auto model = openrts::formats::parse_binary_shape_model(*loaded.body);
        // The images (each name once, with how many image entries carry it),
        // the textures and the primitives that use them.
        std::cout << "images: " << model.images.size() << ", textures: " << model.textures.size() << ", primitive states: "
                  << model.primitive_states.size() << ", draws: " << model.draws.size() << '\n';
        {
            std::map<std::string, int> names;
            for (const auto& image : model.images) ++names[image.generic_string()];
            for (const auto& [name, count] : names) std::cout << "  image " << name << " x" << count << '\n';
        }
        if (!model.level_distances.empty()) {
            std::cout << "distance levels:";
            for (const float distance : model.level_distances) std::cout << ' ' << distance << " m";
            std::cout << '\n';
        }
        std::cout << "matrices: " << model.matrices.size() << '\n';
        for (std::size_t m = 0; m < model.matrices.size(); ++m) {
            const auto& v = model.matrices[m].values;
            std::cout << "  " << m << ' ' << model.matrices[m].name << " parent "
                      << (m < model.hierarchy.size() ? model.hierarchy[m] : -2) << " at (" << v[9] << ", " << v[10] << ", " << v[11]
                      << ") rows (" << v[0] << ' ' << v[1] << ' ' << v[2] << " | " << v[3] << ' ' << v[4] << ' ' << v[5] << " | " << v[6]
                      << ' ' << v[7] << ' ' << v[8] << ")\n";
        }
        std::cout << "animations: " << model.animations.size() << '\n';
        for (const auto& animation : model.animations) {
            std::cout << "  frames " << animation.frame_count << ", rate " << animation.frame_rate << ", nodes with keys "
                      << animation.nodes.size() << '\n';
            for (const auto& node : animation.nodes) {
                std::cout << "    " << node.name << " (matrix " << node.matrix_index << ")\n";
                for (const auto& key : node.positions)
                    std::cout << "      position @" << key.frame << ": " << key.value[0] << ' ' << key.value[1] << ' ' << key.value[2] << '\n';
                for (const auto& key : node.rotations)
                    std::cout << "      rotation @" << key.frame << ": " << key.value[0] << ' ' << key.value[1] << ' ' << key.value[2] << ' '
                              << key.value[3] << '\n';
            }
        }
    }
    return 0;
}
