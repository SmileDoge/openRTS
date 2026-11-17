// openRTS's materials of a model (PBR stage 4, docs/pbr-materials.md): the
// layers over RTS's, their maps repacked for the GPU, MikkTSpace tangents.
#include <openrts/renderer/model_assets.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <mutex>
#include <set>

#include <mikktspace.h>
#include <openrts/core/vfs.hpp>
#include <openrts/formats/png_file.hpp>
#include <openrts/formats/shape_binary.hpp>
#include <openrts/renderer/pbr.hpp>

namespace openrts::renderer {

core::VirtualPath shape_material_path(const core::VirtualPath& shape) {
    return core::VirtualPath::parse(formats::shape_material_name(shape.str())).value_or(shape);
}

formats::Material rts_material(const std::int32_t light_material) {
    const auto surface = rts_surface_material(light_material);
    formats::Material m;
    m.roughness = surface.roughness;
    m.metallic = surface.metallic;
    m.reflectance = surface.reflectance;
    m.specular = surface.specular;
    return m;
}

namespace {

// The inspector's held entries, and the last file of each read well (a file
// with an error now keeps its last good reading, docs/pbr-materials.md).
struct Store {
    std::mutex mutex;
    std::map<std::string, formats::ShapeMaterialFile> held;
    std::map<std::string, formats::ShapeMaterialFile> shape_good;
    std::map<std::string, formats::MaterialFile> texture_good;
    std::set<std::string> told; // messages printed once
};

Store& store() {
    static Store s;
    return s;
}

void tell(const std::vector<std::string>& messages) {
    auto& s = store();
    const std::lock_guard lock(s.mutex);
    for (const auto& message : messages) {
        if (s.told.insert(message).second) std::cout << "material: " << message << '\n';
    }
}

// A texture's material file: its patch (none when there is no file, or it
// has an error and was never read well).
std::optional<formats::MaterialPatch> read_texture_material(const core::Vfs& vfs, const core::VirtualPath& file,
                                                            std::vector<std::string>& messages) {
    if (!vfs.exists(file)) return std::nullopt;
    const auto text = vfs.read(file);
    if (!text) {
        messages.push_back(file.str() + ": cannot be read");
        return std::nullopt;
    }
    auto parsed = formats::parse_material(*text, file.str());
    messages.insert(messages.end(), parsed.warnings.begin(), parsed.warnings.end());
    auto& s = store();
    const std::lock_guard lock(s.mutex);
    if (!parsed.ok()) {
        const auto it = s.texture_good.find(file.key());
        messages.push_back(parsed.error + (it != s.texture_good.end() ? " (its last good reading kept)" : ""));
        if (it == s.texture_good.end()) return std::nullopt;
        return it->second.patch;
    }
    s.texture_good[file.key()] = parsed;
    return parsed.patch;
}

[[nodiscard]] std::optional<core::VirtualPath> map_path(const formats::MaterialMap& map) {
    const auto from = core::VirtualPath::parse(map.from);
    if (!from) return std::nullopt;
    auto relative = map.path;
    std::replace(relative.begin(), relative.end(), '\\', '/');
    return from->parent().resolve(relative);
}

struct Rgba {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> pixels;
};

// A map's file as RGBA8: PNG or ACE.
std::optional<Rgba> decode_map(const core::Vfs& vfs, const core::VirtualPath& file, std::string& error) {
    const auto extension = core::fold_case(file.extension());
    if (!vfs.exists(file)) {
        error = file.str() + ": not found";
        return std::nullopt;
    }
    if (extension == ".png") {
        const auto bytes = vfs.read(file);
        if (!bytes) {
            error = file.str() + ": cannot be read";
            return std::nullopt;
        }
        auto png = formats::decode_png(*bytes);
        if (!png.ok()) {
            error = file.str() + ": " + png.error;
            return std::nullopt;
        }
        return Rgba{png.image->width, png.image->height, std::move(png.image->pixels)};
    }
    if (extension == ".ace") {
        auto ace = formats::decode_ace_rgba(vfs, file);
        if (!ace.ok()) {
            error = file.str() + ": " + ace.error;
            return std::nullopt;
        }
        return Rgba{ace.image->width, ace.image->height, std::move(ace.image->pixels)};
    }
    error = file.str() + ": a map is read from PNG or ACE (" + std::string(file.extension()) + " is not read yet)";
    return std::nullopt;
}

// Nearest-texel resampling to another size (maps of one material drawn at
// different sizes).
[[nodiscard]] Rgba resample(const Rgba& image, const std::uint32_t width, const std::uint32_t height) {
    if (image.width == width && image.height == height) return image;
    Rgba out{width, height, std::vector<std::uint8_t>(std::size_t{width} * height * 4)};
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto sy = std::min<std::uint32_t>(image.height - 1, static_cast<std::uint32_t>((y + 0.5) * image.height / height));
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto sx = std::min<std::uint32_t>(image.width - 1, static_cast<std::uint32_t>((x + 0.5) * image.width / width));
            std::copy_n(&image.pixels[(std::size_t{sy} * image.width + sx) * 4], 4, &out.pixels[(std::size_t{y} * width + x) * 4]);
        }
    }
    return out;
}

[[nodiscard]] std::shared_ptr<const formats::AceDecodeResult> finished(Rgba&& image) {
    auto result = std::make_shared<formats::AceDecodeResult>();
    formats::AceRgbaImage out;
    out.width = image.width;
    out.height = image.height;
    out.mips = formats::build_rgba_mipmaps(image.width, image.height, image.pixels);
    out.pixels = std::move(image.pixels);
    result->image = std::move(out);
    return result;
}

[[nodiscard]] std::shared_ptr<const formats::AceDecodeResult> failed(std::string error) {
    auto result = std::make_shared<formats::AceDecodeResult>();
    result->error = std::move(error);
    return result;
}

// One of a material's maps, made from its files.
std::shared_ptr<const formats::AceDecodeResult> build_map(const core::Vfs& vfs, const MaterialMapKind kind, const formats::Material& m) {
    std::string error;
    const auto one = [&](const formats::MaterialMap& map) -> std::optional<Rgba> {
        const auto file = map_path(map);
        if (!file) {
            error = map.from + ": \"" + map.path + "\" is not a path in the VFS";
            return std::nullopt;
        }
        return decode_map(vfs, *file, error);
    };
    switch (kind) {
    case MaterialMapKind::base_color:
    case MaterialMapKind::emissive: {
        auto image = one(kind == MaterialMapKind::base_color ? *m.base_color : *m.emissive);
        if (!image) return failed(error);
        return finished(std::move(*image));
    }
    case MaterialMapKind::normal: {
        auto image = one(*m.normal);
        if (!image) return failed(error);
        for (std::size_t i = 0; i < image->pixels.size(); i += 4) {
            if (m.normal_y == formats::NormalY::down) image->pixels[i + 1] = static_cast<std::uint8_t>(255 - image->pixels[i + 1]);
            image->pixels[i + 3] = 255;
        }
        return finished(std::move(*image));
    }
    case MaterialMapKind::orm: {
        std::optional<Rgba> packed, ao, roughness, metallic;
        if (m.orm && !(packed = one(*m.orm))) return failed(error);
        if (m.ao_map && !(ao = one(*m.ao_map))) return failed(error);
        if (m.roughness_map && !(roughness = one(*m.roughness_map))) return failed(error);
        if (m.metallic_map && !(metallic = one(*m.metallic_map))) return failed(error);
        std::uint32_t width = 0, height = 0;
        for (const auto* each : {&packed, &ao, &roughness, &metallic}) {
            if (*each) {
                width = std::max(width, (*each)->width);
                height = std::max(height, (*each)->height);
            }
        }
        if (width == 0 || height == 0) return failed("an ORM map without files");
        Rgba out{width, height, std::vector<std::uint8_t>(std::size_t{width} * height * 4, 255)};
        const auto texels = std::size_t{width} * height;
        if (packed) {
            const auto image = resample(*packed, width, height);
            for (std::size_t i = 0; i < texels; ++i) {
                const auto* in = &image.pixels[i * 4];
                auto* o = &out.pixels[i * 4];
                switch (m.layout) {
                case formats::OrmLayout::orm: o[0] = in[0], o[1] = in[1], o[2] = in[2]; break;
                case formats::OrmLayout::mrao: o[0] = in[2], o[1] = in[1], o[2] = in[0]; break;
                case formats::OrmLayout::hdrp_mask: o[0] = in[1], o[1] = static_cast<std::uint8_t>(255 - in[3]), o[2] = in[0]; break;
                }
            }
        }
        const auto channel = [&](const std::optional<Rgba>& single, const int target) {
            if (!single) return;
            const auto image = resample(*single, width, height);
            for (std::size_t i = 0; i < texels; ++i) out.pixels[i * 4 + target] = image.pixels[i * 4];
        };
        channel(ao, 0);
        channel(roughness, 1);
        channel(metallic, 2);
        return finished(std::move(out));
    }
    }
    return failed("unknown map");
}

[[nodiscard]] std::string map_key(const MaterialMapKind kind, const formats::Material& m) {
    const auto file = [](const std::optional<formats::MaterialMap>& map) {
        if (!map) return std::string("-");
        const auto path = map_path(*map);
        return path ? path->key() : map->path;
    };
    switch (kind) {
    case MaterialMapKind::base_color: return "material base " + file(m.base_color);
    case MaterialMapKind::emissive: return "material emissive " + file(m.emissive);
    case MaterialMapKind::normal: return std::string("material normal ") + formats::normal_y_name(m.normal_y) + " " + file(m.normal);
    case MaterialMapKind::orm:
        return std::string("material orm ") + formats::layout_name(m.layout) + " " + file(m.orm) + " " + file(m.ao_map) + " "
            + file(m.roughness_map) + " " + file(m.metallic_map);
    }
    return {};
}

// MikkTSpace over the triangles of the primitives with a normal map. Their
// texture coordinates with v up (1 - v), as Blender and Substance bake: the
// bitangent then points up the image, as a normal map's green.
struct Tangents {
    const formats::ShapeModel* model = nullptr;
    std::vector<std::uint32_t> faces;            // each triangle's first index in model->indices
    std::vector<std::array<float, 4>> corners;   // MikkTSpace's tangent of each corner (faces x 3)

    [[nodiscard]] const formats::ShapeMeshVertex& vertex(const int face, const int corner) const {
        return model->vertices[model->indices[faces[static_cast<std::size_t>(face)] + static_cast<std::uint32_t>(corner)]];
    }
};

int mikk_faces(const SMikkTSpaceContext* context) {
    return static_cast<int>(static_cast<const Tangents*>(context->m_pUserData)->faces.size());
}
int mikk_corners(const SMikkTSpaceContext*, const int) {
    return 3;
}
void mikk_position(const SMikkTSpaceContext* context, float out[], const int face, const int corner) {
    const auto& v = static_cast<const Tangents*>(context->m_pUserData)->vertex(face, corner);
    out[0] = v.position[0], out[1] = v.position[1], out[2] = v.position[2];
}
void mikk_normal(const SMikkTSpaceContext* context, float out[], const int face, const int corner) {
    const auto& v = static_cast<const Tangents*>(context->m_pUserData)->vertex(face, corner);
    out[0] = v.normal[0], out[1] = v.normal[1], out[2] = v.normal[2];
}
void mikk_uv(const SMikkTSpaceContext* context, float out[], const int face, const int corner) {
    const auto& v = static_cast<const Tangents*>(context->m_pUserData)->vertex(face, corner);
    out[0] = v.uv[0], out[1] = 1.0f - v.uv[1];
}
void mikk_set(const SMikkTSpaceContext* context, const float tangent[], const float sign, const int face, const int corner) {
    auto& t = *static_cast<Tangents*>(context->m_pUserData);
    t.corners[static_cast<std::size_t>(face) * 3 + static_cast<std::size_t>(corner)] = {tangent[0], tangent[1], tangent[2],
                                                                                       sign < 0.0f ? -1.0f : 1.0f};
}

} // namespace

MaterialTangents make_material_tangents(formats::ShapeModel& model, const std::vector<bool>& normal_mapped) {
    MaterialTangents result;
    Tangents tangents{.model = &model};
    for (const auto& draw : model.draws) {
        if (draw.primitive_state_index >= normal_mapped.size() || !normal_mapped[draw.primitive_state_index]) continue;
        const auto end = std::min<std::size_t>(std::size_t{draw.first_index} + draw.index_count, model.indices.size());
        for (std::size_t first = draw.first_index; first + 3 <= end; first += 3) {
            const auto a = model.indices[first], b = model.indices[first + 1], c = model.indices[first + 2];
            if (a >= model.vertices.size() || b >= model.vertices.size() || c >= model.vertices.size()) continue;
            const auto& va = model.vertices[a].uv;
            const auto& vb = model.vertices[b].uv;
            const auto& vc = model.vertices[c].uv;
            const auto area = (vb[0] - va[0]) * (vc[1] - va[1]) - (vc[0] - va[0]) * (vb[1] - va[1]);
            if (std::abs(area) < 1e-12f) {
                ++result.degenerate; // no tangent from it: the normal map left off there
                continue;
            }
            tangents.faces.push_back(static_cast<std::uint32_t>(first));
        }
    }
    if (tangents.faces.empty()) return result;
    tangents.corners.assign(tangents.faces.size() * 3, {0.0f, 0.0f, 0.0f, 0.0f});
    SMikkTSpaceInterface interface{};
    interface.m_getNumFaces = mikk_faces;
    interface.m_getNumVerticesOfFace = mikk_corners;
    interface.m_getPosition = mikk_position;
    interface.m_getNormal = mikk_normal;
    interface.m_getTexCoord = mikk_uv;
    interface.m_setTSpaceBasic = mikk_set;
    SMikkTSpaceContext context{.m_pInterface = &interface, .m_pUserData = &tangents};
    if (!genTangSpaceDefault(&context)) {
        result.failed = true;
        return result;
    }
    // Each vertex takes its corners' tangent; one whose corners differ (a
    // mirrored UV seam, faces of both hands on a welded vertex) gets a copy
    // for the others, as MikkTSpace means its vertices to be split.
    result.tangents.assign(model.vertices.size(), {0.0f, 0.0f, 0.0f, 0.0f});
    std::vector<std::uint8_t> written(model.vertices.size(), 0);
    std::map<std::pair<std::uint32_t, std::array<int, 4>>, std::uint32_t> copies;
    const auto same = [](const std::array<float, 4>& a, const std::array<float, 4>& b) {
        return std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]) < 1e-3f && a[3] == b[3];
    };
    for (std::size_t f = 0; f < tangents.faces.size(); ++f) {
        for (std::uint32_t corner = 0; corner < 3; ++corner) {
            auto& index = model.indices[tangents.faces[f] + corner];
            const auto& tangent = tangents.corners[f * 3 + corner];
            if (!written[index]) {
                result.tangents[index] = tangent;
                written[index] = 1;
                continue;
            }
            if (same(result.tangents[index], tangent)) continue;
            const std::array<int, 4> quantized{static_cast<int>(std::lround(tangent[0] * 1000.0f)), static_cast<int>(std::lround(tangent[1] * 1000.0f)),
                                               static_cast<int>(std::lround(tangent[2] * 1000.0f)), static_cast<int>(tangent[3])};
            const auto key = std::pair{index, quantized};
            if (const auto it = copies.find(key); it != copies.end()) {
                index = it->second;
                continue;
            }
            const auto copy = static_cast<std::uint32_t>(model.vertices.size());
            model.vertices.push_back(model.vertices[index]);
            result.tangents.push_back(tangent);
            written.push_back(1);
            copies.emplace(key, copy);
            index = copy;
            ++result.split;
        }
    }
    return result;
}

void hold_shape_material(const core::VirtualPath& shape, std::optional<formats::ShapeMaterialFile> data) {
    auto& s = store();
    const std::lock_guard lock(s.mutex);
    if (data) {
        s.held[shape.key()] = std::move(*data);
    } else {
        s.held.erase(shape.key());
    }
}

formats::ShapeMaterialFile find_shape_material(const core::Vfs& vfs, const core::VirtualPath& shape, bool* held) {
    if (held) *held = false;
    auto& s = store();
    {
        const std::lock_guard lock(s.mutex);
        if (const auto it = s.held.find(shape.key()); it != s.held.end()) {
            if (held) *held = true;
            return it->second;
        }
    }
    const auto file = shape_material_path(shape);
    if (!vfs.exists(file)) return {};
    const auto text = vfs.read(file);
    if (!text) return {.error = file.str() + ": cannot be read"};
    auto parsed = formats::parse_shape_material(*text, file.str());
    const std::lock_guard lock(s.mutex);
    if (!parsed.ok()) {
        if (const auto it = s.shape_good.find(file.key()); it != s.shape_good.end()) {
            auto kept = it->second;
            kept.warnings.push_back(parsed.error + " (its last good reading kept)");
            return kept;
        }
        return parsed;
    }
    s.shape_good[file.key()] = parsed;
    return parsed;
}

void load_model_materials(const core::Vfs& vfs, StreamedModelAsset& asset, TextureDecodeCache* cache) {
    auto& model = asset.model;
    if (!model.ok()) return;
    std::vector<std::string> messages;

    // The texture files' layers, by image: beside the texture, and beside
    // its night or season variant when that is what is drawn.
    std::vector<std::vector<std::pair<formats::MaterialLayer, formats::MaterialPatch>>> by_image(model.images.size());
    for (std::size_t i = 0; i < model.images.size() && i < asset.textures.size(); ++i) {
        const auto& texture = asset.textures[i];
        if (!texture.source_path) continue;
        const auto& source = *texture.source_path;
        auto folder = source.parent();
        bool variant = false;
        const auto folder_name = core::fold_case(folder.filename());
        for (const auto& each : asset.texture_folders) {
            if (core::fold_case(each) == folder_name) {
                variant = true;
                break;
            }
        }
        const auto name = formats::texture_material_name(source.filename());
        const auto base_folder = variant ? folder.parent() : folder;
        if (const auto file = base_folder.resolve(std::string_view(name))) {
            asset.material_files.push_back(*file);
            if (auto patch = read_texture_material(vfs, *file, messages)) by_image[i].emplace_back(formats::MaterialLayer::texture, std::move(*patch));
        }
        if (variant) {
            if (const auto file = folder.resolve(std::string_view(name))) {
                asset.material_files.push_back(*file);
                if (auto patch = read_texture_material(vfs, *file, messages))
                    by_image[i].emplace_back(formats::MaterialLayer::texture_variant, std::move(*patch));
            }
        }
    }

    // The shape's entries.
    bool held = false;
    const auto shape_file = find_shape_material(vfs, asset.source_path, &held);
    if (!shape_file.ok()) messages.push_back(shape_file.error);
    messages.insert(messages.end(), shape_file.warnings.begin(), shape_file.warnings.end());
    const auto signatures = formats::primitive_signatures(model);
    std::vector<std::vector<std::size_t>> entries_of(model.primitive_states.size()); // entries applying to each, in order
    if (!shape_file.empty()) {
        std::vector<std::string> selection;
        const auto chosen = formats::select_primitives(signatures, shape_file, selection);
        for (auto& each : selection) messages.push_back(shape_material_path(asset.source_path).str() + ": " + each);
        for (std::size_t e = 0; e < chosen.size(); ++e) {
            for (const auto index : chosen[e]) {
                if (index < entries_of.size()) entries_of[index].push_back(e);
            }
        }
    }

    const bool any = std::any_of(by_image.begin(), by_image.end(), [](const auto& layers) { return !layers.empty(); })
        || std::any_of(entries_of.begin(), entries_of.end(), [](const auto& list) { return !list.empty(); });
    if (any) {
        asset.materials.resize(model.primitive_states.size());
        std::map<std::string, std::int32_t> map_index;
        const auto add_map = [&](const MaterialMapKind kind, const formats::Material& m) -> std::int32_t {
            const auto key = map_key(kind, m);
            if (const auto it = map_index.find(key); it != map_index.end()) return it->second;
            std::shared_ptr<const formats::AceDecodeResult> image;
            const auto cache_key = "\x01" + key;
            if (cache) {
                if (const auto it = cache->find(cache_key); it != cache->end()) image = it->second;
            }
            if (!image) {
                image = build_map(vfs, kind, m);
                if (cache) cache->emplace(cache_key, image);
            }
            // The maps' files are dependencies whether they load or not.
            for (const auto* map : {&m.base_color, &m.orm, &m.ao_map, &m.roughness_map, &m.metallic_map, &m.normal, &m.emissive}) {
                if (*map) {
                    if (const auto path = map_path(**map)) asset.material_files.push_back(*path);
                }
            }
            if (!image->ok()) {
                messages.push_back(image->error);
                map_index[key] = -1;
                return -1;
            }
            const auto index = static_cast<std::int32_t>(asset.material_maps.size());
            asset.material_maps.push_back({.kind = kind, .key = key, .image = image});
            map_index[key] = index;
            return index;
        };
        for (std::size_t p = 0; p < model.primitive_states.size(); ++p) {
            auto& out = asset.materials[p];
            const auto& signature = signatures[p];
            const auto& state = model.primitive_states[p];
            if (state.texture_count > 0 && state.texture_index < model.textures.size()) {
                const auto image = model.textures[state.texture_index].image_index;
                if (image < by_image.size()) out.layers = by_image[image];
            }
            for (const auto e : entries_of[p])
                out.layers.emplace_back(held ? formats::MaterialLayer::inspector : formats::MaterialLayer::shape, shape_file.primitives[e].patch);
            std::vector<formats::MaterialPatch> patches;
            patches.reserve(out.layers.size());
            for (const auto& [_, patch] : out.layers) patches.push_back(patch);
            out.material = formats::resolve_material(rts_material(signature.light_material), patches);
            if (out.layers.empty()) continue;
            const auto& m = out.material;
            if (m.base_color) out.base_color = add_map(MaterialMapKind::base_color, m);
            if (m.packed_maps()) out.orm = add_map(MaterialMapKind::orm, m);
            if (m.normal) out.normal = add_map(MaterialMapKind::normal, m);
            if (m.emissive) out.emissive = add_map(MaterialMapKind::emissive, m);
        }

        // Tangents for the primitives with a normal map.
        std::vector<bool> normal_mapped(model.primitive_states.size(), false);
        for (std::size_t p = 0; p < asset.materials.size(); ++p) normal_mapped[p] = asset.materials[p].normal >= 0;
        if (std::find(normal_mapped.begin(), normal_mapped.end(), true) != normal_mapped.end()) {
            auto made = make_material_tangents(model, normal_mapped);
            if (made.failed) {
                messages.push_back(asset.source_path.str() + ": the tangents could not be made; its normal maps are left off");
            } else {
                asset.tangents = std::move(made.tangents);
            }
            if (made.degenerate > 0) {
                messages.push_back(asset.source_path.str() + ": " + std::to_string(made.degenerate)
                                   + " triangles with a normal map but no texture area: the map is left off on them");
            }
        }
    }
    // Every material file looked at, for hot reload; duplicates dropped.
    std::sort(asset.material_files.begin(), asset.material_files.end(), [](const auto& a, const auto& b) { return a.key() < b.key(); });
    asset.material_files.erase(std::unique(asset.material_files.begin(), asset.material_files.end(),
                                           [](const auto& a, const auto& b) { return a.key() == b.key(); }),
                               asset.material_files.end());
    asset.material_messages = messages;
    tell(messages);
}

} // namespace openrts::renderer
