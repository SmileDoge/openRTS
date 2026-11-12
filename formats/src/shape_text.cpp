// Text shapes (`SIMISA@@@@@@@@@@JINX0s1t`, usually UTF-16LE) carry the same
// block tree as binary shapes, written as `name [label] ( values blocks )`.
// They are transcoded into the binary token stream, so one parser reads both.

#include <openrts/formats/shape_file.hpp>

#include <bit>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "text_tokenizer.hpp"

namespace openrts::formats {
namespace {

// Value types of a block's scalars, in order (child blocks are not counted):
//   f float, u integer (negative values wrap), x hexadecimal, s string,
//   C count of the child blocks, N count of the scalars that follow;
//   a trailing '*' repeats the type before it.
// Counts are recomputed from the content, so a wrong count in a hand-edited
// file does not break the table.
struct BlockSchema {
    std::uint32_t token;
    std::string_view values;
};

// Tokens are the ones the binary reader uses (shape_binary.cpp). Blocks it
// does not read are dropped.
const std::unordered_map<std::string_view, BlockSchema>& schemas() {
    static const std::unordered_map<std::string_view, BlockSchema> table{
        {"shape", {71, ""}},
        {"shader_names", {72, "C"}},
        {"shader_name", {73, "s"}},
        {"named_shader", {129, "s"}},
        {"texture_filter_names", {74, "C"}},
        {"named_filter_mode", {125, "s"}},
        {"points", {7, "C"}},
        {"point", {2, "fff"}},
        {"uv_points", {9, "C"}},
        {"uv_point", {8, "ff"}},
        {"normals", {5, "C"}},
        {"vector", {3, "fff"}},
        {"matrices", {66, "C"}},
        {"matrix", {65, "ffffffffffff"}},
        {"images", {14, "C"}},
        {"image", {13, "s"}},
        {"textures", {16, "C"}},
        {"texture", {15, "uufx"}},
        {"vtx_states", {47, "C"}},
        {"vtx_state", {46, "xuuux"}},
        {"prim_states", {55, "C"}},
        {"prim_state", {54, "xufuuuu"}},
        {"tex_idxs", {61, "Nu*"}},
        {"lod_controls", {31, "C"}},
        {"lod_control", {32, ""}},
        {"distance_levels", {36, "C"}},
        {"distance_level", {37, ""}},
        {"distance_level_header", {34, ""}},
        // A level's distance: without it every level read as 0 m and the
        // renderer drew the coarsest one at any distance (Kuban's catenary
        // gantries g2wide2.S showed their masts without wires).
        {"dlevel_selection", {35, "f"}},
        {"hierarchy", {67, "Nu*"}},
        {"sub_objects", {38, "C"}},
        {"sub_object", {39, ""}},
        {"vertices", {50, "C"}},
        {"vertex", {48, "xuuxx"}},
        {"vertex_uvs", {49, "Nu*"}},
        {"primitives", {53, "C"}},
        {"prim_state_idx", {56, "u"}},
        {"indexed_trilist", {60, ""}},
        {"vertex_idxs", {63, "Nu*"}},
        {"animations", {29, "C"}},
        {"animation", {28, "uu"}},
        {"anim_nodes", {27, "C"}},
        {"anim_node", {26, ""}},
        {"controllers", {25, "C"}},
        {"tcb_rot", {24, "C"}},
        {"tcb_key", {20, "uf*"}},
        {"slerp_rot", {23, "uffff"}},
        {"linear_pos", {21, "C"}},
        {"linear_key", {19, "ufff"}},
    };
    return table;
}

// Other shape block names. Only known names may carry a label, which keeps a
// hex value followed by a block (`ff000000 vertex_uvs (`) from reading as one.
bool known_block(const std::string_view name) {
    static const std::unordered_map<std::string_view, bool> other{
        {"shape_header", true}, {"volumes", true}, {"vol_sphere", true}, {"light_materials", true},
        {"light_material", true}, {"light_model_cfgs", true}, {"light_model_cfg", true}, {"uv_ops", true},
        {"uv_op_copy", true}, {"uv_op_reflectmapfull", true}, {"uv_op_reflectmap", true},
        {"uv_op_uniformscale", true}, {"uv_op_nonuniformscale", true}, {"colours", true}, {"colour", true},
        {"packed_colour", true}, {"quaternion", true}, {"sort_vectors", true}, {"distance_levels_header", true},
        {"dlevel_bias", true}, {"sub_object_header", true}, {"geometry_info", true},
        {"geometry_nodes", true}, {"geometry_node", true}, {"geometry_node_map", true}, {"cullable_prims", true},
        {"vertex_sets", true}, {"vertex_set", true}, {"normal_idxs", true}, {"flags", true}, {"comment", true},
        {"skip", true}, {"_skip", true}, {"_info", true}, {"tcb_pos", true}, {"anim", true},
        {"texture_filter_name", true}, {"indexed_point_list", true}, {"point_list", true},
        {"indexed_line_list", true}, {"lod_control_header", true},
    };
    return schemas().contains(name) || other.contains(name);
}

struct Node;

// A scalar or a child block, in file order.
struct Item {
    std::string scalar;
    std::unique_ptr<Node> block;
};

struct Node {
    std::string name;  // lower case
    std::string label;
    std::vector<Item> items;
};

[[nodiscard]] std::string lower(std::string_view text) {
    std::string result(text);
    for (auto& character : result) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return result;
}

class TreeReader {
public:
    explicit TreeReader(detail::Tokenizer& tokens) {
        while (auto token = tokens.next()) tokens_.push_back(std::move(*token));
    }

    // The `shape ( ... )` block after the SIMIS header word.
    [[nodiscard]] std::unique_ptr<Node> read_shape(std::string& error) {
        for (; at_ + 1 < tokens_.size(); ++at_) {
            if (lower(tokens_[at_]) == "shape" && tokens_[at_ + 1] == "(") {
                auto node = std::make_unique<Node>();
                node->name = "shape";
                at_ += 2;
                if (!read_items(*node, error)) return nullptr;
                return node;
            }
        }
        error = "Text shape has no shape block.";
        return nullptr;
    }

private:
    [[nodiscard]] bool read_items(Node& node, std::string& error) {
        while (at_ < tokens_.size()) {
            const auto& token = tokens_[at_];
            if (token == ")") {
                ++at_;
                return true;
            }
            if (token == "(") {
                error = "Text shape has a block without a name in " + node.name + ".";
                return false;
            }
            const bool block = at_ + 1 < tokens_.size() && tokens_[at_ + 1] == "(";
            const bool labelled = !block && at_ + 2 < tokens_.size() && tokens_[at_ + 2] == "("
                && tokens_[at_ + 1] != ")" && known_block(lower(token));
            if (!block && !labelled) {
                node.items.push_back({.scalar = token});
                ++at_;
                continue;
            }
            auto child = std::make_unique<Node>();
            child->name = lower(token);
            if (labelled) child->label = tokens_[at_ + 1];
            at_ += labelled ? 3 : 2;
            if (!read_items(*child, error)) return false;
            node.items.push_back({.block = std::move(child)});
        }
        error = "Text shape ends inside " + node.name + ".";
        return false;
    }

    std::vector<std::string> tokens_;
    std::size_t at_{};
};

void put_u32(std::string& out, const std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<char>((value >> shift) & 0xFF));
}

void put_u16(std::string& out, const std::uint32_t value) {
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
}

// UTF-8 to UTF-16LE code units.
[[nodiscard]] std::vector<std::uint16_t> utf16(const std::string_view text) {
    std::vector<std::uint16_t> units;
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::uint32_t code = lead;
        int extra = 0;
        if (lead >= 0xF0) {
            code = lead & 0x07;
            extra = 3;
        } else if (lead >= 0xE0) {
            code = lead & 0x0F;
            extra = 2;
        } else if (lead >= 0xC0) {
            code = lead & 0x1F;
            extra = 1;
        }
        ++i;
        for (; extra > 0 && i < text.size(); --extra, ++i) code = (code << 6) | (static_cast<unsigned char>(text[i]) & 0x3F);
        if (code >= 0x10000) {
            code -= 0x10000;
            units.push_back(static_cast<std::uint16_t>(0xD800 + (code >> 10)));
            units.push_back(static_cast<std::uint16_t>(0xDC00 + (code & 0x3FF)));
        } else {
            units.push_back(static_cast<std::uint16_t>(code));
        }
    }
    return units;
}

[[nodiscard]] std::uint32_t integer_value(const std::string& text, const int base) {
    if (const auto value = detail::parse_integer<std::int64_t>(text, base)) return static_cast<std::uint32_t>(*value);
    if (const auto value = detail::parse_double(text)) return static_cast<std::uint32_t>(static_cast<std::int64_t>(*value));
    return 0;
}

void put_scalar(std::string& out, const char type, const std::string& text) {
    switch (type) {
    case 'f':
        put_u32(out, std::bit_cast<std::uint32_t>(static_cast<float>(detail::parse_double(text).value_or(0.0))));
        break;
    case 'x':
        put_u32(out, integer_value(text, 16));
        break;
    case 's': {
        const auto units = utf16(text);
        put_u16(out, static_cast<std::uint32_t>(units.size()));
        for (const auto unit : units) put_u16(out, unit);
        break;
    }
    default:
        put_u32(out, integer_value(text, 10));
        break;
    }
}

void write_block(std::string& out, const Node& node);

// The payload of a block known to the binary reader.
void write_known(std::string& payload, const Node& node, const std::string_view types) {
    std::size_t type_at = 0;
    const auto next_type = [&] {
        if (type_at >= types.size()) return types.empty() ? 'u' : (types.back() == '*' ? types[types.size() - 2] : 'u');
        if (types[type_at] == '*') return types[type_at - 1];
        return types[type_at++];
    };
    std::size_t blocks = 0;
    for (const auto& item : node.items) blocks += item.block ? 1 : 0;

    for (std::size_t index = 0; index < node.items.size(); ++index) {
        const auto& item = node.items[index];
        if (item.block) {
            write_block(payload, *item.block);
            continue;
        }
        const char type = next_type();
        if (type == 'C') {
            put_u32(payload, static_cast<std::uint32_t>(blocks));
        } else if (type == 'N') {
            std::uint32_t scalars = 0;
            for (std::size_t later = index + 1; later < node.items.size(); ++later) scalars += node.items[later].block ? 0 : 1;
            put_u32(payload, scalars);
        } else {
            put_scalar(payload, type, item.scalar);
        }
    }
    // A table written without its count still gets one.
    if (!types.empty() && (types[0] == 'C' || types[0] == 'N') && type_at == 0) {
        std::string counted;
        put_u32(counted, static_cast<std::uint32_t>(blocks));
        payload.insert(0, counted);
    }
}

void write_block(std::string& out, const Node& node) {
    const auto& table = schemas();
    const auto schema = table.find(node.name);
    std::string payload;
    const auto label = utf16(schema == table.end() ? std::string_view{} : std::string_view(node.label));
    const auto label_units = label.size() > 255 ? std::size_t{255} : label.size();
    payload.push_back(static_cast<char>(label_units));
    for (std::size_t i = 0; i < label_units; ++i) put_u16(payload, label[i]);
    // Unknown blocks keep their place (tables count them) but carry nothing.
    if (schema != table.end()) write_known(payload, node, schema->second.values);
    put_u32(out, schema == table.end() ? 0 : schema->second.token);
    put_u32(out, static_cast<std::uint32_t>(payload.size()));
    out += payload;
}

} // namespace

ShapeInflateResult transcode_text_shape(const std::string_view file_bytes) {
    std::string error;
    const auto text = detail::decode_text(file_bytes, error);
    if (!text) return {.error = error};
    detail::Tokenizer tokens(*text);
    TreeReader reader(tokens);
    const auto shape = reader.read_shape(error);
    if (!shape) return {.error = error};
    std::string body = "JINX0s1b______\r\n";
    write_block(body, *shape);
    return {.body = std::move(body)};
}

} // namespace openrts::formats
