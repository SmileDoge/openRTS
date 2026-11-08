#include "cab_view.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <string>

#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <openrts/formats/shape_animation.hpp>
#include <openrts/formats/shape_file.hpp>

namespace openrts::viewer {

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// A node hidden by scaling it to nothing (a removed key, a dark lamp).
const glm::mat4 hidden = glm::scale(glm::mat4{1.0f}, glm::vec3{0.0f});

constexpr float cell = 0.25f;

// An MSTS matrix as a column-vector glm matrix (each stored row is a column).
glm::mat4 column_matrix(const formats::ShapeMatrix& shape) {
    const auto& v = shape.values;
    glm::mat4 result{1.0f};
    result[0] = glm::vec4{v[0], v[1], v[2], 0.0f};
    result[1] = glm::vec4{v[3], v[4], v[5], 0.0f};
    result[2] = glm::vec4{v[6], v[7], v[8], 0.0f};
    result[3] = glm::vec4{v[9], v[10], v[11], 1.0f};
    return result;
}

// MSTS row-vector matrix: v' = v * M (three basis rows, then the translation).
glm::vec3 to_parent(const formats::ShapeMatrix& m, const glm::vec3& p) {
    const auto& v = m.values;
    return {p.x * v[0] + p.y * v[3] + p.z * v[6] + v[9], p.x * v[1] + p.y * v[4] + p.z * v[7] + v[10],
            p.x * v[2] + p.y * v[5] + p.z * v[8] + v[11]};
}

glm::vec3 to_shape(const formats::ShapeModel& model, std::int32_t node, glm::vec3 p) {
    for (std::size_t guard = 0; node >= 0 && static_cast<std::size_t>(node) < model.matrices.size()
                                && guard <= model.matrices.size();
         ++guard) {
        p = to_parent(model.matrices[static_cast<std::size_t>(node)], p);
        node = static_cast<std::size_t>(node) < model.hierarchy.size() ? model.hierarchy[static_cast<std::size_t>(node)] : -1;
    }
    return p;
}

bool skipped(const std::vector<std::uint8_t>& skip, const std::int32_t node) {
    return node >= 0 && static_cast<std::size_t>(node) < skip.size() && skip[static_cast<std::size_t>(node)] != 0;
}

} // namespace

CabCollision CabCollision::build(const formats::ShapeModel& model) {
    CabCollision out;
    glm::vec2 lo{1e9f}, hi{-1e9f};
    out.lower_ = glm::vec3{1e9f};
    out.upper_ = glm::vec3{-1e9f};
    for (const auto& draw : model.draws) {
        if (draw.level != 0) continue;
        for (std::uint32_t i = 0; i + 2 < draw.index_count; i += 3) {
            Triangle t;
            glm::vec3* corners[3] = {&t.a, &t.b, &t.c};
            bool valid = true;
            for (int k = 0; k < 3; ++k) {
                const auto index = draw.first_index + i + static_cast<std::uint32_t>(k);
                if (index >= model.indices.size() || model.indices[index] >= model.vertices.size()) {
                    valid = false;
                    break;
                }
                const auto& v = model.vertices[model.indices[index]];
                *corners[k] = to_shape(model, draw.matrix_index, {v.position[0], v.position[1], v.position[2]});
            }
            if (!valid) continue;
            const auto n = glm::cross(t.b - t.a, t.c - t.a);
            const float length = glm::length(n);
            if (length < 1e-8f) continue;
            t.normal_y = n.y / length;
            t.node = draw.matrix_index;
            for (const auto* p : corners) {
                lo = glm::min(lo, glm::vec2{p->x, p->z});
                hi = glm::max(hi, glm::vec2{p->x, p->z});
                out.lower_ = glm::min(out.lower_, *p);
                out.upper_ = glm::max(out.upper_, *p);
            }
            out.triangles_.push_back(t);
        }
    }
    if (out.triangles_.empty()) return out;
    out.origin_ = lo;
    out.columns_ = std::max(1, static_cast<int>(std::ceil((hi.x - lo.x) / cell)) + 1);
    out.rows_ = std::max(1, static_cast<int>(std::ceil((hi.y - lo.y) / cell)) + 1);
    const auto cells = static_cast<std::size_t>(out.columns_) * static_cast<std::size_t>(out.rows_);
    std::vector<std::uint32_t> counts(cells + 1, 0);
    auto range = [&](const Triangle& t, auto&& each) {
        const float x0 = std::min({t.a.x, t.b.x, t.c.x}), x1 = std::max({t.a.x, t.b.x, t.c.x});
        const float z0 = std::min({t.a.z, t.b.z, t.c.z}), z1 = std::max({t.a.z, t.b.z, t.c.z});
        const int c0 = std::clamp(static_cast<int>((x0 - out.origin_.x) / cell), 0, out.columns_ - 1);
        const int c1 = std::clamp(static_cast<int>((x1 - out.origin_.x) / cell), 0, out.columns_ - 1);
        const int r0 = std::clamp(static_cast<int>((z0 - out.origin_.y) / cell), 0, out.rows_ - 1);
        const int r1 = std::clamp(static_cast<int>((z1 - out.origin_.y) / cell), 0, out.rows_ - 1);
        for (int r = r0; r <= r1; ++r)
            for (int c = c0; c <= c1; ++c) each(static_cast<std::size_t>(r) * static_cast<std::size_t>(out.columns_) + static_cast<std::size_t>(c));
    };
    for (const auto& t : out.triangles_) range(t, [&](std::size_t i) { ++counts[i + 1]; });
    for (std::size_t i = 1; i < counts.size(); ++i) counts[i] += counts[i - 1];
    out.start_ = counts;
    out.items_.resize(counts.back());
    for (std::uint32_t index = 0; index < out.triangles_.size(); ++index)
        range(out.triangles_[index], [&](std::size_t i) { out.items_[counts[i]++] = index; });
    out.seen_.assign(out.triangles_.size(), 0);
    return out;
}

void CabCollision::node_bounds(const std::size_t nodes, std::vector<glm::vec3>& lower, std::vector<glm::vec3>& upper) const {
    lower.assign(nodes, glm::vec3{1e9f});
    upper.assign(nodes, glm::vec3{-1e9f});
    for (const auto& t : triangles_) {
        if (t.node < 0 || static_cast<std::size_t>(t.node) >= nodes) continue;
        auto& lo = lower[static_cast<std::size_t>(t.node)];
        auto& hi = upper[static_cast<std::size_t>(t.node)];
        for (const auto* p : {&t.a, &t.b, &t.c}) {
            lo = glm::min(lo, *p);
            hi = glm::max(hi, *p);
        }
    }
}

std::optional<CabCollision::Hit> CabCollision::ray(const glm::vec3& origin, const glm::vec3& direction, const float reach,
                                                   const std::vector<std::uint8_t>& skip) const {
    const glm::vec3 end = origin + direction * reach;
    std::optional<Hit> best;
    visit_segment(origin, end, [&](const Triangle& t) {
        if (skipped(skip, t.node)) return;
        const glm::vec3 e1 = t.b - t.a, e2 = t.c - t.a;
        const glm::vec3 p = glm::cross(direction, e2);
        const float det = glm::dot(e1, p);
        if (std::abs(det) < 1e-12f) return;
        const float inv = 1.0f / det;
        const glm::vec3 s = origin - t.a;
        const float u = glm::dot(s, p) * inv;
        if (u < 0.0f || u > 1.0f) return;
        const glm::vec3 q = glm::cross(s, e1);
        const float v = glm::dot(direction, q) * inv;
        if (v < 0.0f || u + v > 1.0f) return;
        const float along = glm::dot(e2, q) * inv;
        if (along < 0.0f || along > reach) return;
        if (!best || along < best->distance) best = Hit{along, t.node};
    });
    return best;
}

std::shared_ptr<CabCollision> CabCollision::load(const core::Vfs& vfs, const core::VirtualPath& shape) {
    const auto body = formats::load_binary_shape_body(vfs, shape);
    if (!body.ok()) return nullptr;
    const auto model = formats::parse_binary_shape_model(*body.body);
    if (!model.ok()) return nullptr;
    return std::make_shared<CabCollision>(build(model));
}

template <class F> void CabCollision::visit(float x0, float z0, float x1, float z1, F&& f) const {
    if (triangles_.empty()) return;
    if (x0 > x1) std::swap(x0, x1);
    if (z0 > z1) std::swap(z0, z1);
    const int c0 = static_cast<int>(std::floor((x0 - origin_.x) / cell));
    const int c1 = static_cast<int>(std::floor((x1 - origin_.x) / cell));
    const int r0 = static_cast<int>(std::floor((z0 - origin_.y) / cell));
    const int r1 = static_cast<int>(std::floor((z1 - origin_.y) / cell));
    if (c1 < 0 || r1 < 0 || c0 >= columns_ || r0 >= rows_) return;
    if (++stamp_ == 0) {
        std::fill(seen_.begin(), seen_.end(), 0u);
        stamp_ = 1;
    }
    for (int r = std::max(r0, 0); r <= std::min(r1, rows_ - 1); ++r) {
        for (int c = std::max(c0, 0); c <= std::min(c1, columns_ - 1); ++c) {
            const auto i = static_cast<std::size_t>(r) * static_cast<std::size_t>(columns_) + static_cast<std::size_t>(c);
            for (auto k = start_[i]; k < start_[i + 1]; ++k) {
                const auto index = items_[k];
                if (seen_[index] == stamp_) continue;
                seen_[index] = stamp_;
                f(triangles_[index]);
            }
        }
    }
}

template <class F> void CabCollision::visit_segment(const glm::vec3& a, const glm::vec3& b, F&& f) const {
    if (triangles_.empty()) return;
    if (++stamp_ == 0) {
        std::fill(seen_.begin(), seen_.end(), 0u);
        stamp_ = 1;
    }
    const auto cell_at = [&](const int c, const int r) {
        if (c < 0 || r < 0 || c >= columns_ || r >= rows_) return;
        const auto i = static_cast<std::size_t>(r) * static_cast<std::size_t>(columns_) + static_cast<std::size_t>(c);
        for (auto k = start_[i]; k < start_[i + 1]; ++k) {
            const auto index = items_[k];
            if (seen_[index] == stamp_) continue;
            seen_[index] = stamp_;
            f(triangles_[index]);
        }
    };
    // Cell by cell along the segment's x-z trace (Amanatides & Woo); where
    // it crosses a corner, both cells beside it too.
    const float ax = (a.x - origin_.x) / cell, az = (a.z - origin_.y) / cell;
    const float bx = (b.x - origin_.x) / cell, bz = (b.z - origin_.y) / cell;
    int c = static_cast<int>(std::floor(ax)), r = static_cast<int>(std::floor(az));
    const int end_c = static_cast<int>(std::floor(bx)), end_r = static_cast<int>(std::floor(bz));
    const float dx = bx - ax, dz = bz - az;
    const int step_c = dx > 0.0f ? 1 : dx < 0.0f ? -1 : 0;
    const int step_r = dz > 0.0f ? 1 : dz < 0.0f ? -1 : 0;
    constexpr float never = 1e30f;
    const float delta_c = step_c != 0 ? 1.0f / std::abs(dx) : never;
    const float delta_r = step_r != 0 ? 1.0f / std::abs(dz) : never;
    float next_c = step_c > 0 ? (static_cast<float>(c) + 1.0f - ax) * delta_c : step_c < 0 ? (ax - static_cast<float>(c)) * delta_c : never;
    float next_r = step_r > 0 ? (static_cast<float>(r) + 1.0f - az) * delta_r : step_r < 0 ? (az - static_cast<float>(r)) * delta_r : never;
    cell_at(c, r);
    for (int guard = std::abs(end_c - c) + std::abs(end_r - r) + 2; (c != end_c || r != end_r) && guard > 0; --guard) {
        if (std::abs(next_c - next_r) < 1e-6f) {
            cell_at(c + step_c, r);
            cell_at(c, r + step_r);
            c += step_c;
            r += step_r;
            next_c += delta_c;
            next_r += delta_r;
        } else if (next_c < next_r) {
            c += step_c;
            next_c += delta_c;
        } else {
            r += step_r;
            next_r += delta_r;
        }
        cell_at(c, r);
    }
}

std::optional<float> CabCollision::floor(const float x, const float z, const float low, const float high,
                                         const std::vector<std::uint8_t>& skip) const {
    std::optional<float> best;
    visit(x, z, x, z, [&](const Triangle& t) {
        if (std::abs(t.normal_y) < 0.6f || skipped(skip, t.node)) return;
        // Barycentric (x, z) in the triangle's ground projection.
        const float d = (t.b.z - t.c.z) * (t.a.x - t.c.x) + (t.c.x - t.b.x) * (t.a.z - t.c.z);
        if (std::abs(d) < 1e-9f) return;
        const float u = ((t.b.z - t.c.z) * (x - t.c.x) + (t.c.x - t.b.x) * (z - t.c.z)) / d;
        const float v = ((t.c.z - t.a.z) * (x - t.c.x) + (t.a.x - t.c.x) * (z - t.c.z)) / d;
        const float w = 1.0f - u - v;
        if (u < 0.0f || v < 0.0f || w < 0.0f) return;
        const float y = u * t.a.y + v * t.b.y + w * t.c.y;
        if (y < low || y > high) return;
        if (!best || y > *best) best = y;
    });
    return best;
}

bool CabCollision::crosses(const glm::vec3& a, const glm::vec3& b, const std::vector<std::uint8_t>& skip) const {
    const glm::vec3 direction = b - a;
    bool hit = false;
    visit_segment(a, b, [&](const Triangle& t) {
        if (hit || skipped(skip, t.node)) return;
        // Möller–Trumbore, the segment a + s·direction, s in [0, 1].
        const glm::vec3 e1 = t.b - t.a, e2 = t.c - t.a;
        const glm::vec3 p = glm::cross(direction, e2);
        const float det = glm::dot(e1, p);
        if (std::abs(det) < 1e-9f) return;
        const float inv = 1.0f / det;
        const glm::vec3 s = a - t.a;
        const float u = glm::dot(s, p) * inv;
        if (u < 0.0f || u > 1.0f) return;
        const glm::vec3 q = glm::cross(s, e1);
        const float v = glm::dot(direction, q) * inv;
        if (v < 0.0f || u + v > 1.0f) return;
        const float along = glm::dot(e2, q) * inv;
        if (along >= 0.0f && along <= 1.0f) hit = true;
    });
    return hit;
}

std::unique_ptr<CabView> CabView::create(const core::Vfs& vfs, const formats::WagonDefinition& engine,
                                         const core::VirtualPath& file) {
    if (!engine.engine || engine.engine->cabin.empty()) return nullptr;
    auto shape = file.parent().resolve(engine.engine->cabin);
    if (!shape) return nullptr;
    auto sd = shape->with_extension(".sd");
    if (core::fold_case(std::string(shape->extension())) == ".repaint") {
        const auto text = vfs.read(*shape);
        const auto repaint = text ? formats::parse_repaint(*text) : formats::Repaint{};
        if (!repaint.ok()) return nullptr;
        const auto base = shape->parent();
        shape = base.resolve(repaint.shape);
        if (!shape) return nullptr;
        if (!repaint.description.empty()) {
            if (const auto d = base.resolve(repaint.description)) sd = *d;
        } else {
            sd = shape->with_extension(".sd");
        }
    }
    const auto bytes = vfs.read(sd);
    if (!bytes || !vfs.exists(*shape)) return nullptr;
    auto view = std::make_unique<CabView>();
    view->shape_ = *shape;
    view->turned_ = engine.engine->cabin_param == 1;
    view->cab_ = formats::parse_cab_description(*bytes);
    if (!view->cab_.ok()) return nullptr;
    view->screens_ = std::make_unique<CabScreens>(view->cab_, view->shape_);
    if (view->screens_->empty()) view->screens_.reset();
    if (!view->cab_.chairs.empty()) {
        const auto& seat = view->cab_.chairs.front().seated;
        view->eye_ = {seat[0], seat[1], seat[2]};
    }
    view->doors_.resize(view->cab_.doors.size());
    view->windows_.resize(view->cab_.windows.size());
    // Opened: open from the start (RTS 0x51CD88 sets State and SetState).
    for (std::size_t i = 0; i < view->doors_.size(); ++i) {
        view->doors_[i].target = view->cab_.doors[i].opened;
        view->doors_[i].open = view->doors_[i].target ? 1.0f : 0.0f;
    }
    for (std::size_t i = 0; i < view->windows_.size(); ++i) {
        view->windows_[i].target = view->cab_.windows[i].opened;
        view->windows_[i].open = view->windows_[i].target ? 1.0f : 0.0f;
    }
    view->acoustics_ = audio::InteriorAcoustics(view->cab_);
    // The cab shape's surfaces and nodes for the engineer on foot, read
    // aside (the renderer keeps no vertices on the CPU).
    view->loading_ = std::async(std::launch::async, [&vfs, path = *shape]() -> std::shared_ptr<Geometry> {
        const auto body = formats::load_binary_shape_body(vfs, path);
        if (!body.ok()) return nullptr;
        const auto model = formats::parse_binary_shape_model(*body.body);
        if (!model.ok()) return nullptr;
        auto g = std::make_shared<Geometry>();
        g->collision = CabCollision::build(model);
        g->rig = std::make_shared<const formats::ShapeModel>(model);
        g->hierarchy = model.hierarchy;
        g->origins.resize(model.matrices.size());
        g->last_frames.resize(model.matrices.size());
        if (!model.animations.empty() && model.animations.front().frame_rate > 0) g->frame_rate = model.animations.front().frame_rate;
        for (std::size_t i = 0; i < model.matrices.size(); ++i) {
            g->origins[i] = to_shape(model, static_cast<std::int32_t>(i), glm::vec3{0.0f});
            g->nodes.emplace(lower(model.matrices[i].name), static_cast<std::int32_t>(i));
            g->last_frames[i] = formats::last_key_frame(model, 0, i);
        }
        return g;
    });
    return view;
}

void CabView::bind(const formats::ShapeModel& rig) {
    std::map<std::string, std::int32_t> nodes;
    for (std::size_t i = 0; i < rig.matrices.size(); ++i) nodes.emplace(lower(rig.matrices[i].name), static_cast<std::int32_t>(i));
    auto find = [&](const std::string& name) -> Bound {
        const auto it = nodes.find(lower(name));
        if (it == nodes.end()) return {};
        return {it->second, formats::last_key_frame(rig, 0, static_cast<std::size_t>(it->second))};
    };
    switches_.clear();
    displays_.clear();
    switch_source_.clear();
    switch_nodes_ = 0;
    for (const auto& s : cab_.switches) {
        switches_.push_back(find(s.name));
        if (switches_.back().node >= 0) ++switch_nodes_;
        switch_source_.push_back(cab_.root_switch(s.id));
    }
    for (const auto& d : cab_.displays) displays_.push_back(find(d.name));
    state_nodes_.clear();
    for (const auto& d : cab_.displays) {
        std::vector<std::int32_t> list;
        for (const auto& name : d.state_nodes) list.push_back(find(name).node);
        state_nodes_.push_back(std::move(list));
    }
    blink_clock_.assign(cab_.displays.size(), 0.0f);
    signal_lights_.clear();
    for (const auto& light : cab_.signalization.lights) signal_lights_.push_back(find(light).node);
    lamp_nodes_.clear();
    for (const auto& lamp : cab_.lights) lamp_nodes_.push_back(lamp.name.empty() ? -1 : find(lamp.name).node);
    bound_ = true;
}

void CabView::step_displays(const float elapsed) {
    blink_clock_.resize(cab_.displays.size(), 0.0f);
    for (std::size_t i = 0; i < cab_.displays.size(); ++i) {
        const float blink = cab_.displays[i].blink;
        if (blink <= 0.0f) continue;
        auto& clock = blink_clock_[i];
        clock += elapsed;
        if (clock > blink * 2.0f) clock = std::fmod(clock, blink * 2.0f);
    }
}

void CabView::poll() {
    if (geometry_ || !loading_.valid()) return;
    if (loading_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    geometry_ = loading_.get();
    if (!geometry_) return;
    for (std::size_t i = 0; i < cab_.doors.size() && i < doors_.size(); ++i) {
        auto& door = doors_[i];
        const auto it = geometry_->nodes.find(lower(cab_.doors[i].name));
        if (it == geometry_->nodes.end()) continue;
        door.node = it->second;
        door.last_frame = geometry_->last_frames[static_cast<std::size_t>(it->second)];
        // The door's node and every node below it.
        door.nodes.push_back(door.node);
        for (bool grew = true; grew;) {
            grew = false;
            for (std::size_t n = 0; n < geometry_->hierarchy.size(); ++n) {
                const auto parent = geometry_->hierarchy[n];
                if (parent < 0 || std::find(door.nodes.begin(), door.nodes.end(), parent) == door.nodes.end()) continue;
                if (std::find(door.nodes.begin(), door.nodes.end(), static_cast<std::int32_t>(n)) != door.nodes.end()) continue;
                door.nodes.push_back(static_cast<std::int32_t>(n));
                grew = true;
            }
        }
    }
    for (std::size_t i = 0; i < cab_.windows.size() && i < windows_.size(); ++i) {
        const auto it = geometry_->nodes.find(lower(cab_.windows[i].name));
        if (it == geometry_->nodes.end()) continue;
        windows_[i].node = it->second;
        windows_[i].last_frame = geometry_->last_frames[static_cast<std::size_t>(it->second)];
    }
    build_owners();
    rebuild_skip();
}

void CabView::build_owners() {
    const auto count = geometry_->hierarchy.size();
    owner_.assign(count, 0);
    // A node belongs to the nearest control above it (itself first).
    std::vector<std::int32_t> direct(count, 0);
    for (std::size_t i = 0; i < cab_.displays.size(); ++i) {
        const auto it = geometry_->nodes.find(lower(cab_.displays[i].name));
        if (it != geometry_->nodes.end()) direct[static_cast<std::size_t>(it->second)] = -static_cast<std::int32_t>(i + 1);
    }
    for (std::size_t i = 0; i < cab_.switches.size(); ++i) {
        const auto it = geometry_->nodes.find(lower(cab_.switches[i].name));
        if (it != geometry_->nodes.end()) direct[static_cast<std::size_t>(it->second)] = static_cast<std::int32_t>(i + 1);
    }
    for (std::size_t n = 0; n < count; ++n) {
        std::int32_t node = static_cast<std::int32_t>(n);
        for (std::size_t guard = 0; node >= 0 && guard <= count; ++guard) {
            if (direct[static_cast<std::size_t>(node)] != 0) {
                owner_[n] = direct[static_cast<std::size_t>(node)];
                break;
            }
            node = geometry_->hierarchy[static_cast<std::size_t>(node)];
        }
    }
    // The middle of each control's surfaces, for its caption.
    std::vector<glm::vec3> lower, upper;
    geometry_->collision.node_bounds(count, lower, upper);
    std::vector<glm::vec3> s_lo(cab_.switches.size(), glm::vec3{1e9f}), s_hi(cab_.switches.size(), glm::vec3{-1e9f});
    std::vector<glm::vec3> d_lo(cab_.displays.size(), glm::vec3{1e9f}), d_hi(cab_.displays.size(), glm::vec3{-1e9f});
    for (std::size_t n = 0; n < count; ++n) {
        if (owner_[n] == 0 || lower[n].x > upper[n].x) continue;
        if (owner_[n] > 0) {
            const auto i = static_cast<std::size_t>(owner_[n] - 1);
            s_lo[i] = glm::min(s_lo[i], lower[n]);
            s_hi[i] = glm::max(s_hi[i], upper[n]);
        } else {
            const auto i = static_cast<std::size_t>(-owner_[n] - 1);
            d_lo[i] = glm::min(d_lo[i], lower[n]);
            d_hi[i] = glm::max(d_hi[i], upper[n]);
        }
    }
    switch_lower_ = s_lo;
    switch_upper_ = s_hi;
    switch_owned_.assign(cab_.switches.size(), {});
    for (std::size_t n = 0; n < count; ++n) {
        if (owner_[n] > 0) switch_owned_[static_cast<std::size_t>(owner_[n] - 1)].push_back(static_cast<std::uint32_t>(n));
    }
    control_centres_.assign(cab_.switches.size(), std::nullopt);
    display_centres_.assign(cab_.displays.size(), std::nullopt);
    for (std::size_t i = 0; i < s_lo.size(); ++i)
        if (s_lo[i].x <= s_hi[i].x) control_centres_[i] = (s_lo[i] + s_hi[i]) * 0.5f;
    for (std::size_t i = 0; i < d_lo.size(); ++i)
        if (d_lo[i].x <= d_hi[i].x) display_centres_[i] = (d_lo[i] + d_hi[i]) * 0.5f;
}

std::optional<glm::vec3> CabView::control_label_point(const std::size_t switch_index) const {
    if (switch_index < control_centres_.size() && control_centres_[switch_index]) return control_centres_[switch_index];
    return control_point(switch_index);
}

std::optional<glm::vec3> CabView::display_label_point(const std::size_t display_index) const {
    if (display_index < display_centres_.size() && display_centres_[display_index]) return display_centres_[display_index];
    return display_point(display_index);
}

glm::mat4 CabView::placement() const noexcept {
    return turned_ ? glm::rotate(glm::mat4{1.0f}, 3.14159265f, glm::vec3{0.0f, 1.0f, 0.0f}) : glm::mat4{1.0f};
}

std::optional<std::size_t> CabView::find_window(const std::string_view name) const {
    const auto wanted = lower(std::string(name));
    for (std::size_t i = 0; i < cab_.windows.size(); ++i)
        if (lower(cab_.windows[i].name) == wanted) return i;
    return std::nullopt;
}

bool CabView::window_open(const std::size_t window) const noexcept {
    // A window without a node of its own is an opening.
    return window < windows_.size() && (windows_[window].node < 0 || windows_[window].open >= 0.95f);
}

float CabView::sound_transmission(const int from, const int to) const {
    // A door or window with a node passes sound as far as the node has
    // opened; one without passes it by its state (RTS 0x5143DC: the node's
    // position, else the activator's State).
    const auto fractions = [](const std::vector<Door>& list) {
        std::vector<float> out(list.size());
        for (std::size_t i = 0; i < list.size(); ++i)
            out[i] = list[i].node >= 0 ? list[i].open : (list[i].target ? 1.0f : 0.0f);
        return out;
    };
    return acoustics_.transmission(from, to, fractions(doors_), fractions(windows_));
}

void CabView::toggle_window(const std::size_t window) {
    if (window >= windows_.size()) return;
    windows_[window].target = !windows_[window].target;
    if (window < cab_.windows.size()) sound(cab_.windows[window].sms_triggers, windows_[window].target);
}

std::optional<CabView::Picked> CabView::pick(const glm::vec3& origin, const glm::vec3& direction, const float reach) const {
    if (!geometry_) return std::nullopt;
    // RTS 0x528EA0: a control answers only with the eye within its Radius of
    // its .sd Position (no Radius: openRTS's reach).
    auto within = [&](const std::array<float, 3>& position, const float radius) {
        if (radius <= 0.0f) return true;
        const glm::vec3 p{position[0], position[1], position[2]};
        return glm::distance(origin, p) <= radius;
    };
    std::optional<Picked> best;
    // The switches whose nodes are drawn away from their rest pose are tested
    // where they are (RTS picks a control by drawing its node, 0x5290C8): the
    // ray taken back into the node's rest pose, against its own surfaces.
    auto skip = skip_;
    std::optional<CabCollision::Hit> hit;
    const auto& rig = geometry_->rig;
    if (rig && !rig->animations.empty() && !switch_frames_.empty()) {
        const auto nodes = owner_.size();
        if (skip.size() < nodes) skip.resize(nodes, 0);
        std::vector<std::uint8_t> only(nodes, 1);
        for (std::size_t i = 0; i < switch_frames_.size() && i < switches_.size(); ++i) {
            const auto node = switches_[i].node;
            if (switch_frames_[i] < 0.0f || node < 0 || static_cast<std::size_t>(node) >= rig->matrices.size()) continue;
            if (i >= switch_owned_.size() || switch_owned_[i].empty()) continue;
            // Its nodes leave the rest-pose test and are tried posed.
            for (const auto n : switch_owned_[i]) skip[n] = 1;
            if (switch_lower_[i].x > switch_upper_[i].x) continue; // no surfaces
            // parent chain x (posed local) x (rest local)^-1 x (parent chain)^-1
            glm::mat4 parent{1.0f};
            for (auto p = rig->hierarchy[static_cast<std::size_t>(node)], guard = 0;
                 p >= 0 && static_cast<std::size_t>(p) < rig->matrices.size() && guard <= static_cast<std::int32_t>(nodes);
                 p = rig->hierarchy[static_cast<std::size_t>(p)], ++guard)
                parent = column_matrix(rig->matrices[static_cast<std::size_t>(p)]) * parent;
            const auto posed = column_matrix(formats::animate_matrix(*rig, 0, static_cast<std::size_t>(node), switch_frames_[i]));
            const auto rest = column_matrix(rig->matrices[static_cast<std::size_t>(node)]);
            const glm::mat4 back = glm::inverse(parent * posed * glm::inverse(rest) * glm::inverse(parent));
            const glm::vec3 o{back * glm::vec4{origin, 1.0f}};
            const glm::vec3 d = glm::normalize(glm::vec3{back * glm::vec4{direction, 0.0f}});
            // The ray against the box of its surfaces first (slabs).
            float t0 = 0.0f, t1 = reach;
            for (int axis = 0; axis < 3 && t0 <= t1; ++axis) {
                const float lo = switch_lower_[i][axis] - 1e-3f, hi = switch_upper_[i][axis] + 1e-3f;
                if (std::abs(d[axis]) < 1e-9f) {
                    if (o[axis] < lo || o[axis] > hi) t0 = reach + 1.0f;
                    continue;
                }
                float near_t = (lo - o[axis]) / d[axis], far_t = (hi - o[axis]) / d[axis];
                if (near_t > far_t) std::swap(near_t, far_t);
                t0 = (std::max)(t0, near_t);
                t1 = (std::min)(t1, far_t);
            }
            if (t0 > t1 || (hit && t0 > hit->distance)) continue;
            for (const auto n : switch_owned_[i]) only[n] = 0;
            const auto posed_hit = geometry_->collision.ray(o, d, reach, only);
            for (const auto n : switch_owned_[i]) only[n] = 1;
            if (posed_hit && (!hit || posed_hit->distance < hit->distance)) hit = posed_hit;
        }
    }
    if (const auto rest_hit = geometry_->collision.ray(origin, direction, reach, skip); rest_hit && (!hit || rest_hit->distance < hit->distance))
        hit = rest_hit;
    if (hit && hit->node >= 0 && static_cast<std::size_t>(hit->node) < owner_.size()) {
        const auto owner = owner_[static_cast<std::size_t>(hit->node)];
        if (owner > 0) {
            const auto i = static_cast<std::size_t>(owner - 1);
            if (within(cab_.switches[i].position, cab_.switches[i].radius)) best = Picked{true, i, hit->distance};
        }
        if (owner < 0) {
            const auto i = static_cast<std::size_t>(-owner - 1);
            if (within(cab_.displays[i].position, cab_.displays[i].radius)) best = Picked{false, i, hit->distance};
        }
    }
    if (!best) {
        // Not a control's own surface: a small control right on the ray,
        // before what the ray meets.
        const float limit = hit ? hit->distance + 0.05f : reach;
        float nearest = 0.04f;
        for (std::size_t i = 0; i < cab_.switches.size(); ++i) {
            // Only controls without a surface of their own (RTS: no node,
            // picked by the Position near the cursor).
            if (i < control_centres_.size() && control_centres_[i]) continue;
            if (!within(cab_.switches[i].position, cab_.switches[i].radius)) continue;
            const auto p = control_point(i);
            if (!p) continue;
            const float along = glm::dot(*p - origin, direction);
            if (along < 0.0f || along > limit) continue;
            const float off = glm::length(*p - (origin + direction * along));
            if (off > nearest) continue;
            nearest = off;
            best = Picked{true, i, along};
        }
    }
    return best;
}

float CabView::switch_frames_per_state(const std::size_t index) const noexcept {
    if (index >= switches_.size() || index >= cab_.switches.size() || switches_[index].node < 0
        || switches_[index].last_frame <= 0.0f || !geometry_)
        return 0.0f;
    const auto& s = cab_.switches[index];
    const std::uint32_t positions = s.num_states; // a KEY switch's key-out state is not one
    if (positions <= 1) return 0.0f;
    auto frames = static_cast<std::uint32_t>(static_cast<float>(geometry_->frame_rate) * s.anim_time
                                             * static_cast<float>(positions - 1));
    if (frames == 0) frames = positions;
    if (frames <= 1) return 0.0f;
    return static_cast<float>(frames - 1) / static_cast<float>(positions - 1);
}

bool CabView::door_open(const std::size_t door) const noexcept {
    if (door >= doors_.size()) return false;
    // Not read yet (the cab of a car he only stands at): as it was left.
    if (!geometry_) return doors_[door].target;
    // A door without a node of its own (DUMMY) is a doorway that is always open.
    return doors_[door].node < 0 || doors_[door].open >= 0.6f;
}

bool CabView::door_moving(const std::size_t door) const noexcept {
    return door < doors_.size() && doors_[door].open != (doors_[door].target ? 1.0f : 0.0f);
}

void CabView::toggle_door(const std::size_t door) {
    if (door >= doors_.size()) return;
    doors_[door].target = !doors_[door].target;
    if (door < cab_.doors.size()) sound(cab_.doors[door].sms_triggers, doors_[door].target);
}

void CabView::open_door(const std::size_t door) {
    if (door >= doors_.size()) return;
    if (!doors_[door].target && door < cab_.doors.size()) sound(cab_.doors[door].sms_triggers, true);
    doors_[door].target = true;
    doors_[door].open = 1.0f;
    rebuild_skip();
}

void CabView::step_doors(const float elapsed) {
    for (std::size_t i = 0; i < windows_.size(); ++i) {
        auto& window = windows_[i];
        const float goal = window.target ? 1.0f : 0.0f;
        const float time = i < cab_.windows.size() && cab_.windows[i].anim_time > 0.05f ? cab_.windows[i].anim_time : 1.0f;
        window.open = goal > window.open ? std::min(goal, window.open + elapsed / time) : std::max(goal, window.open - elapsed / time);
    }
    bool changed = false;
    for (std::size_t i = 0; i < doors_.size(); ++i) {
        auto& door = doors_[i];
        const float goal = door.target ? 1.0f : 0.0f;
        if (door.open == goal) continue;
        const float time = i < cab_.doors.size() && cab_.doors[i].anim_time > 0.05f ? cab_.doors[i].anim_time : 1.0f;
        const float was = door.open;
        door.open = goal > door.open ? std::min(goal, door.open + elapsed / time) : std::max(goal, door.open - elapsed / time);
        if ((was >= 0.6f) != (door.open >= 0.6f)) changed = true;
    }
    if (changed) rebuild_skip();
}

void CabView::rebuild_skip() {
    if (!geometry_) return;
    skip_.assign(geometry_->hierarchy.size(), 0);
    for (const auto& door : doors_) {
        if (door.node < 0 || door.open < 0.6f) continue;
        for (const auto n : door.nodes)
            if (n >= 0 && static_cast<std::size_t>(n) < skip_.size()) skip_[static_cast<std::size_t>(n)] = 1;
    }
}

std::optional<glm::vec3> CabView::control_point(const std::size_t switch_index) const {
    if (switch_index >= cab_.switches.size()) return std::nullopt;
    const auto& p = cab_.switches[switch_index].position;
    if (p[0] != 0.0f || p[1] != 0.0f || p[2] != 0.0f) return glm::vec3{p[0], p[1], p[2]};
    if (!geometry_) return std::nullopt;
    const auto it = geometry_->nodes.find(lower(cab_.switches[switch_index].name));
    if (it == geometry_->nodes.end()) return std::nullopt;
    return geometry_->origins[static_cast<std::size_t>(it->second)];
}

std::optional<glm::vec3> CabView::display_point(const std::size_t display_index) const {
    if (display_index >= cab_.displays.size()) return std::nullopt;
    const auto& p = cab_.displays[display_index].position;
    if (p[0] != 0.0f || p[1] != 0.0f || p[2] != 0.0f) return glm::vec3{p[0], p[1], p[2]};
    if (!geometry_) return std::nullopt;
    const auto it = geometry_->nodes.find(lower(cab_.displays[display_index].name));
    if (it == geometry_->nodes.end()) return std::nullopt;
    return geometry_->origins[static_cast<std::size_t>(it->second)];
}

void CabView::adjustments(const dllhost::CarOutput* state, std::vector<renderer::NodeAdjustment>& out,
                          const std::uint32_t aspect, const Blend* blend) const {
    const auto between = [&](const std::vector<float>* before, const std::uint32_t id, const float now) {
        if (!blend || !before || id >= before->size()) return now;
        const float was = (*before)[id];
        return was + (now - was) * std::clamp(blend->alpha, 0.0f, 1.0f);
    };
    if (!bound_) return;
    switch_frames_.assign(cab_.switches.size(), -1.0f);
    for (std::size_t i = 0; i < cab_.switches.size() && i < switches_.size(); ++i) {
        const auto& b = switches_[i];
        if (b.node < 0) continue;
        const auto& s = cab_.switches[i];
        const bool key = (s.flags & formats::cab_key) != 0;
        // Without a DLL's state: as RTS loads it, a KEY switch with its key out.
        float position = key ? 0.0f : static_cast<float>(s.initial_state);
        std::uint32_t whole = key ? 0u : s.initial_state;
        // A piece following another (ParentObject) is drawn where its root
        // is, every frame (the host gives it the root's SetState per step).
        const std::uint32_t source = i < switch_source_.size() ? switch_source_[i] : s.id;
        if (state && source < state->switches.size()) {
            // Between the steps only a move of up to one position: a DLL that
            // throws a lever over many (C_MAIN 12 -> 1 on the ChS4z) has it
            // there at once, as RTS draws it, not swept through the others.
            // The switch in the mouse's hand: where it is now.
            const float now = state->switches[source].current_frame;
            const auto* before = blend && blend->live_switch != source ? blend->switch_frames : nullptr;
            const bool near = before && source < before->size() && std::abs((*before)[source] - now) <= 1.001f;
            position = near ? between(before, source, now) : now;
            whole = state->switches[source].state;
        }
        if (key) {
            // State 0: the key is out; positions are the states 1..States.
            if (whole == 0) {
                out.push_back({.matrix = b.node, .local = hidden});
                continue;
            }
            position = (std::max)(position - 1.0f, 0.0f);
        }
        const float steps = static_cast<float>(s.num_states > 1 ? s.num_states - 1 : 1);
        const float frame = b.last_frame > 0.0f ? std::clamp(position / steps, 0.0f, 1.0f) * b.last_frame : 0.0f;
        out.push_back({.matrix = b.node, .frame = frame});
        if (b.last_frame > 0.0f) switch_frames_[i] = frame;
    }
    for (std::size_t i = 0; i < cab_.displays.size() && i < displays_.size(); ++i) {
        const auto& b = displays_[i];
        const auto& d = cab_.displays[i];
        float value = 0.0f;
        std::uint32_t lit = d.initial_state;
        if (state && d.id < state->displays.size()) {
            value = state->displays[d.id].value;
            if (d.type == formats::CabDisplayType::arrow) value = between(blend ? blend->display_values : nullptr, d.id, value);
            lit = state->displays[d.id].state;
        }
        // RTS 0x525DB0: with a blink the display is hidden for the second
        // half of each 2 x blink period.
        const bool blinked_off = d.blink > 0.0f && i < blink_clock_.size() && blink_clock_[i] > d.blink;
        if (d.type == formats::CabDisplayType::display && i < state_nodes_.size()) {
            // StateNodes: node j shows for bit j of the State, or with
            // DISPLAY_SEQUENCE for the State j + 1; all hidden while blinked off.
            const bool sequence = (d.flags & formats::cab_check_distance) != 0;
            for (std::size_t j = 0; j < state_nodes_[i].size(); ++j) {
                const auto node = state_nodes_[i][j];
                if (node < 0) continue;
                bool show = sequence ? lit == j + 1 : j < 32 && (lit & (1u << j)) != 0;
                if (blinked_off) show = false;
                if (!show) out.push_back({.matrix = node, .hidden = true});
            }
        }
        if (b.node < 0) continue;
        switch (d.type) {
        case formats::CabDisplayType::arrow: {
            const float span = d.range[1] - d.range[0];
            const float t = span != 0.0f ? std::clamp((value - d.range[0]) / span, 0.0f, 1.0f) : 0.0f;
            out.push_back({.matrix = b.node, .frame = t * b.last_frame});
            break;
        }
        case formats::CabDisplayType::lamp:
            if (blinked_off) lit = 0;
            if (b.last_frame > 0.0f) {
                out.push_back({.matrix = b.node, .frame = lit ? b.last_frame : 0.0f});
            } else if (!lit) {
                out.push_back({.matrix = b.node, .local = hidden}); // a lamp without keys shows only when lit
            }
            break;
        default:
            break; // DISPLAY: its StateNodes above; SCREEN: CabScreens
        }
    }
    // A lamp's node (its Name; rts1.exe 0x514630): without SOURCE_VISIBLE it
    // shows only while the lamp burns (node flag 1 otherwise); with it, it
    // stays and is drawn unlit while lit (node flag 0x40).
    for (std::size_t k = 0; k < cab_.lights.size() && k < lamp_nodes_.size(); ++k) {
        if (lamp_nodes_[k] < 0) continue;
        const bool on = state && k < state->internal_lights.size() && state->internal_lights[k] != 0;
        if (lower(cab_.lights[k].flags).find("source_visible") != std::string::npos) {
            if (on) out.push_back({.matrix = lamp_nodes_[k], .unlit = true});
        } else if (!on) {
            out.push_back({.matrix = lamp_nodes_[k], .hidden = true});
        }
    }
    for (const auto* list : {&doors_, &windows_}) {
        for (const auto& door : *list) {
            if (door.node < 0 || door.last_frame <= 0.0f) continue;
            out.push_back({.matrix = door.node, .frame = door.open * door.last_frame});
        }
    }
    // The locomotive signal lights (RTS 0x525DB0): the aspect is
    // Cabin::Signal.Aspect[0]; the engine's ALSNOn 0 puts every light out,
    // -1 lights them all (a lamp test), another negative value shows the
    // aspect in its low 12 bits; an aspect past the list shows the first.
    const auto& sig = cab_.signalization;
    if (!sig.aspects.empty() && !signal_lights_.empty()) {
        int on = 1;
        std::uint32_t shown = aspect;
        if (state) {
            shown = state->cab_aspect;
            if (state->has_engine) on = static_cast<std::int16_t>(state->engine.alsn_on);
        }
        if (on < 0 && on != -1) shown = static_cast<std::uint32_t>(on) & 0xFFFu;
        if (shown >= sig.aspects.size()) shown = 0;
        const auto& lit = sig.aspects[shown];
        for (std::size_t m = 0; m < signal_lights_.size(); ++m) {
            if (signal_lights_[m] < 0) continue;
            const bool show = on != 0 && (on == -1 || std::find(lit.begin(), lit.end(), static_cast<std::uint32_t>(m)) != lit.end());
            if (!show) out.push_back({.matrix = signal_lights_[m], .hidden = true}); // node flag 1 (0x525DB0)
        }
    }
}

} // namespace openrts::viewer
