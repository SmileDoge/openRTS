// openrts-train-check <root> --cab <TRAINSET folder> <eng>
//
// The 3-D cab of an engine: its cab shape (EngExtended Cabin, through a
// .repaint when there is one) and .sd, and how the .sd's switches and
// displays find their nodes in the shape (RTS binds them by name). Prints
// what is bound and what is not, and the shape's animation frames.

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <cctype>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <openrts/audio/interior_acoustics.hpp>
#include <openrts/core/vfs.hpp>
#include <openrts/formats/cab_description.hpp>
#include <openrts/formats/rolling_stock.hpp>
#include <openrts/formats/shape_binary.hpp>
#include <openrts/formats/shape_file.hpp>

namespace openrts {

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

using Point = std::array<float, 3>;
using Triangle = std::array<Point, 3>;

// MSTS row-vector matrix: v' = v * M, rows are the basis images, then the translation.
Point to_parent(const formats::ShapeMatrix& m, const Point& p) {
    const auto& v = m.values;
    return {p[0] * v[0] + p[1] * v[3] + p[2] * v[6] + v[9], p[0] * v[1] + p[1] * v[4] + p[2] * v[7] + v[10],
            p[0] * v[2] + p[1] * v[5] + p[2] * v[8] + v[11]};
}

// The nearest distance level's triangles in shape coordinates.
std::vector<Triangle> triangles(const formats::ShapeModel& model) {
    std::vector<Triangle> out;
    for (const auto& draw : model.draws) {
        if (draw.level != 0) continue;
        for (std::uint32_t i = 0; i + 2 < draw.index_count; i += 3) {
            Triangle t{};
            for (int k = 0; k < 3; ++k) {
                const auto& v = model.vertices[model.indices[draw.first_index + i + k]];
                Point p{v.position[0], v.position[1], v.position[2]};
                std::int32_t node = draw.matrix_index;
                for (std::size_t guard = 0; node >= 0 && static_cast<std::size_t>(node) < model.matrices.size()
                                            && guard <= model.matrices.size();
                     ++guard) {
                    p = to_parent(model.matrices[static_cast<std::size_t>(node)], p);
                    node = model.hierarchy[static_cast<std::size_t>(node)];
                }
                t[static_cast<std::size_t>(k)] = p;
            }
            out.push_back(t);
        }
    }
    return out;
}

// The height of a triangle straight above or below (x, z), if it covers it.
std::optional<float> height_at(const Triangle& t, const float x, const float z) {
    const float x0 = t[0][0], z0 = t[0][2];
    const float d = (t[1][2] - t[2][2]) * (x0 - t[2][0]) + (t[2][0] - t[1][0]) * (z0 - t[2][2]);
    if (std::abs(d) < 1e-9f) return std::nullopt;
    const float a = ((t[1][2] - t[2][2]) * (x - t[2][0]) + (t[2][0] - t[1][0]) * (z - t[2][2])) / d;
    const float b = ((t[2][2] - z0) * (x - t[2][0]) + (x0 - t[2][0]) * (z - t[2][2])) / d;
    const float c = 1.0f - a - b;
    if (a < 0.0f || b < 0.0f || c < 0.0f) return std::nullopt;
    return a * t[0][1] + b * t[1][1] + c * t[2][1];
}

// The shape reports shared by --cab and --shape: --node-bounds A,B,... (each
// named node's first-level draws: bounds in the shape's space, image, shader,
// light material, UV and vertex-alpha ranges) and --image-nodes (the nodes
// each image is drawn on).
void report_shape(const formats::ShapeModel& model, const std::vector<std::string>& args) {
    if (const auto at = std::find(args.begin(), args.end(), "--draw-vertices"); at != args.end() && at + 1 != args.end()) {
        // One draw's triangles in the shape's space: --draw-vertices <draw index>
        // (as --node-bounds numbers them), "x y z" per vertex, three a triangle.
        const auto d = static_cast<std::size_t>(std::atoi((at + 1)->c_str()));
        if (d < model.draws.size()) {
            const auto& draw = model.draws[d];
            for (std::uint32_t k = 0; k < draw.index_count; ++k) {
                const auto index = model.indices[draw.first_index + k];
                if (index >= model.vertices.size()) continue;
                float p[3] = {model.vertices[index].position[0], model.vertices[index].position[1], model.vertices[index].position[2]};
                std::int32_t node = draw.matrix_index;
                for (std::size_t guard = 0; node >= 0 && guard <= model.matrices.size(); ++guard) {
                    const auto& m = model.matrices[node].values;
                    const float q[3] = {p[0] * m[0] + p[1] * m[3] + p[2] * m[6] + m[9], p[0] * m[1] + p[1] * m[4] + p[2] * m[7] + m[10],
                                        p[0] * m[2] + p[1] * m[5] + p[2] * m[8] + m[11]};
                    p[0] = q[0];
                    p[1] = q[1];
                    p[2] = q[2];
                    node = model.hierarchy[node];
                }
                std::printf("v %.5f %.5f %.5f uv %.5f %.5f\n", p[0], p[1], p[2], model.vertices[index].uv[0],
                            model.vertices[index].uv[1]);
            }
        }
    }
    if (const auto at = std::find(args.begin(), args.end(), "--anim-nodes"); at != args.end() && at + 1 != args.end()) {
        // The animation keys of the named nodes: --anim-nodes A,B,...
        const std::string wanted = "," + lower(*(at + 1)) + ",";
        for (std::size_t a = 0; a < model.animations.size(); ++a) {
            for (const auto& node : model.animations[a].nodes) {
                if (wanted.find("," + lower(node.name) + ",") == std::string::npos) continue;
                std::printf("animation %zu (%u frames, %u a second) node %s:\n", a, model.animations[a].frame_count,
                            model.animations[a].frame_rate, node.name.c_str());
                for (const auto& key : node.positions)
                    std::printf("  frame %u position %.5f %.5f %.5f\n", key.frame, key.value[0], key.value[1], key.value[2]);
                for (const auto& key : node.rotations)
                    std::printf("  frame %u rotation %.5f %.5f %.5f %.5f\n", key.frame, key.value[0], key.value[1], key.value[2],
                                key.value[3]);
            }
        }
    }
    if (const auto at = std::find(args.begin(), args.end(), "--node-bounds"); at != args.end()) {
        // Bounds in the cab shape's space of the named nodes' first-level
        // geometry and the order of their draws: --node-bounds A,B,...
        const std::string wanted = at + 1 != args.end() ? lower(*(at + 1)) : std::string();
        for (std::size_t d = 0; d < model.draws.size(); ++d) {
            const auto& draw = model.draws[d];
            if (draw.level != 0 || draw.matrix_index < 0 || static_cast<std::size_t>(draw.matrix_index) >= model.matrices.size()) continue;
            const auto name = lower(model.matrices[draw.matrix_index].name);
            if (wanted != "*" && ("," + wanted + ",").find("," + name + ",") == std::string::npos) continue; // "*": every node
            // The node chain to the root (MSTS row-vector matrices).
            float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
            float uv_lo[2] = {1e9f, 1e9f}, uv_hi[2] = {-1e9f, -1e9f};
            unsigned alpha_lo = 255, alpha_hi = 0;
            for (std::uint32_t k = 0; k < draw.index_count; ++k) {
                const auto index = model.indices[draw.first_index + k];
                if (index >= model.vertices.size()) continue;
                const auto& vertex = model.vertices[index];
                for (int c = 0; c < 2; ++c) {
                    uv_lo[c] = (std::min)(uv_lo[c], vertex.uv[c]);
                    uv_hi[c] = (std::max)(uv_hi[c], vertex.uv[c]);
                }
                alpha_lo = (std::min)(alpha_lo, vertex.colour1 >> 24);
                alpha_hi = (std::max)(alpha_hi, vertex.colour1 >> 24);
                float p[3] = {model.vertices[index].position[0], model.vertices[index].position[1], model.vertices[index].position[2]};
                std::int32_t node = draw.matrix_index;
                for (std::size_t guard = 0; node >= 0 && guard <= model.matrices.size(); ++guard) {
                    const auto& m = model.matrices[node].values;
                    const float q[3] = {p[0] * m[0] + p[1] * m[3] + p[2] * m[6] + m[9], p[0] * m[1] + p[1] * m[4] + p[2] * m[7] + m[10],
                                        p[0] * m[2] + p[1] * m[5] + p[2] * m[8] + m[11]};
                    p[0] = q[0];
                    p[1] = q[1];
                    p[2] = q[2];
                    node = model.hierarchy[node];
                }
                for (int c = 0; c < 3; ++c) {
                    lo[c] = (std::min)(lo[c], p[c]);
                    hi[c] = (std::max)(hi[c], p[c]);
                }
            }
            const auto& state = model.primitive_states[draw.primitive_state_index];
            const auto image = state.texture_count && state.texture_index < model.textures.size() ? static_cast<int>(model.textures[state.texture_index].image_index) : -1;
            const int material = state.vertex_state_index >= 0 && static_cast<std::size_t>(state.vertex_state_index) < model.vertex_states.size()
                ? model.vertex_states[state.vertex_state_index].light_material_index
                : 0;
            std::printf("draw %zu %s image %d shader %u (%s) material %d z_bias %.3f zbuf %u alpha_test %u: x %.3f..%.3f y %.3f..%.3f z %.4f..%.4f"
                        " uv %.3f..%.3f / %.3f..%.3f vertex alpha %u..%u\n",
                        d, model.matrices[draw.matrix_index].name.c_str(), image, state.shader_index,
                        state.shader_index < model.shader_names.size() ? model.shader_names[state.shader_index].c_str() : "?", material,
                        state.z_bias, state.z_buffer_mode, state.alpha_test_mode, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], uv_lo[0], uv_hi[0], uv_lo[1], uv_hi[1],
                        alpha_lo, alpha_hi);
        }
    }
    if (std::find(args.begin(), args.end(), "--image-nodes") != args.end()) {
        // Which nodes each image is drawn on (first distance level).
        std::map<std::uint32_t, std::set<std::string>> by_image;
        for (const auto& draw : model.draws) {
            if (draw.level != 0 || draw.primitive_state_index >= model.primitive_states.size()) continue;
            const auto& state = model.primitive_states[draw.primitive_state_index];
            if (state.texture_count == 0 || state.texture_index >= model.textures.size()) continue;
            const auto image = model.textures[state.texture_index].image_index;
            const auto node = draw.matrix_index >= 0 && static_cast<std::size_t>(draw.matrix_index) < model.matrices.size()
                ? model.matrices[draw.matrix_index].name
                : std::string("?");
            by_image[image].insert(node + " (" + std::to_string(model.shader_names.size() > state.shader_index ? state.shader_index : 0)
                                   + ":" + (state.shader_index < model.shader_names.size() ? model.shader_names[state.shader_index] : "?") + ")");
        }
        for (const auto& [image, names] : by_image) {
            std::printf("image %u nodes:", image);
            for (const auto& name : names) std::printf(" %s", name.c_str());
            std::printf("\n");
        }
    }
}

} // namespace

int cab_check(const core::Vfs& vfs, const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::fprintf(stderr, "--cab <TRAINSET folder> <eng>\n");
        return 2;
    }
    const auto def = formats::load_wagon(vfs, args[0], args[1], true);
    if (!def.ok() || !def.engine || def.engine->cabin.empty()) {
        std::fprintf(stderr, "%s/%s: no engine with a Cabin\n", args[0].c_str(), args[1].c_str());
        return 2;
    }
    const auto eng = formats::wagon_path(args[0], args[1], true);
    auto shape = eng.parent().resolve(def.engine->cabin);
    if (!shape) return 2;
    auto sd = shape->with_extension(".sd");
    if (core::fold_case(std::string(shape->extension())) == ".repaint") {
        const auto text = vfs.read(*shape);
        const auto repaint = text ? formats::parse_repaint(*text) : formats::Repaint{};
        if (!repaint.ok()) {
            std::fprintf(stderr, "%s: not a repaint\n", std::string(shape->str()).c_str());
            return 2;
        }
        const auto base = shape->parent();
        shape = base.resolve(repaint.shape);
        sd = repaint.description.empty() ? shape->with_extension(".sd") : *base.resolve(repaint.description);
        std::printf("repaint: shape %s, %zu texture replacements\n", repaint.shape.c_str(), repaint.textures.size());
    }
    std::printf("cab shape %s\ncab sd    %s\n", std::string(shape->str()).c_str(), std::string(sd.str()).c_str());
    const auto sd_bytes = vfs.read(sd);
    if (!sd_bytes) {
        std::fprintf(stderr, "no .sd\n");
        return 2;
    }
    const auto cab = formats::parse_cab_description(*sd_bytes);
    if (!cab.ok()) {
        std::fprintf(stderr, "sd: %s\n", cab.error.c_str());
        return 2;
    }
    const auto body = formats::load_binary_shape_body(vfs, *shape);
    if (!body.ok()) {
        std::fprintf(stderr, "shape: %s\n", body.error.c_str());
        return 2;
    }
    const auto model = formats::parse_binary_shape_model(*body.body);
    std::map<std::string, std::size_t> nodes;
    for (std::size_t i = 0; i < model.matrices.size(); ++i) nodes[lower(model.matrices[i].name)] = i;
    // The shape's images, by index (TextureChanges name them by it).
    for (std::size_t i = 0; i < model.images.size(); ++i) std::printf("image %zu: %s\n", i, model.images[i].string().c_str());
    std::size_t identity = 0;
    for (std::size_t i = 0; i < model.textures.size(); ++i) identity += model.textures[i].image_index == i ? 1 : 0;
    std::printf("textures: %zu, %zu of them name the image of their own index\n", model.textures.size(), identity);
    report_shape(model, args);
    std::printf("shape: %zu nodes, %zu animations", model.matrices.size(), model.animations.size());
    if (!model.animations.empty()) std::printf(" (first: %u frames)", model.animations[0].frame_count);
    std::printf("\n");
    std::size_t bound = 0;
    std::vector<std::string> missing;
    for (const auto& s : cab.switches) {
        if (nodes.count(lower(s.name))) ++bound;
        else missing.push_back("switch " + std::to_string(s.id) + " " + s.name);
    }
    std::printf("switches: %zu, bound to a node: %zu\n", cab.switches.size(), bound);
    bound = 0;
    for (const auto& d : cab.displays) {
        if (nodes.count(lower(d.name))) ++bound;
        else if (d.type != formats::CabDisplayType::screen) missing.push_back("display " + std::to_string(d.id) + " " + d.name);
    }
    std::printf("displays: %zu, bound to a node: %zu\n", cab.displays.size(), bound);
    std::printf("lights %zu, texture changes %zu, animations %zu, signal lights %zu (scripted %d)\n", cab.lights.size(),
                cab.texture_changes.size(), cab.animations.size(), cab.signalization.lights.size(),
                cab.signalization.scripted ? 1 : 0);
    for (std::size_t i = 0; i < missing.size() && i < 30; ++i) std::printf("  no node: %s\n", missing[i].c_str());

    // Where the seat is in the shape: the surfaces straight below and above
    // each chair position (the nearest distance level).
    const auto tris = triangles(model);
    float lo[3] = {1e9f, 1e9f, 1e9f};
    float hi[3] = {-1e9f, -1e9f, -1e9f};
    for (const auto& t : tris) {
        for (const auto& p : t) {
            for (int a = 0; a < 3; ++a) {
                lo[a] = (std::min)(lo[a], p[a]);
                hi[a] = (std::max)(hi[a], p[a]);
            }
        }
    }
    std::printf("shape bounds: x %.2f..%.2f  y %.2f..%.2f  z %.2f..%.2f\n", lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
    for (const auto& chair : cab.chairs) {
        auto report = [&](const char* what, const std::array<float, 3>& p) {
            std::vector<float> hits;
            for (const auto& t : tris) {
                if (const auto y = height_at(t, p[0], p[2])) hits.push_back(*y);
            }
            std::sort(hits.begin(), hits.end());
            float below = -1e9f;
            float above = 1e9f;
            for (const auto y : hits) {
                if (y <= p[1]) below = y;
                else if (y < above) above = y;
            }
            std::printf("%s (%.2f %.2f %.2f): surface below %s, above %s; all surfaces here:", what, p[0], p[1], p[2],
                        below > -1e8f ? std::to_string(below).c_str() : "none",
                        above < 1e8f ? std::to_string(above).c_str() : "none");
            for (std::size_t i = 0; i < hits.size() && i < 16; ++i) std::printf(" %.2f", hits[i]);
            std::printf("\n");
        };
        report("seat Pos1", chair.seated);
        report("standing Pos2", chair.standing);
    }
    // The chairs (InteriorChairs): none, and the engineer sits at the cab
    // shape's origin (a cab whose .sd lost its InteriorGeometryInfo).
    std::printf("chairs: %zu\n", cab.chairs.size());
    for (const auto& chair : cab.chairs) {
        std::printf("chair at (%.2f %.2f %.2f), seated (%.2f %.2f %.2f), head out window %s\n", chair.position[0], chair.position[1],
                    chair.position[2], chair.seated[0], chair.seated[1], chair.seated[2],
                    chair.head_out_window.empty() ? "-" : chair.head_out_window.c_str());
    }
    for (const auto& window : cab.windows) {
        const auto it = nodes.find(lower(window.name));
        bool animated = false;
        if (it != nodes.end()) {
            for (const auto& a : model.animations)
                for (const auto& n : a.nodes)
                    if (n.matrix_index == static_cast<std::int32_t>(it->second)) animated = true;
        }
        std::printf("window %s (opens %d) head out at (%.2f %.2f %.2f): node %s%s\n", window.name.c_str(),
                    window.can_open ? 1 : 0, window.head_out[0], window.head_out[1], window.head_out[2],
                    it != nodes.end() ? "yes" : "no", animated ? ", animated" : "");
    }
    for (const auto& door : cab.doors) {
        const auto it = nodes.find(lower(door.name));
        bool animated = false;
        if (it != nodes.end()) {
            for (const auto& a : model.animations) {
                for (const auto& n : a.nodes) {
                    if (n.matrix_index == static_cast<std::int32_t>(it->second)) animated = true;
                }
            }
        }
        std::printf("door %s (%s - %s) at (%.2f %.2f %.2f): node %s%s\n", door.name.c_str(), door.from.c_str(),
                    door.to.c_str(), door.position[0], door.position[1], door.position[2],
                    it != nodes.end() ? "yes" : "no", animated ? ", animated" : "");
    }
    // Rooms and sound proofing (docs/sound.md): the share of an outside sound
    // heard in each room with everything shut, the windows open, all open;
    // and between the rooms with everything shut.
    if (!cab.areas.empty()) {
        const audio::InteriorAcoustics rooms(cab);
        const std::vector<float> doors_shut(cab.doors.size(), 0.0f), doors_open(cab.doors.size(), 1.0f);
        const std::vector<float> windows_shut(cab.windows.size(), 0.0f), windows_open(cab.windows.size(), 1.0f);
        constexpr int out = audio::InteriorAcoustics::outside;
        for (std::size_t r = 0; r < cab.areas.size(); ++r) {
            const auto& area = cab.areas[r];
            const int room = static_cast<int>(r);
            std::printf("room %s (%.2f %.2f .. %.2f %.2f): outside heard %.2f shut, %.2f windows open, %.2f all open;",
                        area.name.c_str(), area.x1, area.z1, area.x2, area.z2,
                        rooms.transmission(room, out, doors_shut, windows_shut),
                        rooms.transmission(room, out, doors_shut, windows_open),
                        rooms.transmission(room, out, doors_open, windows_open));
            for (std::size_t s = 0; s < cab.areas.size(); ++s) {
                if (s == r) continue;
                std::printf(" from %s %.2f", cab.areas[s].name.c_str(),
                            rooms.transmission(static_cast<int>(s), room, doors_shut, windows_shut));
            }
            std::printf("\n");
        }
        for (const auto& window : cab.windows) {
            const int room = window.has_position ? rooms.room_at(window.position[0], window.position[2]) : out;
            std::printf("window %s: room %s, SoundProof %.2f / %.2f\n", window.name.c_str(),
                        room == out ? "none" : cab.areas[static_cast<std::size_t>(room)].name.c_str(), window.sound_closed,
                        window.sound_open);
        }
        for (const auto& door : cab.doors)
            std::printf("door %s: SoundProof %.2f / %.2f%s\n", door.name.c_str(), door.sound_closed, door.sound_open,
                        door.opened ? ", opened" : "");
    }
    return 0;
}

// openrts-train-check <root> --prim-flags <virtual folder>: the prim_state
// flags (the first field, RTS reads it as hex; bit 0x100: drawn unlit,
// 0x4B042C) of every .s in the folder, with the shapes that set any.
int prim_flags_check(const core::Vfs& vfs, const std::vector<std::string>& args) {
    const auto folder = args.empty() ? std::nullopt : core::VirtualPath::parse(args[0]);
    if (!folder) {
        std::fprintf(stderr, "--prim-flags <virtual folder>\n");
        return 2;
    }
    std::map<std::uint32_t, std::size_t> counts;
    std::map<std::uint32_t, std::vector<std::string>> shapes;
    std::size_t files = 0;
    for (const auto& item : vfs.list(*folder)) {
        const auto& entry = item.path;
        if (item.directory || core::fold_case(std::string(entry.extension())) != ".s") continue;
        const auto body = formats::load_binary_shape_body(vfs, entry);
        if (!body.ok()) continue;
        const auto model = formats::parse_binary_shape_model(*body.body);
        ++files;
        std::set<std::uint32_t> seen;
        for (const auto& state : model.primitive_states) {
            ++counts[state.flags];
            if (state.flags != 0 && seen.insert(state.flags).second) shapes[state.flags].push_back(std::string(entry.filename()));
        }
    }
    std::printf("%zu shapes\n", files);
    for (const auto& [flags, count] : counts) {
        std::printf("flags %08X: %zu prim_states", flags, count);
        if (const auto it = shapes.find(flags); it != shapes.end()) {
            std::printf(" in %zu shapes:", it->second.size());
            for (std::size_t i = 0; i < it->second.size() && i < 8; ++i) std::printf(" %s", it->second[i].c_str());
        }
        std::printf("\n");
    }
    return 0;
}

// openrts-train-check <root> --shape <virtual .s path> [--image-nodes] [--node-bounds A,B]
int shape_check(const core::Vfs& vfs, const std::vector<std::string>& args) {
    const auto path = args.empty() ? std::nullopt : core::VirtualPath::parse(args[0]);
    if (!path) {
        std::fprintf(stderr, "--shape <virtual path of a .s>\n");
        return 2;
    }
    const auto body = formats::load_binary_shape_body(vfs, *path);
    if (!body.ok()) {
        std::fprintf(stderr, "shape: %s\n", body.error.c_str());
        return 2;
    }
    const auto model = formats::parse_binary_shape_model(*body.body);
    for (std::size_t i = 0; i < model.images.size(); ++i) std::printf("image %zu: %s\n", i, model.images[i].string().c_str());
    report_shape(model, args);
    std::printf("shape: %zu nodes, %zu draws\n", model.matrices.size(), model.draws.size());
    return 0;
}

} // namespace openrts
