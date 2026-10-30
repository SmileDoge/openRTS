#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <openrts/core/vfs.hpp>
#include <openrts/formats/ace_file.hpp>
#include <openrts/formats/shape_binary.hpp>

namespace openrts::renderer {

// One image of a shape, resolved and decoded on a worker thread.
struct StreamedTextureAsset {
    std::string image_name; // as written in the shape
    std::optional<core::VirtualPath> source_path;
    // Every candidate looked at, so a texture that appears later (a new file,
    // a mod) is noticed by hot reload.
    std::vector<core::VirtualPath> candidates;
    std::optional<formats::AceRgbaImage> image;
    std::string error;
};

// CPU-resident model assembled by a worker thread. Creating the RenderModel
// remains a render-thread operation because it owns WebGPU calls.
struct StreamedModelAsset {
    core::VirtualPath source_path;
    formats::ShapeModel model;
    std::vector<StreamedTextureAsset> textures;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty() && model.ok(); }
    // Keys of every file the model depends on: the shape, its .sd and all
    // texture candidates.
    [[nodiscard]] std::vector<std::string> dependency_keys() const;
};

// Decoded textures shared between the models of one load batch.
using TextureDecodeCache = std::unordered_map<std::string, std::shared_ptr<const formats::AceDecodeResult>>;

// Where an image name of `shape` may live, in search order: the preferred
// folders (a route's TEXTURES: global track shapes keep their textures there),
// next to the shape, the shape's TEXTURES and its parent's TEXTURES
// (GLOBAL/SHAPES -> GLOBAL/TEXTURES). Names may use `\` and `..`.
[[nodiscard]] std::vector<core::VirtualPath> shape_texture_candidates(const core::VirtualPath& shape,
                                                                      std::string_view image_name,
                                                                      const std::vector<core::VirtualPath>& preferred);

// Worker-thread job: parses the shape and decodes every texture it uses.
[[nodiscard]] StreamedModelAsset load_model_asset(const core::Vfs& vfs,
                                                  const core::VirtualPath& shape,
                                                  const std::vector<core::VirtualPath>& texture_directories,
                                                  TextureDecodeCache* cache = nullptr);

} // namespace openrts::renderer
