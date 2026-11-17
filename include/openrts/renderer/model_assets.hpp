#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <openrts/core/vfs.hpp>
#include <openrts/formats/ace_file.hpp>
#include <openrts/formats/material.hpp>
#include <openrts/formats/shape_binary.hpp>
#include <openrts/formats/shape_description.hpp>
#include <openrts/formats/shape_override.hpp>
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

// A map of openRTS's materials (PBR stage 4, docs/pbr-materials.md) as the
// GPU takes it: RGBA8 with its mip levels. ORM is always occlusion,
// roughness, metallic (repacked from the file's layout, single maps put in
// their channels); a normal map's green points up (a "down" one flipped).
enum class MaterialMapKind : std::uint8_t { base_color, orm, normal, emissive };
struct MaterialMapAsset {
    MaterialMapKind kind{};
    std::string key; // the files and how they were packed: the GPU cache's key
    std::shared_ptr<const formats::AceDecodeResult> image;
};

// A primitive state's material, and the layers over RTS's it came from (the
// inspector shows where each field came from).
struct PrimitiveMaterialAsset {
    formats::Material material;
    std::vector<std::pair<formats::MaterialLayer, formats::MaterialPatch>> layers;
    // Into StreamedModelAsset::material_maps; -1: none.
    std::int32_t base_color = -1, orm = -1, normal = -1, emissive = -1;
};

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
    // The light material each primitive state has in the shape itself,
    // before its `<shape>.openrts` (the inspector's override) changed some.
    std::vector<std::int32_t> original_light;
    // For an RTS .repaint (source_path): the shape and .sd it names.
    std::vector<core::VirtualPath> sources;
    // openRTS's materials (load_model_materials): per primitive state (empty:
    // RTS's for all), their maps, the tangents of the vertices of primitives
    // with a normal map (MikkTSpace, xyz and the bitangent's sign; empty
    // without), the material files and maps looked for (hot reload), and what
    // was wrong with them.
    std::vector<PrimitiveMaterialAsset> materials;
    std::vector<MaterialMapAsset> material_maps;
    std::vector<std::array<float, 4>> tangents;
    std::vector<core::VirtualPath> material_files;
    std::vector<std::string> material_messages;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty() && model.ok(); }
    // Keys of every file the model depends on: the shape, its .sd, a
    // repaint's shape and .sd, and all texture candidates.
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

// openRTS's override of a shape (formats/shape_override.hpp): the file
// beside it, `<shape file>.openrts`.
[[nodiscard]] core::VirtualPath shape_override_path(const core::VirtualPath& shape);
// The inspector's overrides held for this run: they win over the file (it
// is written to a mod that may only be mounted at the next start).
// std::nullopt drops the one held.
void hold_shape_override(const core::VirtualPath& shape, std::optional<formats::ShapeOverride> data);
// The override for `shape`: the one held, else its file, else none.
[[nodiscard]] formats::ShapeOverride find_shape_override(const core::Vfs& vfs, const core::VirtualPath& shape);

// openRTS's materials of a shape (formats/material.hpp): the file beside it,
// `<shape file>.material.json5`.
[[nodiscard]] core::VirtualPath shape_material_path(const core::VirtualPath& shape);
// The inspector's materials held for this run, as hold_shape_override.
void hold_shape_material(const core::VirtualPath& shape, std::optional<formats::ShapeMaterialFile> data);
// The shape's material entries: the inspector's held ones, else its file's
// (the last one read well when it now has an error), else none. `held`:
// whether they are the inspector's.
[[nodiscard]] formats::ShapeMaterialFile find_shape_material(const core::Vfs& vfs, const core::VirtualPath& shape, bool* held = nullptr);
// RTS's material of a primitive by its light material (renderer/pbr.hpp).
[[nodiscard]] formats::Material rts_material(std::int32_t light_material);
// MikkTSpace tangents (xyz and the bitangent's sign) of the vertices of
// `model`'s primitive states marked in `normal_mapped`, with the texture
// coordinates' v up (1 - v), as Blender and Substance Painter bake: the
// bitangent points up the image, as a normal map's green. A vertex whose
// corners need different tangents (a mirrored UV seam) is copied (`split`
// copies appended to model.vertices, the indices moved to them). Triangles
// without a texture area get none (`degenerate`); a vertex without one has 0.
struct MaterialTangents {
    std::vector<std::array<float, 4>> tangents; // per vertex, the copies' too; empty: none made
    std::size_t degenerate = 0;
    std::size_t split = 0;
    bool failed = false;
};
[[nodiscard]] MaterialTangents make_material_tangents(formats::ShapeModel& model, const std::vector<bool>& normal_mapped);

// The primitives' materials of `asset` (a shape's, loaded): RTS's, then the
// texture's file (and its night or season variant's), then the shape's
// entries; their maps decoded and repacked (through `cache`), the tangents
// for normal maps. Nothing when no file says anything.
void load_model_materials(const core::Vfs& vfs, StreamedModelAsset& asset, TextureDecodeCache* cache = nullptr);

// Worker-thread job: parses the shape and decodes every texture it uses.
// `shape` may be an RTS .repaint: the Shape and ShapeDescription it names,
// with its TextureReplace images; the model keeps the repaint's path.
[[nodiscard]] StreamedModelAsset load_model_asset(const core::Vfs& vfs,
                                                  const core::VirtualPath& shape,
                                                  const std::vector<core::VirtualPath>& texture_directories,
                                                  TextureDecodeCache* cache = nullptr,
                                                  const TextureVariant& variant = {});

} // namespace openrts::renderer
