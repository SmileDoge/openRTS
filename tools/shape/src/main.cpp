// openrts-shape: MSTS / RTS shapes for artists (PBR stage 4, docs/pbr-materials.md).
//
//   openrts-shape export <file.s> [--out <folder>]
//
// The shape's nearest distance level as glTF 2.0 (<name>.gltf, <name>.bin and
// its textures as PNG) to paint in Substance Painter or Blender: a node per
// matrix with its name and hierarchy, a mesh per node, a material per
// primitive state (named by its label in the shape) with the texture as its
// base colour, and in each material's extras.openrts what openRTS's material
// files select it by (the shape, the primitive state's index, label, image,
// node, shader and light material, and the steadiest `match`). The textures
// are found as the game finds them (beside the shape, its TEXTURES, the
// parent's TEXTURES) in the installation the shape is in.
//
// MSTS shapes are left-handed (x right, y up, z forward), glTF right-handed:
// z is negated and the triangles' order reversed. The texture coordinates
// are as they are: both put (0, 0) at the image's top left.

#include <algorithm>
#include <chrono>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <openrts/core/installation.hpp>
#include <openrts/core/json5.hpp>
#include <openrts/core/vfs.hpp>
#include <openrts/formats/ace_file.hpp>
#include <openrts/formats/material.hpp>
#include <openrts/formats/png_file.hpp>
#include <openrts/formats/shape_binary.hpp>
#include <openrts/formats/shape_file.hpp>
#include <openrts/formats/shape_override.hpp>

namespace {

namespace fs = std::filesystem;
namespace json5 = openrts::core::json5;
using namespace openrts;

[[nodiscard]] std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

[[nodiscard]] fs::path from_utf8(const std::string_view text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

void usage() {
    std::cout << "openrts-shape export <file.s> [--out <folder>]\n"
                 "  The shape's nearest distance level as glTF 2.0 (.gltf, .bin, PNG textures) for Substance Painter\n"
                 "  and Blender: a material per primitive state, what openRTS's material files select it by in its\n"
                 "  extras (docs/pbr-materials.md).\n";
}

// Where an image of a shape is: beside it, its TEXTURES, its parent's TEXTURES.
[[nodiscard]] std::optional<core::VirtualPath> find_texture(const core::Vfs& vfs, const core::VirtualPath& shape, const std::string& image) {
    auto name = image;
    std::replace(name.begin(), name.end(), '\\', '/');
    const auto folder = shape.parent();
    std::vector<std::optional<core::VirtualPath>> candidates{folder.resolve(name)};
    if (const auto textures = folder.resolve(std::string_view("TEXTURES"))) candidates.push_back(textures->resolve(name));
    if (const auto textures = folder.parent().resolve(std::string_view("TEXTURES"))) candidates.push_back(textures->resolve(name));
    for (const auto& candidate : candidates) {
        if (candidate && vfs.exists(*candidate)) return candidate;
    }
    return std::nullopt;
}

// The glTF file being written: its JSON parts and its binary buffer.
struct Gltf {
    std::string buffer;
    std::vector<std::string> buffer_views, accessors, meshes, nodes, materials, textures, images;

    std::size_t view(const void* data, const std::size_t bytes, const int target) {
        while (buffer.size() % 4 != 0) buffer.push_back('\0');
        const auto offset = buffer.size();
        buffer.append(static_cast<const char*>(data), bytes);
        std::ostringstream out;
        out << "{\"buffer\":0,\"byteOffset\":" << offset << ",\"byteLength\":" << bytes;
        if (target) out << ",\"target\":" << target;
        out << "}";
        buffer_views.push_back(out.str());
        return buffer_views.size() - 1;
    }
    std::size_t accessor(const std::size_t view, const std::size_t count, const char* type, const int component, const std::string& bounds = {}) {
        std::ostringstream out;
        out << "{\"bufferView\":" << view << ",\"componentType\":" << component << ",\"count\":" << count << ",\"type\":\"" << type << "\""
            << bounds << "}";
        accessors.push_back(out.str());
        return accessors.size() - 1;
    }
};

[[nodiscard]] std::string join(const std::vector<std::string>& items) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) out += (i ? "," : "") + items[i];
    return out;
}

[[nodiscard]] std::string num(const double value) {
    return json5::number(value, true);
}

// RTS's defaults for the material's numbers (renderer/pbr.hpp), a starting point.
[[nodiscard]] float rts_roughness(const std::int32_t light_material) {
    if (light_material == -6) return 0.5f;
    if (light_material == -7) return 0.35f;
    return 1.0f;
}

// What the export is doing, as it goes (testers 2026-11-22: a big shape
// took a minute without a word and looked hung): each step with the
// seconds since the start.
class Progress {
public:
    void step(const std::string& what) const {
        const std::chrono::duration<double> since = std::chrono::steady_clock::now() - start_;
        char time[32];
        std::snprintf(time, sizeof time, "[%5.1f s] ", since.count());
        std::cout << time << what << std::endl;
    }

private:
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

int export_shape(const fs::path& file, fs::path out_folder) {
    const Progress progress;
    std::error_code error;
    const auto absolute = fs::absolute(file, error);
    if (!fs::is_regular_file(absolute, error)) {
        std::cerr << "openrts-shape: no file " << utf8(file) << '\n';
        return 1;
    }
    // The installation the shape is in, as the game mounts it; else its
    // folder's parent (TEXTURES beside the shape's folder).
    core::Vfs vfs;
    fs::path root;
    if (const auto installation = core::find_installation_root(absolute.parent_path())) {
        root = *installation;
        progress.step("Reading the installation's file list: " + utf8(root));
        core::mount_installation(vfs, root, core::load_installation_config(root));
    } else {
        progress.step("Not in an installation: textures from " + utf8(absolute.parent_path().parent_path()));
        root = absolute.parent_path().parent_path();
        vfs.mount_directory(root, {});
        vfs.build_index();
    }
    const auto relative = fs::relative(absolute, root, error);
    const auto shape = core::VirtualPath::parse(utf8(relative.generic_u8string() == u8"" ? absolute.filename() : relative));
    if (!shape) {
        std::cerr << "openrts-shape: " << utf8(absolute) << " is not inside " << utf8(root) << '\n';
        return 1;
    }
    progress.step("Reading the shape " + shape->str());
    const auto body = formats::load_binary_shape_body(vfs, *shape);
    if (!body.ok()) {
        std::cerr << "openrts-shape: " << shape->str() << ": " << body.error << '\n';
        return 1;
    }
    const auto model = formats::parse_binary_shape_model(*body.body);
    if (!model.ok()) {
        std::cerr << "openrts-shape: " << shape->str() << ": " << model.error << '\n';
        return 1;
    }
    progress.step("The shape read: " + std::to_string(model.images.size()) + " images");
    if (out_folder.empty()) out_folder = absolute.parent_path() / (absolute.stem().u8string() + u8"_gltf");
    fs::create_directories(out_folder, error);
    if (error) {
        std::cerr << "openrts-shape: cannot make " << utf8(out_folder) << '\n';
        return 1;
    }
    const auto name = utf8(absolute.stem());
    Gltf gltf;
    const auto signatures = formats::primitive_signatures(model);

    // Images: each texture once, as PNG.
    std::map<std::uint32_t, std::size_t> image_texture; // shape image -> glTF texture
    const auto texture_of = [&](const std::uint32_t image) -> std::optional<std::size_t> {
        if (const auto it = image_texture.find(image); it != image_texture.end()) return it->second;
        if (image >= model.images.size()) return std::nullopt;
        const auto image_name = model.images[image].generic_string();
        const auto path = find_texture(vfs, *shape, image_name);
        if (!path) {
            std::cerr << "  texture not found: " << image_name << '\n';
            return std::nullopt;
        }
        progress.step("Texture " + std::to_string(image_texture.size() + 1) + ": " + path->str());
        const auto decoded = formats::decode_ace_rgba(vfs, *path);
        if (!decoded.ok()) {
            std::cerr << "  texture " << path->str() << ": " << decoded.error << '\n';
            return std::nullopt;
        }
        const auto png_name = utf8(from_utf8(path->filename()).stem()) + ".png";
        std::ofstream(out_folder / from_utf8(png_name), std::ios::binary)
            << formats::encode_png(decoded.image->width, decoded.image->height, decoded.image->pixels);
        gltf.images.push_back("{\"uri\":" + json5::quote(png_name) + ",\"name\":" + json5::quote(path->filename()) + "}");
        gltf.textures.push_back("{\"sampler\":0,\"source\":" + std::to_string(gltf.images.size() - 1) + "}");
        image_texture[image] = gltf.textures.size() - 1;
        return gltf.textures.size() - 1;
    };

    // Materials: one per primitive state drawn at the nearest level.
    std::map<std::uint32_t, std::size_t> state_material;
    const auto material_of = [&](const std::uint32_t index) -> std::size_t {
        if (const auto it = state_material.find(index); it != state_material.end()) return it->second;
        const auto& state = model.primitive_states[index];
        const auto& signature = signatures[index];
        std::ostringstream out;
        const auto label = signature.name.empty() ? "prim_" + std::to_string(index) + (signature.image.empty() ? "" : "_" + signature.image)
                                                  : signature.name;
        out << "{\"name\":" << json5::quote(label) << ",\"pbrMetallicRoughness\":{";
        bool textured = false;
        if (state.texture_count > 0 && state.texture_index < model.textures.size()) {
            if (const auto texture = texture_of(model.textures[state.texture_index].image_index)) {
                out << "\"baseColorTexture\":{\"index\":" << *texture << "},";
                textured = true;
            }
        }
        (void)textured;
        out << "\"metallicFactor\":0,\"roughnessFactor\":" << num(rts_roughness(signature.light_material)) << "}";
        const bool blend = signature.shader.find("Blend") != std::string::npos;
        if (state.alpha_test_mode != 0) {
            out << ",\"alphaMode\":\"MASK\",\"alphaCutoff\":0.5";
        } else if (blend) {
            out << ",\"alphaMode\":\"BLEND\"";
        }
        // What openRTS's material files select it by.
        const auto match = formats::stable_selector(signatures, index);
        std::string selector;
        if (match.name) selector += (selector.empty() ? "" : ",") + std::string("\"name\":") + json5::quote(*match.name);
        if (match.image) selector += (selector.empty() ? "" : ",") + std::string("\"image\":") + json5::quote(*match.image);
        if (match.matrix) selector += (selector.empty() ? "" : ",") + std::string("\"matrix\":") + json5::quote(*match.matrix);
        if (match.index) selector += (selector.empty() ? "" : ",") + std::string("\"index\":") + std::to_string(*match.index);
        out << ",\"extras\":{\"openrts\":{\"shape\":" << json5::quote(shape->str()) << ",\"index\":" << index << ",\"name\":" << json5::quote(signature.name)
            << ",\"image\":" << json5::quote(signature.image) << ",\"matrix\":" << json5::quote(signature.matrix) << ",\"shader\":"
            << json5::quote(signature.shader) << ",\"light_material\":" << signature.light_material << ",\"match\":{" << selector << "}}}}";
        gltf.materials.push_back(out.str());
        state_material[index] = gltf.materials.size() - 1;
        return gltf.materials.size() - 1;
    };

    // Meshes: the nearest level's primitives, by the node they hang on; each
    // primitive with its own vertices (those its triangles use).
    std::map<std::int32_t, std::vector<std::string>> node_primitives;
    std::size_t triangles = 0;
    for (const auto& draw : model.draws) {
        if (draw.level != 0 || draw.primitive_state_index >= model.primitive_states.size()) continue;
        const auto end = std::min<std::size_t>(std::size_t{draw.first_index} + draw.index_count, model.indices.size());
        std::map<std::uint32_t, std::uint32_t> local;
        std::vector<float> positions, normals, uvs;
        std::vector<std::uint32_t> indices;
        std::array<float, 3> low{1e30f, 1e30f, 1e30f}, high{-1e30f, -1e30f, -1e30f};
        for (std::size_t first = draw.first_index; first + 3 <= end; first += 3) {
            // Reversed: (a, c, b), z negated below.
            for (const std::size_t corner : {first, first + 2, first + 1}) {
                const auto vertex = model.indices[corner];
                if (vertex >= model.vertices.size()) continue;
                auto [it, added] = local.emplace(vertex, static_cast<std::uint32_t>(local.size()));
                if (added) {
                    const auto& v = model.vertices[vertex];
                    const std::array<float, 3> p{v.position[0], v.position[1], -v.position[2]};
                    for (int k = 0; k < 3; ++k) {
                        positions.push_back(p[static_cast<std::size_t>(k)]);
                        low[static_cast<std::size_t>(k)] = std::min(low[static_cast<std::size_t>(k)], p[static_cast<std::size_t>(k)]);
                        high[static_cast<std::size_t>(k)] = std::max(high[static_cast<std::size_t>(k)], p[static_cast<std::size_t>(k)]);
                    }
                    std::array<float, 3> n{v.normal[0], v.normal[1], -v.normal[2]};
                    const auto length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                    if (length > 1e-6f) {
                        for (auto& c : n) c /= length;
                    } else {
                        n = {0.0f, 1.0f, 0.0f};
                    }
                    normals.insert(normals.end(), n.begin(), n.end());
                    uvs.push_back(v.uv[0]);
                    uvs.push_back(v.uv[1]);
                }
                indices.push_back(it->second);
            }
        }
        if (indices.empty()) continue;
        triangles += indices.size() / 3;
        const auto count = positions.size() / 3;
        const std::string bounds = ",\"min\":[" + num(low[0]) + "," + num(low[1]) + "," + num(low[2]) + "],\"max\":[" + num(high[0]) + ","
            + num(high[1]) + "," + num(high[2]) + "]";
        const auto position = gltf.accessor(gltf.view(positions.data(), positions.size() * 4, 34962), count, "VEC3", 5126, bounds);
        const auto normal = gltf.accessor(gltf.view(normals.data(), normals.size() * 4, 34962), count, "VEC3", 5126);
        const auto uv = gltf.accessor(gltf.view(uvs.data(), uvs.size() * 4, 34962), count, "VEC2", 5126);
        const auto index = gltf.accessor(gltf.view(indices.data(), indices.size() * 4, 34963), indices.size(), "SCALAR", 5125);
        std::ostringstream out;
        out << "{\"attributes\":{\"POSITION\":" << position << ",\"NORMAL\":" << normal << ",\"TEXCOORD_0\":" << uv << "},\"indices\":" << index
            << ",\"material\":" << material_of(draw.primitive_state_index) << "}";
        node_primitives[draw.matrix_index].push_back(out.str());
    }

    // Nodes: a node per matrix, z mirrored (S M S, S = diag(1, 1, -1)).
    std::vector<std::vector<std::size_t>> children(model.matrices.size());
    std::vector<std::size_t> roots;
    for (std::size_t i = 0; i < model.matrices.size(); ++i) {
        const auto parent = i < model.hierarchy.size() ? model.hierarchy[i] : -1;
        if (parent >= 0 && static_cast<std::size_t>(parent) < model.matrices.size() && static_cast<std::size_t>(parent) != i) {
            children[static_cast<std::size_t>(parent)].push_back(i);
        } else {
            roots.push_back(i);
        }
    }
    for (std::size_t i = 0; i < model.matrices.size(); ++i) {
        const auto& v = model.matrices[i].values;
        // Column-major: columns (v0 v1 v2), (v3 v4 v5), (v6 v7 v8), (v9 v10 v11).
        float m[16] = {v[0], v[1], v[2], 0, v[3], v[4], v[5], 0, v[6], v[7], v[8], 0, v[9], v[10], v[11], 1};
        const float s[4] = {1, 1, -1, 1};
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) m[c * 4 + r] *= s[r] * s[c];
        }
        std::ostringstream out;
        out << "{\"name\":" << json5::quote(model.matrices[i].name.empty() ? "node_" + std::to_string(i) : model.matrices[i].name) << ",\"matrix\":[";
        for (int k = 0; k < 16; ++k) out << (k ? "," : "") << num(m[k]);
        out << "]";
        if (const auto it = node_primitives.find(static_cast<std::int32_t>(i)); it != node_primitives.end()) {
            gltf.meshes.push_back("{\"name\":" + json5::quote(model.matrices[i].name) + ",\"primitives\":[" + join(it->second) + "]}");
            out << ",\"mesh\":" << gltf.meshes.size() - 1;
        }
        if (!children[i].empty()) {
            out << ",\"children\":[";
            for (std::size_t k = 0; k < children[i].size(); ++k) out << (k ? "," : "") << children[i][k];
            out << "]";
        }
        out << "}";
        gltf.nodes.push_back(out.str());
    }
    // Primitives on no matrix: a node of their own.
    if (const auto it = node_primitives.find(-1); it != node_primitives.end()) {
        gltf.meshes.push_back("{\"name\":\"unattached\",\"primitives\":[" + join(it->second) + "]}");
        gltf.nodes.push_back("{\"name\":\"unattached\",\"mesh\":" + std::to_string(gltf.meshes.size() - 1) + "}");
        roots.push_back(gltf.nodes.size() - 1);
    }

    const auto bin_name = name + ".bin";
    std::ofstream(out_folder / from_utf8(bin_name), std::ios::binary) << gltf.buffer;
    std::ostringstream json;
    json << "{\"asset\":{\"version\":\"2.0\",\"generator\":\"openRTS openrts-shape\"},\"scene\":0,\"scenes\":[{\"name\":" << json5::quote(name)
         << ",\"nodes\":[";
    for (std::size_t k = 0; k < roots.size(); ++k) json << (k ? "," : "") << roots[k];
    json << "]}],\"nodes\":[" << join(gltf.nodes) << "],\"meshes\":[" << join(gltf.meshes) << "],\"materials\":[" << join(gltf.materials)
         << "],\"textures\":[" << join(gltf.textures) << "],\"images\":[" << join(gltf.images)
         << "],\"samplers\":[{\"magFilter\":9729,\"minFilter\":9987,\"wrapS\":10497,\"wrapT\":10497}],\"accessors\":[" << join(gltf.accessors)
         << "],\"bufferViews\":[" << join(gltf.buffer_views) << "],\"buffers\":[{\"uri\":" << json5::quote(bin_name)
         << ",\"byteLength\":" << gltf.buffer.size() << "}]}\n";
    progress.step("Writing " + name + ".gltf and " + bin_name);
    std::ofstream(out_folder / from_utf8(name + ".gltf"), std::ios::binary) << json.str();
    std::cout << shape->str() << " -> " << utf8(out_folder / from_utf8(name + ".gltf")) << ": " << gltf.nodes.size() << " nodes, " << gltf.meshes.size()
              << " meshes, " << gltf.materials.size() << " materials, " << gltf.images.size() << " textures, " << triangles << " triangles\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        usage();
        return args.empty() ? 1 : 0;
    }
    if (args[0] != "export" || args.size() < 2) {
        usage();
        return 1;
    }
    fs::path out;
    for (std::size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--out" && i + 1 < args.size()) {
            out = from_utf8(args[++i]);
        } else {
            std::cerr << "openrts-shape: unknown option " << args[i] << '\n';
            return 1;
        }
    }
    return export_shape(from_utf8(args[1]), out);
}
