#pragma once

// openRTS's own materials (PBR stage 4, docs/pbr-materials.md): what a
// surface is for the light beyond what RTS's data says. JSON5 files
// (core/json5.hpp) in the VFS, so mods and packs add them without touching
// the originals:
//
//  - beside a texture, for every model drawing it: `body.ace` ->
//    `body.material.json5`, format "openrts-material/1";
//  - beside a shape, for some of its primitives: `<shape file>.material.json5`
//    (`loco.s.material.json5`), format "openrts-shape-material/1", entries
//    selecting primitives by name, image and node, or index and image.
//
// A primitive's material is RTS's (its light material, renderer/pbr.hpp),
// then the texture's file, then the shape's entries, then the inspector's
// changes not saved yet, merged field by field (MaterialPatch: every field
// optional). The parameters are glTF's (metallic-roughness,
// KHR_materials_specular, KHR_materials_emissive_strength).

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace openrts::formats {

struct ShapeModel;

// How a packed map holds ambient occlusion, roughness and metallic:
//  - orm: R occlusion, G roughness, B metallic (glTF, Blender, Substance's
//    "glTF PBR Metal Roughness"; openRTS's own);
//  - mrao: R metallic, G roughness, B occlusion;
//  - hdrp_mask: Unity HDRP's mask, R metallic, G occlusion, B detail mask,
//    A smoothness (roughness = 1 - smoothness).
// Whatever the file's, it is repacked to ORM when loaded.
enum class OrmLayout : std::uint8_t { orm, mrao, hdrp_mask };
// A normal map's green: up (OpenGL, glTF, Blender) or down (DirectX,
// Unreal; flipped when loaded).
enum class NormalY : std::uint8_t { up, down };
enum class AlphaMode : std::uint8_t { opaque, mask, blend };

[[nodiscard]] const char* layout_name(OrmLayout layout) noexcept;
[[nodiscard]] const char* normal_y_name(NormalY y) noexcept;

// A map a material file names: as written, and the file that named it (its
// folder resolves the path).
struct MaterialMap {
    std::string path; // as written, '/' or '\'
    std::string from; // the material file's virtual path
    bool operator==(const MaterialMap&) const = default;
};

// Some of a material's fields; the ones not set come from the layer below.
struct MaterialPatch {
    std::optional<std::array<float, 4>> base_color_factor; // times the texture, linear
    std::optional<MaterialMap> base_color;                 // instead of the shape's own texture
    std::optional<MaterialMap> orm;                        // packed occlusion, roughness, metallic
    std::optional<OrmLayout> layout;                       // orm's; with a new `orm` alone: orm
    std::optional<MaterialMap> ao_map, roughness_map, metallic_map; // one each (their red), over orm's channel
    std::optional<float> roughness;   // perceptual, 0..1; times the map's
    std::optional<float> metallic;    // 0..1; times the map's
    std::optional<float> reflectance; // a dielectric's F0, 0..0.16 (0.04: IOR 1.5)
    std::optional<float> specular;    // KHR_materials_specular's factor, 0..1
    std::optional<float> ao_strength; // 0..1: how much the map's occlusion darkens
    std::optional<MaterialMap> normal;
    std::optional<NormalY> normal_y;  // normal's; with a new `normal` alone: up
    std::optional<float> normal_strength; // 0..2
    std::optional<MaterialMap> emissive;
    std::optional<std::array<float, 3>> emissive_factor; // linear
    std::optional<float> emissive_strength;              // 0..100
    std::optional<AlphaMode> alpha_mode;  // read, not applied yet
    std::optional<float> alpha_cutoff;    // read, not applied yet
    std::optional<bool> double_sided;     // read, not applied yet
    std::optional<bool> wet;              // read, not applied yet (rain, PBR's wetness)

    [[nodiscard]] bool empty() const noexcept;
    // `over`'s fields over these. A new map without its partner resets it to
    // the default (orm without layout: orm; normal without normal_y: up), so
    // the partner of another map is never carried over; a new `orm` drops the
    // single maps below it. Numbers never drop a map.
    void merge(const MaterialPatch& over);
    bool operator==(const MaterialPatch&) const = default;
};

// Where a field of a primitive's material came from.
enum class MaterialLayer : std::uint8_t { rts, texture, texture_variant, shape, inspector };
[[nodiscard]] const char* layer_name(MaterialLayer layer) noexcept;

// A primitive's material, every field set.
struct Material {
    std::array<float, 4> base_color_factor{1.0f, 1.0f, 1.0f, 1.0f};
    std::optional<MaterialMap> base_color;
    std::optional<MaterialMap> orm;
    OrmLayout layout = OrmLayout::orm;
    std::optional<MaterialMap> ao_map, roughness_map, metallic_map;
    float roughness = 1.0f;
    float metallic = 0.0f;
    float reflectance = 0.04f;
    float specular = 1.0f;
    float ao_strength = 1.0f;
    std::optional<MaterialMap> normal;
    NormalY normal_y = NormalY::up;
    float normal_strength = 1.0f;
    std::optional<MaterialMap> emissive;
    std::array<float, 3> emissive_factor{0.0f, 0.0f, 0.0f};
    float emissive_strength = 1.0f;
    bool wet = false;

    [[nodiscard]] bool packed_maps() const noexcept { return orm || ao_map || roughness_map || metallic_map; }
    bool operator==(const Material&) const = default;
};

// `rts` (every field as RTS's data makes it) with the layers over it, in
// order. A layer bringing a roughness or metallic map makes the number a
// factor on it, as glTF has it: where the number is still RTS's, it becomes
// 1 (and RTS's specular factor 1: the map says how the surface shines); an
// emissive map with no factor set anywhere takes 1.
[[nodiscard]] Material resolve_material(const Material& rts, std::span<const MaterialPatch> layers);

// A material file (`body.material.json5`, "openrts-material/1").
struct MaterialFile {
    MaterialPatch patch;
    std::vector<std::string> warnings; // unknown keys, values out of range or of the wrong type (left out)
    std::string error;                 // the file is not taken
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};
// `file`: its virtual path, for the maps' `from` and the messages.
[[nodiscard]] MaterialFile parse_material(std::string_view text, std::string_view file);

// Which primitives of a shape an entry is for. Every field given has to
// match: `name` the primitive state's label in the shape, `image` its
// texture's file name (case does not count), `matrix` the node it hangs on,
// `index` its index among the shape's primitive states. Matching more than
// one applies to none (a warning names them) unless `all` says so.
struct PrimitiveSelector {
    std::optional<std::string> name;
    std::optional<std::string> image;
    std::optional<std::string> matrix;
    std::optional<std::uint32_t> index;
    bool all = false;
    bool operator==(const PrimitiveSelector&) const = default;
};

struct ShapeMaterialEntry {
    PrimitiveSelector match;
    MaterialPatch patch;
    int line = 0; // in its file
};

// A shape's material file (`<shape>.material.json5`, "openrts-shape-material/1").
struct ShapeMaterialFile {
    std::vector<ShapeMaterialEntry> primitives;
    std::vector<std::string> warnings;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
    [[nodiscard]] bool empty() const noexcept { return primitives.empty(); }
};
[[nodiscard]] ShapeMaterialFile parse_shape_material(std::string_view text, std::string_view file);
// As the inspector writes it (machine-written: its own file in a mod, no
// comments kept); maps named relative to `file`'s folder as they came.
[[nodiscard]] std::string write_shape_material(const ShapeMaterialFile& data);

// What identifies a primitive state of a shape.
struct PrimitiveSignature {
    std::uint32_t index{};
    std::string name;   // the primitive state's label
    std::string image;  // its texture's file name, lower case
    std::string shader;
    std::string matrix; // the node of its vertex state
    std::int32_t light_material = -5;
};
[[nodiscard]] std::vector<PrimitiveSignature> primitive_signatures(const ShapeModel& model);
// The primitive states each entry of `file` is for (none when it matches
// nothing, or several without `all`); `warnings` gets why.
[[nodiscard]] std::vector<std::vector<std::uint32_t>> select_primitives(std::span<const PrimitiveSignature> primitives,
                                                                        const ShapeMaterialFile& file, std::vector<std::string>& warnings);
// The steadiest selector of primitive `index` that matches it alone: its
// name if no other has it, else its image and node, else its index and image.
[[nodiscard]] PrimitiveSelector stable_selector(std::span<const PrimitiveSignature> primitives, std::uint32_t index);

// The material file beside a texture: `body.ace` -> `body.material.json5`
// (the same folder; the texture's extension dropped).
[[nodiscard]] std::string texture_material_name(std::string_view texture_file);
// The material file beside a shape: `loco.s` -> `loco.s.material.json5`.
[[nodiscard]] std::string shape_material_name(std::string_view shape_file);

} // namespace openrts::formats
