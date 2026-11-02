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
#include <openrts/formats/shape_description.hpp>
#include <openrts/renderer/rts_season.hpp>

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

// What the world looks like for ESD_Alternative_Texture: night, the season
// and the weather pick an image's subfolder (TEXTURES\NIGHT, \WINTER, ...).
struct TextureVariant {
    bool night = false;
    rts::Season season = rts::Season::summer;
    rts::Weather weather = rts::Weather::clear;
    // FullBright surfaces lit as ordinary ones (light_fullbright(), the
    // experimental path): an image drawn only on them keeps its day texture
    // at night (its NIGHT one is darkened for being unlit).
    bool fullbright_lit = false;
    bool operator==(const TextureVariant&) const = default;
};

// The subfolder a shape with this ESD_Alternative_Texture value takes its
// images from in this world (rts1.exe 0x4224F8, the whole value): 252 the
// season's (WINTERSNOW / WINTER / AUTUMNSNOW / AUTUMN / SPRINGSNOW / SPRING,
// none in summer); 256 NIGHT at night; 257 NIGHT at night, else SNOW; any
// other non-zero value SNOW. SNOW: in winter, or when it snows outside
// summer. An image missing there is taken from the folder itself (openRTS).
[[nodiscard]] std::vector<std::string> alternative_texture_folders(std::uint32_t flags, const TextureVariant& variant);
// What a model's textures depend on in this world: its folders, and whether
// its FullBright-only images keep their day textures at night.
[[nodiscard]] std::vector<std::string> texture_variant_key(std::uint32_t flags, bool fullbright_images, const TextureVariant& variant);

// CPU-resident model assembled by a worker thread. Creating the RenderModel
// remains a render-thread operation because it owns WebGPU calls.
struct StreamedModelAsset {
    core::VirtualPath source_path;
    formats::ShapeModel model;
    std::vector<StreamedTextureAsset> textures;
    // From the shape's .sd: its ESD_Alternative_Texture bits and the folders
    // they gave for the variant it was loaded with, and its light sources.
    std::uint32_t alternative_texture{};
    std::vector<std::string> texture_folders;
    bool fullbright_images = false;       // some image is drawn only on FullBright surfaces
    std::vector<std::string> texture_key; // texture_variant_key it was loaded with
    std::vector<formats::ShapeLight> lights;
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
                                                  TextureDecodeCache* cache = nullptr,
                                                  const TextureVariant& variant = {});

} // namespace openrts::renderer
