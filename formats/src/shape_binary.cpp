#include <openrts/formats/shape_binary.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>

namespace openrts::formats {
namespace {

constexpr std::uint32_t token_image = 13;
constexpr std::uint32_t token_images = 14;
constexpr std::uint32_t token_point = 2;
constexpr std::uint32_t token_vector = 3;
constexpr std::uint32_t token_normals = 5;
constexpr std::uint32_t token_points = 7;
constexpr std::uint32_t token_uv_point = 8;
constexpr std::uint32_t token_uv_points = 9;
constexpr std::uint32_t token_matrices = 66;
constexpr std::uint32_t token_matrix = 65;
constexpr std::uint32_t token_lod_controls = 31;
constexpr std::uint32_t token_lod_control = 32;
constexpr std::uint32_t token_distance_levels = 36;
constexpr std::uint32_t token_distance_level = 37;
constexpr std::uint32_t token_distance_level_header = 34;
constexpr std::uint32_t token_sub_objects = 38;
constexpr std::uint32_t token_sub_object = 39;
constexpr std::uint32_t token_vertex = 48;
constexpr std::uint32_t token_vertex_uvs = 49;
constexpr std::uint32_t token_vertices = 50;
constexpr std::uint32_t token_primitives = 53;
constexpr std::uint32_t token_prim_state_idx = 56;
constexpr std::uint32_t token_indexed_trilist = 60;
constexpr std::uint32_t token_vertex_indices = 63;
constexpr std::uint32_t token_shape = 71;
constexpr std::uint32_t token_textures = 16;
constexpr std::uint32_t token_texture = 15;
constexpr std::uint32_t token_vtx_states = 47;
constexpr std::uint32_t token_vtx_state = 46;
constexpr std::uint32_t token_prim_states = 55;
constexpr std::uint32_t token_prim_state = 54;
constexpr std::uint32_t token_tex_idxs = 61;
constexpr std::uint32_t token_hierarchy = 67;
constexpr std::uint32_t token_shader_names = 72;
constexpr std::uint32_t token_shader_name = 73;
constexpr std::uint32_t token_named_shader = 129;
constexpr std::uint32_t token_texture_filter_names = 74;
constexpr std::uint32_t token_named_filter_mode = 125;
// Animations (docs/reverse/shape-animation.md).
constexpr std::uint32_t token_linear_key = 19;
constexpr std::uint32_t token_tcb_key = 20;
constexpr std::uint32_t token_linear_pos = 21;
constexpr std::uint32_t token_slerp_rot = 23;
constexpr std::uint32_t token_tcb_rot = 24;
constexpr std::uint32_t token_controllers = 25;
constexpr std::uint32_t token_anim_node = 26;
constexpr std::uint32_t token_anim_nodes = 27;
constexpr std::uint32_t token_animation = 28;
constexpr std::uint32_t token_animations = 29;

struct Block {
    std::uint32_t token{};
    std::size_t payload{};
    std::size_t end{};
    std::size_t label{};     // offset of the UTF-16 label
    std::size_t label_units{};
};

[[nodiscard]] std::optional<std::uint32_t> read_u32(std::string_view bytes, std::size_t& offset, std::size_t limit) {
    if (offset + 4 > limit) return std::nullopt;
    const auto value = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset]))
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 1])) << 8)
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 2])) << 16)
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 3])) << 24);
    offset += 4;
    return value;
}

[[nodiscard]] std::optional<float> read_f32(std::string_view bytes, std::size_t& offset, std::size_t limit) {
    const auto bits = read_u32(bytes, offset, limit);
    return bits ? std::optional<float>(std::bit_cast<float>(*bits)) : std::nullopt;
}

[[nodiscard]] std::optional<Block> read_block(std::string_view bytes, std::size_t& offset, std::size_t limit) {
    const auto token = read_u32(bytes, offset, limit);
    const auto payload_bytes = read_u32(bytes, offset, limit);
    if (!token || !payload_bytes || *payload_bytes > limit - offset) return std::nullopt;
    const auto payload_start = offset;
    const auto end = payload_start + *payload_bytes;
    if (payload_start >= end) return std::nullopt;
    const auto label_units = static_cast<unsigned char>(bytes[offset++]);
    if (label_units > (end - offset) / 2) return std::nullopt;
    const auto label = offset;
    offset += static_cast<std::size_t>(label_units) * 2;
    return Block{.token = *token, .payload = offset, .end = end, .label = label, .label_units = label_units};
}

// The block's label (node and matrix names); non-ASCII units become '?'.
[[nodiscard]] std::string block_label(std::string_view bytes, const Block& block) {
    std::string name;
    for (std::size_t i = 0; i < block.label_units; ++i) {
        const auto unit = static_cast<unsigned char>(bytes[block.label + 2 * i])
            | (static_cast<unsigned char>(bytes[block.label + 2 * i + 1]) << 8);
        name.push_back(unit < 0x80 ? static_cast<char>(unit) : '?');
    }
    return name;
}

[[nodiscard]] std::optional<std::string> read_utf16le_string(std::string_view bytes, std::size_t& offset, std::size_t limit) {
    if (offset + 2 > limit) return std::nullopt;
    const auto units = static_cast<std::size_t>(static_cast<unsigned char>(bytes[offset])
                                                | (static_cast<unsigned char>(bytes[offset + 1]) << 8));
    offset += 2;
    if (units > (limit - offset) / 2) return std::nullopt;
    std::string value;
    value.reserve(units);
    for (std::size_t index = 0; index < units; ++index) {
        const auto unit = static_cast<unsigned char>(bytes[offset])
            | (static_cast<unsigned char>(bytes[offset + 1]) << 8);
        if (unit > 0x7F) return std::nullopt; // UTF-8 conversion is a later general string layer.
        value.push_back(static_cast<char>(unit));
        offset += 2;
    }
    return value;
}

[[nodiscard]] bool is_binary_subheader(std::string_view body) {
    return body.size() >= 16 && body.substr(0, 8) == "JINX0s1b";
}

// Vertices of untextured primitives have no UV (`vertex_uvs ( 0 )`).
constexpr std::uint32_t no_uv = 0xFFFFFFFFu;

struct VertexReference {
    std::uint32_t point_index{};
    std::uint32_t uv_index{};
    std::uint32_t normal_index{};
    std::uint32_t colour1{};
    std::uint32_t colour2{};
};

using Normal = std::array<float, 3>;

[[nodiscard]] std::optional<std::vector<Normal>> read_normals(std::string_view body, const Block& table) {
    std::size_t offset = table.payload;
    const auto count = read_u32(body, offset, table.end);
    if (!count) return std::nullopt;
    std::vector<Normal> normals;
    normals.reserve(*count);
    for (std::uint32_t index = 0; index < *count; ++index) {
        const auto vector = read_block(body, offset, table.end);
        if (!vector || vector->token != token_vector) return std::nullopt;
        std::size_t vector_offset = vector->payload;
        Normal value{};
        for (float& component : value) {
            const auto parsed = read_f32(body, vector_offset, vector->end);
            if (!parsed) return std::nullopt;
            component = *parsed;
        }
        if (vector_offset != vector->end) return std::nullopt;
        normals.push_back(value);
        offset = vector->end;
    }
    return offset == table.end ? std::optional(std::move(normals)) : std::nullopt;
}

[[nodiscard]] std::optional<std::vector<ShapeMeshVertex>> read_points(
    std::string_view body, const Block& table) {
    std::size_t offset = table.payload;
    const auto count = read_u32(body, offset, table.end);
    if (!count) return std::nullopt;
    std::vector<ShapeMeshVertex> points;
    points.reserve(*count);
    for (std::uint32_t index = 0; index < *count; ++index) {
        const auto point = read_block(body, offset, table.end);
        if (!point || point->token != token_point) return std::nullopt;
        std::size_t point_offset = point->payload;
        ShapeMeshVertex value;
        for (float& component : value.position) {
            const auto parsed = read_f32(body, point_offset, point->end);
            if (!parsed) return std::nullopt;
            component = *parsed;
        }
        if (point_offset != point->end) return std::nullopt;
        points.push_back(value);
        offset = point->end;
    }
    return offset == table.end ? std::optional(std::move(points)) : std::nullopt;
}

[[nodiscard]] std::optional<std::vector<float>> read_uv_points(std::string_view body, const Block& table) {
    std::size_t offset = table.payload;
    const auto count = read_u32(body, offset, table.end);
    if (!count) return std::nullopt;
    std::vector<float> uvs;
    uvs.reserve(static_cast<std::size_t>(*count) * 2);
    for (std::uint32_t index = 0; index < *count; ++index) {
        const auto point = read_block(body, offset, table.end);
        if (!point || point->token != token_uv_point) return std::nullopt;
        std::size_t uv_offset = point->payload;
        for (unsigned component = 0; component < 2; ++component) {
            const auto parsed = read_f32(body, uv_offset, point->end);
            if (!parsed) return std::nullopt;
            uvs.push_back(*parsed);
        }
        if (uv_offset != point->end) return std::nullopt;
        offset = point->end;
    }
    return offset == table.end ? std::optional(std::move(uvs)) : std::nullopt;
}

[[nodiscard]] std::optional<std::vector<VertexReference>> read_vertices(
    std::string_view body, const Block& table) {
    std::size_t offset = table.payload;
    const auto count = read_u32(body, offset, table.end);
    if (!count) return std::nullopt;
    std::vector<VertexReference> vertices;
    vertices.reserve(*count);
    for (std::uint32_t index = 0; index < *count; ++index) {
        const auto vertex = read_block(body, offset, table.end);
        if (!vertex || vertex->token != token_vertex) return std::nullopt;
        std::size_t vertex_offset = vertex->payload;
        const auto flags = read_u32(body, vertex_offset, vertex->end);
        const auto point_index = read_u32(body, vertex_offset, vertex->end);
        const auto normal_index = read_u32(body, vertex_offset, vertex->end);
        const auto colour_1 = read_u32(body, vertex_offset, vertex->end);
        const auto colour_2 = read_u32(body, vertex_offset, vertex->end);
        static_cast<void>(flags);
        if (!point_index || !normal_index || !colour_1 || !colour_2) return std::nullopt;
        const auto vertex_uvs = read_block(body, vertex_offset, vertex->end);
        if (!vertex_uvs || vertex_uvs->token != token_vertex_uvs) return std::nullopt;
        std::size_t uv_offset = vertex_uvs->payload;
        const auto uv_count = read_u32(body, uv_offset, vertex_uvs->end);
        const auto uv_index = !uv_count ? std::nullopt
            : *uv_count > 0         ? read_u32(body, uv_offset, vertex_uvs->end)
                                    : std::optional(no_uv);
        if (!uv_index) return std::nullopt;
        for (std::uint32_t uv = 1; uv < *uv_count; ++uv) {
            if (!read_u32(body, uv_offset, vertex_uvs->end)) return std::nullopt;
        }
        if (uv_offset != vertex_uvs->end) return std::nullopt;
        vertex_offset = vertex_uvs->end;
        if (vertex_offset != vertex->end && vertex_offset + 4 != vertex->end) return std::nullopt;
        vertices.push_back({
            .point_index = *point_index,
            .uv_index = *uv_index,
            .normal_index = *normal_index,
            .colour1 = *colour_1,
            .colour2 = *colour_2,
        });
        offset = vertex->end;
    }
    return offset == table.end ? std::optional(std::move(vertices)) : std::nullopt;
}

[[nodiscard]] std::optional<std::vector<std::uint32_t>> read_first_triangle_list(
    std::string_view body, const Block& primitives) {
    std::size_t offset = primitives.payload;
    const auto count = read_u32(body, offset, primitives.end);
    if (!count) return std::nullopt;
    for (std::uint32_t item = 0; item < *count; ++item) {
        const auto primitive = read_block(body, offset, primitives.end);
        if (!primitive) return std::nullopt;
        offset = primitive->end;
        if (primitive->token != token_indexed_trilist) continue;

        std::size_t list_offset = primitive->payload;
        const auto vertex_indices = read_block(body, list_offset, primitive->end);
        if (!vertex_indices || vertex_indices->token != token_vertex_indices) return std::nullopt;
        std::size_t index_offset = vertex_indices->payload;
        const auto index_count = read_u32(body, index_offset, vertex_indices->end);
        if (!index_count || *index_count == 0 || *index_count % 3 != 0) return std::nullopt;
        std::vector<std::uint32_t> indices;
        indices.reserve(*index_count);
        for (std::uint32_t index = 0; index < *index_count; ++index) {
            const auto value = read_u32(body, index_offset, vertex_indices->end);
            if (!value) return std::nullopt;
            indices.push_back(*value);
        }
        if (index_offset != vertex_indices->end) return std::nullopt;
        return indices;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::pair<std::vector<VertexReference>, std::vector<std::uint32_t>>>
read_first_subobject(std::string_view body, const Block& sub_objects) {
    std::size_t offset = sub_objects.payload;
    const auto count = read_u32(body, offset, sub_objects.end);
    if (!count) return std::nullopt;
    for (std::uint32_t index = 0; index < *count; ++index) {
        const auto sub_object = read_block(body, offset, sub_objects.end);
        if (!sub_object || sub_object->token != token_sub_object) return std::nullopt;
        offset = sub_object->end;
        std::optional<std::vector<VertexReference>> vertices;
        std::optional<std::vector<std::uint32_t>> indices;
        std::size_t child_offset = sub_object->payload;
        while (child_offset < sub_object->end) {
            const auto child = read_block(body, child_offset, sub_object->end);
            if (!child) return std::nullopt;
            child_offset = child->end;
            if (child->token == token_vertices) vertices = read_vertices(body, *child);
            else if (child->token == token_primitives) indices = read_first_triangle_list(body, *child);
            if ((child->token == token_vertices && !vertices) || (child->token == token_primitives && !indices))
                return std::nullopt;
        }
        if (vertices && indices) return std::pair(std::move(*vertices), std::move(*indices));
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::pair<std::vector<VertexReference>, std::vector<std::uint32_t>>>
read_first_lod_subobject(std::string_view body, const Block& lod_controls) {
    std::size_t controls_offset = lod_controls.payload;
    const auto controls_count = read_u32(body, controls_offset, lod_controls.end);
    if (!controls_count) return std::nullopt;
    for (std::uint32_t control_index = 0; control_index < *controls_count; ++control_index) {
        const auto control = read_block(body, controls_offset, lod_controls.end);
        if (!control || control->token != token_lod_control) return std::nullopt;
        controls_offset = control->end;
        std::size_t control_offset = control->payload;
        while (control_offset < control->end) {
            const auto child = read_block(body, control_offset, control->end);
            if (!child) return std::nullopt;
            control_offset = child->end;
            if (child->token != token_distance_levels) continue;
            std::size_t levels_offset = child->payload;
            const auto levels_count = read_u32(body, levels_offset, child->end);
            if (!levels_count) return std::nullopt;
            for (std::uint32_t level_index = 0; level_index < *levels_count; ++level_index) {
                const auto level = read_block(body, levels_offset, child->end);
                if (!level || level->token != token_distance_level) return std::nullopt;
                levels_offset = level->end;
                std::size_t level_offset = level->payload;
                while (level_offset < level->end) {
                    const auto level_child = read_block(body, level_offset, level->end);
                    if (!level_child) return std::nullopt;
                    level_offset = level_child->end;
                    if (level_child->token == token_sub_objects) {
                        if (const auto mesh = read_first_subobject(body, *level_child)) return mesh;
                    }
                }
            }
        }
    }
    return std::nullopt;
}

struct RawPrimitiveList {
    std::uint32_t primitive_state{};
    std::vector<std::uint32_t> indices;
};

struct RawSubobject {
    std::vector<VertexReference> vertices;
    std::vector<RawPrimitiveList> lists;
};

struct RawLodLevel {
    std::vector<RawSubobject> subobjects;
    std::vector<std::int32_t> hierarchy;
    float selection{}; // dlevel_selection, m
};

[[nodiscard]] std::optional<std::vector<std::int32_t>> read_hierarchy(
    std::string_view body, const Block& hierarchy) {
    std::size_t offset = hierarchy.payload;
    const auto count = read_u32(body, offset, hierarchy.end);
    if (!count) return std::nullopt;
    std::vector<std::int32_t> result;
    result.reserve(*count);
    for (std::uint32_t index = 0; index < *count; ++index) {
        const auto value = read_u32(body, offset, hierarchy.end);
        if (!value) return std::nullopt;
        result.push_back(static_cast<std::int32_t>(*value));
    }
    return offset == hierarchy.end ? std::optional(std::move(result)) : std::nullopt;
}

[[nodiscard]] std::optional<std::vector<std::uint32_t>> read_triangle_indices(
    std::string_view body, const Block& primitive) {
    std::size_t offset = primitive.payload;
    while (offset < primitive.end) {
        const auto child = read_block(body, offset, primitive.end);
        if (!child) return std::nullopt;
        offset = child->end;
        if (child->token != token_vertex_indices) continue;
        std::size_t index_offset = child->payload;
        const auto count = read_u32(body, index_offset, child->end);
        if (!count || *count == 0 || *count % 3 != 0) return std::nullopt;
        std::vector<std::uint32_t> result;
        result.reserve(*count);
        for (std::uint32_t index = 0; index < *count; ++index) {
            const auto value = read_u32(body, index_offset, child->end);
            if (!value) return std::nullopt;
            result.push_back(*value);
        }
        return index_offset == child->end ? std::optional(std::move(result)) : std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<RawSubobject> read_subobject_all(
    std::string_view body, const Block& sub_object) {
    RawSubobject result;
    std::uint32_t current_state = 0;
    std::size_t offset = sub_object.payload;
    while (offset < sub_object.end) {
        const auto child = read_block(body, offset, sub_object.end);
        if (!child) return std::nullopt;
        offset = child->end;
        if (child->token == token_vertices) {
            const auto vertices = read_vertices(body, *child);
            if (!vertices) return std::nullopt;
            result.vertices = *vertices;
        } else if (child->token == token_primitives) {
            std::size_t primitive_offset = child->payload;
            const auto count = read_u32(body, primitive_offset, child->end);
            if (!count) return std::nullopt;
            for (std::uint32_t index = 0; index < *count; ++index) {
                const auto primitive = read_block(body, primitive_offset, child->end);
                if (!primitive) return std::nullopt;
                primitive_offset = primitive->end;
                if (primitive->token == token_prim_state_idx) {
                    std::size_t state_offset = primitive->payload;
                    const auto state = read_u32(body, state_offset, primitive->end);
                    if (!state || state_offset != primitive->end) return std::nullopt;
                    current_state = *state;
                } else if (primitive->token == token_indexed_trilist) {
                    const auto indices = read_triangle_indices(body, *primitive);
                    if (!indices) return std::nullopt;
                    result.lists.push_back({.primitive_state = current_state, .indices = *indices});
                }
            }
            if (primitive_offset != child->end) return std::nullopt;
        }
    }
    return result.vertices.empty() ? std::nullopt : std::optional(std::move(result));
}

constexpr std::uint32_t token_dlevel_selection = 35;

// Every distance level of the first lod_control (MSTS shapes have one).
[[nodiscard]] std::optional<std::vector<RawLodLevel>> read_lod_levels(std::string_view body, const Block& lod_controls) {
    std::size_t controls_offset = lod_controls.payload;
    const auto controls_count = read_u32(body, controls_offset, lod_controls.end);
    if (!controls_count || *controls_count == 0) return std::nullopt;
    const auto control = read_block(body, controls_offset, lod_controls.end);
    if (!control || control->token != token_lod_control) return std::nullopt;
    std::size_t control_offset = control->payload;
    while (control_offset < control->end) {
        const auto child = read_block(body, control_offset, control->end);
        if (!child) return std::nullopt;
        control_offset = child->end;
        if (child->token != token_distance_levels) continue;
        std::size_t levels_offset = child->payload;
        const auto count = read_u32(body, levels_offset, child->end);
        if (!count || *count == 0) return std::nullopt;
        std::vector<RawLodLevel> levels;
        for (std::uint32_t index = 0; index < *count; ++index) {
            const auto level = read_block(body, levels_offset, child->end);
            if (!level || level->token != token_distance_level) return std::nullopt;
            levels_offset = level->end;
            RawLodLevel result;
            std::size_t level_offset = level->payload;
            while (level_offset < level->end) {
                const auto level_child = read_block(body, level_offset, level->end);
                if (!level_child) return std::nullopt;
                level_offset = level_child->end;
                if (level_child->token == token_distance_level_header) {
                    std::size_t header_offset = level_child->payload;
                    while (header_offset < level_child->end) {
                        const auto header_child = read_block(body, header_offset, level_child->end);
                        if (!header_child) return std::nullopt;
                        header_offset = header_child->end;
                        if (header_child->token == token_hierarchy) {
                            const auto hierarchy = read_hierarchy(body, *header_child);
                            if (!hierarchy) return std::nullopt;
                            result.hierarchy = *hierarchy;
                        } else if (header_child->token == token_dlevel_selection) {
                            std::size_t at = header_child->payload;
                            result.selection = read_f32(body, at, header_child->end).value_or(0.0f);
                        }
                    }
                    continue;
                }
                if (level_child->token != token_sub_objects) continue;
                std::size_t object_offset = level_child->payload;
                const auto object_count = read_u32(body, object_offset, level_child->end);
                if (!object_count) return std::nullopt;
                result.subobjects.reserve(*object_count);
                for (std::uint32_t object = 0; object < *object_count; ++object) {
                    const auto subobject = read_block(body, object_offset, level_child->end);
                    if (!subobject || subobject->token != token_sub_object) return std::nullopt;
                    object_offset = subobject->end;
                    const auto parsed = read_subobject_all(body, *subobject);
                    if (!parsed) return std::nullopt;
                    result.subobjects.push_back(*parsed);
                }
                if (object_offset != level_child->end) return std::nullopt;
            }
            levels.push_back(std::move(result));
        }
        if (levels.front().subobjects.empty()) return std::nullopt;
        return levels;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<RawLodLevel> read_first_lod_subobjects(
    std::string_view body, const Block& lod_controls) {
    std::size_t controls_offset = lod_controls.payload;
    const auto controls_count = read_u32(body, controls_offset, lod_controls.end);
    if (!controls_count) return std::nullopt;
    for (std::uint32_t control_index = 0; control_index < *controls_count; ++control_index) {
        const auto control = read_block(body, controls_offset, lod_controls.end);
        if (!control || control->token != token_lod_control) return std::nullopt;
        controls_offset = control->end;
        std::size_t control_offset = control->payload;
        while (control_offset < control->end) {
            const auto child = read_block(body, control_offset, control->end);
            if (!child) return std::nullopt;
            control_offset = child->end;
            if (child->token != token_distance_levels) continue;
            std::size_t levels_offset = child->payload;
            const auto count = read_u32(body, levels_offset, child->end);
            if (!count) return std::nullopt;
            const auto level = read_block(body, levels_offset, child->end);
            if (!level || level->token != token_distance_level) return std::nullopt;
            std::size_t level_offset = level->payload;
            RawLodLevel result;
            while (level_offset < level->end) {
                const auto level_child = read_block(body, level_offset, level->end);
                if (!level_child) return std::nullopt;
                level_offset = level_child->end;
                if (level_child->token == token_distance_level_header) {
                    std::size_t header_offset = level_child->payload;
                    while (header_offset < level_child->end) {
                        const auto header_child = read_block(body, header_offset, level_child->end);
                        if (!header_child) return std::nullopt;
                        header_offset = header_child->end;
                        if (header_child->token == token_hierarchy) {
                            const auto hierarchy = read_hierarchy(body, *header_child);
                            if (!hierarchy) return std::nullopt;
                            result.hierarchy = *hierarchy;
                        }
                    }
                    continue;
                }
                if (level_child->token != token_sub_objects) continue;
                std::size_t object_offset = level_child->payload;
                const auto object_count = read_u32(body, object_offset, level_child->end);
                if (!object_count) return std::nullopt;
                result.subobjects.reserve(*object_count);
                for (std::uint32_t object = 0; object < *object_count; ++object) {
                    const auto subobject = read_block(body, object_offset, level_child->end);
                    if (!subobject || subobject->token != token_sub_object) return std::nullopt;
                    object_offset = subobject->end;
                    const auto parsed = read_subobject_all(body, *subobject);
                    if (!parsed) return std::nullopt;
                    result.subobjects.push_back(*parsed);
                }
                return object_offset == level_child->end ? std::optional(std::move(result)) : std::nullopt;
            }
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::vector<ShapeMatrix>> read_matrices(std::string_view body, const Block& table) {
    std::size_t offset = table.payload;
    const auto count = read_u32(body, offset, table.end);
    if (!count) return std::nullopt;
    std::vector<ShapeMatrix> result;
    result.reserve(*count);
    for (std::uint32_t index = 0; index < *count; ++index) {
        const auto matrix = read_block(body, offset, table.end);
        if (!matrix || matrix->token != token_matrix) return std::nullopt;
        std::size_t matrix_offset = matrix->payload;
        ShapeMatrix value;
        for (float& component : value.values) {
            const auto parsed = read_f32(body, matrix_offset, matrix->end);
            if (!parsed) return std::nullopt;
            component = *parsed;
        }
        if (matrix_offset != matrix->end) return std::nullopt;
        value.name = block_label(body, *matrix);
        result.push_back(value);
        offset = matrix->end;
    }
    return offset == table.end ? std::optional(std::move(result)) : std::nullopt;
}

[[nodiscard]] std::optional<std::vector<ShapeTextureState>> read_textures(std::string_view body, const Block& table) {
    std::size_t offset = table.payload;
    const auto count = read_u32(body, offset, table.end);
    if (!count) return std::nullopt;
    std::vector<ShapeTextureState> result;
    result.reserve(*count);
    for (std::uint32_t index = 0; index < *count; ++index) {
        const auto texture = read_block(body, offset, table.end);
        if (!texture || texture->token != token_texture) return std::nullopt;
        std::size_t texture_offset = texture->payload;
        const auto image_index = read_u32(body, texture_offset, texture->end);
        const auto filter_mode = read_u32(body, texture_offset, texture->end);
        const auto lod_bias = read_f32(body, texture_offset, texture->end);
        if (!image_index || !filter_mode || !lod_bias) return std::nullopt;
        result.push_back({
            .image_index = *image_index,
            .filter_mode_index = *filter_mode,
            .mipmap_lod_bias = *lod_bias,
        });
        offset = texture->end;
    }
    return offset == table.end ? std::optional(std::move(result)) : std::nullopt;
}

// Reads a counted table of single-string blocks (shader or filter names).
[[nodiscard]] std::optional<std::vector<std::string>> read_name_table(std::string_view body,
                                                                      const Block& table,
                                                                      const std::uint32_t entry_token,
                                                                      const std::uint32_t alternative_token) {
    std::size_t offset = table.payload;
    const auto count = read_u32(body, offset, table.end);
    if (!count) return std::nullopt;
    std::vector<std::string> result;
    result.reserve(*count);
    for (std::uint32_t index = 0; index < *count; ++index) {
        const auto entry = read_block(body, offset, table.end);
        if (!entry || (entry->token != entry_token && entry->token != alternative_token)) return std::nullopt;
        std::size_t entry_offset = entry->payload;
        const auto name = read_utf16le_string(body, entry_offset, entry->end);
        if (!name || entry_offset != entry->end) return std::nullopt;
        result.push_back(*name);
        offset = entry->end;
    }
    return offset == table.end ? std::optional(std::move(result)) : std::nullopt;
}

[[nodiscard]] std::optional<std::vector<std::string>> read_shader_names(std::string_view body, const Block& table) {
    return read_name_table(body, table, token_shader_name, token_named_shader);
}

[[nodiscard]] std::optional<std::vector<std::string>> read_filter_names(std::string_view body, const Block& table) {
    return read_name_table(body, table, token_named_filter_mode, token_named_filter_mode);
}

[[nodiscard]] std::optional<std::vector<ShapeVertexState>> read_vtx_states(std::string_view body,
                                                                           const Block& table) {
    std::size_t offset = table.payload;
    const auto count = read_u32(body, offset, table.end);
    if (!count) return std::nullopt;
    std::vector<ShapeVertexState> result;
    result.reserve(*count);
    for (std::uint32_t index = 0; index < *count; ++index) {
        const auto state = read_block(body, offset, table.end);
        if (!state || state->token != token_vtx_state) return std::nullopt;
        std::size_t state_offset = state->payload;
        const auto flags = read_u32(body, state_offset, state->end);
        const auto matrix_index = read_u32(body, state_offset, state->end);
        const auto light_material = read_u32(body, state_offset, state->end);
        const auto light_config = read_u32(body, state_offset, state->end);
        if (!flags || !matrix_index || !light_material || !light_config) return std::nullopt;
        result.push_back({
            .flags = *flags,
            .matrix_index = static_cast<std::int32_t>(*matrix_index),
            .light_material_index = static_cast<std::int32_t>(*light_material),
            .light_config_index = *light_config,
        });
        offset = state->end;
    }
    return offset == table.end ? std::optional(std::move(result)) : std::nullopt;
}

[[nodiscard]] std::optional<std::vector<ShapePrimitiveState>> read_prim_states(
    std::string_view body, const Block& table) {
    std::size_t offset = table.payload;
    const auto count = read_u32(body, offset, table.end);
    if (!count) return std::nullopt;
    std::vector<ShapePrimitiveState> result;
    result.reserve(*count);
    for (std::uint32_t index = 0; index < *count; ++index) {
        const auto state = read_block(body, offset, table.end);
        if (!state || state->token != token_prim_state) return std::nullopt;
        std::size_t state_offset = state->payload;
        const auto flags = read_u32(body, state_offset, state->end);
        const auto shader = read_u32(body, state_offset, state->end);
        const auto texture_indices = read_block(body, state_offset, state->end);
        if (!texture_indices || texture_indices->token != token_tex_idxs) return std::nullopt;
        std::size_t texture_offset = texture_indices->payload;
        const auto texture_count = read_u32(body, texture_offset, texture_indices->end);
        const auto texture_index = texture_count && *texture_count > 0
            ? read_u32(body, texture_offset, texture_indices->end) : std::optional<std::uint32_t>(0);
        if (!texture_index || texture_offset > texture_indices->end) return std::nullopt;
        for (std::uint32_t texture = 1; texture < *texture_count; ++texture) {
            if (!read_u32(body, texture_offset, texture_indices->end)) return std::nullopt;
        }
        if (texture_offset != texture_indices->end) return std::nullopt;
        state_offset = texture_indices->end;
        const auto z_bias = read_f32(body, state_offset, state->end);
        const auto vertex_state = read_u32(body, state_offset, state->end);
        if (!texture_index || !vertex_state) return std::nullopt;
        ShapePrimitiveState parsed{.flags = *flags, .texture_index = *texture_index,
                                   .texture_count = *texture_count,
                                   .shader_index = *shader,
                                   .vertex_state_index = static_cast<std::int32_t>(*vertex_state),
                                   .z_bias = *z_bias};
        if (state_offset + 4 <= state->end) {
            const auto alpha = read_u32(body, state_offset, state->end);
            if (!alpha) return std::nullopt;
            parsed.alpha_test_mode = *alpha;
        }
        if (state_offset + 8 <= state->end) {
            if (!read_u32(body, state_offset, state->end)) return std::nullopt;
            const auto z_mode = read_u32(body, state_offset, state->end);
            if (!z_mode) return std::nullopt;
            parsed.z_buffer_mode = *z_mode;
        }
        result.push_back(parsed);
        offset = state->end;
    }
    return offset == table.end ? std::optional(std::move(result)) : std::nullopt;
}

} // namespace

ShapeImageTable extract_image_filenames_from_binary_shape(std::string_view body) {
    if (!is_binary_subheader(body)) return {.error = "Binary shape body has no JINX0s1b subheader."};
    std::size_t offset = 16;
    const auto root = read_block(body, offset, body.size());
    if (!root || root->token != token_shape) return {.error = "Binary shape has no bounded root shape block."};

    offset = root->payload;
    while (offset < root->end) {
        const auto child = read_block(body, offset, root->end);
        if (!child) return {.error = "Malformed root child block."};
        if (child->token != token_images) {
            offset = child->end;
            continue;
        }

        std::size_t table_offset = child->payload;
        const auto count = read_u32(body, table_offset, child->end);
        if (!count) return {.error = "Images table has no count."};
        ShapeImageTable result;
        for (std::uint32_t index = 0; index < *count; ++index) {
            const auto image = read_block(body, table_offset, child->end);
            if (!image || image->token != token_image) return {.error = "Images table has an invalid image entry."};
            std::size_t image_offset = image->payload;
            const auto filename = read_utf16le_string(body, image_offset, image->end);
            if (!filename || image_offset != image->end) return {.error = "Image entry has an invalid filename."};
            result.filenames.emplace_back(*filename);
            table_offset = image->end;
        }
        if (table_offset != child->end) return {.error = "Images table has trailing bytes."};
        return result;
    }
    return {.error = "Binary shape has no images table."};
}

ShapeMesh extract_first_mesh_from_binary_shape(const std::string_view body) {
    if (!is_binary_subheader(body)) return {.error = "Binary shape body has no JINX0s1b subheader."};
    std::size_t offset = 16;
    const auto root = read_block(body, offset, body.size());
    if (!root || root->token != token_shape) return {.error = "Binary shape has no bounded root shape block."};

    std::optional<std::vector<ShapeMeshVertex>> points;
    std::optional<std::vector<float>> uv_points;
    std::optional<std::pair<std::vector<VertexReference>, std::vector<std::uint32_t>>> source_mesh;
    offset = root->payload;
    while (offset < root->end) {
        const auto child = read_block(body, offset, root->end);
        if (!child) return {.error = "Malformed root child block."};
        offset = child->end;
        if (child->token == token_points) points = read_points(body, *child);
        else if (child->token == token_uv_points) uv_points = read_uv_points(body, *child);
        else if (child->token == token_lod_controls) source_mesh = read_first_lod_subobject(body, *child);
        if ((child->token == token_points && !points) || (child->token == token_uv_points && !uv_points)
            || (child->token == token_lod_controls && !source_mesh))
            return {.error = "First-shape-viewer binary table has an unsupported layout."};
    }
    if (!points || !uv_points || !source_mesh) return {.error = "Binary shape lacks points, UVs, or a triangle-list subobject."};

    ShapeMesh mesh;
    mesh.vertices.reserve(source_mesh->first.size());
    for (const auto& reference : source_mesh->first) {
        const bool has_uv = reference.uv_index != no_uv;
        if (reference.point_index >= points->size()
            || (has_uv && static_cast<std::size_t>(reference.uv_index) * 2 + 1 >= uv_points->size()))
            return {.error = "Binary shape vertex references an out-of-range point or UV."};
        auto vertex = (*points)[reference.point_index];
        if (has_uv) {
            vertex.uv[0] = (*uv_points)[static_cast<std::size_t>(reference.uv_index) * 2];
            vertex.uv[1] = (*uv_points)[static_cast<std::size_t>(reference.uv_index) * 2 + 1];
        }
        mesh.vertices.push_back(vertex);
    }
    mesh.indices = std::move(source_mesh->second);
    for (const auto index : mesh.indices) {
        if (index >= mesh.vertices.size()) return {.error = "Binary shape triangle index is out of range."};
    }
    return mesh;
}

namespace {

[[nodiscard]] bool same_name(const std::string_view a, const std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

// Reads one controller's keys. Unknown key types make the controller empty.
void read_controller(std::string_view body, const Block& controller, ShapeAnimationNode& node) {
    std::size_t offset = controller.payload;
    const auto count = read_u32(body, offset, controller.end);
    if (!count) return;
    for (std::uint32_t i = 0; i < *count && offset < controller.end; ++i) {
        const auto key = read_block(body, offset, controller.end);
        if (!key) return;
        std::size_t at = key->payload;
        const auto frame = read_u32(body, at, key->end);
        if (!frame) return;
        if (controller.token == token_linear_pos && key->token == token_linear_key) {
            ShapePositionKey position{.frame = *frame};
            for (auto& value : position.value) value = read_f32(body, at, key->end).value_or(0.0f);
            node.positions.push_back(position);
        } else if (controller.token == token_tcb_rot && (key->token == token_tcb_key || key->token == token_slerp_rot)) {
            // tcb_key also carries tension, continuity, bias, in, out: all
            // zero in installed content, so keys are interpolated with slerp.
            ShapeRotationKey rotation{.frame = *frame};
            for (auto& value : rotation.value) value = read_f32(body, at, key->end).value_or(0.0f);
            node.rotations.push_back(rotation);
        }
        offset = key->end;
    }
}

// Block 29. Nodes bind to matrices by name, ignoring case, as RTS does
// (0x43A944). A malformed block yields no animations; the model still loads.
[[nodiscard]] std::vector<ShapeAnimation> read_animations(std::string_view body, const Block& table,
                                                          const std::vector<ShapeMatrix>& matrices) {
    std::vector<ShapeAnimation> result;
    std::size_t offset = table.payload;
    const auto count = read_u32(body, offset, table.end);
    if (!count) return {};
    for (std::uint32_t a = 0; a < *count && offset < table.end; ++a) {
        const auto block = read_block(body, offset, table.end);
        if (!block || block->token != token_animation) return {};
        offset = block->end;
        std::size_t at = block->payload;
        const auto frames = read_u32(body, at, block->end);
        const auto rate = read_u32(body, at, block->end);
        if (!frames || !rate) return {};
        ShapeAnimation animation{.frame_count = *frames, .frame_rate = *rate};
        const auto nodes = read_block(body, at, block->end);
        if (!nodes || nodes->token != token_anim_nodes) return {};
        std::size_t node_offset = nodes->payload;
        const auto node_count = read_u32(body, node_offset, nodes->end);
        if (!node_count) return {};
        for (std::uint32_t n = 0; n < *node_count && node_offset < nodes->end; ++n) {
            const auto node_block = read_block(body, node_offset, nodes->end);
            if (!node_block || node_block->token != token_anim_node) return {};
            node_offset = node_block->end;
            ShapeAnimationNode node{.name = block_label(body, *node_block)};
            for (std::size_t m = 0; m < matrices.size(); ++m) {
                if (same_name(matrices[m].name, node.name)) {
                    node.matrix_index = static_cast<std::int32_t>(m);
                    break;
                }
            }
            std::size_t inner = node_block->payload;
            while (inner < node_block->end) {
                const auto controllers = read_block(body, inner, node_block->end);
                if (!controllers) break;
                inner = controllers->end;
                if (controllers->token != token_controllers) continue;
                std::size_t c = controllers->payload;
                const auto controller_count = read_u32(body, c, controllers->end);
                for (std::uint32_t k = 0; controller_count && k < *controller_count && c < controllers->end; ++k) {
                    const auto controller = read_block(body, c, controllers->end);
                    if (!controller) break;
                    c = controller->end;
                    read_controller(body, *controller, node);
                }
            }
            if (!node.positions.empty() || !node.rotations.empty()) animation.nodes.push_back(std::move(node));
        }
        result.push_back(std::move(animation));
    }
    return result;
}

} // namespace

ShapeModel parse_binary_shape_model(const std::string_view body) {
    if (!is_binary_subheader(body)) return {.error = "Binary shape body has no JINX0s1b subheader."};
    std::size_t offset = 16;
    const auto root = read_block(body, offset, body.size());
    if (!root || root->token != token_shape) return {.error = "Binary shape has no bounded root shape block."};

    std::optional<std::vector<ShapeMeshVertex>> points;
    std::optional<std::vector<float>> uv_points;
    std::optional<std::vector<ShapeMatrix>> matrices;
    std::optional<std::vector<ShapeTextureState>> textures;
    std::optional<std::vector<std::string>> shader_names;
    std::optional<std::vector<Normal>> normals;
    std::optional<std::vector<std::string>> filter_names;
    std::optional<std::vector<ShapeVertexState>> vertex_states;
    std::optional<std::vector<ShapePrimitiveState>> primitive_states;
    std::optional<std::vector<RawLodLevel>> levels;
    std::optional<Block> animations_block;

    offset = root->payload;
    while (offset < root->end) {
        const auto child = read_block(body, offset, root->end);
        if (!child) return {.error = "Malformed root child block."};
        offset = child->end;
        if (child->token == token_points) points = read_points(body, *child);
        else if (child->token == token_uv_points) uv_points = read_uv_points(body, *child);
        else if (child->token == token_normals) normals = read_normals(body, *child);
        else if (child->token == token_texture_filter_names) filter_names = read_filter_names(body, *child);
        else if (child->token == token_matrices) matrices = read_matrices(body, *child);
        else if (child->token == token_textures) textures = read_textures(body, *child);
        else if (child->token == token_shader_names) shader_names = read_shader_names(body, *child);
        else if (child->token == token_vtx_states) vertex_states = read_vtx_states(body, *child);
        else if (child->token == token_prim_states) primitive_states = read_prim_states(body, *child);
        else if (child->token == token_lod_controls) levels = read_lod_levels(body, *child);
        else if (child->token == token_animations) animations_block = *child;
        if ((child->token == token_points && !points) || (child->token == token_uv_points && !uv_points)
            || (child->token == token_matrices && !matrices) || (child->token == token_textures && !textures)
            || (child->token == token_shader_names && !shader_names)
            || (child->token == token_normals && !normals)
            || (child->token == token_texture_filter_names && !filter_names)
            || (child->token == token_vtx_states && !vertex_states) || (child->token == token_prim_states && !primitive_states)
            || (child->token == token_lod_controls && !levels)) {
            return {.error = "Shape table " + std::to_string(child->token) + " uses an unsupported binary layout."};
        }
    }
    if (!points || !uv_points || !levels) {
        return {.error = "Binary shape lacks points, UVs, or subobjects in its first distance level."};
    }

    ShapeModel model;
    const auto images = extract_image_filenames_from_binary_shape(body);
    if (images.ok()) model.images = images.filenames;
    if (matrices) model.matrices = *matrices;
    if (model.matrices.empty()) {
        ShapeMatrix identity{};
        identity.values[0] = identity.values[4] = identity.values[8] = 1.0f;
        model.matrices.push_back(identity);
    }
    model.hierarchy = levels->front().hierarchy;
    if (model.hierarchy.empty()) model.hierarchy.assign(model.matrices.size(), -1);
    if (model.hierarchy.size() != model.matrices.size()) {
        return {.error = "Distance-level hierarchy count does not match the matrix table."};
    }
    for (std::size_t index = 0; index < model.hierarchy.size(); ++index) {
        // -2 marks a matrix this distance level does not use (A1t300r*dConcrete.s).
        if (model.hierarchy[index] < -1) model.hierarchy[index] = -1;
        const auto parent = model.hierarchy[index];
        if (parent >= static_cast<std::int32_t>(model.matrices.size())) {
            return {.error = "Distance-level hierarchy has an out-of-range parent matrix."};
        }
    }
    // RTS never applies matrix 0: a node's world matrix is its chain up to,
    // not including, matrix 0 (0x4ACDF8), and the draws multiply a
    // primitive's matrix only when its index is not 0 (0x4B1664, 0x4B042C,
    // 0x4ADA94). So matrix 0 is the identity and nothing is above it; its
    // name stays (animations and DLLs find nodes by name). TSRE5 draws it so
    // too. Kievskij hod's pineline6.s has MAIN at (0, 13.5, 0) over trees
    // from y 0: with it the tree line hung 13.5 m above the ground.
    {
        auto& first = model.matrices.front();
        for (std::size_t k = 0; k < 12; ++k) first.values[k] = (k == 0 || k == 4 || k == 8) ? 1.0f : 0.0f;
        model.hierarchy.front() = -1;
    }
    if (animations_block) model.animations = read_animations(body, *animations_block, model.matrices);
    if (textures) model.textures = *textures;
    if (shader_names) model.shader_names = *shader_names;
    if (filter_names) model.filter_names = *filter_names;
    if (vertex_states) model.vertex_states = *vertex_states;
    if (primitive_states) model.primitive_states = *primitive_states;
    if (model.primitive_states.empty()) model.primitive_states.push_back({});

    if (levels->size() > 1) {
        for (const auto& each : *levels) model.level_distances.push_back(each.selection);
    }
    for (std::size_t level_index = 0; level_index < levels->size() && level_index < 255; ++level_index) {
    for (const auto& subobject : (*levels)[level_index].subobjects) {
        const auto base_vertex = static_cast<std::uint32_t>(model.vertices.size());
        for (const auto& reference : subobject.vertices) {
            const bool has_uv = reference.uv_index != no_uv;
            if (reference.point_index >= points->size()
                || (has_uv && static_cast<std::size_t>(reference.uv_index) * 2 + 1 >= uv_points->size())) {
                return {.error = "Binary shape vertex references an out-of-range point or UV."};
            }
            auto vertex = (*points)[reference.point_index];
            if (has_uv) {
                vertex.uv[0] = (*uv_points)[static_cast<std::size_t>(reference.uv_index) * 2];
                vertex.uv[1] = (*uv_points)[static_cast<std::size_t>(reference.uv_index) * 2 + 1];
            }
            if (normals && reference.normal_index < normals->size()) {
                const auto& normal = (*normals)[reference.normal_index];
                std::copy(normal.begin(), normal.end(), vertex.normal);
            }
            vertex.colour1 = reference.colour1;
            vertex.colour2 = reference.colour2;
            model.vertices.push_back(vertex);
        }
        for (const auto& list : subobject.lists) {
            if (list.primitive_state >= model.primitive_states.size()) {
                return {.error = "Subobject references an out-of-range primitive state."};
            }
            const auto& primitive_state = model.primitive_states[list.primitive_state];
            std::int32_t matrix_index = 0;
            if (primitive_state.vertex_state_index >= 0
                && static_cast<std::size_t>(primitive_state.vertex_state_index) < model.vertex_states.size()) {
                matrix_index = model.vertex_states[primitive_state.vertex_state_index].matrix_index;
            }
            if (matrix_index < 0 || static_cast<std::size_t>(matrix_index) >= model.matrices.size()) {
                return {.error = "Primitive state references an out-of-range matrix."};
            }
            const auto first_index = static_cast<std::uint32_t>(model.indices.size());
            for (const auto index : list.indices) {
                if (index >= subobject.vertices.size()) return {.error = "Binary shape triangle index is out of range."};
                model.indices.push_back(base_vertex + index);
            }
            model.draws.push_back({.first_index = first_index,
                                   .index_count = static_cast<std::uint32_t>(list.indices.size()),
                                   .primitive_state_index = list.primitive_state,
                                   .matrix_index = matrix_index,
                                   .level = static_cast<std::uint8_t>(level_index)});
        }
    }
    }
    if (!model.ok()) model.error = "First shape distance level has no indexed triangle lists.";
    return model;
}

} // namespace openrts::formats
