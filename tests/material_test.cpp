// openRTS's materials (PBR stage 4): the JSON5 reader (core/json5.hpp), the
// material files, their merging field by field and the shape files'
// selectors (formats/material.hpp).
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include <openrts/core/json5.hpp>
#include <openrts/core/vfs.hpp>
#include <openrts/formats/material.hpp>
#include <openrts/formats/png_file.hpp>
#include <openrts/formats/shape_binary.hpp>
#include <openrts/renderer/model_assets.hpp>

using namespace openrts;

namespace {

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

void json5_checks() {
    namespace json5 = core::json5;
    const auto parsed = json5::parse(R"(// a comment
{
  unquoted: 'single',  /* block */
  "quoted": "a\"b\\cA",
  hex: 0x1F, lead: .5, trail: 2., plus: +3, exp: 1e2,
  list: [1, 2, 3,],
  nested: { yes: true, no: false, nothing: null, },
  line: 'one \
two',
}
)");
    check(parsed.ok(), "JSON5 parses: " + parsed.error);
    if (!parsed.ok()) return;
    const auto& root = *parsed.value;
    check(root.is_object() && root.members.size() == 10, "ten members");
    check(root.find("unquoted") && root.find("unquoted")->string == "single", "an unquoted key, a single-quoted string");
    check(root.find("quoted") && root.find("quoted")->string == "a\"b\\cA", "escapes");
    check(root.find("hex") && root.find("hex")->number == 31.0, "hexadecimal");
    check(root.find("lead") && root.find("lead")->number == 0.5, "a leading point");
    check(root.find("trail") && root.find("trail")->number == 2.0, "a trailing point");
    check(root.find("plus") && root.find("plus")->number == 3.0, "a plus sign");
    check(root.find("exp") && root.find("exp")->number == 100.0, "an exponent");
    check(root.find("list") && root.find("list")->items.size() == 3, "a comma after the last item");
    const auto* nested = root.find("nested");
    check(nested && nested->find("yes")->boolean && !nested->find("no")->boolean && nested->find("nothing")->is_null(), "true, false, null");
    check(root.find("line") && root.find("line")->string == "one two", "a string continued on the next line");
    check(root.find("hex")->line == 5 && root.find("hex")->column == 8, "where a value begins (" + root.find("hex")->where() + ")");

    const auto broken = json5::parse("{\n  a: 1,\n  b: ,\n}");
    check(!broken.ok() && broken.line == 3, "an error says its line (" + broken.error + ")");
    check(!json5::parse("{ a: 1 } x").ok(), "nothing after the value");
    check(!json5::parse("{ a: 'open").ok(), "an unclosed string");
    check(!json5::parse("/* open").ok(), "an unclosed comment");
    check(json5::number(0.5) == "0.5" && json5::number(3.0) == "3" && json5::number(0.1) == "0.1" && json5::number(0.1f, true) == "0.1", "numbers written short");
    check(json5::quote("a\"b") == "\"a\\\"b\"", "quotes written");
}

void material_checks() {
    using namespace formats;
    const auto file = parse_material(R"({
  format: "openrts-material/1",
  orm: "body_orm.png",
  layout: "mrao",
  normal: "body_n.png", normal_y: "down",
  roughness: 0.8,
  metallic: 2,          // out of range: left out
  colour: 1,            // unknown
  emissive: "body_e.png", emissive_strength: 4,
})", "TRAINS/TRAINSET/LOCO/body.material.json5");
    check(file.ok(), "a material file reads: " + file.error);
    check(file.patch.orm && file.patch.orm->path == "body_orm.png" && file.patch.orm->from == "TRAINS/TRAINSET/LOCO/body.material.json5",
          "a map and its file");
    check(file.patch.layout == OrmLayout::mrao && file.patch.normal_y == NormalY::down, "layout and normal_y");
    check(file.patch.roughness == 0.8f && !file.patch.metallic, "a value out of range left out");
    check(file.warnings.size() == 2, "two warnings (" + std::to_string(file.warnings.size()) + ")");
    check(!parse_material("{ format: 'openrts-material/2' }", "x").ok(), "another major version is not taken");
    check(parse_material("{ format: 'openrts-material/1.3' }", "x").ok(), "a minor version is");
    check(!parse_material("{ roughness: 1 }", "x").ok(), "no format: not taken");
    check(!parse_material("{ format: 'openrts-material/1', orm: '/abs.png' }", "x").patch.orm, "an absolute map path is not taken");

    // Merging: a new map without its partner resets the partner; numbers
    // never drop a map; a new orm drops single maps below it.
    MaterialPatch below{.orm = MaterialMap{"a.png", "x"}, .layout = OrmLayout::mrao, .roughness_map = MaterialMap{"r.png", "x"},
                        .normal = MaterialMap{"n.png", "x"}, .normal_y = NormalY::down};
    MaterialPatch numbers{.roughness = 0.3f, .metallic = 0.0f};
    auto merged = below;
    merged.merge(numbers);
    check(merged.orm && merged.roughness_map && merged.normal && merged.roughness == 0.3f, "numbers keep the maps");
    MaterialPatch new_maps{.orm = MaterialMap{"b.png", "y"}, .normal = MaterialMap{"m.png", "y"}};
    merged.merge(new_maps);
    check(merged.layout == OrmLayout::orm, "a new orm without layout: orm, not the one below");
    check(merged.normal_y == NormalY::up, "a new normal without normal_y: up");
    check(!merged.roughness_map, "a new orm drops the single maps below");

    // Resolving over RTS's: a map makes RTS's numbers 1, set numbers stay.
    const Material rts{.roughness = 0.5f, .metallic = 0.0f, .specular = 0.1f};
    const MaterialPatch with_orm{.orm = MaterialMap{"o.png", "x"}};
    const auto m = resolve_material(rts, std::span<const MaterialPatch>(&with_orm, 1));
    check(m.roughness == 1.0f && m.metallic == 1.0f && m.specular == 1.0f, "a map takes over RTS's numbers (glTF factors)");
    const MaterialPatch layers[] = {with_orm, MaterialPatch{.roughness = 0.6f}};
    const auto m2 = resolve_material(rts, layers);
    check(m2.roughness == 0.6f && m2.metallic == 1.0f, "a number set over the map is its factor");
    const auto m3 = resolve_material(rts, std::span<const MaterialPatch>(&numbers, 1));
    check(m3.roughness == 0.3f && m3.specular == 0.1f, "without maps RTS's other numbers stay");
    const MaterialPatch glow{.emissive = MaterialMap{"e.png", "x"}};
    check(resolve_material(rts, std::span<const MaterialPatch>(&glow, 1)).emissive_factor[0] == 1.0f, "an emissive map: factor 1");
    check(resolve_material(rts, {}).emissive_factor[0] == 0.0f, "no emission by default");
    check(resolve_material(rts, {}) == rts, "no layers: RTS's material");

    check(texture_material_name("body.ace") == "body.material.json5", "body.ace -> body.material.json5");
    check(texture_material_name("TEXTURES/a.b/c") == "TEXTURES/a.b/c.material.json5", "a folder's dot is not the extension");
    check(shape_material_name("loco.s") == "loco.s.material.json5", "loco.s -> loco.s.material.json5");
}

void selector_checks() {
    using namespace formats;
    ShapeModel model;
    model.images = {"body.ace", "glass.ace"};
    model.textures = {{.image_index = 0}, {.image_index = 1}};
    model.shader_names = {"TexDiff", "BlendATexDiff"};
    model.matrices = {{.name = "MAIN"}, {.name = "DOOR"}};
    model.vertex_states = {{.matrix_index = 0, .light_material_index = -5}, {.matrix_index = 1, .light_material_index = -7}};
    model.primitive_states = {
        {.texture_index = 0, .texture_count = 1, .shader_index = 0, .vertex_state_index = 0, .name = "Body"},
        {.texture_index = 0, .texture_count = 1, .shader_index = 0, .vertex_state_index = 1, .name = "Body"},
        {.texture_index = 1, .texture_count = 1, .shader_index = 1, .vertex_state_index = 1, .name = "Glass"},
    };
    const auto signatures = primitive_signatures(model);
    check(signatures.size() == 3 && signatures[1].matrix == "DOOR" && signatures[2].image == "glass.ace" && signatures[1].light_material == -7,
          "signatures: name, image, node, light material");
    const auto file = parse_shape_material(R"({
  format: "openrts-shape-material/1",
  primitives: [
    { match: { name: "glass" }, roughness: 0.05 },
    { match: { name: "Body" }, roughness: 0.4 },
    { match: { image: "BODY.ACE", matrix: "door" }, metallic: 1 },
    { match: { index: 0, image: "body.ace" }, roughness: 0.5 },
    { match: { image: "body.ace", all: true }, specular: 0.5 },
    { match: { name: "none" }, roughness: 1 },
    { roughness: 1 },
  ],
})", "SHAPES/loco.s.material.json5");
    check(file.ok() && file.primitives.size() == 6, "six entries (one without match left out)");
    std::vector<std::string> warnings;
    const auto chosen = select_primitives(signatures, file, warnings);
    check(chosen.size() == 6, "a choice per entry");
    if (chosen.size() == 6) {
        check(chosen[0] == std::vector<std::uint32_t>{2}, "by name, case not counted");
        check(chosen[1].empty(), "an ambiguous name: none");
        check(chosen[2] == std::vector<std::uint32_t>{1}, "by image and node");
        check(chosen[3] == std::vector<std::uint32_t>{0}, "by index with its image");
        check(chosen[4] == (std::vector<std::uint32_t>{0, 1}), "all: true takes every match");
        check(chosen[5].empty(), "matching nothing");
    }
    check(warnings.size() == 2, "warnings for the ambiguous and the missing (" + std::to_string(warnings.size()) + ")");
    check(stable_selector(signatures, 2) == PrimitiveSelector{.name = "Glass"}, "a unique name is the steadiest");
    check(stable_selector(signatures, 1) == (PrimitiveSelector{.image = "body.ace", .matrix = "DOOR"}), "else image and node");
    // Written and read back.
    ShapeMaterialFile out;
    out.primitives.push_back({.match = stable_selector(signatures, 1), .patch = {.roughness = 0.25f, .metallic = 1.0f}});
    const auto text = write_shape_material(out);
    const auto back = parse_shape_material(text, "x");
    check(back.ok() && back.primitives.size() == 1 && back.primitives[0].match == out.primitives[0].match
              && back.primitives[0].patch == out.primitives[0].patch,
          "written and read back the same:\n" + text);
}

// A quad in the xy plane facing +z, the texture's top (v 0) at its top: the
// tangent along +x (u), the bitangent up the image (+y), sign +1. With a
// second quad mirrored in u sharing an edge, the shared vertices are split.
formats::ShapeModel quad_model(const bool mirrored_half) {
    formats::ShapeModel model;
    model.images = {"body.ace"};
    model.textures = {{.image_index = 0}};
    model.shader_names = {"TexDiff"};
    model.matrices = {{.name = "MAIN"}};
    model.vertex_states = {{.matrix_index = 0, .light_material_index = -5}};
    model.primitive_states = {{.texture_index = 0, .texture_count = 1, .shader_index = 0, .vertex_state_index = 0, .name = "Body"}};
    const auto vertex = [](float x, float y, float u, float v) {
        formats::ShapeMeshVertex out;
        out.position[0] = x, out.position[1] = y, out.position[2] = 0.0f;
        out.normal[0] = 0.0f, out.normal[1] = 0.0f, out.normal[2] = 1.0f;
        out.uv[0] = u, out.uv[1] = v;
        return out;
    };
    model.vertices = {vertex(0, 0, 0, 1), vertex(1, 0, 1, 1), vertex(1, 1, 1, 0), vertex(0, 1, 0, 0)};
    model.indices = {0, 1, 2, 0, 2, 3};
    if (mirrored_half) {
        // A second quad to the right sharing the edge 1-2, its u running back.
        model.vertices.push_back(vertex(2, 0, 0, 1));
        model.vertices.push_back(vertex(2, 1, 0, 0));
        model.indices.insert(model.indices.end(), {1, 4, 5, 1, 5, 2});
    }
    model.draws = {{.first_index = 0, .index_count = static_cast<std::uint32_t>(model.indices.size()), .primitive_state_index = 0, .matrix_index = 0}};
    return model;
}

void tangent_checks() {
    auto model = quad_model(false);
    const auto made = renderer::make_material_tangents(model, {true});
    check(!made.failed && made.tangents.size() == 4, "a tangent per vertex");
    if (made.tangents.size() == 4) {
        const auto& t = made.tangents[0];
        check(std::abs(t[0] - 1.0f) < 1e-3f && std::abs(t[1]) < 1e-3f && t[3] == 1.0f,
              "tangent along +u, the bitangent up the image (" + std::to_string(t[0]) + ", " + std::to_string(t[1]) + ", w " + std::to_string(t[3]) + ")");
    }
    auto mirrored = quad_model(true);
    const auto vertices = mirrored.vertices.size();
    const auto split = renderer::make_material_tangents(mirrored, {true});
    check(split.split == 2 && mirrored.vertices.size() == vertices + 2 && split.tangents.size() == mirrored.vertices.size(),
          "the vertices of a mirrored seam are split (" + std::to_string(split.split) + ")");
    if (split.tangents.size() == mirrored.vertices.size()) {
        const auto right = mirrored.indices[6]; // the right quad's copy of vertex 1
        check(right != 1 && split.tangents[right][0] < -0.99f, "the mirrored half's tangent runs back along -x");
    }
    auto flat = quad_model(false);
    for (auto& v : flat.vertices) v.uv[0] = v.uv[1] = 0.5f;
    const auto none = renderer::make_material_tangents(flat, {true});
    check(none.degenerate == 2 && none.tangents.empty(), "triangles without a texture area get no tangents");
}

// The whole of a model's materials from files in a folder: the texture's
// file (mrao, a normal map green down), the shape's entry by name.
void loading_checks() {
    namespace fs = std::filesystem;
    const auto work = fs::temp_directory_path() / "openrts-material-test";
    fs::remove_all(work);
    fs::create_directories(work / "SHAPES");
    const auto write = [&](const std::string& name, const std::string& text) {
        std::ofstream(work / "SHAPES" / name, std::ios::binary) << text;
    };
    // 2 x 1 maps: mrao texels (metal 10, rough 20, ao 30), (40, 50, 60).
    write("body_mrao.png", formats::encode_png(2, 1, {10, 20, 30, 255, 40, 50, 60, 255}));
    write("body_n.png", formats::encode_png(2, 1, {128, 200, 255, 255, 128, 100, 255, 255}));
    write("body.material.json5", "{ format: \"openrts-material/1\", orm: \"body_mrao.png\", layout: \"mrao\", normal: \"body_n.png\", normal_y: \"down\", roughness: 0.5 }");
    write("loco.s.material.json5", "{ format: \"openrts-shape-material/1\", primitives: [ { match: { name: \"Body\" }, metallic: 0.25 } ] }");
    core::Vfs vfs;
    vfs.mount_directory(work, {});
    vfs.build_index();

    renderer::StreamedModelAsset asset;
    asset.source_path = *core::VirtualPath::parse("SHAPES/loco.s");
    asset.model = quad_model(false);
    asset.textures.push_back({.image_name = "body.ace", .source_path = core::VirtualPath::parse("SHAPES/body.ace")});
    renderer::load_model_materials(vfs, asset);
    check(asset.materials.size() == 1, "a material per primitive state");
    if (asset.materials.size() == 1) {
        const auto& m = asset.materials[0];
        check(m.layers.size() == 2 && m.layers[0].first == formats::MaterialLayer::texture && m.layers[1].first == formats::MaterialLayer::shape,
              "the texture's layer, then the shape's");
        check(m.material.roughness == 0.5f && m.material.metallic == 0.25f && m.material.specular == 1.0f,
              "numbers from both, RTS's specular given way to the map");
        check(m.orm >= 0 && m.normal >= 0, "an ORM and a normal map");
        if (m.orm >= 0) {
            const auto& image = asset.material_maps[static_cast<std::size_t>(m.orm)].image;
            check(image && image->ok() && image->image->pixels.size() == 8, "the ORM map made");
            if (image && image->ok()) {
                const auto& px = image->image->pixels;
                check(px[0] == 30 && px[1] == 20 && px[2] == 10 && px[4] == 60 && px[5] == 50 && px[6] == 40, "mrao repacked to ORM");
            }
        }
        if (m.normal >= 0) {
            const auto& image = asset.material_maps[static_cast<std::size_t>(m.normal)].image;
            check(image && image->ok() && image->image->pixels[1] == 55 && image->image->pixels[5] == 155, "a green-down normal map flipped");
        }
    }
    check(asset.tangents.size() == asset.model.vertices.size(), "tangents for the normal map");
    const auto keys = asset.dependency_keys();
    check(std::find(keys.begin(), keys.end(), core::VirtualPath::parse("SHAPES/body.material.json5")->key()) != keys.end()
              && std::find(keys.begin(), keys.end(), core::VirtualPath::parse("SHAPES/body_n.png")->key()) != keys.end(),
          "the material file and its maps are dependencies (hot reload)");

    // A broken file keeps its last good reading.
    write("body.material.json5", "{ format: \"openrts-material/1\", roughness: }");
    vfs.build_index();
    renderer::StreamedModelAsset again;
    again.source_path = asset.source_path;
    again.model = quad_model(false);
    again.textures = asset.textures;
    renderer::load_model_materials(vfs, again);
    check(again.materials.size() == 1 && again.materials[0].material.roughness == 0.5f, "a file with an error keeps its last good reading");
    fs::remove_all(work);
}

// The SDK's templates and the demo mod read without errors or warnings.
void sdk_checks() {
#ifdef OPENRTS_SOURCE_DIR
    namespace fs = std::filesystem;
    const fs::path folder = fs::path(OPENRTS_SOURCE_DIR) / "sdk" / "materials";
    std::size_t files = 0;
    for (const auto& entry : fs::recursive_directory_iterator(folder)) {
        const auto name = entry.path().filename().string();
        if (!entry.is_regular_file() || name.size() < 15 || name.substr(name.size() - 15) != ".material.json5") continue;
        std::ifstream in(entry.path(), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        ++files;
        if (name.size() > 17 && name.find(".s.material.json5") != std::string::npos) {
            const auto file = formats::parse_shape_material(text, name);
            check(file.ok() && file.warnings.empty() && !file.empty(), name + " reads cleanly: " + file.error + (file.warnings.empty() ? "" : file.warnings[0]));
        } else {
            const auto file = formats::parse_material(text, name);
            check(file.ok() && file.warnings.empty(), name + " reads cleanly: " + file.error + (file.warnings.empty() ? "" : file.warnings[0]));
        }
    }
    check(files >= 7, "the SDK's material files found (" + std::to_string(files) + ")");
#endif
}

} // namespace

int main() {
    json5_checks();
    material_checks();
    selector_checks();
    tangent_checks();
    loading_checks();
    sdk_checks();
    std::cout << (failures ? "FAILED" : "materials: all checks passed") << '\n';
    return failures ? 1 : 0;
}
