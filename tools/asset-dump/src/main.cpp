#include <filesystem>
#include <iostream>
#include <optional>
#include <string_view>

#include <openrts/assets/assets.hpp>
#include <openrts/formats/ace_file.hpp>
#include <openrts/formats/shape_binary.hpp>
#include <openrts/formats/shape_description.hpp>
#include <openrts/formats/shape_file.hpp>

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
                 "   or: openrts_asset_dump --root <installation> --sd <relative-sd-path> [--route <name>]\n";
}

void print_resolution(const std::string_view label, const openrts::assets::ResolutionResult& result) {
    std::cout << label << " requested: " << result.requested_path.string() << '\n';
    for (const auto& attempt : result.attempts) {
        std::cout << "tried: " << attempt.candidate.string()
                  << (attempt.found ? " [found]" : " [missing]") << '\n';
    }
    std::cout << label << " resolved: "
              << (result.found() ? result.resolved_path->string() : "<not found>") << '\n';
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
        else if (option == "--kind") kind = parse_kind(value);
        else if (option == "--route") context.route_name = value;
        else if (option == "--trainset") context.trainset_name = value;
        else {
            print_usage();
            return 64;
        }
    }

    if (!root || (sd && (asset || kind)) || (!sd && (!asset || !kind))) {
        print_usage();
        return 64;
    }

    const openrts::assets::ContentResolver resolver({.installation_root = *root});
    if (sd) {
        const auto sd_result = resolver.resolve(AssetKind::shape, *sd, context);
        print_resolution("sd", sd_result);
        if (!sd_result.found()) return 2;

        const auto description = openrts::formats::load_shape_description(*sd_result.resolved_path);
        if (!description.ok()) {
            std::cout << "sd parse error: " << description.error << '\n';
            return 3;
        }
        std::cout << "shape reference: " << description.value->shape_file.string() << '\n';
        const auto shape_result = resolver.resolve_relative_to_directory(
            AssetKind::shape,
            sd_result.resolved_path->parent_path(),
            description.value->shape_file);
        print_resolution("shape", shape_result);
        if (!shape_result.found()) return 2;

        const auto probe = openrts::formats::probe_shape_file(*shape_result.resolved_path);
        std::cout << "shape representation: " << openrts::formats::to_string(probe.representation) << '\n'
                  << "shape probe: " << probe.diagnostic << '\n';
        if (probe.declared_inflated_size) {
            std::cout << "shape declared inflated size: " << *probe.declared_inflated_size << " bytes\n";
        }
        if (probe.representation == openrts::formats::ShapeRepresentation::compressed_binary) {
            const auto inflated = openrts::formats::inflate_narrow_compressed_shape(*shape_result.resolved_path);
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
                    const auto texture_result = resolver.resolve(
                        AssetKind::texture,
                        filename,
                        context);
                    print_resolution("texture", texture_result);
                    if (texture_result.found()) {
                        print_ace_probe(openrts::formats::probe_ace_file_path(*texture_result.resolved_path));
                        const auto decoded = openrts::formats::decode_structured_ace_rgba_path(
                            *texture_result.resolved_path);
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

    const auto result = resolver.resolve(*kind, *asset, context);

    std::cout << "requested: " << result.requested_path.string() << '\n'
              << "kind: " << openrts::assets::to_string(result.kind) << '\n';
    print_resolution("asset", result);
    return result.found() ? 0 : 2;
}
