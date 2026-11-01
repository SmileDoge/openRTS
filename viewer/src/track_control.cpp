#ifdef OPENRTS_HAS_WGPU
#include "track_control.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#ifdef OPENRTS_HAS_IMGUI
#include <imgui.h>
#endif

namespace openrts::viewer {
namespace {

constexpr double tile = 2048.0;

[[nodiscard]] const char* role_name(const TrainView::Role role) {
    switch (role) {
    case TrainView::Role::driven: return "driven";
    case TrainView::Role::ai: return "test driver";
    case TrainView::Role::standing: return "standing";
    }
    return "?";
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

} // namespace

TrackControl::TrackControl(simulation::TrackNetwork& network, const formats::RoutePosition origin)
    : network_(&network), origin_(origin) {
    build_lines();
}

double TrackControl::world_x(const simulation::TrackPoint& point) const {
    return (point.tile_x - origin_.tile_x) * tile + point.x - origin_.x;
}

double TrackControl::world_z(const simulation::TrackPoint& point) const {
    return (point.tile_z - origin_.tile_z) * tile + point.z - origin_.z;
}

void TrackControl::build_lines() {
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

TrackControl::Pick TrackControl::pick(const float mouse_x, const float mouse_y, TrainView& trains,
                                      const simulation::SignalSystem* signals) const {
    const double cx = canvas_x_ + canvas_w_ * 0.5, cy = canvas_y_ + canvas_h_ * 0.5;
    const auto sx = [&](const double x) { return cx + (x - centre_x_) * scale_; };
    const auto sy = [&](const double z) { return cy - (z - centre_z_) * scale_; };
    Pick result;
    result.x = centre_x_ + (mouse_x - cx) / scale_;
    result.z = centre_z_ - (mouse_y - cy) / scale_;
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
    // Path points of the edited path.
    if (editing_) {
        if (const auto start = network_->point(plan_.start.node, plan_.start.offset)) {
            consider(std::hypot(sx(world_x(*start)) - mouse_x, sy(world_z(*start)) - mouse_y) - 2.0,
                     Pick{.kind = PickKind::start_point});
        }
        for (std::size_t i = 0; i < plan_.points.size(); ++i) {
            if (const auto where = network_->point(plan_.points[i].node, plan_.points[i].offset)) {
                consider(std::hypot(sx(world_x(*where)) - mouse_x, sy(world_z(*where)) - mouse_y) - 2.0,
                         Pick{.kind = PickKind::plan_point, .point = i});
            }
        }
    }
    // Trains: their cars.
    for (const auto& summary : trains.summaries()) {
        const auto* train = trains.train_by_id(summary.id);
        if (!train || !train->placed()) continue;
        for (const auto& car : train->placements()) {
            const double d = segment_distance(mouse_x, mouse_y, sx(world_x(car.front)), sy(world_z(car.front)),
                                              sx(world_x(car.rear)), sy(world_z(car.rear)), t);
            consider(d - 1.0, Pick{.kind = PickKind::train, .train = summary.id});
        }
    }
    // Junctions.
    for (std::uint32_t n = 1; n < network_->node_count(); ++n) {
        const auto* node = network_->node(n);
        if (!node || node->kind != formats::TrackNodeKind::junction) continue;
        const auto where = network_->node_position(n);
        if (!where) continue;
        consider(std::hypot(sx(world_x(*where)) - mouse_x, sy(world_z(*where)) - mouse_y), Pick{.kind = PickKind::junction, .junction = n});
    }
    // Signals.
    if (signals && show_signals_) {
        const auto& objects = signals->objects();
        for (std::uint32_t i = 0; i < objects.size(); ++i) {
            const auto& p = objects[i].placement.position;
            const simulation::TrackPoint where{.tile_x = p.tile_x, .tile_z = p.tile_z, .x = p.x, .y = p.y, .z = p.z};
            consider(std::hypot(sx(world_x(where)) - mouse_x, sy(world_z(where)) - mouse_y) + 1.0,
                     Pick{.kind = PickKind::signal, .signal = i});
        }
    }
    if (result.kind != PickKind::none) return result;
    // The track.
    best = 12.0;
    for (const auto& line : lines_) {
        for (std::size_t i = 0; i + 1 < line.x.size(); ++i) {
            const double d = segment_distance(mouse_x, mouse_y, sx(line.x[i]), sy(line.z[i]), sx(line.x[i + 1]), sy(line.z[i + 1]), t);
            if (d >= best) continue;
            best = d;
            result.kind = PickKind::track;
            result.location = {line.node, line.offset[i] + (line.offset[i + 1] - line.offset[i]) * t, true};
        }
    }
    return result;
}

void TrackControl::apply_plan(TrainView& trains) {
    if (selected_ == 0) return;
    const auto error = trains.assign_path(selected_, plan_);
    if (error.empty()) {
        const auto* route = trains.dispatcher().route(selected_);
        message_ = "path set: " + std::to_string(route ? route->legs.size() : 0) + " leg(s)";
    } else {
        message_ = "path: " + error;
    }
}

void TrackControl::start_editing(TrainView& trains, const std::uint64_t id) {
    selected_ = id;
    editing_ = true;
    if (const auto* plan = trains.dispatcher().plan(id)) {
        plan_ = *plan;
        return;
    }
    plan_ = {};
    plan_.name = "path";
    if (const auto* train = trains.train_by_id(id)) {
        // From the train's rear, the way its front faces.
        if (const auto rear = train->rear_end()) plan_.start = {rear->node(), rear->offset(), rear->forward()};
    }
    message_ = "editing: right-click the track to place waiting, reverse and end points";
}

void TrackControl::draw(TrainView& trains, simulation::SignalSystem* signals) {
#ifdef OPENRTS_HAS_IMGUI
    if (!open_) return;
    if (!centred_) {
        centred_ = true;
        const auto* driven = trains.train();
        if (driven && driven->placed() && driven->front_end()) {
            const auto head = driven->front_end()->pose();
            centre_x_ = world_x(head);
            centre_z_ = world_z(head);
        }
    }
    ImGui::SetNextWindowSize(ImVec2(1150.0f, 720.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Track control (F7)", &open_)) {
        ImGui::End();
        return;
    }
    ImGui::BeginChild("panel", ImVec2(320.0f, 0.0f), true);
    draw_panel(trains, signals);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("map", ImVec2(0.0f, 0.0f), true, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    draw_map(trains, signals);
    ImGui::EndChild();
    ImGui::End();
#else
    (void)trains;
    (void)signals;
#endif
}

void TrackControl::draw_map(TrainView& trains, const simulation::SignalSystem* signals) {
#ifdef OPENRTS_HAS_IMGUI
    const ImVec2 corner = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.x = (std::max)(size.x, 50.0f);
    size.y = (std::max)(size.y, 50.0f);
    canvas_x_ = corner.x;
    canvas_y_ = corner.y;
    canvas_w_ = size.x;
    canvas_h_ = size.y;
    ImGui::InvisibleButton("canvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    auto& io = ImGui::GetIO();
    const double cx = corner.x + size.x * 0.5, cy = corner.y + size.y * 0.5;

    // Wheel zooms about the cursor; right drag pans; a right click opens the
    // menu for what is under the cursor; a left click selects.
    if (hovered && io.MouseWheel != 0.0f) {
        const double wx = centre_x_ + (io.MousePos.x - cx) / scale_;
        const double wz = centre_z_ - (io.MousePos.y - cy) / scale_;
        scale_ = std::clamp(scale_ * std::pow(1.25, static_cast<double>(io.MouseWheel)), 0.002, 20.0);
        centre_x_ = wx - (io.MousePos.x - cx) / scale_;
        centre_z_ = wz + (io.MousePos.y - cy) / scale_;
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) drag_ = 0.0f;
    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f)) {
        centre_x_ -= io.MouseDelta.x / scale_;
        centre_z_ += io.MouseDelta.y / scale_;
        drag_ += std::abs(io.MouseDelta.x) + std::abs(io.MouseDelta.y);
    }
    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && drag_ < 4.0f) {
        menu_ = pick(io.MousePos.x, io.MousePos.y, trains, signals);
        ImGui::OpenPopup("track_control_menu");
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const auto picked = pick(io.MousePos.x, io.MousePos.y, trains, signals);
        if (picked.kind == PickKind::train) selected_ = picked.train;
        if (picked.kind == PickKind::signal) selected_signal_ = picked.signal;
    }

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
    for (const auto& line : lines_) {
        if (line.max_x < left || line.min_x > right || line.max_z < bottom || line.min_z > top) continue;
        points.clear();
        for (std::size_t i = 0; i < line.x.size(); ++i) points.emplace_back(sx(line.x[i]), sy(line.z[i]));
        draw->AddPolyline(points.data(), static_cast<int>(points.size()), IM_COL32(30, 30, 30, 255), 0, 2.0f);
    }
    const auto visible = [&](const ImVec2& p) {
        return p.x > corner.x - 20 && p.x < far_corner.x + 20 && p.y > corner.y - 20 && p.y < far_corner.y + 20;
    };
    // A polyline along a stretch of a vector node.
    const auto stretch = [&](const std::uint32_t node, const double from, const double to, std::vector<ImVec2>& out) {
        const double length = std::abs(to - from);
        const int steps = (std::max)(1, static_cast<int>(length / 10.0));
        for (int i = 0; i <= steps; ++i) {
            const double offset = from + (to - from) * i / steps;
            if (const auto p = network_->point(node, offset)) out.push_back(at(*p));
        }
    };

    // Paths: the reserved way green, the rest of the leg blue, later legs pale.
    for (const auto& summary : trains.summaries()) {
        if (!show_all_paths_ && summary.id != selected_) continue;
        const auto* route = trains.dispatcher().route(summary.id);
        const auto* progress = trains.dispatcher().progress(summary.id);
        if (!route || !progress) continue;
        for (std::size_t l = progress->leg; l < route->legs.size(); ++l) {
            const auto& leg = route->legs[l];
            for (const auto& span : leg.spans) {
                const double end = span.start + span.length();
                const auto piece = [&](double from, double to, const ImU32 colour, const float width) {
                    from = (std::max)(from, span.start);
                    to = (std::min)(to, end);
                    if (to <= from) return;
                    points.clear();
                    const double a = span.forward ? span.from + (from - span.start) : span.from - (from - span.start);
                    const double b = span.forward ? span.from + (to - span.start) : span.from - (to - span.start);
                    stretch(span.node, a, b, points);
                    if (points.size() > 1) draw->AddPolyline(points.data(), static_cast<int>(points.size()), colour, 0, width);
                };
                if (l == progress->leg) {
                    piece(progress->head, progress->authority, IM_COL32(0, 170, 0, 230), 6.0f);
                    piece(progress->authority, leg.length(), IM_COL32(40, 90, 230, 200), 4.0f);
                } else {
                    piece(0.0, leg.length(), IM_COL32(120, 150, 240, 160), 3.0f);
                }
            }
            for (const auto& mark : leg.marks) {
                const auto location = leg.at(mark.distance);
                if (!location) continue;
                const auto where = network_->point(location->node, location->offset);
                if (!where) continue;
                const char* letter = mark.kind == simulation::RouteMarkKind::wait      ? "W"
                                     : mark.kind == simulation::RouteMarkKind::reverse ? "R"
                                     : mark.kind == simulation::RouteMarkKind::end     ? "E"
                                                                                       : "o";
                const auto p = at(*where);
                draw->AddCircleFilled(p, 7.0f, IM_COL32(255, 255, 255, 230));
                draw->AddText(ImVec2(p.x - 4.0f, p.y - 7.0f), IM_COL32(20, 20, 160, 255), letter);
            }
        }
    }
    // The edited path's points.
    if (editing_) {
        if (const auto start = network_->point(plan_.start.node, plan_.start.offset)) {
            const auto p = at(*start);
            const float heading = start->heading + (plan_.start.forward ? 0.0f : 3.14159265f);
            const ImVec2 tip(p.x + std::sin(heading) * 12.0f, p.y - std::cos(heading) * 12.0f);
            draw->AddLine(p, tip, IM_COL32(200, 0, 120, 255), 3.0f);
            draw->AddCircleFilled(p, 6.0f, IM_COL32(200, 0, 120, 255));
            draw->AddText(ImVec2(p.x + 6.0f, p.y + 2.0f), IM_COL32(120, 0, 80, 255), "S");
        }
        for (std::size_t i = 0; i < plan_.points.size(); ++i) {
            const auto where = network_->point(plan_.points[i].node, plan_.points[i].offset);
            if (!where) continue;
            const auto p = at(*where);
            draw->AddCircle(p, 8.0f, IM_COL32(200, 0, 120, 255), 0, 2.0f);
        }
    }
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
                points.clear();
                const double reach = (std::min)(25.0, branch->length);
                stretch(branch->index, pin.direction == 1 ? 0.0 : branch->length, pin.direction == 1 ? reach : branch->length - reach, points);
                if (points.size() > 1) draw->AddPolyline(points.data(), static_cast<int>(points.size()), IM_COL32(0, 200, 0, 255), 0, 4.0f);
            }
        }
        draw->AddCircleFilled(p, scale_ > 0.4 ? 4.0f : 2.5f, colour);
        if (show_junction_ids_ && scale_ > 0.4) {
            char text[16];
            std::snprintf(text, sizeof text, "%u", n);
            draw->AddText(ImVec2(p.x + 5.0f, p.y - 14.0f), IM_COL32(60, 60, 60, 255), text);
        }
    }
    // Signals: a square in the colour of the obeyed NORMAL head.
    if (signals && show_signals_ && scale_ > 0.15) {
        const auto& objects = signals->objects();
        const auto& heads = signals->heads();
        for (std::uint32_t i = 0; i < objects.size(); ++i) {
            const auto& pos = objects[i].placement.position;
            const auto p = at(simulation::TrackPoint{.tile_x = pos.tile_x, .tile_z = pos.tile_z, .x = pos.x, .y = pos.y, .z = pos.z});
            if (!visible(p)) continue;
            int aspect = -1;
            for (const auto h : objects[i].heads) {
                const auto& head = heads[h];
                if (head.type && head.type->function == formats::SignalFunction::normal) aspect = (std::max)(aspect, static_cast<int>(head.state));
            }
            if (aspect < 0 && !objects[i].heads.empty()) aspect = static_cast<int>(heads[objects[i].heads.front()].state);
            const ImU32 colour = aspect <= 1 ? IM_COL32(230, 0, 0, 255) : aspect <= 5 ? IM_COL32(240, 200, 0, 255) : IM_COL32(0, 190, 0, 255);
            const float r = 3.5f;
            draw->AddRectFilled(ImVec2(p.x - r, p.y - r), ImVec2(p.x + r, p.y + r), colour);
            draw->AddRect(ImVec2(p.x - r, p.y - r), ImVec2(p.x + r, p.y + r),
                          selected_signal_ == i ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 255));
        }
    }
    // Platforms and stations (names), as Track control writes them.
    if (show_platforms_ && scale_ > 0.3) {
        for (const auto& item : network_->database().items) {
            if (item.kind != formats::TrackItemKind::platform || !item.has_position || item.name.empty()) continue;
            const auto p = at(simulation::TrackPoint{.tile_x = item.tile_x, .tile_z = item.tile_z, .x = item.x, .z = item.z});
            if (!visible(p)) continue;
            draw->AddText(ImVec2(p.x + 4.0f, p.y + 4.0f), IM_COL32(0, 0, 180, 255), item.name.c_str());
        }
    }
    // Trains: their cars, in the colour of their role; the selected one outlined.
    for (const auto& summary : trains.summaries()) {
        const auto* train = trains.train_by_id(summary.id);
        if (!train || !train->placed()) continue;
        const ImU32 colour = summary.role == TrainView::Role::driven ? IM_COL32(200, 30, 200, 255)
                           : summary.role == TrainView::Role::ai     ? IM_COL32(40, 60, 220, 255)
                                                                     : IM_COL32(140, 120, 20, 255);
        const auto cars = train->placements();
        const float width = std::clamp(static_cast<float>(scale_ * 3.2), 4.0f, 14.0f);
        for (const auto& car : cars) {
            const auto a = at(car.front), b = at(car.rear);
            if (summary.id == selected_) draw->AddLine(a, b, IM_COL32(255, 255, 255, 255), width + 4.0f);
            draw->AddLine(a, b, summary.derailed ? IM_COL32(255, 0, 0, 255) : colour, width);
        }
        if (!cars.empty()) {
            const auto head = at(cars.front().front);
            char text[160];
            std::snprintf(text, sizeof text, "%s %.0f km/h", summary.name.c_str(), std::abs(summary.speed) * 3.6);
            draw->AddText(ImVec2(head.x + 6.0f, head.y - 16.0f), IM_COL32(0, 0, 0, 255), text);
        }
    }
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
    draw_menu(trains, const_cast<simulation::SignalSystem*>(signals));
#else
    (void)trains;
    (void)signals;
#endif
}

void TrackControl::draw_menu(TrainView& trains, simulation::SignalSystem* signals) {
#ifdef OPENRTS_HAS_IMGUI
    if (!ImGui::BeginPopup("track_control_menu")) return;
    const auto add_point = [&](const simulation::PlanPointKind kind) {
        simulation::PlanPoint point;
        point.kind = kind;
        point.node = menu_.location.node;
        point.offset = menu_.location.offset;
        point.wait = kind == simulation::PlanPointKind::wait ? wait_seconds_ : 0.0f;
        const bool has_end = !plan_.points.empty() && plan_.points.back().kind == simulation::PlanPointKind::end;
        if (kind == simulation::PlanPointKind::end) {
            if (has_end) plan_.points.back() = point;
            else plan_.points.push_back(point);
        } else if (has_end) {
            plan_.points.insert(plan_.points.end() - 1, point);
        } else {
            plan_.points.push_back(point);
        }
        apply_plan(trains);
    };
    switch (menu_.kind) {
    case PickKind::train: {
        const auto id = menu_.train;
        const auto summaries = trains.summaries();
        const auto it = std::find_if(summaries.begin(), summaries.end(), [&](const auto& s) { return s.id == id; });
        if (it == summaries.end()) break;
        selected_ = id;
        ImGui::TextDisabled("%s (%s, %zu cars)", it->name.c_str(), role_name(it->role), it->cars);
        if (ImGui::MenuItem("Jump to train")) trains.follow_train(id);
        if (ImGui::MenuItem("Drive it (keyboard)", nullptr, it->role == TrainView::Role::driven)) message_ = trains.set_role(id, TrainView::Role::driven);
        if (ImGui::MenuItem("Test driver follows its path", nullptr, it->role == TrainView::Role::ai)) message_ = trains.set_role(id, TrainView::Role::ai);
        if (ImGui::MenuItem("Standing", nullptr, it->role == TrainView::Role::standing)) message_ = trains.set_role(id, TrainView::Role::standing);
        ImGui::Separator();
        if (editing_ && selected_ == id) {
            if (ImGui::MenuItem("Stop editing the path")) editing_ = false;
        } else if (ImGui::MenuItem(trains.dispatcher().plan(id) ? "Edit path" : "Create path")) {
            start_editing(trains, id);
        }
        if (ImGui::MenuItem("Cancel path", nullptr, false, trains.dispatcher().plan(id) != nullptr)) {
            trains.clear_path(id);
            editing_ = false;
        }
        if (ImGui::BeginMenu("Load path")) {
            for (const auto& path : trains.route_paths()) {
                const auto label = path.file + (path.name.empty() ? "" : ": " + path.name);
                if (ImGui::MenuItem(label.c_str())) {
                    const auto error = trains.assign_route_path(id, path);
                    message_ = error.empty() ? "path " + path.file + " set" : error;
                    editing_ = false;
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Save path as", trains.dispatcher().plan(id) != nullptr)) {
            ImGui::InputText("name", path_name_, sizeof path_name_);
            if (ImGui::Button("Save")) {
                const auto error = trains.save_path(id, path_name_);
                message_ = error.empty() ? std::string("saved to ") + trains.user_path_folder().string() : error;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndMenu();
        }
        if (it->cars > 1 && ImGui::BeginMenu("Uncouple cars")) {
            for (std::size_t k = 1; k < it->cars; ++k) {
                char label[48];
                std::snprintf(label, sizeof label, "before car %zu", k + 1);
                if (ImGui::MenuItem(label)) {
                    const auto error = trains.uncouple_at(id, k);
                    message_ = error.empty() ? "uncoupled" : error;
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Remove train")) message_ = trains.remove(id);
        break;
    }
    case PickKind::junction: {
        const auto n = menu_.junction;
        const auto* node = network_->node(n);
        const auto* state = network_->junction(n);
        if (!node || !state) break;
        ImGui::TextDisabled("Junction %u: branch %u of %zu%s", n, state->selected, node->branch_count(),
                            state->occupied ? ", occupied" : "");
        for (const auto& [owner, branch] : state->reservations) {
            const auto* train = trains.train_by_id(owner);
            std::string name = "train " + std::to_string(owner);
            for (const auto& summary : trains.summaries()) {
                if (summary.id == owner) name = summary.name;
            }
            ImGui::TextDisabled("  reserved for %s, branch %u%s", name.c_str(), branch, train ? "" : " (gone)");
        }
        if (ImGui::MenuItem("Throw")) {
            const auto next = static_cast<std::uint32_t>((state->selected + 1) % (std::max<std::size_t>)(node->branch_count(), 1));
            message_ = network_->throw_junction(n, next) ? "junction " + std::to_string(n) + " thrown"
                                                          : "refused: the junction is occupied or held by a path";
        }
        // Switch it on the edited path: the next branch at that pass.
        if (editing_ && selected_ != 0) {
            if (const auto* route = trains.dispatcher().route(selected_)) {
                int pass = 0;
                for (const auto& leg : route->legs) {
                    for (const auto& j : leg.junctions) {
                        if (j.junction != n || !j.facing) continue;
                        ++pass;
                        char label[64];
                        std::snprintf(label, sizeof label, "Switch junction on the path (pass %d)", pass);
                        if (ImGui::MenuItem(label) && j.point < plan_.points.size()) {
                            auto& point = plan_.points[j.point];
                            point.branches[n] = static_cast<std::uint32_t>((j.branch + 1) % (std::max<std::size_t>)(node->branch_count(), 1));
                            point.through.clear();
                            apply_plan(trains);
                        }
                    }
                }
            }
        }
        break;
    }
    case PickKind::signal:
        selected_signal_ = menu_.signal;
        ImGui::TextDisabled("Signal object %u", menu_.signal);
        if (ImGui::MenuItem("Signal info")) selected_signal_ = menu_.signal;
        if (signals && ImGui::MenuItem("Run all heads every step", nullptr, false)) signals->set_full_evaluation(true);
        break;
    case PickKind::start_point:
        if (ImGui::MenuItem("Change start direction")) {
            plan_.start.forward = !plan_.start.forward;
            apply_plan(trains);
        }
        break;
    case PickKind::plan_point:
        if (menu_.point < plan_.points.size()) {
            auto& point = plan_.points[menu_.point];
            if (point.kind == simulation::PlanPointKind::wait && ImGui::InputFloat("wait, s", &point.wait, 10.0f, 60.0f, "%.0f")) {
                apply_plan(trains);
            }
            if (ImGui::MenuItem("Remove point")) {
                plan_.points.erase(plan_.points.begin() + static_cast<std::ptrdiff_t>(menu_.point));
                apply_plan(trains);
            }
        }
        break;
    case PickKind::track:
        ImGui::TextDisabled("Track node %u, %.0f m", menu_.location.node, menu_.location.offset);
        if (editing_) {
            if (ImGui::MenuItem("Place start point here")) {
                plan_.start = {menu_.location.node, menu_.location.offset, plan_.start.forward};
                apply_plan(trains);
            }
            if (ImGui::MenuItem("Place a point to pass")) add_point(simulation::PlanPointKind::via);
            if (ImGui::BeginMenu("Place waiting point")) {
                ImGui::InputFloat("seconds", &wait_seconds_, 10.0f, 60.0f, "%.0f");
                if (ImGui::Button("Place")) {
                    add_point(simulation::PlanPointKind::wait);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Place reverse point")) add_point(simulation::PlanPointKind::reverse);
            if (ImGui::MenuItem("Place end point")) add_point(simulation::PlanPointKind::end);
        } else if (selected_ != 0 && ImGui::MenuItem("Path for the selected train to here")) {
            start_editing(trains, selected_);
            plan_.points.clear();
            add_point(simulation::PlanPointKind::end);
        }
        if (ImGui::BeginMenu("Put a consist here")) {
            ImGui::InputText("filter", consist_filter_, sizeof consist_filter_);
            const auto& consists = trains.consist_list();
            std::string filter = consist_filter_;
            for (auto& c : filter) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ImGui::BeginListBox("##consists", ImVec2(320.0f, 200.0f))) {
                int shown = 0;
                for (int i = 0; i < static_cast<int>(consists.size()) && shown < 300; ++i) {
                    const auto& path = consists[static_cast<std::size_t>(i)];
                    auto name = path.substr(path.find_last_of('/') + 1);
                    auto folded = name;
                    for (auto& c : folded) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (!filter.empty() && folded.find(filter) == std::string::npos) continue;
                    ++shown;
                    if (ImGui::Selectable(name.c_str(), i == consist_choice_)) consist_choice_ = i;
                }
                ImGui::EndListBox();
            }
            ImGui::Checkbox("facing against the node", &spawn_turned_);
            ImGui::Checkbox("with a test driver", &spawn_ai_);
            if (consist_choice_ >= 0 && consist_choice_ < static_cast<int>(consists.size()) && ImGui::Button("Put it here")) {
                const auto where = simulation::TrackLocation{menu_.location.node, menu_.location.offset, !spawn_turned_};
                const auto error = trains.spawn(consists[static_cast<std::size_t>(consist_choice_)], where,
                                                spawn_ai_ ? TrainView::Role::ai : TrainView::Role::standing);
                message_ = error.empty() ? "consist put on the track" : error;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndMenu();
        }
        break;
    case PickKind::none:
        ImGui::TextDisabled("Nothing here");
        break;
    }
    ImGui::EndPopup();
#else
    (void)trains;
    (void)signals;
#endif
}

void TrackControl::draw_panel(TrainView& trains, simulation::SignalSystem* signals) {
#ifdef OPENRTS_HAS_IMGUI
    ImGui::TextUnformatted("Trains");
    const auto summaries = trains.summaries();
    if (ImGui::BeginListBox("##trains", ImVec2(-1.0f, 150.0f))) {
        for (const auto& summary : summaries) {
            char label[200];
            std::snprintf(label, sizeof label, "%s  [%s]  %.0f km/h%s##%llu", summary.name.c_str(), role_name(summary.role),
                          std::abs(summary.speed) * 3.6, summary.derailed ? "  DERAILED" : "",
                          static_cast<unsigned long long>(summary.id));
            if (ImGui::Selectable(label, summary.id == selected_)) selected_ = summary.id;
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) trains.follow_train(summary.id);
        }
        ImGui::EndListBox();
    }
    const auto selected = std::find_if(summaries.begin(), summaries.end(), [&](const auto& s) { return s.id == selected_; });
    if (selected != summaries.end()) {
        ImGui::SeparatorText(selected->name.c_str());
        ImGui::Text("%s, %zu cars, %.0f m", role_name(selected->role), selected->cars, selected->length);
        if (selected->target >= 0.0f) ImGui::Text("Test driver aims at %.0f km/h", selected->target * 3.6f);
        ImGui::TextWrapped("%s", trains.path_status(selected->id).c_str());
        if (const auto* train = trains.train_by_id(selected->id); train && train->placed()) {
            const auto limit = trains.speed_limits().evaluate(*train, false);
            if (limit.current > 0.0f) {
                if (limit.ahead > 0.0f) {
                    ImGui::Text("Speed limit %.0f km/h, next %.0f km/h in %.0f m", limit.current * 3.6f, limit.ahead * 3.6f, limit.ahead_distance);
                } else {
                    ImGui::Text("Speed limit %.0f km/h", limit.current * 3.6f);
                }
            }
        }
        if (ImGui::Button("Jump to")) trains.follow_train(selected->id);
        ImGui::SameLine();
        if (editing_ && ImGui::Button("Stop editing")) {
            editing_ = false;
        } else if (!editing_ && ImGui::Button(trains.dispatcher().plan(selected->id) ? "Edit path" : "Create path")) {
            start_editing(trains, selected->id);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel path")) {
            trains.clear_path(selected->id);
            editing_ = false;
        }
        if (ImGui::Button("Drive")) message_ = trains.set_role(selected->id, TrainView::Role::driven);
        ImGui::SameLine();
        if (ImGui::Button("Test driver")) message_ = trains.set_role(selected->id, TrainView::Role::ai);
        ImGui::SameLine();
        if (ImGui::Button("Stand")) message_ = trains.set_role(selected->id, TrainView::Role::standing);
        if (editing_) {
            ImGui::TextWrapped("Editing: right-click the track for start, waiting, reverse and end points; a junction to switch it "
                               "on the path; a point to remove it.");
        }
    }
    if (selected_signal_ && signals && *selected_signal_ < signals->objects().size()) {
        const auto& object = signals->objects()[*selected_signal_];
        ImGui::SeparatorText("Signal");
        ImGui::Text("UiD %u, %zu head(s)", object.placement.uid, object.heads.size());
        for (const auto h : object.heads) {
            const auto& head = signals->heads()[h];
            ImGui::BulletText("%s: aspect %s, draw state %.0f, enabled %.0f, block %d, route %d",
                              head.type ? head.type->name.c_str() : "?", formats::to_string(head.aspect()), head.draw_state,
                              head.enabled, signals->block_state(h), signals->route_set(h));
        }
    }
    ImGui::SeparatorText("Show");
    ImGui::Checkbox("Signals", &show_signals_);
    ImGui::SameLine();
    ImGui::Checkbox("Platforms", &show_platforms_);
    ImGui::Checkbox("Junction numbers", &show_junction_ids_);
    ImGui::SameLine();
    ImGui::Checkbox("Every path", &show_all_paths_);
    if (!message_.empty()) ImGui::TextWrapped("%s", message_.c_str());
    ImGui::TextDisabled("Wheel: zoom. Right drag: move. Right click: menu.\nLeft click: select. Double click a train: follow it.");
    ImGui::TextDisabled("Trains: magenta driven, blue test driver, olive standing.\nJunctions: orange reserved, red occupied.");
#else
    (void)trains;
    (void)signals;
#endif
}

} // namespace openrts::viewer
#endif
