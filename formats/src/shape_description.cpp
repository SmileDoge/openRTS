#include <openrts/formats/shape_description.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <utility>

#include "cargo_internal.hpp"
#include "shape_lights.hpp"
#include "text_tree.hpp"

namespace openrts::formats {
namespace {

class TokenReader {
public:
    explicit TokenReader(std::string_view text) : text_(text) {}

    [[nodiscard]] std::optional<std::string> next() {
        skip_whitespace_and_comments();
        if (position_ >= text_.size()) return std::nullopt;
        if (text_[position_] == '(' || text_[position_] == ')') return std::string(1, text_[position_++]);
        if (text_[position_] == '"') return read_quoted();
        return read_bare();
    }

private:
    void skip_whitespace_and_comments() {
        for (;;) {
            while (position_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[position_]))) ++position_;
            if (position_ + 1 >= text_.size() || text_[position_] != '/' || text_[position_ + 1] != '/') return;
            position_ += 2;
            while (position_ < text_.size() && text_[position_] != '\n') ++position_;
        }
    }

    [[nodiscard]] std::string read_quoted() {
        ++position_;
        std::string value;
        while (position_ < text_.size() && text_[position_] != '"') value.push_back(text_[position_++]);
        if (position_ < text_.size()) ++position_;
        return value;
    }

    [[nodiscard]] std::string read_bare() {
        const auto begin = position_;
        while (position_ < text_.size() && !std::isspace(static_cast<unsigned char>(text_[position_]))
               && text_[position_] != '(' && text_[position_] != ')') ++position_;
        return std::string(text_.substr(begin, position_ - begin));
    }

    std::string_view text_;
    std::size_t position_ = 0;
};

[[nodiscard]] bool equal_ignore_case(const std::string& left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(left[index]))
            != std::tolower(static_cast<unsigned char>(right[index]))) return false;
    }
    return true;
}

void append_utf8(std::string& output, const char32_t code_point) {
    if (code_point <= 0x7F) {
        output.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
        output.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
}

[[nodiscard]] std::optional<std::string> decode_file_text(const std::string& bytes, std::string& error) {
    if (bytes.size() < 2 || static_cast<unsigned char>(bytes[0]) != 0xFF
        || static_cast<unsigned char>(bytes[1]) != 0xFE) {
        return bytes;
    }
    if (bytes.size() % 2 != 0) {
        error = "UTF-16LE SD file has an odd byte count.";
        return std::nullopt;
    }

    std::string utf8;
    for (std::size_t index = 2; index < bytes.size(); index += 2) {
        const auto unit = static_cast<char16_t>(static_cast<unsigned char>(bytes[index])
                                                | (static_cast<unsigned char>(bytes[index + 1]) << 8));
        char32_t code_point = unit;
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (index + 3 >= bytes.size()) {
                error = "UTF-16LE SD file ends in a high surrogate.";
                return std::nullopt;
            }
            const auto low = static_cast<char16_t>(static_cast<unsigned char>(bytes[index + 2])
                                                   | (static_cast<unsigned char>(bytes[index + 3]) << 8));
            if (low < 0xDC00 || low > 0xDFFF) {
                error = "UTF-16LE SD file has an invalid surrogate pair.";
                return std::nullopt;
            }
            code_point = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
            index += 2;
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            error = "UTF-16LE SD file has an unpaired low surrogate.";
            return std::nullopt;
        }
        append_utf8(utf8, code_point);
    }
    return utf8;
}

[[nodiscard]] const detail::TextNode* find_node(const detail::TextNode& node, const std::string_view name) {
    for (const auto& child : node.children) {
        if (child.name == name) return &child;
        if (const auto* found = find_node(child, name)) return found;
    }
    return nullptr;
}

[[nodiscard]] float value(const detail::TextNode* node, const std::size_t index, const float fallback = 0.0f) {
    return node ? static_cast<float>(node->number(index, fallback)) : fallback;
}

// ESD_Alternative_Texture and ESD_LightSources of the Shape block.
void read_extensions(const std::string_view text, ShapeDescription& description) {
    detail::TextNode root;
    detail::read_text_tree(text, root);
    if (const auto* alternative = find_node(root, "esd_alternative_texture")) {
        description.alternative_texture = static_cast<std::uint32_t>((std::max)(0.0, alternative->number(0, 0.0)));
    }
    // RTS 0x4BB7BC: ESD_Complex's boxes, else the one ESD_Bounding_Box.
    auto box = [&](const detail::TextNode& node, const std::size_t first) {
        WalkBox b;
        if (first) {
            b.rotation = {value(&node, 0), value(&node, 1), value(&node, 2)};
            b.translation = {value(&node, 3), value(&node, 4), value(&node, 5)};
        }
        for (std::size_t k = 0; k < 3; ++k) {
            const float p = value(&node, first + k), q = value(&node, first + 3 + k);
            b.low[k] = (std::min)(p, q);
            b.high[k] = (std::max)(p, q);
        }
        description.boxes.push_back(b);
    };
    if (const auto* complex = find_node(root, "esd_complex")) {
        for (const auto& node : complex->children)
            if (node.name == "esd_complex_box" && node.values.size() >= 12) box(node, 6);
    } else if (const auto* bounding = find_node(root, "esd_bounding_box"); bounding && bounding->values.size() >= 6) {
        box(*bounding, 0);
    }
    if (const auto* lights = find_node(root, "esd_lightsources")) description.lights = detail::read_light_sources(*lights);
    if (const auto* pickup = find_node(root, "esd_pickup_object")) description.pickup = detail::read_pickup_description(*pickup);
}

} // namespace

namespace detail {

std::vector<ShapeLight> read_light_sources(const TextNode& list) {
    std::vector<ShapeLight> out;
    for (const auto& node : list.children) {
        if (node.name != "lightsource") continue;
        ShapeLight light;
        const auto* position = node.child("position");
        const auto* points_to = node.child("pointsto");
        light.position = {value(position, 0), value(position, 1), value(position, 2)};
        light.points_to = {value(points_to, 0), value(points_to, 1), value(points_to, 2)};
        light.radius = {value(node.child("radius"), 0), value(node.child("radius"), 1)};
        if (const auto* colour = node.child("colour"); colour && !colour->values.empty()) {
            light.colour = static_cast<std::uint32_t>(std::strtoul(colour->values.front().c_str(), nullptr, 16));
        }
        if (const auto* flags = node.child("flags")) {
            for (const auto& word : flags->values) {
                if (!light.flags.empty()) light.flags.push_back(' ');
                light.flags += detail::lower_ascii(word);
            }
        }
        light.distance = value(node.child("distance"), 0);
        light.spot_exponent = value(node.child("spotexponent"), 0);
        light.source_radius = value(node.child("sourceradius"), 0);
        light.auto_when_world_lit = value(node.child("autowhenworldlit"), 0);
        light.light_fade = value(node.child("lightfade"), 0);
        light.candela = value(node.child("ortscandela"), 0);
        light.lumens = value(node.child("ortslumens"), 0);
        light.kelvin = value(node.child("ortskelvin"), 0);
        if (const auto* n = node.child("ortsname"); n && !n->values.empty()) light.name = n->values.front();
        if (const auto* array = node.child("lightarray")) {
            const float count = value(array, 0);
            if (count >= 2.0f) {
                light.array_count = static_cast<std::uint32_t>(count);
                light.array_step = {value(array, 1), value(array, 2), value(array, 3)};
            }
        }
        if (const auto* attenuation = node.child("attenuation")) {
            // Attenuation ( D1 D2 Type ): the start and length shares, then the kind.
            const float start = value(attenuation, 0), length = value(attenuation, 1), kind = value(attenuation, 2);
            if (start >= 0.0f && length > 0.0f) light.attenuation = {kind, start, length};
        }
        out.push_back(std::move(light));
    }
    return out;
}

} // namespace detail

std::string write_shape_lights(const std::vector<ShapeLight>& lights, const std::string_view indent) {
    const std::string in(indent);
    char buffer[160];
    const auto numbers = [&](const char* name, std::initializer_list<float> values) {
        std::string out = std::string(" ") + name + " (";
        for (const float v : values) {
            std::snprintf(buffer, sizeof buffer, " %.6g", static_cast<double>(v));
            out += buffer;
        }
        return out + " )";
    };
    std::string out = in + "LightSources ( " + std::to_string(lights.size()) + "\n";
    for (const auto& l : lights) {
        out += in + "\tLightSource (";
        out += numbers("Position", {l.position[0], l.position[1], l.position[2]});
        out += numbers("PointsTo", {l.points_to[0], l.points_to[1], l.points_to[2]});
        out += numbers("Radius", {l.radius[0], l.radius[1]});
        std::snprintf(buffer, sizeof buffer, " Colour ( %08x )", l.colour);
        out += buffer;
        if (!l.flags.empty()) out += " Flags ( " + l.flags + " )";
        if (l.distance > 0.0f) out += numbers("Distance", {l.distance});
        out += numbers("Attenuation", {l.attenuation[1], l.attenuation[2], l.attenuation[0]});
        if (l.spot_exponent != 0.0f) out += numbers("SpotExponent", {l.spot_exponent});
        if (l.source_radius != 0.0f) out += numbers("SourceRadius", {l.source_radius});
        if (l.auto_when_world_lit > 0.0f) out += numbers("AutoWhenWorldLit", {l.auto_when_world_lit});
        if (l.light_fade != 0.0f) out += numbers("LightFade", {l.light_fade});
        if (l.candela > 0.0f) out += numbers("ORTSCandela", {l.candela});
        if (l.lumens > 0.0f) out += numbers("ORTSLumens", {l.lumens});
        if (l.kelvin > 0.0f) out += numbers("ORTSKelvin", {l.kelvin});
        if (!l.name.empty()) out += " ORTSName ( \"" + l.name + "\" )";
        if (l.array_count >= 2)
            out += numbers("LightArray", {static_cast<float>(l.array_count), l.array_step[0], l.array_step[1], l.array_step[2]});
        out += " )\n";
    }
    return out + in + ")\n";
}

ShapeDescriptionResult parse_shape_description(std::string_view text) {
    TokenReader tokens(text);
    while (const auto token = tokens.next()) {
        if (!equal_ignore_case(*token, "shape")) continue;
        const auto open_paren = tokens.next();
        const auto shape_name = tokens.next();
        if (!open_paren || *open_paren != "(" || !shape_name || *shape_name == ")") {
            return {.error = "Malformed Shape(...) node in SD file."};
        }
        ShapeDescription description{.shape_file = *shape_name};
        read_extensions(text, description);
        return {.value = std::move(description)};
    }
    return {.error = "SD file does not contain a Shape(...) node."};
}

ShapeDescriptionResult load_shape_description(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) return {.error = "Cannot open SD file: " + file.string()};
    const std::string text(std::istreambuf_iterator<char>(input), {});
    std::string decode_error;
    const auto decoded = decode_file_text(text, decode_error);
    if (!decoded) return {.error = std::move(decode_error)};
    return parse_shape_description(*decoded);
}

ShapeDescriptionResult load_shape_description(const core::Vfs& vfs, const core::VirtualPath& file) {
    std::string error;
    const auto bytes = vfs.read(file, &error);
    if (!bytes) return {.error = std::move(error)};
    std::string decode_error;
    const auto decoded = decode_file_text(*bytes, decode_error);
    if (!decoded) return {.error = std::move(decode_error)};
    return parse_shape_description(*decoded);
}

} // namespace openrts::formats
