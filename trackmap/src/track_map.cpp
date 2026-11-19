#include <openrts/trackmap/track_map.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <map>

#include <openrts/ui/fonts.hpp>
#include <openrts/ui/text.hpp>

namespace openrts::trackmap {
namespace {

using ui::tr;

constexpr double tile = 2048.0;

// A label running along a track on the screen, so that names of things on
// tracks side by side (signals, platforms, sidings) lie on their own tracks
// instead of over each other where the tracks run up the screen. `along`:
// the track's direction on the screen; the text is turned so it never reads
// upside down. `size`: its height in px. Its nearer edge goes `side` px off
// the track (`towards`: the side, any point there; none: above the line as
// it reads), starting at `anchor` (or centred on it).
void text_along(ImDrawList* draw, const ImVec2 anchor, ImVec2 along, const ImU32 colour, const char* text, const float size,
                const float side, const std::optional<ImVec2> towards, const bool centred) {
    const float length = std::sqrt(along.x * along.x + along.y * along.y);
    if (length < 1e-6f) {
        along = ImVec2(1.0f, 0.0f);
    } else {
        along = ImVec2(along.x / length, along.y / length);
    }
    // Reading left to right (or downwards on a vertical track).
    if (along.x < 0.0f || (along.x == 0.0f && along.y < 0.0f)) along = ImVec2(-along.x, -along.y);
    // The glyphs' "up" follows the reading direction alone; the side only
    // moves the text (flipping this "up" had mirrored the letters).
    const ImVec2 up(along.y, -along.x);
    float off = 1.0f;
    if (towards) {
        const float d = (towards->x - anchor.x) * up.x + (towards->y - anchor.y) * up.y;
        if (d < 0.0f) off = -1.0f;
    }
    ImFont* font = ImGui::GetFont();
    const ImVec2 box = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    // Laid flat with its middle-left (or middle) at the anchor, then turned
    // about it and moved `side` px off the line. It is laid at the anchor,
    // inside the map: ImGui drops the glyphs of text laid outside the clip
    // rectangle, and laid at the screen's corner the names had vanished.
    const ImVec2 corner(centred ? -box.x * 0.5f : 0.0f, -box.y * 0.5f);
    const int first = draw->VtxBuffer.Size;
    draw->AddText(font, size, ImVec2(anchor.x + corner.x, anchor.y + corner.y), colour, text);
    const float away = off * (side + box.y * 0.5f);
    const ImVec2 shift(anchor.x + up.x * away, anchor.y + up.y * away);
    for (int i = first; i < draw->VtxBuffer.Size; ++i) {
        auto& v = draw->VtxBuffer[i].pos;
        // Glyph y grows down; the text's down is -up.
        const float x = v.x - anchor.x, y = v.y - anchor.y;
        v = ImVec2(shift.x + x * along.x - y * up.x, shift.y + x * along.y - y * up.y);
    }
}

// Distance from (px, py) to the segment a-b, all in pixels; `t` gets the
// position along it (0..1).
[[nodiscard]] double segment_distance(const double px, const double py, const double ax, const double ay, const double bx,
                                      const double by, double& t) {
    const double dx = bx - ax, dy = by - ay;
    const double length = dx * dx + dy * dy;
    t = length > 0.0 ? std::clamp(((px - ax) * dx + (py - ay) * dy) / length, 0.0, 1.0) : 0.0;
    const double cx = ax + dx * t - px, cy = ay + dy * t - py;
    return std::sqrt(cx * cx + cy * cy);
}

// A name folded for comparing as RTS's lstrcmpiW does here: ASCII and
// Cyrillic letters in lower case (UTF-8 in, UTF-8 out).
[[nodiscard]] std::string fold_name(const std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            out.push_back(static_cast<char>(std::tolower(c)));
            ++i;
            continue;
        }
        if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) {
            unsigned code = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3Fu);
            if (code >= 0x410 && code <= 0x42F) code += 0x20; // А-Я
            else if (code == 0x401) code = 0x451;               // Ё
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3Fu)));
            i += 2;
            continue;
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

[[nodiscard]] simulation::TrackPoint place_of(const formats::RoutePosition& p) {
    return {.tile_x = p.tile_x, .tile_z = p.tile_z, .x = p.x, .y = p.y, .z = p.z};
}

} // namespace

void draw_direction_arrow(ImDrawList* draw, const ImVec2 front, const ImVec2 rear, const ImU32 fill, const ImU32 outline, const float size) {
    float dx = front.x - rear.x, dy = front.y - rear.y;
    const float span = std::sqrt(dx * dx + dy * dy);
    if (span < 1e-4f) return;
    dx /= span;
    dy /= span;
    const ImVec2 base(front.x + dx * 2.0f, front.y + dy * 2.0f);
    const ImVec2 tip(base.x + dx * size, base.y + dy * size);
    const float half = size * 0.6f;
    const ImVec2 wing_a(base.x - dy * half, base.y + dx * half);
    const ImVec2 wing_b(base.x + dy * half, base.y - dx * half);
    draw->AddTriangleFilled(tip, wing_a, wing_b, fill);
    draw->AddTriangle(tip, wing_a, wing_b, outline, 1.5f);
}

TrackMap::TrackMap(const simulation::TrackNetwork& network, const formats::RoutePosition origin) : network_(&network), origin_(origin) {
    build_lines();
    build_places();
}

void TrackMap::centre(const double x, const double z) noexcept {
    centre_x_ = x;
    centre_z_ = z;
}

void TrackMap::centre_on(const simulation::TrackPoint& point, const double min_scale) {
    centre_x_ = world_x(point);
    centre_z_ = world_z(point);
    scale_ = (std::max)(scale_, min_scale);
}

double TrackMap::world_x(const simulation::TrackPoint& point) const {
    return (point.tile_x - origin_.tile_x) * tile + point.x - origin_.x;
}

double TrackMap::world_z(const simulation::TrackPoint& point) const {
    return (point.tile_z - origin_.tile_z) * tile + point.z - origin_.z;
}

ImVec2 TrackMap::screen(const double x, const double z) const {
    const double cx = canvas_x_ + canvas_w_ * 0.5, cy = canvas_y_ + canvas_h_ * 0.5;
    return {static_cast<float>(cx + (x - centre_x_) * scale_), static_cast<float>(cy - (z - centre_z_) * scale_)};
}

ImVec2 TrackMap::screen(const simulation::TrackPoint& point) const {
    return screen(world_x(point), world_z(point));
}

std::vector<simulation::TrackPoint> TrackMap::along(const std::uint32_t node, const double from, const double to) const {
    std::vector<simulation::TrackPoint> out;
    const double length = std::abs(to - from);
    const int steps = (std::max)(1, static_cast<int>(length / 10.0));
    for (int i = 0; i <= steps; ++i) {
        if (const auto p = network_->point(node, from + (to - from) * i / steps)) out.push_back(*p);
    }
    return out;
}

void TrackMap::fit(const std::vector<simulation::TrackPoint>& points, const double max_scale) {
    if (points.empty()) return;
    double min_x = 1e300, max_x = -1e300, min_z = 1e300, max_z = -1e300;
    bool inside = true;
    constexpr float margin = 90.0f; // room for names by the points
    for (const auto& p : points) {
        const double x = world_x(p), z = world_z(p);
        min_x = (std::min)(min_x, x);
        max_x = (std::max)(max_x, x);
        min_z = (std::min)(min_z, z);
        max_z = (std::max)(max_z, z);
        const auto s = screen(x, z);
        inside = inside && s.x >= canvas_x_ + margin && s.x <= canvas_x_ + canvas_w_ - margin && s.y >= canvas_y_ + margin &&
                 s.y <= canvas_y_ + canvas_h_ - margin;
    }
    if (inside) return;
    centre_x_ = (min_x + max_x) * 0.5;
    centre_z_ = (min_z + max_z) * 0.5;
    const double w = (std::max)(1.0, static_cast<double>(canvas_w_) - 2.0 * margin);
    const double h = (std::max)(1.0, static_cast<double>(canvas_h_) - 2.0 * margin);
    const double needed = (std::min)(w / (std::max)(max_x - min_x, 1.0), h / (std::max)(max_z - min_z, 1.0));
    scale_ = std::clamp((std::min)(needed, (std::max)(scale_, max_scale)), 0.002, 20.0);
}

std::vector<simulation::TrackPoint> TrackMap::along(const simulation::RouteLeg& leg, double from, double to) const {
    std::vector<simulation::TrackPoint> out;
    for (const auto& span : leg.spans) {
        const double a = (std::max)(from, span.start);
        const double b = (std::min)(to, span.start + span.length());
        if (b <= a) continue;
        const double o1 = span.forward ? span.from + (a - span.start) : span.from - (a - span.start);
        const double o2 = span.forward ? span.from + (b - span.start) : span.from - (b - span.start);
        auto piece = along(span.node, o1, o2);
        out.insert(out.end(), piece.begin(), piece.end());
    }
    return out;
}

void TrackMap::build_lines() {
    lines_.clear();
    for (std::uint32_t n = 1; n < network_->node_count(); ++n) {
        const auto* node = network_->node(n);
        if (!node || node->kind != formats::TrackNodeKind::vector || node->sections.empty()) continue;
        Line line{.node = n};
        const double step = 10.0;
        for (double offset = 0.0;; offset += step) {
            const double at = (std::min)(offset, node->length);
            if (const auto point = network_->point(n, at)) {
                line.x.push_back(world_x(*point));
                line.z.push_back(world_z(*point));
                line.offset.push_back(at);
            }
            if (at >= node->length) break;
        }
        if (line.x.empty()) continue;
        line.min_x = *std::min_element(line.x.begin(), line.x.end());
        line.max_x = *std::max_element(line.x.begin(), line.x.end());
        line.min_z = *std::min_element(line.z.begin(), line.z.end());
        line.max_z = *std::max_element(line.z.begin(), line.z.end());
        lines_.push_back(std::move(line));
    }
}

void TrackMap::build_places() {
    // Stations: their platform items grouped by station name, at the mean of
    // the items' positions (0x555624 keeps a running mean and a count).
    {
        std::map<std::string, std::size_t> by_name;
        for (const auto& item : network_->database().items) {
            if (item.kind != formats::TrackItemKind::platform || item.station.empty() || !item.has_position) continue;
            const simulation::TrackPoint at{.tile_x = item.tile_x, .tile_z = item.tile_z, .x = item.x, .y = item.y, .z = item.z};
            const auto key = fold_name(item.station);
            auto [it, added] = by_name.emplace(key, stations_.size());
            if (added) stations_.push_back({.name = item.station});
            auto& station = stations_[it->second];
            ++station.items;
            station.x += (world_x(at) - station.x) / station.items;
            station.z += (world_z(at) - station.z) / station.items;
        }
    }
    // Every TDB item's place on the network.
    std::map<std::uint32_t, std::pair<std::uint32_t, double>> where;
    for (std::uint32_t n = 1; n < network_->node_count(); ++n) {
        const auto* node = network_->node(n);
        if (!node || node->kind != formats::TrackNodeKind::vector) continue;
        for (const auto& entry : node->items) where[entry.item] = {n, entry.offset};
    }
    const auto& items = network_->database().items;
    for (const auto& [id, place] : where) {
        if (id >= items.size()) continue;
        const auto& item = items[id];
        const bool platform = item.kind == formats::TrackItemKind::platform;
        if (!platform && item.kind != formats::TrackItemKind::siding) continue;
        const auto other = where.find(item.linked);
        if (other != where.end() && item.linked < id) continue; // each pair once
        Place result{.platform = platform, .name = item.name};
        if (platform && !item.station.empty() && item.station != item.name) result.name = item.station + " (" + item.name + ")";
        const auto add = [&](const std::uint32_t node, const double offset) {
            if (const auto p = network_->point(node, offset)) {
                result.x.push_back(world_x(*p));
                result.z.push_back(world_z(*p));
            }
        };
        if (other != where.end() && other->second.first == place.first) {
            const double a = place.second, b = other->second.second;
            const int steps = (std::max)(1, static_cast<int>(std::abs(b - a) / 10.0));
            for (int k = 0; k <= steps; ++k) add(place.first, a + (b - a) * k / steps);
            result.length = std::abs(b - a);
        } else {
            add(place.first, place.second);
            if (other != where.end()) add(other->second.first, other->second.second);
            // Ends on two nodes: the straight distance between them.
            if (result.x.size() == 2) result.length = std::hypot(result.x[1] - result.x[0], result.z[1] - result.z[0]);
        }
        if (!result.x.empty()) places_.push_back(std::move(result));
    }
}

void TrackMap::hover_tooltip(const ImVec2 mouse, const simulation::SignalSystem* signals) const {
    // A signal within 8 px.
    if (signals && layers.signals && scale_ > 0.15) {
        const auto& objects = signals->objects();
        const auto& heads = signals->heads();
        std::optional<std::uint32_t> nearest;
        double best = 8.0;
        for (std::uint32_t i = 0; i < objects.size(); ++i) {
            const auto p = screen(place_of(objects[i].placement.position));
            const double d = std::hypot(p.x - mouse.x, p.y - mouse.y);
            if (d < best) {
                best = d;
                nearest = i;
            }
        }
        if (nearest) {
            const auto name = signal_name ? signal_name(*nearest) : std::string();
            ImGui::BeginTooltip();
            if (name.empty()) {
                ImGui::Text(tr("Signal %u (no name)"), *nearest);
            } else {
                ImGui::Text(tr("Signal %s"), name.c_str());
            }
            // Its heads (a signal is drawn by one or more: each its sigcfg
            // type and its own lamps): the aspect, what is lit (the draw
            // state's name, "dark" none) and the speed.
            ImGui::TextDisabled(tr("%zu heads: type, aspect, lit, speed"), objects[*nearest].heads.size());
            for (const auto h : objects[*nearest].heads) {
                const auto& head = heads[h];
                const auto aspect = static_cast<formats::SignalAspect>(std::clamp(static_cast<int>(head.state), 0, 9));
                std::string lit = tr("dark");
                if (head.type && head.draw_state >= 0.0f) {
                    for (const auto& state : head.type->draw_states) {
                        if (state.index == static_cast<std::uint32_t>(head.draw_state)) lit = '"' + state.name + '"';
                    }
                }
                if (head.speed >= 0.0f) {
                    ImGui::TextDisabled(tr("%s: %s, %s, %.0f km/h"), head.type ? head.type->name.c_str() : "?", formats::to_string(aspect), lit.c_str(),
                                        head.speed * 3.6);
                } else {
                    ImGui::TextDisabled(tr("%s: %s, %s"), head.type ? head.type->name.c_str() : "?", formats::to_string(aspect), lit.c_str());
                }
            }
            if (signal_notes) {
                for (const auto& note : signal_notes(*nearest)) ImGui::TextUnformatted(note.c_str());
            }
            ImGui::EndTooltip();
            return;
        }
    }
    // A platform or a siding within 6 px of its line.
    if ((layers.platforms || layers.sidings) && scale_ > 0.05) {
        const Place* nearest = nullptr;
        double best = 6.0;
        for (const auto& place : places_) {
            if (place.platform ? !layers.platforms : !layers.sidings) continue;
            for (std::size_t k = 0; k < place.x.size(); ++k) {
                double d = 0.0;
                if (k + 1 < place.x.size()) {
                    double t = 0.0;
                    const auto a = screen(place.x[k], place.z[k]), b = screen(place.x[k + 1], place.z[k + 1]);
                    d = segment_distance(mouse.x, mouse.y, a.x, a.y, b.x, b.y, t);
                } else if (place.x.size() == 1) {
                    const auto a = screen(place.x[k], place.z[k]);
                    d = std::hypot(a.x - mouse.x, a.y - mouse.y);
                } else {
                    continue;
                }
                if (d < best) {
                    best = d;
                    nearest = &place;
                }
            }
        }
        if (nearest) {
            ImGui::BeginTooltip();
            ImGui::Text("%s", nearest->name.empty() ? tr("(no name)") : nearest->name.c_str());
            if (nearest->length > 0.0) {
                ImGui::TextDisabled(nearest->platform ? tr("platform, %.0f m") : tr("siding, %.0f m"), nearest->length);
            } else {
                ImGui::TextDisabled("%s", nearest->platform ? tr("platform") : tr("siding"));
            }
            ImGui::EndTooltip();
        }
    }
}

Pick TrackMap::pick(const ImVec2 mouse, const simulation::SignalSystem* signals) const {
    const double cx = canvas_x_ + canvas_w_ * 0.5, cy = canvas_y_ + canvas_h_ * 0.5;
    Pick result;
    result.x = centre_x_ + (mouse.x - cx) / scale_;
    result.z = centre_z_ - (mouse.y - cy) / scale_;
    double best = 9.0;
    double t = 0.0;
    const auto consider = [&](const double d, const Pick& candidate) {
        if (d < best) {
            best = d;
            const double x = result.x, z = result.z;
            result = candidate;
            result.x = x;
            result.z = z;
        }
    };
    // Trains: their cars.
    for (const auto& train : trains) {
        for (const auto& [front, rear] : train.cars) {
            const auto a = screen(front), b = screen(rear);
            consider(segment_distance(mouse.x, mouse.y, a.x, a.y, b.x, b.y, t) - 1.0, Pick{.kind = PickKind::train, .train = train.id});
        }
    }
    // Junctions.
    for (std::uint32_t n = 1; n < network_->node_count(); ++n) {
        const auto* node = network_->node(n);
        if (!node || node->kind != formats::TrackNodeKind::junction) continue;
        const auto where = network_->node_position(n);
        if (!where) continue;
        const auto p = screen(*where);
        consider(std::hypot(p.x - mouse.x, p.y - mouse.y), Pick{.kind = PickKind::junction, .junction = n});
    }
    // Signals.
    if (signals && layers.signals) {
        const auto& objects = signals->objects();
        for (std::uint32_t i = 0; i < objects.size(); ++i) {
            const auto p = screen(place_of(objects[i].placement.position));
            consider(std::hypot(p.x - mouse.x, p.y - mouse.y) + 1.0, Pick{.kind = PickKind::signal, .signal = i});
        }
    }
    if (result.kind != PickKind::none) return result;
    if (const auto track = nearest_track(mouse, 12.0)) {
        result.kind = PickKind::track;
        result.location = *track;
    }
    return result;
}

std::optional<simulation::TrackLocation> TrackMap::nearest_track(const ImVec2 mouse, const double reach) const {
    std::optional<simulation::TrackLocation> result;
    double best = reach;
    double t = 0.0;
    for (const auto& line : lines_) {
        for (std::size_t i = 0; i + 1 < line.x.size(); ++i) {
            const auto a = screen(line.x[i], line.z[i]), b = screen(line.x[i + 1], line.z[i + 1]);
            const double d = segment_distance(mouse.x, mouse.y, a.x, a.y, b.x, b.y, t);
            if (d >= best) continue;
            best = d;
            result = simulation::TrackLocation{line.node, line.offset[i] + (line.offset[i + 1] - line.offset[i]) * t, true};
        }
    }
    return result;
}

TrackMap::Frame TrackMap::draw(const simulation::SignalSystem* signals, const bool quiet) {
    Frame frame;
    const ImVec2 corner = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.x = (std::max)(size.x, 50.0f);
    size.y = (std::max)(size.y, 50.0f);
    canvas_x_ = corner.x;
    canvas_y_ = corner.y;
    canvas_w_ = size.x;
    canvas_h_ = size.y;
    ImGui::InvisibleButton("track_map", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    frame.hovered = ImGui::IsItemHovered();
    frame.active = ImGui::IsItemActive();
    auto& io = ImGui::GetIO();
    frame.mouse = io.MousePos;
    const double cx = corner.x + size.x * 0.5, cy = corner.y + size.y * 0.5;

    // The wheel zooms about the cursor; a right drag pans; a right click
    // without a drag is a menu; a left click picks.
    if (frame.hovered && io.MouseWheel != 0.0f) {
        const double wx = centre_x_ + (io.MousePos.x - cx) / scale_;
        const double wz = centre_z_ - (io.MousePos.y - cy) / scale_;
        scale_ = std::clamp(scale_ * std::pow(1.25, static_cast<double>(io.MouseWheel)), 0.002, 20.0);
        centre_x_ = wx - (io.MousePos.x - cx) / scale_;
        centre_z_ = wz + (io.MousePos.y - cy) / scale_;
    }
    if (frame.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) drag_ = 0.0f;
    if (frame.active && ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f)) {
        centre_x_ -= io.MouseDelta.x / scale_;
        centre_z_ += io.MouseDelta.y / scale_;
        drag_ += std::abs(io.MouseDelta.x) + std::abs(io.MouseDelta.y);
    }
    if (frame.hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && drag_ < 4.0f) frame.right_click = pick(io.MousePos, signals);
    if (frame.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) frame.left_click = pick(io.MousePos, signals);
    if (frame.hovered && !frame.active && !quiet) hover_tooltip(io.MousePos, signals);

    const auto sx = [&](const double x) { return static_cast<float>(cx + (x - centre_x_) * scale_); };
    const auto sy = [&](const double z) { return static_cast<float>(cy - (z - centre_z_) * scale_); };
    const auto at = [&](const simulation::TrackPoint& p) { return ImVec2(sx(world_x(p)), sy(world_z(p))); };
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 far_corner(corner.x + size.x, corner.y + size.y);
    // RTS track control: light green background, black track.
    draw->AddRectFilled(corner, far_corner, IM_COL32(204, 255, 204, 255));
    draw->PushClipRect(corner, far_corner, true);
    const double half_w = size.x * 0.5 / scale_, half_h = size.y * 0.5 / scale_;
    const double left = centre_x_ - half_w, right = centre_x_ + half_w;
    const double bottom = centre_z_ - half_h, top = centre_z_ + half_h;
    std::vector<ImVec2> points;
    const auto polyline = [&](const std::vector<simulation::TrackPoint>& route_points, const ImU32 colour, const float width) {
        points.clear();
        for (const auto& p : route_points) points.push_back(at(p));
        if (points.size() > 1) draw->AddPolyline(points.data(), static_cast<int>(points.size()), colour, 0, width);
    };
    // The track's line; platforms and sidings are drawn over it as wide.
    constexpr float track_width = 2.0f;
    for (const auto& line : lines_) {
        if (line.max_x < left || line.min_x > right || line.max_z < bottom || line.min_z > top) continue;
        points.clear();
        for (std::size_t i = 0; i < line.x.size(); ++i) points.emplace_back(sx(line.x[i]), sy(line.z[i]));
        draw->AddPolyline(points.data(), static_cast<int>(points.size()), IM_COL32(30, 30, 30, 255), 0, track_width);
    }
    const auto visible = [&](const ImVec2& p) {
        return p.x > corner.x - 20 && p.x < far_corner.x + 20 && p.y > corner.y - 20 && p.y < far_corner.y + 20;
    };
    // The owner's lines: wire, paths, routes.
    for (const auto& line : lines) polyline(line.points, line.colour, line.width);
    // Junctions: red occupied, orange reserved; the set branch in green
    // when zoomed in.
    for (std::uint32_t n = 1; n < network_->node_count(); ++n) {
        const auto* node = network_->node(n);
        if (!node || node->kind != formats::TrackNodeKind::junction) continue;
        const auto where = network_->node_position(n);
        if (!where) continue;
        const auto p = at(*where);
        if (!visible(p)) continue;
        const auto* state = network_->junction(n);
        ImU32 colour = IM_COL32(90, 90, 90, 255);
        if (state && !state->reservations.empty()) colour = IM_COL32(240, 150, 0, 255);
        if (state && state->occupied) colour = IM_COL32(220, 0, 0, 255);
        if (state && scale_ > 0.4 && state->selected + 1 < node->pins.size()) {
            const auto& pin = node->pins[1 + state->selected];
            const auto* branch = network_->node(pin.node);
            if (branch && branch->kind == formats::TrackNodeKind::vector) {
                const double reach = (std::min)(25.0, branch->length);
                polyline(along(branch->index, pin.direction == 1 ? 0.0 : branch->length, pin.direction == 1 ? reach : branch->length - reach),
                         IM_COL32(0, 200, 0, 255), 4.0f);
            }
        }
        draw->AddCircleFilled(p, scale_ > 0.4 ? 4.0f : 2.5f, colour);
        if (layers.junction_ids && scale_ > 0.4) {
            char text[16];
            std::snprintf(text, sizeof text, "%u", n);
            draw->AddText(ImVec2(p.x + 5.0f, p.y - 14.0f), IM_COL32(60, 60, 60, 255), text);
        }
    }
    // Signals: a square in the colour of the obeyed NORMAL head.
    if (signals && layers.signals && scale_ > 0.15) {
        const auto& objects = signals->objects();
        const auto& heads = signals->heads();
        for (std::uint32_t i = 0; i < objects.size(); ++i) {
            const auto p = at(place_of(objects[i].placement.position));
            if (!visible(p)) continue;
            int aspect = -1;
            for (const auto h : objects[i].heads) {
                const auto& head = heads[h];
                if (head.type && head.type->function == formats::SignalFunction::normal) aspect = (std::max)(aspect, static_cast<int>(head.state));
            }
            if (aspect < 0 && !objects[i].heads.empty()) aspect = static_cast<int>(heads[objects[i].heads.front()].state);
            const ImU32 colour = aspect <= 1 ? IM_COL32(230, 0, 0, 255) : aspect <= 5 ? IM_COL32(240, 200, 0, 255) : IM_COL32(0, 190, 0, 255);
            // Which track it governs: a line from the signal to the place of
            // each head on the track, and a mark there pointing the way the
            // head protects.
            for (const auto h : objects[i].heads) {
                const auto& head = heads[h];
                if (!head.placed) continue;
                const auto track = network_->point(head.node, head.offset);
                if (!track) continue;
                const auto q = at(*track);
                draw->AddLine(p, q, IM_COL32(60, 60, 60, 220), 1.5f);
                const float heading = track->heading + (head.forward ? 0.0f : 3.14159265f);
                const ImVec2 dir(std::sin(heading), -std::cos(heading));
                const float mark = std::clamp(static_cast<float>(scale_ * 4.0), 4.0f, 10.0f);
                draw->AddLine(q, ImVec2(q.x + dir.x * mark, q.y + dir.y * mark), colour, 2.0f);
                draw->AddCircleFilled(q, 2.0f, IM_COL32(40, 40, 40, 255));
            }
            const float r = 3.5f;
            draw->AddRectFilled(ImVec2(p.x - r, p.y - r), ImVec2(p.x + r, p.y + r), colour);
            draw->AddRect(ImVec2(p.x - r, p.y - r), ImVec2(p.x + r, p.y + r),
                          selected_signal == i ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 255));
            // The names are sized to the map: 2.5 m tall, at most 16 px, and
            // left out below 7 px (a fixed size made a mess zoomed out).
            const float name_size = (std::min)(static_cast<float>(2.5 * scale_), 16.0f);
            if (layers.signal_names && signal_name && name_size >= 7.0f) {
                const auto name = signal_name(i);
                if (!name.empty()) {
                    // Along the track its first head governs, beside it on
                    // the signal's side; at the signal when it has no place.
                    std::optional<simulation::TrackPoint> track;
                    for (const auto h : objects[i].heads) {
                        if (heads[h].placed && (track = network_->point(heads[h].node, heads[h].offset))) break;
                    }
                    if (track) {
                        const ImVec2 dir(std::sin(track->heading), -std::cos(track->heading));
                        text_along(draw, at(*track), dir, IM_COL32(120, 60, 0, 255), name.c_str(), name_size, name_size * 0.35f, p, false);
                    } else {
                        draw->AddText(ImVec2(p.x + 5.0f, p.y - 15.0f), IM_COL32(120, 60, 0, 255), name.c_str());
                    }
                }
            }
        }
    }
    // Platforms (blue) and sidings (yellow): the track's line in their colour
    // between their two ends, and the name above its middle.
    if ((layers.platforms || layers.sidings) && scale_ > 0.05) {
        for (const auto& place : places_) {
            if (place.platform ? !layers.platforms : !layers.sidings) continue;
            points.clear();
            for (std::size_t k = 0; k < place.x.size(); ++k) points.emplace_back(sx(place.x[k]), sy(place.z[k]));
            const auto& middle = points[points.size() / 2];
            // Drawn when its line's box meets the map: a long platform seen
            // in part, its middle and ends off the screen, had vanished.
            float min_x = points.front().x, max_x = min_x, min_y = points.front().y, max_y = min_y;
            for (const auto& q : points) {
                min_x = (std::min)(min_x, q.x);
                max_x = (std::max)(max_x, q.x);
                min_y = (std::min)(min_y, q.y);
                max_y = (std::max)(max_y, q.y);
            }
            if (max_x < corner.x - 20 || min_x > far_corner.x + 20 || max_y < corner.y - 20 || min_y > far_corner.y + 20) continue;
            const ImU32 colour = place.platform ? IM_COL32(30, 90, 230, 255) : IM_COL32(230, 190, 0, 255);
            const float width = track_width;
            if (points.size() > 1) {
                draw->AddPolyline(points.data(), static_cast<int>(points.size()), colour, 0, width);
            } else {
                draw->AddCircleFilled(points.front(), width * 0.6f, colour);
            }
            // Names sized to the map: 3.5 m tall, at most 18 px, left out below 7 px.
            const float name_size = (std::min)(static_cast<float>(3.5 * scale_), 18.0f);
            if (name_size >= 7.0f && !place.name.empty()) {
                // Along the line at its middle, just above it as it reads.
                const auto& a = points[points.size() > 2 ? points.size() / 2 - 1 : 0];
                const auto& b = points[(std::min)(points.size() / 2 + 1, points.size() - 1)];
                text_along(draw, middle, ImVec2(b.x - a.x, b.y - a.y), place.platform ? IM_COL32(0, 0, 170, 255) : IM_COL32(130, 100, 0, 255),
                           place.name.c_str(), name_size, width * 0.5f + 2.0f, std::nullopt, true);
            }
        }
    }
    // Station names (RTS 0x53F990): black Arial at the middle of the
    // station's platform items, a little right and up. RTS shows them while
    // its zoom (half the view in units of 64 m) is below 3500 and the station
    // has more platform items than zoom / 300, so the big stations stay named
    // farthest out.
    if (layers.station_names) {
        const double half = (std::min)(size.x, size.y) * 0.5;
        const double rts_zoom = half / scale_ / 64.0;
        if (rts_zoom < 3500.0) {
            ImFont* font = ui::map_label_font() ? ui::map_label_font() : ImGui::GetFont();
            const float font_size = ui::map_label_font() ? ui::map_label_font()->FontSize : ImGui::GetFontSize() * 1.3f;
            const float offset = static_cast<float>(0.03 * half);
            for (const auto& station : stations_) {
                if (station.items * 300.0 <= rts_zoom) continue;
                const ImVec2 base(sx(station.x) + offset, sy(station.z) - offset);
                if (!visible(base)) continue;
                draw->AddText(font, font_size, ImVec2(base.x, base.y - font_size), IM_COL32(0, 0, 0, 255), station.name.c_str());
            }
        }
    }
    // The owner's marks.
    for (const auto& mark : marks) {
        const auto p = at(mark.at);
        if (!visible(p)) continue;
        if (mark.ring) {
            draw->AddCircle(p, mark.radius, mark.colour, 0, 2.0f);
        } else {
            draw->AddCircleFilled(p, mark.radius, mark.colour);
        }
        if (!mark.letter.empty()) draw->AddText(ImVec2(p.x - 4.0f, p.y - 7.0f), mark.letter_colour, mark.letter.c_str());
    }
    // Trains: their cars; the selected one outlined.
    for (const auto& train : trains) {
        const float width = std::clamp(static_cast<float>(scale_ * 3.2), 4.0f, 14.0f);
        for (const auto& [front, rear] : train.cars) {
            const auto a = at(front), b = at(rear);
            if (train.id == selected_train) draw->AddLine(a, b, IM_COL32(255, 255, 255, 255), width + 4.0f);
            draw->AddLine(a, b, train.derailed ? IM_COL32(255, 0, 0, 255) : train.colour, width);
        }
        // The way it faces: an arrow ahead of its head, along its first car
        // (the whole train when that is too short on the screen to tell).
        if (!train.cars.empty()) {
            const auto head = at(train.cars.front().first);
            auto behind = at(train.cars.front().second);
            if (std::hypot(head.x - behind.x, head.y - behind.y) < 3.0f) behind = at(train.cars.back().second);
            const ImU32 fill = train.derailed ? IM_COL32(255, 0, 0, 255) : train.colour;
            const ImU32 edge = train.id == selected_train ? IM_COL32(255, 255, 255, 255) : IM_COL32(20, 20, 20, 255);
            draw_direction_arrow(draw, head, behind, fill, edge, std::clamp(width * 1.8f, 12.0f, 24.0f));
        }
        if (!train.cars.empty() && !train.label.empty()) {
            const auto head = at(train.cars.front().first);
            draw->AddText(ImVec2(head.x + 6.0f, head.y - 16.0f), IM_COL32(0, 0, 0, 255), train.label.c_str());
        }
    }
    if (overlay) overlay(draw);
    // Scale bar.
    {
        double metres = 1.0;
        while (metres * scale_ < 60.0) metres *= metres >= 1000.0 ? 2.0 : 10.0;
        const float length = static_cast<float>(metres * scale_);
        const ImVec2 a(corner.x + 12.0f, far_corner.y - 14.0f);
        draw->AddLine(a, ImVec2(a.x + length, a.y), IM_COL32(0, 0, 0, 255), 2.0f);
        char text[32];
        std::snprintf(text, sizeof text, metres >= 1000.0 ? "%.0f km" : "%.0f m", metres >= 1000.0 ? metres / 1000.0 : metres);
        draw->AddText(ImVec2(a.x, a.y - 16.0f), IM_COL32(0, 0, 0, 255), text);
    }
    draw->PopClipRect();
    return frame;
}

} // namespace openrts::trackmap
