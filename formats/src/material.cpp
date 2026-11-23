#include <openrts/formats/material.hpp>

#include <algorithm>
#include <cmath>
#include <sstream>

#include <openrts/core/json5.hpp>
#include <openrts/formats/shape_binary.hpp>
#include <openrts/formats/shape_override.hpp>

namespace openrts::formats {

namespace json5 = core::json5;

const char* layout_name(const OrmLayout layout) noexcept {
    switch (layout) {
    case OrmLayout::orm: return "orm";
    case OrmLayout::mrao: return "mrao";
    case OrmLayout::hdrp_mask: return "hdrp_mask";
    }
    return "orm";
}

const char* normal_y_name(const NormalY y) noexcept {
    return y == NormalY::down ? "down" : "up";
}

const char* layer_name(const MaterialLayer layer) noexcept {
    switch (layer) {
    case MaterialLayer::rts: return "RTS";
    case MaterialLayer::texture: return "texture";
    case MaterialLayer::texture_variant: return "texture variant";
    case MaterialLayer::shape: return "shape";
    case MaterialLayer::inspector: return "inspector";
    }
    return "RTS";
}

bool MaterialPatch::empty() const noexcept {
    return *this == MaterialPatch{};
}

void MaterialPatch::merge(const MaterialPatch& over) {
    const auto take = [](auto& field, const auto& value) {
        if (value) field = value;
    };
    take(base_color_factor, over.base_color_factor);
    take(base_color, over.base_color);
    if (over.orm) {
        orm = over.orm;
        layout = over.layout ? over.layout : std::optional<OrmLayout>(OrmLayout::orm);
        // The new packed map says all three; single maps below it go.
        if (!over.ao_map) ao_map.reset();
        if (!over.roughness_map) roughness_map.reset();
        if (!over.metallic_map) metallic_map.reset();
    } else {
        take(layout, over.layout);
    }
    take(ao_map, over.ao_map);
    take(roughness_map, over.roughness_map);
    take(metallic_map, over.metallic_map);
    take(roughness, over.roughness);
    take(metallic, over.metallic);
    take(reflectance, over.reflectance);
    take(specular, over.specular);
    take(ao_strength, over.ao_strength);
    if (over.normal) {
        normal = over.normal;
        normal_y = over.normal_y ? over.normal_y : std::optional<NormalY>(NormalY::up);
    } else {
        take(normal_y, over.normal_y);
    }
    take(normal_strength, over.normal_strength);
    take(emissive, over.emissive);
    take(emissive_factor, over.emissive_factor);
    take(emissive_strength, over.emissive_strength);
    take(height, over.height);
    take(height_scale, over.height_scale);
    take(height_reference, over.height_reference);
    take(snow, over.snow);
    take(alpha_mode, over.alpha_mode);
    take(alpha_cutoff, over.alpha_cutoff);
    take(double_sided, over.double_sided);
    take(wet, over.wet);
}

Material resolve_material(const Material& rts, const std::span<const MaterialPatch> layers) {
    MaterialPatch merged;
    for (const auto& layer : layers) merged.merge(layer);
    Material m = rts;
    if (merged.base_color_factor) m.base_color_factor = *merged.base_color_factor;
    if (merged.base_color) m.base_color = merged.base_color;
    if (merged.orm) m.orm = merged.orm;
    if (merged.layout) m.layout = *merged.layout;
    if (merged.ao_map) m.ao_map = merged.ao_map;
    if (merged.roughness_map) m.roughness_map = merged.roughness_map;
    if (merged.metallic_map) m.metallic_map = merged.metallic_map;
    // A map makes the number its factor (glTF): RTS's numbers give way to 1.
    const bool roughness_map = m.orm || m.roughness_map;
    const bool metallic_map = m.orm || m.metallic_map;
    m.roughness = merged.roughness ? *merged.roughness : (roughness_map ? 1.0f : rts.roughness);
    m.metallic = merged.metallic ? *merged.metallic : (metallic_map ? 1.0f : rts.metallic);
    m.specular = merged.specular ? *merged.specular : (roughness_map || metallic_map ? 1.0f : rts.specular);
    if (merged.reflectance) m.reflectance = *merged.reflectance;
    if (merged.ao_strength) m.ao_strength = *merged.ao_strength;
    if (merged.normal) m.normal = merged.normal;
    if (merged.normal_y) m.normal_y = *merged.normal_y;
    if (merged.normal_strength) m.normal_strength = *merged.normal_strength;
    if (merged.emissive) m.emissive = merged.emissive;
    if (merged.emissive_factor) {
        m.emissive_factor = *merged.emissive_factor;
    } else if (m.emissive) {
        m.emissive_factor = {1.0f, 1.0f, 1.0f};
    }
    if (merged.emissive_strength) m.emissive_strength = *merged.emissive_strength;
    if (merged.height) m.height = merged.height;
    if (merged.height_scale) m.height_scale = *merged.height_scale;
    if (merged.height_reference) m.height_reference = *merged.height_reference;
    if (merged.snow) m.snow = merged.snow;
    if (merged.wet) m.wet = *merged.wet;
    return m;
}

namespace {

[[nodiscard]] std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

[[nodiscard]] bool same(const std::string_view a, const std::string_view b) {
    return a.size() == b.size() && lower(a) == lower(b);
}

// The fields of a material, read from an object's members; anything else is
// left to the caller (`rest`).
class PatchReader {
public:
    PatchReader(const std::string_view file, std::vector<std::string>& warnings) : file_(file), warnings_(warnings) {}

    // Whether `key` is a material field (read into `patch`, or warned about).
    bool read(const std::string& key, const json5::Value& value, MaterialPatch& patch) {
        if (key == "base_color_factor") {
            patch.base_color_factor = colour<4>(key, value, 0.0f, 1.0f, true);
        } else if (key == "base_color") {
            patch.base_color = map(key, value);
        } else if (key == "orm") {
            patch.orm = map(key, value);
        } else if (key == "layout") {
            if (const auto text = word(key, value, {"orm", "mrao", "hdrp_mask", "hdrp-mask"})) {
                patch.layout = *text == "orm" ? OrmLayout::orm : *text == "mrao" ? OrmLayout::mrao : OrmLayout::hdrp_mask;
            }
        } else if (key == "ao_map" || key == "ao") {
            patch.ao_map = map(key, value);
        } else if (key == "roughness_map") {
            patch.roughness_map = map(key, value);
        } else if (key == "metallic_map") {
            patch.metallic_map = map(key, value);
        } else if (key == "roughness") {
            patch.roughness = number(key, value, 0.0f, 1.0f);
        } else if (key == "metallic") {
            patch.metallic = number(key, value, 0.0f, 1.0f);
        } else if (key == "reflectance") {
            patch.reflectance = number(key, value, 0.0f, 1.0f);
        } else if (key == "specular") {
            patch.specular = number(key, value, 0.0f, 1.0f);
        } else if (key == "ao_strength") {
            patch.ao_strength = number(key, value, 0.0f, 1.0f);
        } else if (key == "normal") {
            patch.normal = map(key, value);
        } else if (key == "normal_y") {
            if (const auto text = word(key, value, {"up", "down"})) patch.normal_y = *text == "down" ? NormalY::down : NormalY::up;
        } else if (key == "normal_strength") {
            patch.normal_strength = number(key, value, 0.0f, 2.0f);
        } else if (key == "emissive") {
            patch.emissive = map(key, value);
        } else if (key == "emissive_factor") {
            if (const auto c = colour<3>(key, value, 0.0f, 1.0f, false)) patch.emissive_factor = std::array<float, 3>{(*c)[0], (*c)[1], (*c)[2]};
        } else if (key == "emissive_strength") {
            patch.emissive_strength = number(key, value, 0.0f, 100.0f);
        } else if (key == "height") {
            patch.height = map(key, value);
        } else if (key == "snow") {
            patch.snow = map(key, value);
        } else if (key == "height_scale") {
            patch.height_scale = number(key, value, 0.0f, 0.1f);
        } else if (key == "height_reference") {
            patch.height_reference = number(key, value, 0.0f, 1.0f);
        } else if (key == "alpha_mode") {
            if (const auto text = word(key, value, {"opaque", "mask", "blend"})) {
                patch.alpha_mode = *text == "opaque" ? AlphaMode::opaque : *text == "mask" ? AlphaMode::mask : AlphaMode::blend;
                warn(value, "alpha_mode is read but not applied yet");
            }
        } else if (key == "alpha_cutoff") {
            patch.alpha_cutoff = number(key, value, 0.0f, 1.0f);
            if (patch.alpha_cutoff) warn(value, "alpha_cutoff is read but not applied yet");
        } else if (key == "double_sided") {
            if (value.is_boolean()) {
                patch.double_sided = value.boolean;
                warn(value, "double_sided is read but not applied yet");
            } else {
                warn(value, "double_sided: true or false expected");
            }
        } else if (key == "wet") {
            if (value.is_boolean()) {
                patch.wet = value.boolean;
            } else if (value.is_object()) {
                patch.wet = true;
            } else {
                warn(value, "wet: true, false or { kind: ... } expected");
            }
        } else {
            return false;
        }
        return true;
    }

    void warn(const json5::Value& value, const std::string& what) {
        warnings_.push_back(std::string(file_) + ": " + what + " (" + value.where() + ")");
    }

private:
    std::string_view file_;
    std::vector<std::string>& warnings_;

    std::optional<float> number(const std::string& key, const json5::Value& value, const float low, const float high) {
        if (!value.is_number() || !std::isfinite(value.number)) {
            warn(value, key + ": a number expected");
            return std::nullopt;
        }
        if (value.number < low || value.number > high) {
            std::ostringstream text;
            text << key << ": " << value.number << " is out of " << low << ".." << high;
            warn(value, text.str());
            return std::nullopt;
        }
        return static_cast<float>(value.number);
    }

    template <std::size_t n>
    std::optional<std::array<float, 4>> colour(const std::string& key, const json5::Value& value, const float low, const float high,
                                               const bool alpha) {
        if (!value.is_array() || (value.items.size() != n && !(alpha && value.items.size() == 3))) {
            warn(value, key + ": [" + (alpha ? std::string("r, g, b] or [r, g, b, a") : std::string("r, g, b")) + "] expected");
            return std::nullopt;
        }
        std::array<float, 4> out{1.0f, 1.0f, 1.0f, 1.0f};
        for (std::size_t i = 0; i < value.items.size(); ++i) {
            const auto item = number(key, value.items[i], low, high);
            if (!item) return std::nullopt;
            out[i] = *item;
        }
        return out;
    }

    std::optional<MaterialMap> map(const std::string& key, const json5::Value& value) {
        if (!value.is_string() || value.string.empty()) {
            warn(value, key + ": a file name expected");
            return std::nullopt;
        }
        const auto& path = value.string;
        if (path.front() == '/' || path.front() == '\\' || path.find(':') != std::string::npos) {
            warn(value, key + ": \"" + path + "\" must be relative to this file's folder");
            return std::nullopt;
        }
        return MaterialMap{.path = path, .from = std::string(file_)};
    }

    std::optional<std::string> word(const std::string& key, const json5::Value& value, std::initializer_list<const char*> allowed) {
        if (value.is_string()) {
            for (const char* each : allowed) {
                if (value.string == each) return value.string == "hdrp-mask" ? std::string("hdrp_mask") : value.string;
            }
        }
        std::string list;
        for (const char* each : allowed) list += (list.empty() ? "\"" : ", \"") + std::string(each) + "\"";
        warn(value, key + ": one of " + list + " expected");
        return std::nullopt;
    }
};

// "openrts-material/1" or "openrts-material/1.x"; empty when it fits.
[[nodiscard]] std::string check_format(const json5::Value& root, const std::string_view expected) {
    const auto* format = root.find("format");
    if (!format || !format->is_string()) return "no format (format: \"" + std::string(expected) + "\")";
    const auto& text = format->string;
    if (text == expected || (text.size() > expected.size() && text.compare(0, expected.size(), expected) == 0 && text[expected.size()] == '.'))
        return {};
    return "the format \"" + text + "\" is not \"" + std::string(expected) + "\"";
}

} // namespace

MaterialFile parse_material(const std::string_view text, const std::string_view file) {
    MaterialFile result;
    const auto parsed = json5::parse(text);
    if (!parsed.ok()) {
        result.error = std::string(file) + ": " + parsed.error;
        return result;
    }
    const auto& root = *parsed.value;
    if (!root.is_object()) {
        result.error = std::string(file) + ": an object { ... } expected";
        return result;
    }
    if (auto problem = check_format(root, "openrts-material/1"); !problem.empty()) {
        result.error = std::string(file) + ": " + problem;
        return result;
    }
    PatchReader reader(file, result.warnings);
    for (const auto& [key, value] : root.members) {
        if (key == "format") continue;
        if (!reader.read(key, value, result.patch)) reader.warn(value, "unknown key \"" + key + "\"");
    }
    return result;
}

ShapeMaterialFile parse_shape_material(const std::string_view text, const std::string_view file) {
    ShapeMaterialFile result;
    const auto parsed = json5::parse(text);
    if (!parsed.ok()) {
        result.error = std::string(file) + ": " + parsed.error;
        return result;
    }
    const auto& root = *parsed.value;
    if (!root.is_object()) {
        result.error = std::string(file) + ": an object { ... } expected";
        return result;
    }
    if (auto problem = check_format(root, "openrts-shape-material/1"); !problem.empty()) {
        result.error = std::string(file) + ": " + problem;
        return result;
    }
    PatchReader reader(file, result.warnings);
    for (const auto& [key, value] : root.members) {
        if (key == "format") continue;
        if (key != "primitives") {
            reader.warn(value, "unknown key \"" + key + "\"");
            continue;
        }
        if (!value.is_array()) {
            reader.warn(value, "primitives: a list [ ... ] expected");
            continue;
        }
        for (const auto& item : value.items) {
            if (!item.is_object()) {
                reader.warn(item, "a primitive's entry { match: ..., ... } expected");
                continue;
            }
            ShapeMaterialEntry entry{.line = item.line};
            bool has_match = false;
            for (const auto& [field, field_value] : item.members) {
                if (field == "match") {
                    if (!field_value.is_object()) {
                        reader.warn(field_value, "match: { name, image, matrix, index } expected");
                        continue;
                    }
                    has_match = true;
                    for (const auto& [m, m_value] : field_value.members) {
                        if (m == "all") {
                            if (m_value.is_boolean()) entry.match.all = m_value.boolean;
                            else reader.warn(m_value, "all: true or false expected");
                        } else if (m == "index") {
                            if (m_value.is_number() && m_value.number >= 0 && m_value.number == std::floor(m_value.number))
                                entry.match.index = static_cast<std::uint32_t>(m_value.number);
                            else reader.warn(m_value, "index: a whole number expected");
                        } else if (m == "name" || m == "image" || m == "matrix") {
                            if (!m_value.is_string()) {
                                reader.warn(m_value, m + ": a string expected");
                                continue;
                            }
                            (m == "name" ? entry.match.name : m == "image" ? entry.match.image : entry.match.matrix) = m_value.string;
                        } else {
                            reader.warn(m_value, "unknown key \"" + m + "\" in match");
                        }
                    }
                } else if (field == "all") {
                    if (field_value.is_boolean()) entry.match.all = field_value.boolean;
                    else reader.warn(field_value, "all: true or false expected");
                } else if (!reader.read(field, field_value, entry.patch)) {
                    reader.warn(field_value, "unknown key \"" + field + "\"");
                }
            }
            const auto& s = entry.match;
            if (!has_match || (!s.name && !s.image && !s.matrix && !s.index && !s.all)) {
                reader.warn(item, "an entry without match: left out (match: { all: true } is for every primitive)");
                continue;
            }
            result.primitives.push_back(std::move(entry));
        }
    }
    return result;
}

namespace {

void write_patch(std::ostream& out, const MaterialPatch& p) {
    const auto num = [](const float value) { return json5::number(static_cast<double>(value), true); };
    const auto field = [&](const char* key, const std::string& value) { out << ", " << key << ": " << value; };
    const auto colour = [&](const auto& c, const std::size_t n) {
        std::string text = "[";
        for (std::size_t i = 0; i < n; ++i) text += (i ? ", " : "") + num(c[i]);
        return text + "]";
    };
    if (p.base_color_factor) field("base_color_factor", colour(*p.base_color_factor, 4));
    if (p.base_color) field("base_color", json5::quote(p.base_color->path));
    if (p.orm) field("orm", json5::quote(p.orm->path));
    if (p.layout) field("layout", json5::quote(layout_name(*p.layout)));
    if (p.ao_map) field("ao_map", json5::quote(p.ao_map->path));
    if (p.roughness_map) field("roughness_map", json5::quote(p.roughness_map->path));
    if (p.metallic_map) field("metallic_map", json5::quote(p.metallic_map->path));
    if (p.roughness) field("roughness", num(*p.roughness));
    if (p.metallic) field("metallic", num(*p.metallic));
    if (p.reflectance) field("reflectance", num(*p.reflectance));
    if (p.specular) field("specular", num(*p.specular));
    if (p.ao_strength) field("ao_strength", num(*p.ao_strength));
    if (p.normal) field("normal", json5::quote(p.normal->path));
    if (p.normal_y) field("normal_y", json5::quote(normal_y_name(*p.normal_y)));
    if (p.normal_strength) field("normal_strength", num(*p.normal_strength));
    if (p.emissive) field("emissive", json5::quote(p.emissive->path));
    if (p.emissive_factor) field("emissive_factor", colour(*p.emissive_factor, 3));
    if (p.emissive_strength) field("emissive_strength", num(*p.emissive_strength));
    if (p.height) field("height", json5::quote(p.height->path));
    if (p.height_scale) field("height_scale", num(*p.height_scale));
    if (p.height_reference) field("height_reference", num(*p.height_reference));
    if (p.snow) field("snow", json5::quote(p.snow->path));
    if (p.wet) field("wet", *p.wet ? "true" : "false");
}

} // namespace

std::string write_shape_material(const ShapeMaterialFile& data) {
    std::ostringstream out;
    out << "// openRTS: changes to this shape's materials made in the inspector.\n"
           "// Machine-written into openrts_edits; a file of your own is never changed.\n"
           "{\n  format: \"openrts-shape-material/1\",\n  primitives: [\n";
    for (const auto& entry : data.primitives) {
        out << "    { match: {";
        bool first = true;
        const auto item = [&](const char* key, const std::string& value) {
            out << (first ? " " : ", ") << key << ": " << value;
            first = false;
        };
        if (entry.match.name) item("name", json5::quote(*entry.match.name));
        if (entry.match.image) item("image", json5::quote(*entry.match.image));
        if (entry.match.matrix) item("matrix", json5::quote(*entry.match.matrix));
        if (entry.match.index) item("index", std::to_string(*entry.match.index));
        if (entry.match.all) item("all", "true");
        out << " }";
        write_patch(out, entry.patch);
        out << " },\n";
    }
    out << "  ],\n}\n";
    return out.str();
}

std::vector<PrimitiveSignature> primitive_signatures(const ShapeModel& model) {
    std::vector<PrimitiveSignature> result;
    result.reserve(model.primitive_states.size());
    for (std::size_t i = 0; i < model.primitive_states.size(); ++i) {
        const auto& state = model.primitive_states[i];
        PrimitiveSignature s{.index = static_cast<std::uint32_t>(i), .name = state.name,
                             .image = primitive_image(model, static_cast<std::uint32_t>(i))};
        if (state.shader_index < model.shader_names.size()) s.shader = model.shader_names[state.shader_index];
        if (state.vertex_state_index >= 0 && static_cast<std::size_t>(state.vertex_state_index) < model.vertex_states.size()) {
            const auto& vertex = model.vertex_states[static_cast<std::size_t>(state.vertex_state_index)];
            s.light_material = vertex.light_material_index;
            if (vertex.matrix_index >= 0 && static_cast<std::size_t>(vertex.matrix_index) < model.matrices.size())
                s.matrix = model.matrices[static_cast<std::size_t>(vertex.matrix_index)].name;
        }
        result.push_back(std::move(s));
    }
    return result;
}

namespace {

[[nodiscard]] bool matches(const PrimitiveSignature& p, const PrimitiveSelector& s) {
    if (s.name && !same(p.name, *s.name)) return false;
    if (s.image) {
        // A path's folders do not count: the file name.
        const auto slash = s.image->find_last_of("/\\");
        if (!same(p.image, slash == std::string::npos ? *s.image : s.image->substr(slash + 1))) return false;
    }
    if (s.matrix && !same(p.matrix, *s.matrix)) return false;
    if (s.index && p.index != *s.index) return false;
    return true;
}

[[nodiscard]] std::string describe(const PrimitiveSelector& s) {
    std::string text;
    const auto add = [&](const std::string& part) { text += (text.empty() ? "" : ", ") + part; };
    if (s.name) add("name \"" + *s.name + "\"");
    if (s.image) add("image \"" + *s.image + "\"");
    if (s.matrix) add("matrix \"" + *s.matrix + "\"");
    if (s.index) add("index " + std::to_string(*s.index));
    if (text.empty()) text = "every primitive";
    return text;
}

} // namespace

std::vector<std::vector<std::uint32_t>> select_primitives(const std::span<const PrimitiveSignature> primitives, const ShapeMaterialFile& file,
                                                          std::vector<std::string>& warnings) {
    std::vector<std::vector<std::uint32_t>> result;
    result.reserve(file.primitives.size());
    for (const auto& entry : file.primitives) {
        std::vector<std::uint32_t> chosen;
        for (const auto& p : primitives) {
            if (matches(p, entry.match)) chosen.push_back(p.index);
        }
        if (chosen.empty()) {
            warnings.push_back("line " + std::to_string(entry.line) + ": " + describe(entry.match) + " matches no primitive");
        } else if (chosen.size() > 1 && !entry.match.all) {
            std::string list;
            for (const auto index : chosen) {
                const auto& p = primitives[index];
                list += (list.empty() ? "" : "; ") + std::to_string(index) + (p.name.empty() ? "" : " \"" + p.name + "\"") + " " + p.image
                    + (p.matrix.empty() ? "" : " on " + p.matrix);
            }
            warnings.push_back("line " + std::to_string(entry.line) + ": " + describe(entry.match) + " matches " + std::to_string(chosen.size())
                               + " primitives (" + list + "): applied to none; add all: true to mean all of them");
            chosen.clear();
        }
        result.push_back(std::move(chosen));
    }
    return result;
}

PrimitiveSelector stable_selector(const std::span<const PrimitiveSignature> primitives, const std::uint32_t index) {
    if (index >= primitives.size()) return {.index = index};
    const auto& p = primitives[index];
    const auto count = [&](const PrimitiveSelector& s) {
        return std::count_if(primitives.begin(), primitives.end(), [&](const PrimitiveSignature& each) { return matches(each, s); });
    };
    if (!p.name.empty()) {
        const PrimitiveSelector by_name{.name = p.name};
        if (count(by_name) == 1) return by_name;
    }
    if (!p.image.empty()) {
        const PrimitiveSelector by_image{.image = p.image, .matrix = p.matrix.empty() ? std::nullopt : std::optional<std::string>(p.matrix)};
        if (count(by_image) == 1) return by_image;
    }
    PrimitiveSelector by_index{.index = index};
    if (!p.image.empty()) by_index.image = p.image;
    return by_index;
}

std::string texture_material_name(const std::string_view texture_file) {
    const auto slash = texture_file.find_last_of("/\\");
    const auto dot = texture_file.find_last_of('.');
    const auto stem = dot != std::string_view::npos && (slash == std::string_view::npos || dot > slash) ? texture_file.substr(0, dot) : texture_file;
    return std::string(stem) + ".material.json5";
}

std::string shape_material_name(const std::string_view shape_file) {
    return std::string(shape_file) + ".material.json5";
}

} // namespace openrts::formats
