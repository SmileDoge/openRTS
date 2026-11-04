#include <openrts/renderer/model_assets.hpp>

#include <algorithm>

#include <openrts/formats/cab_description.hpp>
#include <openrts/formats/shape_file.hpp>

#include "forest_shape.hpp"
#include "transfer_shape.hpp"

namespace openrts::renderer {
namespace {

// One image: the first of `candidates` that exists, with the alternative
// folders (`folders`) tried first inside each candidate's folder; decoded
// through `cache` when given.
[[nodiscard]] StreamedTextureAsset resolve_texture(const core::Vfs& vfs, std::string name, std::vector<core::VirtualPath> candidates,
                                                   const std::vector<std::string>& folders, TextureDecodeCache* cache) {
    StreamedTextureAsset texture{.image_name = std::move(name)};
    texture.candidates = std::move(candidates);
    if (!folders.empty()) {
        // The alternative folders first, inside each folder the image may be in.
        std::vector<core::VirtualPath> alternatives;
        for (const auto& folder : folders) {
            for (const auto& candidate : texture.candidates) {
                const auto sub = candidate.parent().resolve(std::string_view(folder));
                const auto file = sub ? sub->resolve(candidate.filename()) : std::nullopt;
                if (file && std::find(alternatives.begin(), alternatives.end(), *file) == alternatives.end()) alternatives.push_back(*file);
            }
        }
        texture.candidates.insert(texture.candidates.begin(), alternatives.begin(), alternatives.end());
    }
    for (const auto& candidate : texture.candidates) {
        if (vfs.exists(candidate)) {
            texture.source_path = candidate;
            break;
        }
    }
    if (!texture.source_path) {
        texture.error = "Texture unresolved: " + texture.image_name;
        return texture;
    }
    std::shared_ptr<const formats::AceDecodeResult> decoded;
    if (cache) {
        if (const auto it = cache->find(texture.source_path->key()); it != cache->end()) decoded = it->second;
    }
    if (!decoded) {
        decoded = std::make_shared<formats::AceDecodeResult>(formats::decode_ace_rgba(vfs, *texture.source_path));
        if (cache) cache->emplace(texture.source_path->key(), decoded);
    }
    if (decoded->ok()) {
        texture.image = *decoded->image;
    } else {
        texture.error = decoded->error;
    }
    return texture;
}

// A transfer (transfer_shape.hpp), loaded as a shape. Its texture comes from
// the route's TEXTURES, from SNOW when it snows (rts1.exe 0x41B6D0 builds its
// path with 0x4224F8 and the value 1).
[[nodiscard]] StreamedModelAsset load_transfer_asset(const core::Vfs& vfs, const core::VirtualPath& path, TextureDecodeCache* cache,
                                                     const TextureVariant& variant) {
    StreamedModelAsset result{.source_path = path};
    auto transfer = build_transfer_shape(vfs, path);
    if (!transfer.error.empty()) {
        result.error = transfer.error;
        return result;
    }
    result.model = std::move(transfer.model);
    result.alternative_texture = 1;
    result.texture_folders = alternative_texture_folders(result.alternative_texture, variant);
    result.texture_key = texture_variant_key(result.alternative_texture, false, variant);
    std::vector<core::VirtualPath> candidates;
    if (transfer.texture_folder) {
        if (auto file = transfer.texture_folder->resolve(transfer.texture)) candidates.push_back(std::move(*file));
    }
    result.textures.push_back(resolve_texture(vfs, transfer.texture, std::move(candidates), result.texture_folders, cache));
    return result;
}

// A forest (forest_shape.hpp), loaded as a shape. Its tree texture comes
// from the route's TEXTURES (else GLOBAL\TEXTURES), from the season's
// folder (rts1.exe 0x41B6D0 builds its path with 0x4224F8 and 252).
[[nodiscard]] StreamedModelAsset load_forest_asset(const core::Vfs& vfs, const core::VirtualPath& path, TextureDecodeCache* cache,
                                                   const TextureVariant& variant) {
    StreamedModelAsset result{.source_path = path};
    auto forest = build_forest_shape(vfs, path);
    if (!forest.error.empty()) {
        result.error = forest.error;
        return result;
    }
    result.model = std::move(forest.model);
    result.alternative_texture = 252;
    result.texture_folders = alternative_texture_folders(result.alternative_texture, variant);
    result.texture_key = texture_variant_key(result.alternative_texture, false, variant);
    std::vector<core::VirtualPath> candidates;
    if (forest.route_textures) {
        if (auto file = forest.route_textures->resolve(forest.texture)) candidates.push_back(std::move(*file));
    }
    if (const auto global = core::VirtualPath::parse("GLOBAL/TEXTURES")) {
        if (auto file = global->resolve(forest.texture)) candidates.push_back(std::move(*file));
    }
    result.textures.push_back(resolve_texture(vfs, forest.texture, std::move(candidates), result.texture_folders, cache));
    return result;
}

} // namespace

std::vector<std::string> StreamedModelAsset::dependency_keys() const {
    std::vector<std::string> keys{source_path.key(), source_path.with_extension(".sd").key()};
    for (const auto& source : sources) keys.push_back(source.key());
    for (const auto& texture : textures) {
        for (const auto& candidate : texture.candidates) keys.push_back(candidate.key());
    }
    return keys;
}

std::vector<std::string> alternative_texture_folders(const std::uint32_t flags, const TextureVariant& variant) {
    // rts1.exe 0x4224F8: the whole value, not its bits.
    const bool snowing = variant.weather == rts::Weather::snow;
    const auto season = variant.season;
    const bool snow = season == rts::Season::winter || (season != rts::Season::summer && snowing);
    switch (flags) {
    case 0:
        return {};
    case 0xFC: // the six season folders
        switch (season) {
        case rts::Season::winter: return {snowing ? "WINTERSNOW" : "WINTER"};
        case rts::Season::autumn: return {snowing ? "AUTUMNSNOW" : "AUTUMN"};
        case rts::Season::spring: return {snowing ? "SPRINGSNOW" : "SPRING"};
        case rts::Season::summer: return {};
        }
        return {};
    case 0x100: // night
        return variant.night ? std::vector<std::string>{"NIGHT"} : std::vector<std::string>{};
    case 0x101: // night, else snow
        if (variant.night) return {"NIGHT"};
        return snow ? std::vector<std::string>{"SNOW"} : std::vector<std::string>{};
    default: // any other value: snow
        return snow ? std::vector<std::string>{"SNOW"} : std::vector<std::string>{};
    }
}

std::vector<std::string> texture_variant_key(const std::uint32_t flags, const bool fullbright_images, const TextureVariant& variant) {
    auto key = alternative_texture_folders(flags, variant);
    if (variant.fullbright_lit && fullbright_images && std::find(key.begin(), key.end(), "NIGHT") != key.end()) key.emplace_back("fullbright-day");
    return key;
}

std::vector<core::VirtualPath> shape_texture_candidates(const core::VirtualPath& shape,
                                                        const std::string_view image_name,
                                                        const std::vector<core::VirtualPath>& preferred) {
    std::vector<core::VirtualPath> result;
    const auto add = [&](const std::optional<core::VirtualPath>& path) {
        if (path && std::find(result.begin(), result.end(), *path) == result.end()) result.push_back(*path);
    };
    for (const auto& directory : preferred) add(directory.resolve(image_name));
    const auto directory = shape.parent();
    add(directory.resolve(image_name));
    if (const auto textures = directory.resolve(std::string_view("TEXTURES"))) add(textures->resolve(image_name));
    if (const auto textures = directory.parent().resolve(std::string_view("TEXTURES"))) add(textures->resolve(image_name));
    return result;
}

StreamedModelAsset load_model_asset(const core::Vfs& vfs,
                                    const core::VirtualPath& shape,
                                    const std::vector<core::VirtualPath>& texture_directories,
                                    TextureDecodeCache* cache,
                                    const TextureVariant& variant) {
    if (shape.extension() == ".transfer") return load_transfer_asset(vfs, shape, cache, variant);
    if (shape.extension() == ".forest") return load_forest_asset(vfs, shape, cache, variant);
    StreamedModelAsset result{.source_path = shape};
    // An RTS .repaint (TSR's ChS2 and others: the .rtseng's WagonShape, a
    // cab's Cabin): Repaint ( Shape ( "x.s" ) ShapeDescription ( "x.sd" )
    // TextureReplace ( "old.ace" "new.ace" ) ... ), named relative to it.
    core::VirtualPath geometry = shape;
    core::VirtualPath description_file = shape.with_extension(".sd");
    formats::Repaint repaint;
    if (core::fold_case(shape.extension()) == ".repaint") {
        const auto text = vfs.read(shape);
        if (!text) {
            result.error = "cannot read the repaint";
            return result;
        }
        repaint = formats::parse_repaint(*text);
        const auto target = repaint.ok() ? shape.parent().resolve(repaint.shape) : std::nullopt;
        if (!target) {
            result.error = "a repaint without a Shape";
            return result;
        }
        geometry = *target;
        description_file = geometry.with_extension(".sd");
        if (!repaint.description.empty()) {
            if (const auto named = shape.parent().resolve(repaint.description)) description_file = *named;
        }
        result.sources = {geometry, description_file};
    }
    if (const auto description = formats::load_shape_description(vfs, description_file); description.ok()) {
        result.alternative_texture = description.value->alternative_texture;
        result.lights = description.value->lights;
    }
    result.texture_folders = alternative_texture_folders(result.alternative_texture, variant);

    const auto body = formats::load_binary_shape_body(vfs, geometry);
    if (!body.ok()) {
        result.error = body.error;
        return result;
    }
    result.model = formats::parse_binary_shape_model(*body.body);
    if (!result.model.ok()) {
        result.error = result.model.error;
        return result;
    }

    // Images drawn only on FullBright surfaces (light material -8).
    std::vector<int> fullbright_use(result.model.images.size(), 0); // 1 FullBright only, 2 also other
    for (const auto& draw : result.model.draws) {
        if (draw.primitive_state_index >= result.model.primitive_states.size()) continue;
        const auto& state = result.model.primitive_states[draw.primitive_state_index];
        if (state.texture_count == 0 || state.texture_index >= result.model.textures.size()) continue;
        const auto image = result.model.textures[state.texture_index].image_index;
        if (image >= fullbright_use.size()) continue;
        const bool fullbright = state.vertex_state_index >= 0
            && static_cast<std::size_t>(state.vertex_state_index) < result.model.vertex_states.size()
            && result.model.vertex_states[state.vertex_state_index].light_material_index == -8;
        fullbright_use[image] = fullbright ? (fullbright_use[image] == 2 ? 2 : 1) : 2;
    }
    result.fullbright_images = std::find(fullbright_use.begin(), fullbright_use.end(), 1) != fullbright_use.end();
    result.texture_key = texture_variant_key(result.alternative_texture, result.fullbright_images, variant);

    result.textures.reserve(result.model.images.size());
    for (std::size_t index = 0; index < result.model.images.size(); ++index) {
        const auto& image = result.model.images[index];
        const auto name = image.generic_string();
        auto folders = result.texture_folders;
        if (variant.fullbright_lit && fullbright_use[index] == 1) std::erase(folders, std::string("NIGHT"));
        if (repaint.ok()) {
            // A replaced image: its new file beside the repaint, then beside
            // the shape; the others as the shape has them.
            const auto file_name = [](const std::string& path) {
                const auto slash = path.find_last_of("/\\");
                return core::fold_case(slash == std::string::npos ? path : path.substr(slash + 1));
            };
            const auto swap = std::find_if(repaint.textures.begin(), repaint.textures.end(),
                                           [&](const auto& pair) { return file_name(pair.first) == file_name(name); });
            const auto& wanted = swap != repaint.textures.end() ? swap->second : name;
            auto candidates = swap != repaint.textures.end() ? shape_texture_candidates(shape, wanted, texture_directories)
                                                             : std::vector<core::VirtualPath>{};
            for (auto& candidate : shape_texture_candidates(geometry, wanted, texture_directories)) {
                if (std::find(candidates.begin(), candidates.end(), candidate) == candidates.end()) candidates.push_back(std::move(candidate));
            }
            auto texture = resolve_texture(vfs, wanted, std::move(candidates), folders, cache);
            texture.image_name = name;
            result.textures.push_back(std::move(texture));
            continue;
        }
        result.textures.push_back(resolve_texture(vfs, name, shape_texture_candidates(shape, name, texture_directories), folders, cache));
    }
    return result;
}

} // namespace openrts::renderer
