#include <openrts/renderer/model_assets.hpp>

#include <algorithm>
#include <map>
#include <mutex>

#include <openrts/formats/cab_description.hpp>
#include <openrts/formats/shape_file.hpp>

#include "cargo_shape.hpp"
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
    result.texture_key = texture_variant_key(result.alternative_texture, variant);
    std::vector<core::VirtualPath> candidates;
    if (transfer.texture_folder) {
        if (auto file = transfer.texture_folder->resolve(transfer.texture)) candidates.push_back(std::move(*file));
    }
    result.textures.push_back(resolve_texture(vfs, transfer.texture, std::move(candidates), result.texture_folders, cache));
    return result;
}

// openRTS's cargo pile or stream (cargo_shape.hpp), loaded as a shape; its
// texture from the folder it names.
[[nodiscard]] StreamedModelAsset load_cargo_asset(const core::Vfs& vfs, const core::VirtualPath& path, TextureDecodeCache* cache,
                                                  const TextureVariant& variant) {
    StreamedModelAsset result{.source_path = path};
    auto cargo = build_cargo_shape(path);
    if (!cargo.error.empty()) {
        result.error = cargo.error;
        return result;
    }
    result.model = std::move(cargo.model);
    result.texture_folders = alternative_texture_folders(0, variant);
    result.texture_key = texture_variant_key(0, variant);
    std::vector<core::VirtualPath> candidates;
    if (cargo.texture) candidates.push_back(*cargo.texture);
    result.textures.push_back(resolve_texture(vfs, result.model.images.front().generic_string(), std::move(candidates), {}, cache));
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
    result.texture_key = texture_variant_key(result.alternative_texture, variant);
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

core::VirtualPath shape_override_path(const core::VirtualPath& shape) {
    return core::VirtualPath::parse(shape.str() + ".openrts").value_or(shape);
}

namespace {

struct HeldOverrides {
    std::mutex mutex;
    std::map<std::string, formats::ShapeOverride> by_shape; // by shape key
};

HeldOverrides& held_overrides() {
    static HeldOverrides held;
    return held;
}

} // namespace

void hold_shape_override(const core::VirtualPath& shape, std::optional<formats::ShapeOverride> data) {
    auto& held = held_overrides();
    const std::lock_guard lock(held.mutex);
    if (data) {
        held.by_shape[shape.key()] = std::move(*data);
    } else {
        held.by_shape.erase(shape.key());
    }
}

formats::ShapeOverride find_shape_override(const core::Vfs& vfs, const core::VirtualPath& shape) {
    {
        auto& held = held_overrides();
        const std::lock_guard lock(held.mutex);
        if (const auto it = held.by_shape.find(shape.key()); it != held.by_shape.end()) return it->second;
    }
    const auto file = shape_override_path(shape);
    if (!vfs.exists(file)) return {};
    if (const auto text = vfs.read(file)) return formats::parse_shape_override(*text);
    return {};
}

std::vector<std::string> StreamedModelAsset::dependency_keys() const {
    std::vector<std::string> keys{source_path.key(), source_path.with_extension(".sd").key(), shape_override_path(source_path).key()};
    for (const auto& source : sources) keys.push_back(source.key());
    keys.push_back(shape_material_path(source_path).key());
    for (const auto& file : material_files) keys.push_back(file.key());
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

std::vector<std::string> texture_variant_key(const std::uint32_t flags, const TextureVariant& variant) {
    return alternative_texture_folders(flags, variant);
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
    if (shape.extension() == ".cargopile" || shape.extension() == ".cargostream") return load_cargo_asset(vfs, shape, cache, variant);
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
        result.pickup = description.value->pickup;
    }
    // openRTS's own changes to it (the inspector's, from a mod): NightTextures
    // ( 0 ) drops the night from its alternative textures (256 none, 257 the
    // snow alone), so every key made of them agrees.
    const auto own = find_shape_override(vfs, shape);
    if (own.lights) result.lights = *own.lights; // the inspector's lamps over the .sd's
    if (own.night_textures && !*own.night_textures) {
        if (result.alternative_texture == 0x100) result.alternative_texture = 0;
        else if (result.alternative_texture == 0x101) result.alternative_texture = 1;
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
    result.original_light = formats::apply_shape_override(result.model, own);

    result.texture_key = texture_variant_key(result.alternative_texture, variant);

    result.textures.reserve(result.model.images.size());
    for (std::size_t index = 0; index < result.model.images.size(); ++index) {
        const auto& image = result.model.images[index];
        const auto name = image.generic_string();
        const auto& folders = result.texture_folders;
        if (repaint.ok()) {
            // A replaced image: its new file beside the repaint, then beside
            // the shape; the others as the shape has them.
            const auto file_name = [](const std::string& path) {
                const auto slash = path.find_last_of("/\\");
                return core::fold_case(slash == std::string::npos ? path : path.substr(slash + 1));
            };
            const auto swap = std::find_if(repaint.textures.begin(), repaint.textures.end(),
                                           [&](const auto& pair) { return file_name(pair.first) == file_name(name); });
            // RTS 0x4BCCFC: the new file is the repaint's folder followed by
            // the text, so a leading slash ("/CabBCHEPT/CAB1KE_BCH.ace", the
            // VL80s and 2TE10 cabs) is still a folder beside the repaint.
            std::string wanted = name;
            if (swap != repaint.textures.end()) {
                const auto start = swap->second.find_first_not_of("/\\");
                if (start != std::string::npos) wanted = swap->second.substr(start);
            }
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
    // openRTS's materials (PBR stage 4).
    load_model_materials(vfs, result, cache);
    return result;
}

} // namespace openrts::renderer
