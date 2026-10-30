#include <openrts/renderer/model_assets.hpp>

#include <openrts/formats/shape_file.hpp>

namespace openrts::renderer {

std::vector<std::string> StreamedModelAsset::dependency_keys() const {
    std::vector<std::string> keys{source_path.key(), source_path.with_extension(".sd").key()};
    for (const auto& texture : textures) {
        for (const auto& candidate : texture.candidates) keys.push_back(candidate.key());
    }
    return keys;
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
                                    TextureDecodeCache* cache) {
    StreamedModelAsset result{.source_path = shape};

    const auto body = formats::load_binary_shape_body(vfs, shape);
    if (!body.ok()) {
        result.error = body.error;
        return result;
    }
    result.model = formats::parse_binary_shape_model(*body.body);
    if (!result.model.ok()) {
        result.error = result.model.error;
        return result;
    }

    result.textures.reserve(result.model.images.size());
    for (const auto& image : result.model.images) {
        const auto name = image.generic_string();
        StreamedTextureAsset texture{.image_name = name};
        texture.candidates = shape_texture_candidates(shape, name, texture_directories);
        for (const auto& candidate : texture.candidates) {
            if (vfs.exists(candidate)) {
                texture.source_path = candidate;
                break;
            }
        }
        if (!texture.source_path) {
            texture.error = "Texture unresolved: " + name;
            result.textures.push_back(std::move(texture));
            continue;
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
        result.textures.push_back(std::move(texture));
    }
    return result;
}

} // namespace openrts::renderer
