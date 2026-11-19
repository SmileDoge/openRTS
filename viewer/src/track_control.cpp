#ifdef OPENRTS_HAS_WGPU
#include "track_control.hpp"

#include "file_dialog.hpp"
#include "ui_fonts.hpp"
#include "ui_text.hpp"
#include "viewer_state.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include <openrts/simulation/track_traveller.hpp>

#ifdef OPENRTS_HAS_IMGUI
#include <imgui.h>
#endif

namespace openrts::viewer {
namespace {

constexpr double tile = 2048.0;

[[nodiscard]] const char* role_name(const TrainView::Role role) {
    switch (role) {
    case TrainView::Role::driven: return tr("driven");
    case TrainView::Role::ai: return tr("traffic (AI)");
    case TrainView::Role::standing: return tr("standing");
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
    : network_(&network), origin_(origin), map_(network, origin) {}

void TrackControl::show_train(TrainView& trains, const std::uint64_t train) {
    const auto* t = trains.train_by_id(train);
    const auto front = t && t->placed() ? t->front_end() : std::nullopt;
    if (!front) return;
    selected_ = train;
    map_.centre_on(front->pose(), 1.0);
    open_ = true;
}

double TrackControl::map_x(const double scene_x) const {
    return (scene_origin_.tile_x - origin_.tile_x) * tile + scene_origin_.x - origin_.x + scene_x;
}

double TrackControl::map_z(const double scene_z) const {
    return (scene_origin_.tile_z - origin_.tile_z) * tile + scene_origin_.z - origin_.z + scene_z;
}

void TrackControl::request_teleport(const double x, const double z, const simulation::TrackPoint* track) {
    Teleport request;
    request.at = origin_;
    request.at.x = origin_.x + x;
    request.at.z = origin_.z + z;
    if (track) {
        request.at.y = track->y;
        request.on_track = true;
        // Looking along the track (camera forward is -sin / -cos of yaw).
        request.yaw = track->heading + 3.14159265f;
    } else {
        // Keep looking the way the camera looks.
        request.yaw = std::atan2(-forward_.x, -forward_.z);
    }
    teleport_ = request;
}

double TrackControl::world_x(const simulation::TrackPoint& point) const {
    return map_.world_x(point);
}

double TrackControl::world_z(const simulation::TrackPoint& point) const {
    return map_.world_z(point);
}

TrackControl::Pick TrackControl::pick(const float mouse_x, const float mouse_y, TrainView& trains,
                                      const simulation::SignalSystem* signals) const {
    (void)trains;
    const ImVec2 mouse(mouse_x, mouse_y);
    // The consist being placed comes before anything under it.
    if (placement_.active && placement_car_at(mouse_x, mouse_y) >= 0) {
        const auto picked = map_.pick(mouse, signals);
        return Pick{.kind = PickKind::placement, .x = picked.x, .z = picked.z};
    }
    // Path points of the edited path within 7 px.
    if (editing_) {
        double best = 7.0;
        std::optional<Pick> found;
        const auto consider = [&](const simulation::TrackPoint& at, const Pick& candidate) {
            const auto p = map_.screen(at);
            const double d = std::hypot(p.x - mouse_x, p.y - mouse_y);
            if (d < best) {
                best = d;
                found = candidate;
            }
        };
        if (const auto start = network_->point(plan_.start.node, plan_.start.offset)) consider(*start, Pick{.kind = PickKind::start_point});
        for (std::size_t i = 0; i < plan_.points.size(); ++i) {
            if (const auto where = network_->point(plan_.points[i].node, plan_.points[i].offset)) {
                consider(*where, Pick{.kind = PickKind::plan_point, .point = i});
            }
        }
        if (found) return *found;
    }
    const auto picked = map_.pick(mouse, signals);
    Pick result{.train = picked.train, .junction = picked.junction, .signal = picked.signal, .location = picked.location, .x = picked.x, .z = picked.z};
    switch (picked.kind) {
    case trackmap::PickKind::train: result.kind = PickKind::train; break;
    case trackmap::PickKind::junction: result.kind = PickKind::junction; break;
    case trackmap::PickKind::signal: result.kind = PickKind::signal; break;
    case trackmap::PickKind::track: result.kind = PickKind::track; break;
    default: break;
    }
    return result;
}

std::optional<simulation::TrackLocation> TrackControl::nearest_track(const float mouse_x, const float mouse_y, const double reach) const {
    return map_.nearest_track(ImVec2(mouse_x, mouse_y), reach);
}

int TrackControl::placement_car_at(const float mouse_x, const float mouse_y) const {
    if (!placement_.active) return -1;
    const double reach = (std::max)(8.0, map_.scale() * 3.2 * 0.5 + 4.0);
    double best = reach;
    double t = 0.0;
    int found = -1;
    for (std::size_t i = 0; i < placement_.cars.size(); ++i) {
        const auto& [front, rear] = placement_.cars[i];
        const auto a = map_.screen(front), b = map_.screen(rear);
        const double d = segment_distance(mouse_x, mouse_y, a.x, a.y, b.x, b.y, t);
        if (d < best) {
            best = d;
            found = static_cast<int>(i);
        }
    }
    return found;
}

void TrackControl::start_placement(TrainView& trains, const std::string& consist, simulation::TrackLocation where) {
    TrainView::ConsistLayout layout;
    if (const auto error = trains.consist_layout(consist, layout); !error.empty()) {
        message_ = error;
        return;
    }
    placement_ = {};
    placement_.active = true;
    placement_.consist = consist;
    placement_.layout = std::move(layout);
    // The front where the menu was opened, the cars behind it: whichever way
    // they fit, along the node first.
    where.forward = true;
    placement_.head = where;
    lay_placement(trains);
    if (!placement_.fits) {
        placement_.head.forward = false;
        lay_placement(trains);
        if (!placement_.fits) {
            placement_.head.forward = true;
            lay_placement(trains);
        }
    }
    message_ = trf("placing %s: drag it along the track, click another track to move it there", placement_.layout.name.c_str());
}

void TrackControl::lay_placement(TrainView& trains) {
    auto& p = placement_;
    p.cars.clear();
    p.fits = true;
    p.overlaps.clear();
    for (std::size_t i = 0; i < p.layout.lengths.size(); ++i) {
        simulation::TrackTraveller front(*network_, p.head.node, p.head.offset, p.head.forward);
        const auto to_front = front.move(-p.layout.fronts[i]);
        simulation::TrackTraveller rear = front;
        const auto to_rear = rear.move(-p.layout.lengths[i]);
        if (!to_front.ok() || !to_rear.ok()) {
            p.fits = false;
            if (to_front.ok()) p.cars.emplace_back(front.pose(), rear.pose());
            break;
        }
        p.cars.emplace_back(front.pose(), rear.pose());
    }
    // Another train in the way: a car end or middle of the preview within
    // 1.6 m of one of its cars (tracks lie 4 m and more apart).
    double t = 0.0;
    for (const auto& summary : trains.summaries()) {
        const auto* train = trains.train_by_id(summary.id);
        if (!train || !train->placed()) continue;
        for (const auto& car : train->placements()) {
            // placements() spans the bogies (35 % of the length either side
            // of the centre): out to the car ends.
            const double fx0 = world_x(car.front), fz0 = world_z(car.front), rx0 = world_x(car.rear), rz0 = world_z(car.rear);
            constexpr double ends = (0.5 - 0.35) / 0.7;
            const double ax = fx0 + (fx0 - rx0) * ends, az = fz0 + (fz0 - rz0) * ends;
            const double bx = rx0 - (fx0 - rx0) * ends, bz = rz0 - (fz0 - rz0) * ends;
            for (const auto& [front, rear] : p.cars) {
                const double fx = world_x(front), fz = world_z(front), rx = world_x(rear), rz = world_z(rear);
                const double points[3][2] = {{fx, fz}, {(fx + rx) * 0.5, (fz + rz) * 0.5}, {rx, rz}};
                for (const auto& point : points) {
                    if (segment_distance(point[0], point[1], ax, az, bx, bz, t) < 1.6) {
                        p.overlaps = summary.name;
                        return;
                    }
                }
            }
        }
    }
}

void TrackControl::slide_placement(TrainView& trains, const double metres) {
    if (!placement_.active || metres == 0.0) return;
    const auto previous = placement_.head;
    const bool fitted = placement_.fits;
    simulation::TrackTraveller head(*network_, previous.node, previous.offset, previous.forward);
    head.move(metres);
    placement_.head = {head.node(), head.offset(), head.forward()};
    lay_placement(trains);
    if (fitted && !placement_.fits) {
        // The front ran off the track or the rear would: stay where it was.
        placement_.head = previous;
        lay_placement(trains);
    }
}

void TrackControl::move_placement(TrainView& trains, simulation::TrackLocation where) {
    if (!placement_.active) return;
    // Facing the way it faced on the screen as far as the new track allows.
    simulation::TrackTraveller old(*network_, placement_.head.node, placement_.head.offset, placement_.head.forward);
    const float facing = old.pose().heading;
    simulation::TrackTraveller along(*network_, where.node, where.offset, true);
    where.forward = std::cos(along.pose().heading - facing) >= 0.0f;
    placement_.head = where;
    lay_placement(trains);
}

void TrackControl::turn_placement(TrainView& trains) {
    if (!placement_.active) return;
    simulation::TrackTraveller tail(*network_, placement_.head.node, placement_.head.offset, placement_.head.forward);
    if (!tail.move(-placement_.layout.length).ok()) {
        message_ = tr("turning needs the whole consist on the track");
        return;
    }
    tail.reverse();
    placement_.head = {tail.node(), tail.offset(), tail.forward()};
    lay_placement(trains);
}

void TrackControl::finish_placement(TrainView& trains) {
    if (!placement_.active) return;
    if (!placement_.fits) {
        message_ = tr("the consist does not fit here: not enough track");
        return;
    }
    // A network game: the server places it (the console), nothing here.
    if (network_actions_.throw_junction) {
        if (network_actions_.place_consist) {
            network_actions_.place_consist(placement_.consist, placement_.head);
            message_ = trf("%s: asked the server to place it", placement_.layout.name.c_str());
        }
        placement_ = {};
        return;
    }
    const std::optional<simulation::TrainCategory> category =
        spawn_category_ == 1   ? std::optional{simulation::TrainCategory::passenger}
        : spawn_category_ == 2 ? std::optional{simulation::TrainCategory::freight}
        : spawn_category_ == 3 ? std::optional{simulation::TrainCategory::other}
                               : std::nullopt;
    const int state = spawn_state_ == 2 ? 0 : spawn_state_ == 1 ? 1 : -1;
    const auto error = trains.spawn(placement_.consist, placement_.head, spawn_ai_ ? TrainView::Role::ai : TrainView::Role::standing, category,
                                    state);
    if (!error.empty()) {
        message_ = error;
        return;
    }
    message_ = trf("%s put on the track", placement_.layout.name.c_str());
    placement_ = {};
}

void TrackControl::apply_plan(TrainView& trains) {
    if (selected_ == 0) return;
    const auto error = trains.assign_path(selected_, plan_);
    if (error.empty()) {
        const auto* route = trains.dispatcher().route(selected_);
        message_ = tr("path set: ") + std::to_string(route ? route->legs.size() : 0) + " leg(s)";
    } else {
        message_ = tr("path: ") + error;
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
    message_ = tr("editing: right-click the track to place waiting, reverse and end points");
}

// The consist's type and state, as the ТЧД console's train form gives them
// ("Редактирование поезда": its type and Холодный / Включен / Движется).
void TrackControl::spawn_options() {
    const char* categories[] = {tr("by its cars"), tr("passenger"), tr("freight"), tr("other")};
    ImGui::SetNextItemWidth(150.0f);
    ImGui::Combo(tr("Train type##spawn"), &spawn_category_, categories, 4);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", tr(spawn_category_ == 0 ? "(for the speed posts: freight without passenger cars, else passenger)"
                                                      : "(for the speed posts, as the dispatcher's train form)"));
    const char* states[] = {tr("running"), tr("on, standing"), tr("cold")};
    ImGui::SetNextItemWidth(150.0f);
    ImGui::Combo(tr("State##spawn"), &spawn_state_, states, 3);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", tr(spawn_state_ == 2   ? "(everything off: no air in the pipe, reservoirs and main reservoirs, pantographs down)"
                                 : spawn_state_ == 1 ? "(standing ready: its DLLs start on, the air charged)"
                                                     : "(ready to go, as before)"));
}

void TrackControl::draw(TrainView& trains, simulation::SignalSystem* signals) {
#ifdef OPENRTS_HAS_IMGUI
    if (!open_) return;
    if (!centred_) {
        centred_ = true;
        const auto* driven = trains.train();
        if (driven && driven->placed() && driven->front_end()) map_.centre_on(driven->front_end()->pose());
    }
    ImGui::SetNextWindowSize(ImVec2(1150.0f, 720.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(tr("Track control (Ctrl+Alt+F7)###track_control"), &open_)) {
        ImGui::End();
        return;
    }
    focused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImGui::BeginChild("panel", ImVec2(320.0f, 0.0f), true);
    draw_panel(trains, signals);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("map", ImVec2(0.0f, 0.0f), true, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    draw_map(trains, signals);
    ImGui::EndChild();
    ImGui::End();
    draw_signal_window(signals);
#else
    (void)trains;
    (void)signals;
#endif
}

void TrackControl::draw_map(TrainView& trains, const simulation::SignalSystem* signals) {
#ifdef OPENRTS_HAS_IMGUI
    auto& io = ImGui::GetIO();
    map_.trains.clear();
    map_.lines.clear();
    map_.marks.clear();
    map_.selected_train = selected_;
    map_.selected_signal = selected_signal_;
    // Electrification: each stretch in its supply type's colour (the
    // .electrotype's AARRGGBB), dead wire grey; track without wire stays black.
    auto& electro = trains.electrification();
    if (show_electrification_ && electro.mode() == simulation::Electrification::Mode::sections) {
        if (electro_built_.size() < network_->node_count()) {
            electro_built_.assign(network_->node_count(), false);
            electro_stretches_.assign(network_->node_count(), {});
        }
        for (std::uint32_t n = 1; n < network_->node_count(); ++n) {
            const auto* node = network_->node(n);
            if (!node || node->kind != formats::TrackNodeKind::vector) continue;
            if (!electro_built_[n]) {
                electro_stretches_[n] = electro.stretches(n);
                electro_built_[n] = true;
            }
            for (const auto& piece : electro_stretches_[n]) {
                if (piece.state == simulation::LineState::no_wire) continue;
                ImU32 colour = IM_COL32(150, 150, 150, 255);
                if (piece.state == simulation::LineState::live) {
                    if (const auto* type = electro.supply_type(static_cast<std::size_t>(piece.section))) {
                        const auto c = type->colour;
                        colour = IM_COL32((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff, 255);
                    }
                }
                map_.lines.push_back({.points = map_.along(n, piece.from, piece.to), .colour = colour, .width = 3.0f});
            }
        }
    }
    // Paths: the reserved way green, the rest of the leg blue, later legs pale.
    for (const auto& summary : trains.summaries()) {
        if (!show_all_paths_ && summary.id != selected_) continue;
        const auto* route = trains.dispatcher().route(summary.id);
        const auto* progress = trains.dispatcher().progress(summary.id);
        if (!route || !progress) continue;
        for (std::size_t l = progress->leg; l < route->legs.size(); ++l) {
            const auto& leg = route->legs[l];
            if (l == progress->leg) {
                map_.lines.push_back({.points = map_.along(leg, progress->head, progress->authority), .colour = IM_COL32(0, 170, 0, 230), .width = 6.0f});
                map_.lines.push_back({.points = map_.along(leg, progress->authority, leg.length()), .colour = IM_COL32(40, 90, 230, 200), .width = 4.0f});
            } else {
                map_.lines.push_back({.points = map_.along(leg, 0.0, leg.length()), .colour = IM_COL32(120, 150, 240, 160), .width = 3.0f});
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
                map_.marks.push_back({.at = *where, .colour = IM_COL32(255, 255, 255, 230), .letter = letter, .letter_colour = IM_COL32(20, 20, 160, 255)});
            }
        }
    }
    // The edited path's points (its start: overlay, with its direction).
    if (editing_) {
        for (const auto& point : plan_.points) {
            if (const auto where = network_->point(point.node, point.offset)) {
                map_.marks.push_back({.at = *where, .colour = IM_COL32(200, 0, 120, 255), .radius = 8.0f, .ring = true});
            }
        }
    }
    // The routes the server holds (0x4619): green from each train's head
    // (its rear, laid rear first) along the junctions as they stand.
    if (network_actions_.route_left) {
        for (const auto& summary : trains.summaries()) {
            const auto route = network_actions_.route_left(summary.id);
            const auto* train = trains.train_by_id(summary.id);
            if (!route || route->first <= 0.0f || !train || !train->placed()) continue;
            const auto end = route->second ? train->rear_end() : train->front_end();
            if (!end) continue;
            simulation::TrackTraveller walk(*network_, end->node(), end->offset(), route->second ? !end->forward() : end->forward());
            trackmap::MapLine line{.colour = IM_COL32(0, 190, 0, 210), .width = 5.0f};
            line.points.push_back(walk.pose());
            const double step = (std::max)(2.0, 6.0 / map_.scale());
            for (double done = 0.0; done < route->first;) {
                const double move = (std::min)(step, route->first - done);
                if (walk.move(move).moved <= 0.0) break;
                done += move;
                line.points.push_back(walk.pose());
            }
            map_.lines.push_back(std::move(line));
        }
    }
    // Trains: their cars, in the colour of their role.
    for (const auto& summary : trains.summaries()) {
        const auto* train = trains.train_by_id(summary.id);
        if (!train || !train->placed()) continue;
        trackmap::MapTrain t{.id = summary.id, .derailed = summary.derailed};
        t.colour = summary.role == TrainView::Role::driven ? IM_COL32(200, 30, 200, 255)
                 : summary.role == TrainView::Role::ai     ? IM_COL32(40, 60, 220, 255)
                                                           : IM_COL32(140, 120, 20, 255);
        for (const auto& car : train->placements()) t.cars.emplace_back(car.front, car.rear);
        char text[160];
        std::snprintf(text, sizeof text, tr("%s %.0f km/h"), summary.name.c_str(), std::abs(summary.speed) * 3.6);
        t.label = text;
        map_.trains.push_back(std::move(t));
    }
    // Over it all: the edited path's start, the consist being placed, the player.
    map_.overlay = [&](ImDrawList* draw) {
        const auto at = [&](const simulation::TrackPoint& p) { return map_.screen(p); };
        if (editing_) {
            if (const auto start = network_->point(plan_.start.node, plan_.start.offset)) {
                const auto p = at(*start);
                const float heading = start->heading + (plan_.start.forward ? 0.0f : 3.14159265f);
                const ImVec2 tip(p.x + std::sin(heading) * 12.0f, p.y - std::cos(heading) * 12.0f);
                draw->AddLine(p, tip, IM_COL32(200, 0, 120, 255), 3.0f);
                draw->AddCircleFilled(p, 6.0f, IM_COL32(200, 0, 120, 255));
                draw->AddText(ImVec2(p.x + 6.0f, p.y + 2.0f), IM_COL32(120, 0, 80, 255), "S");
            }
        }
        // The consist being placed: outlined cars, the front one orange with
        // an arrow the way the train faces; red outline where it does not fit
        // or runs into a train.
        if (placement_.active && !placement_.cars.empty()) {
            const float width = std::clamp(static_cast<float>(map_.scale() * 3.2), 5.0f, 14.0f);
            const bool trouble = !placement_.fits || !placement_.overlaps.empty();
            const ImU32 outline = trouble ? IM_COL32(220, 0, 0, 255) : IM_COL32(20, 20, 20, 255);
            for (std::size_t i = 0; i < placement_.cars.size(); ++i) {
                auto a = at(placement_.cars[i].first), b = at(placement_.cars[i].second);
                // A small gap between the cars when they are long enough on
                // the screen.
                const float dx = b.x - a.x, dy = b.y - a.y;
                const float span = std::sqrt(dx * dx + dy * dy);
                if (span > 8.0f) {
                    const float gap = 1.5f / span;
                    a = ImVec2(a.x + dx * gap, a.y + dy * gap);
                    b = ImVec2(b.x - dx * gap, b.y - dy * gap);
                }
                const ImU32 fill = i == 0 ? IM_COL32(255, 120, 0, 255)
                                 : placement_.layout.engines[i] ? IM_COL32(255, 200, 120, 235)
                                                               : IM_COL32(250, 250, 250, 235);
                draw->AddLine(a, b, outline, width + 3.0f);
                draw->AddLine(a, b, fill, width);
            }
            // The arrow ahead of the front car.
            const auto front = at(placement_.cars.front().first);
            auto rear = at(placement_.cars.front().second);
            if (std::hypot(front.x - rear.x, front.y - rear.y) < 3.0f) rear = at(placement_.cars.back().second);
            trackmap::draw_direction_arrow(draw, front, rear, IM_COL32(255, 120, 0, 255), outline, std::clamp(width * 2.2f, 18.0f, 30.0f));
            char text[200];
            std::snprintf(text, sizeof text, tr("%s: %zu cars, %.0f m%s"), placement_.layout.name.c_str(), placement_.layout.lengths.size(),
                          placement_.layout.length, !placement_.fits ? tr(" - does not fit") : !placement_.overlaps.empty() ? tr(" - runs into a train") : "");
            draw->AddText(ImVec2(front.x + 10.0f, front.y + 8.0f), trouble ? IM_COL32(200, 0, 0, 255) : IM_COL32(0, 0, 0, 255), text);
        }
        // The player (the camera): a figure with a cone the way it looks.
        const auto p = map_.screen(map_x(eye_.x), map_z(eye_.z));
        float fx = forward_.x, fy = -forward_.z; // screen: z up the map
        const float length = std::sqrt(fx * fx + fy * fy);
        if (length > 1e-3f) {
            fx /= length;
            fy /= length;
            constexpr float spread = 0.55f; // about 30 degrees either side
            const float c = std::cos(spread), sn = std::sin(spread);
            const ImVec2 a(p.x + (fx * c - fy * sn) * 46.0f, p.y + (fx * sn + fy * c) * 46.0f);
            const ImVec2 b(p.x + (fx * c + fy * sn) * 46.0f, p.y + (-fx * sn + fy * c) * 46.0f);
            draw->AddTriangleFilled(p, a, b, IM_COL32(230, 90, 20, 70));
            draw->AddLine(p, a, IM_COL32(230, 90, 20, 160), 1.5f);
            draw->AddLine(p, b, IM_COL32(230, 90, 20, 160), 1.5f);
        }
        // A little person: head and body, white edged.
        const ImU32 body = IM_COL32(230, 90, 20, 255), edge = IM_COL32(255, 255, 255, 255);
        const ImVec2 head(p.x, p.y - 9.0f);
        draw->AddRectFilled(ImVec2(p.x - 5.5f, p.y - 4.5f), ImVec2(p.x + 5.5f, p.y + 7.0f), edge, 4.0f);
        draw->AddRectFilled(ImVec2(p.x - 4.0f, p.y - 3.0f), ImVec2(p.x + 4.0f, p.y + 5.5f), body, 3.0f);
        draw->AddCircleFilled(head, 5.0f, edge);
        draw->AddCircleFilled(head, 3.6f, body);
    };
    const auto frame = map_.draw(signals, ImGui::IsPopupOpen("track_control_menu"));
    map_.overlay = nullptr;
    // A right click without a drag opens the menu for what is under it (the
    // map panned with a right drag); a left click selects.
    if (frame.right_click) {
        menu_ = pick(frame.mouse.x, frame.mouse.y, trains, signals);
        ImGui::OpenPopup("track_control_menu");
    }
    if (placement_.active) {
        // Placing a consist: drag a car along the track; a click on another
        // track moves the front there. Space turns it, Enter places it,
        // Escape drops it, Left / Right nudge it 1 m (Shift: 10 m).
        if (frame.left_click) {
            placement_.grabbed = placement_car_at(frame.mouse.x, frame.mouse.y);
            if (placement_.grabbed < 0) {
                if (const auto where = nearest_track(frame.mouse.x, frame.mouse.y, 14.0)) move_placement(trains, *where);
            }
        }
        if (placement_.grabbed >= 0) {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                placement_.grabbed = -1;
            } else if (!placement_.cars.empty() && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
                // Along the grabbed car on the screen (front to rear points
                // back, so the front is the positive way).
                const auto index = (std::min)(static_cast<std::size_t>(placement_.grabbed), placement_.cars.size() - 1);
                const auto& [front, rear] = placement_.cars[index];
                const double scale = map_.scale();
                const double dx = (world_x(front) - world_x(rear)) * scale;
                const double dy = -(world_z(front) - world_z(rear)) * scale;
                const double span = std::hypot(dx, dy);
                if (span > 1e-6) slide_placement(trains, (io.MouseDelta.x * dx + io.MouseDelta.y * dy) / span / scale);
            }
        }
        if (frame.hovered || focused_) {
            const double step = io.KeyShift ? 10.0 : 1.0;
            if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) turn_placement(trains);
            if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) finish_placement(trains);
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                placement_ = {};
                message_ = tr("placing dropped");
            }
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) slide_placement(trains, step);
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) slide_placement(trains, -step);
        }
    } else if (frame.left_click) {
        const auto picked = pick(frame.mouse.x, frame.mouse.y, trains, signals);
        if (picked.kind == PickKind::train) selected_ = picked.train;
        if (picked.kind == PickKind::signal) selected_signal_ = picked.signal;
    }
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
        ImGui::TextDisabled(tr("%s (%s, %zu cars)"), it->name.c_str(), role_name(it->role), it->cars);
        if (ImGui::MenuItem(tr("Jump to train"))) trains.follow_train(id);
        // A network game: the server's train. Who drives it, its route and
        // the rest are the console window's.
        if (network_actions_.throw_junction) {
            const bool routed = network_actions_.route_left && network_actions_.route_left(id).has_value();
            if (network_actions_.cancel_route && ImGui::MenuItem(tr("Cancel the route"), nullptr, false, routed)) {
                network_actions_.cancel_route(id);
                message_ = tr("route cancelled");
            }
            if (network_actions_.route_to && !routed) ImGui::TextDisabled("%s", tr("a route: pick the train, then right-click the track"));
            if (network_actions_.remove_train && ImGui::MenuItem(tr("Remove train"))) {
                network_actions_.remove_train(id);
                message_ = tr("asked the server to remove it");
            }
            ImGui::TextDisabled("%s", tr("a driver and a route: in the console's window"));
            break;
        }
        if (ImGui::MenuItem(tr("Drive it (keyboard)"), nullptr, it->role == TrainView::Role::driven)) message_ = trains.set_role(id, TrainView::Role::driven);
        if (ImGui::MenuItem(tr("Test driver follows its path"), nullptr, it->role == TrainView::Role::ai)) message_ = trains.set_role(id, TrainView::Role::ai);
        if (ImGui::MenuItem(tr("Standing"), nullptr, it->role == TrainView::Role::standing)) message_ = trains.set_role(id, TrainView::Role::standing);
        ImGui::Separator();
        if (editing_ && selected_ == id) {
            if (ImGui::MenuItem(tr("Stop editing the path"))) editing_ = false;
        } else if (ImGui::MenuItem(trains.dispatcher().plan(id) ? tr("Edit path") : tr("Create path"))) {
            start_editing(trains, id);
        }
        if (ImGui::MenuItem(tr("Cancel path"), nullptr, false, trains.dispatcher().plan(id) != nullptr)) {
            trains.clear_path(id);
            editing_ = false;
        }
        if (ImGui::BeginMenu(tr("Load path"))) {
            for (const auto& path : trains.route_paths()) {
                const auto label = path.file + (path.name.empty() ? "" : ": " + path.name);
                if (ImGui::MenuItem(label.c_str())) {
                    const auto error = trains.assign_route_path(id, path);
                    message_ = error.empty() ? trf("path %s set", path.file.c_str()) : error;
                    editing_ = false;
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(tr("Save path as"), trains.dispatcher().plan(id) != nullptr)) {
            ImGui::InputText(tr("name"), path_name_, sizeof path_name_);
            if (ImGui::Button(tr("Save"))) {
                const auto error = trains.save_path(id, path_name_);
                message_ = error.empty() ? trf("saved to %s", trains.user_path_folder().string().c_str()) : error;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndMenu();
        }
        if (it->cars > 1 && ImGui::BeginMenu(tr("Uncouple cars"))) {
            for (std::size_t k = 1; k < it->cars; ++k) {
                char label[48];
                std::snprintf(label, sizeof label, tr("before car %zu"), k + 1);
                if (ImGui::MenuItem(label)) {
                    const auto error = trains.uncouple_at(id, k);
                    message_ = error.empty() ? std::string(tr("uncoupled")) : error;
                }
            }
            ImGui::EndMenu();
        }
        // RTS train flags for the signals: the mode its chain gives them
        // (activities' SetSignalMode) and the head types it obeys.
        if (signals && ImGui::BeginMenu(tr("Signals for this train"))) {
            auto settings = signals->train_settings(id);
            bool changed = false;
            ImGui::TextDisabled(tr("Mode its signals get"));
            if (ImGui::MenuItem(tr("as the signals are"), nullptr, !settings.mode)) {
                settings.mode.reset();
                changed = true;
            }
            if (ImGui::MenuItem(tr("shunting (SHUNT)"), nullptr, settings.mode == simulation::SignalMode::shunt)) {
                settings.mode = simulation::SignalMode::shunt;
                changed = true;
            }
            if (ImGui::MenuItem(tr("train (NORMAL)"), nullptr, settings.mode == simulation::SignalMode::normal)) {
                settings.mode = simulation::SignalMode::normal;
                changed = true;
            }
            ImGui::Separator();
            changed |= ImGui::Checkbox(tr("obeys shunting signals"), &settings.obey_shunting);
            changed |= ImGui::Checkbox(tr("obeys distance signals"), &settings.obey_distance);
            changed |= ImGui::Checkbox(tr("obeys info signals"), &settings.obey_info);
            if (changed) signals->set_train_settings(id, settings);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(tr("Remove train"))) message_ = trains.remove(id);
        break;
    }
    case PickKind::junction: {
        const auto n = menu_.junction;
        const auto* node = network_->node(n);
        const auto* state = network_->junction(n);
        if (!node || !state) break;
        ImGui::TextDisabled(tr("Junction %u: branch %u of %zu%s"), n, state->selected, node->branch_count(),
                            state->occupied ? tr(", occupied") : "");
        for (const auto& [owner, branch] : state->reservations) {
            const auto* train = trains.train_by_id(owner);
            std::string name = trf("train %llu", static_cast<unsigned long long>(owner));
            for (const auto& summary : trains.summaries()) {
                if (summary.id == owner) name = summary.name;
            }
            ImGui::TextDisabled(tr("  reserved for %s, branch %u%s"), name.c_str(), branch, train ? "" : tr(" (gone)"));
        }
        if (ImGui::MenuItem(tr("Teleport here"))) {
            if (const auto at = network_->node_position(n)) request_teleport(world_x(*at), world_z(*at), &*at);
        }
        if (ImGui::MenuItem(tr("Throw"))) {
            const auto next = static_cast<std::uint32_t>((state->selected + 1) % (std::max<std::size_t>)(node->branch_count(), 1));
            if (network_actions_.throw_junction) {
                network_actions_.throw_junction(n, next);
                message_ = trf("junction %u: asked the server", n);
            } else {
                message_ = network_->throw_junction(n, next) ? trf("junction %u thrown", n)
                                                              : std::string(tr("refused: the junction is occupied or held by a path"));
            }
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
                        std::snprintf(label, sizeof label, tr("Switch junction on the path (pass %d)"), pass);
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
        ImGui::TextDisabled(tr("Signal object %u"), menu_.signal);
        if (ImGui::MenuItem(tr("Signal info"))) signal_window_ = menu_.signal;
        if (signals && ImGui::MenuItem(tr("Run all heads every step"), nullptr, false)) signals->set_full_evaluation(true);
        // A network game: the server's signals. The dispatcher's console
        // holds a head at an aspect there, or gives it back to its script.
        if (network_actions_.throw_junction) {
            if (network_actions_.set_signal && signals && menu_.signal < signals->objects().size()) {
                ImGui::Separator();
                for (const auto h : signals->objects()[menu_.signal].heads) {
                    const auto& head = signals->heads()[h];
                    char label[160];
                    std::snprintf(label, sizeof label, tr("Aspect of %s##server%u"), head.type ? head.type->name.c_str() : "?", h);
                    if (!ImGui::BeginMenu(label)) continue;
                    if (ImGui::MenuItem(tr("by the script"))) network_actions_.set_signal(head.track_item, std::nullopt);
                    if (head.type) {
                        for (std::size_t k = 0; k < head.type->aspects.size(); ++k) {
                            const auto& entry = head.type->aspects[k];
                            std::snprintf(label, sizeof label, tr("%s (%s)##serveraspect%zu"), formats::to_string(entry.aspect), entry.draw_state.c_str(), k);
                            if (ImGui::MenuItem(label)) network_actions_.set_signal(head.track_item, entry.aspect);
                        }
                    }
                    ImGui::EndMenu();
                }
            }
            break;
        }
        // RTS SignalOverride per head: a mode and / or an aspect in place of
        // what the script gives (an activity or the dispatcher sets these).
        if (signals && menu_.signal < signals->objects().size()) {
            ImGui::Separator();
            for (const auto h : signals->objects()[menu_.signal].heads) {
                const auto& head = signals->heads()[h];
                const auto* current = signals->override_of(h);
                char label[160];
                std::snprintf(label, sizeof label, tr("Override %s%s##%u"), head.type ? head.type->name.c_str() : "?",
                              current ? tr(" (on)") : "", h);
                if (!ImGui::BeginMenu(label)) continue;
                auto value = current ? *current : simulation::SignalOverride{};
                bool changed = false;
                ImGui::TextDisabled(tr("Mode"));
                if (ImGui::MenuItem(tr("by the script and the train"), nullptr, !value.mode)) {
                    value.mode.reset();
                    changed = true;
                }
                constexpr const char* mode_names[] = {"NORMAL", "SHUNT", "FAILED (dark)", "EXCL (left out)"};
                for (int m = 0; m < 4; ++m) {
                    if (ImGui::MenuItem(tr(mode_names[m]), nullptr, value.mode && static_cast<int>(*value.mode) == m)) {
                        value.mode = static_cast<simulation::SignalMode>(m);
                        changed = true;
                    }
                }
                ImGui::Separator();
                ImGui::TextDisabled(tr("Aspect"));
                if (ImGui::MenuItem(tr("by the script"), nullptr, !value.aspect)) {
                    value.aspect.reset();
                    value.draw_state.reset();
                    value.speed.reset();
                    changed = true;
                }
                if (head.type) {
                    for (std::size_t k = 0; k < head.type->aspects.size(); ++k) {
                        const auto& entry = head.type->aspects[k];
                        std::snprintf(label, sizeof label, tr("%s (%s)##aspect%zu"), formats::to_string(entry.aspect), entry.draw_state.c_str(), k);
                        if (ImGui::MenuItem(label, nullptr, value.aspect == entry.aspect)) {
                            value.aspect = entry.aspect;
                            value.draw_state = -2; // the type's draw state for it
                            value.speed = entry.speed;
                            changed = true;
                        }
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem(tr("Clear the override"), nullptr, false, current != nullptr)) {
                    signals->clear_override(h);
                    message_ = tr("override cleared");
                } else if (changed) {
                    if (!value.mode && !value.aspect) {
                        signals->clear_override(h);
                        message_ = tr("override cleared");
                    } else {
                        signals->set_override(h, value);
                        message_ = tr("override set");
                    }
                }
                ImGui::EndMenu();
            }
        }
        break;
    case PickKind::start_point:
        if (ImGui::MenuItem(tr("Change start direction"))) {
            plan_.start.forward = !plan_.start.forward;
            apply_plan(trains);
        }
        break;
    case PickKind::plan_point:
        if (menu_.point < plan_.points.size()) {
            auto& point = plan_.points[menu_.point];
            if (point.kind == simulation::PlanPointKind::wait && ImGui::InputFloat(tr("wait, s"), &point.wait, 10.0f, 60.0f, "%.0f")) {
                apply_plan(trains);
            }
            if (ImGui::MenuItem(tr("Remove point"))) {
                plan_.points.erase(plan_.points.begin() + static_cast<std::ptrdiff_t>(menu_.point));
                apply_plan(trains);
            }
        }
        break;
    case PickKind::track:
        ImGui::TextDisabled(tr("Track node %u, %.0f m"), menu_.location.node, menu_.location.offset);
        if (placement_.active && ImGui::MenuItem(tr("Move the consist being placed here"))) move_placement(trains, menu_.location);
        if (network_actions_.route_to && selected_ != 0 && ImGui::MenuItem(tr("Route for the selected train to here"))) {
            network_actions_.route_to(selected_, menu_.location);
            message_ = tr("route sent to the server");
        }
        if (ImGui::MenuItem(tr("Teleport here"))) {
            if (const auto at = network_->point(menu_.location.node, menu_.location.offset)) {
                request_teleport(world_x(*at), world_z(*at), &*at);
            }
        }
        if (editing_) {
            if (ImGui::MenuItem(tr("Place start point here"))) {
                plan_.start = {menu_.location.node, menu_.location.offset, plan_.start.forward};
                apply_plan(trains);
            }
            if (ImGui::MenuItem(tr("Place a point to pass"))) add_point(simulation::PlanPointKind::via);
            if (ImGui::BeginMenu(tr("Place waiting point"))) {
                ImGui::InputFloat(tr("seconds"), &wait_seconds_, 10.0f, 60.0f, "%.0f");
                if (ImGui::Button(tr("Place"))) {
                    add_point(simulation::PlanPointKind::wait);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem(tr("Place reverse point"))) add_point(simulation::PlanPointKind::reverse);
            if (ImGui::MenuItem(tr("Place end point"))) add_point(simulation::PlanPointKind::end);
        } else if (selected_ != 0 && ImGui::MenuItem(tr("Path for the selected train to here"))) {
            start_editing(trains, selected_);
            plan_.points.clear();
            add_point(simulation::PlanPointKind::end);
        }
        if ((!network_actions_.throw_junction || network_actions_.place_consist) && ImGui::BeginMenu(tr("Put a consist here"))) {
            // Choosing a consist shows it here first (a preview to drag and
            // turn), it is placed on Enter / "Put it here".
            ImGui::InputText(tr("filter"), consist_filter_, sizeof consist_filter_);
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
                    // The same file name in two folders (CONSISTS and
                    // CONSISTS_RTS, a copy): each its own item, the folder said.
                    const bool twin = (i > 0 && consists[static_cast<std::size_t>(i - 1)].ends_with("/" + name)) ||
                                      (i + 1 < static_cast<int>(consists.size()) && consists[static_cast<std::size_t>(i + 1)].ends_with("/" + name));
                    const auto label = twin ? name + "  (" + path.substr(0, path.find_last_of('/')) + ")" : name;
                    ImGui::PushID(i);
                    if (ImGui::Selectable(label.c_str(), i == consist_choice_)) {
                        consist_choice_ = i;
                        start_placement(trains, path, menu_.location);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::PopID();
                }
                ImGui::EndListBox();
            }
            ImGui::Checkbox(tr("as traffic (AI)"), &spawn_ai_);
            spawn_options();
            // A .con from anywhere: the system's dialog, starting in the
            // folder the last one came from.
            if (ImGui::Button(tr("Browse..."))) {
                auto folder = remembered_folder("consist_folder");
                std::error_code folder_error;
                if (folder.empty() || !std::filesystem::is_directory(folder, folder_error)) folder = trains.default_consist_folder();
                if (const auto file = open_file_dialog(folder, L"Consists (*.con)", L"*.con")) {
                    remember_folder("consist_folder", file->parent_path());
                    const auto text = file->u8string();
                    start_placement(trains, std::string(text.begin(), text.end()), menu_.location);
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled(tr("a .con from anywhere (the folder is remembered)"));
            ImGui::EndMenu();
        }
        break;
    case PickKind::placement:
        ImGui::TextDisabled(tr("%s, %zu cars"), placement_.layout.name.c_str(), placement_.layout.lengths.size());
        if (ImGui::MenuItem(tr("Put it here"), tr("Enter"), false, placement_.fits)) finish_placement(trains);
        if (ImGui::MenuItem(tr("Turn around"), tr("Space"))) turn_placement(trains);
        ImGui::MenuItem(tr("As traffic (AI)"), nullptr, &spawn_ai_);
        if (ImGui::MenuItem(tr("Drop it"), tr("Esc"))) {
            placement_ = {};
            message_ = tr("placing dropped");
        }
        break;
    case PickKind::none:
        ImGui::TextDisabled(tr("Nothing here"));
        if (ImGui::MenuItem(tr("Teleport here"))) request_teleport(menu_.x, menu_.z, nullptr);
        break;
    }
    ImGui::EndPopup();
#else
    (void)trains;
    (void)signals;
#endif
}

void TrackControl::draw_signal_window(simulation::SignalSystem* signals) {
#ifdef OPENRTS_HAS_IMGUI
    if (!signal_window_ || !signals || *signal_window_ >= signals->objects().size()) {
        signal_window_.reset();
        return;
    }
    const auto index = *signal_window_;
    const auto& object = signals->objects()[index];
    const auto name = map_.signal_name ? map_.signal_name(index) : std::string{};
    char title[160];
    std::snprintf(title, sizeof title, tr("Signal %s###signal_info"), name.empty() ? std::to_string(index).c_str() : name.c_str());
    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(560.0f, 420.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(title, &open)) {
        const auto& p = object.placement.position;
        ImGui::TextWrapped(tr("Object %u, UiD %u, shape %s"), index, object.placement.uid, object.placement.shape_file.string().c_str());
        ImGui::Text(tr("Tile %d %d, x %.1f, z %.1f"), p.tile_x, p.tile_z, p.x, p.z);
        if (ImGui::Button(tr("Teleport to the signal"))) {
            const simulation::TrackPoint at{.tile_x = p.tile_x, .tile_z = p.tile_z, .x = p.x, .y = p.y, .z = p.z};
            request_teleport(world_x(at), world_z(at), nullptr);
        }
        ImGui::SameLine();
        bool overridden = false;
        for (const auto h : object.heads) overridden = overridden || signals->override_of(h);
        ImGui::BeginDisabled(!overridden);
        if (ImGui::Button(tr("Clear the overrides"))) {
            for (const auto h : object.heads) signals->clear_override(h);
        }
        ImGui::EndDisabled();
        constexpr const char* functions[] = {"?", "NORMAL", "SHUNTING", "DISTANCE", "REPEATER", "INFO"};
        constexpr const char* mode_names[] = {"NORMAL", "SHUNT", "FAILED", "EXCL"};
        for (const auto h : object.heads) {
            const auto& head = signals->heads()[h];
            const int fn = head.type ? std::clamp(static_cast<int>(head.type->function), 0, 5) : 0;
            char header[160];
            std::snprintf(header, sizeof header, tr("Head %u: %s (%s)"), h, head.type ? head.type->name.c_str() : "?", functions[fn]);
            ImGui::SeparatorText(header);
            // The draw state's name from the type.
            const char* draw_name = tr("dark");
            if (head.type && head.draw_state >= 0.0f) {
                for (const auto& state : head.type->draw_states) {
                    if (state.index == static_cast<std::uint32_t>(head.draw_state)) draw_name = state.name.c_str();
                }
            }
            char limit[48];
            if (head.speed >= 0.0f) std::snprintf(limit, sizeof limit, tr("%.0f km/h"), head.speed * 3.6f);
            else std::snprintf(limit, sizeof limit, "%s", tr("no limit"));
            ImGui::TextWrapped(tr("Aspect %s, lights %s (%.0f), speed limit %s"), formats::to_string(head.aspect()), draw_name,
                               head.draw_state, limit);
            ImGui::TextWrapped(tr("Enabled %.0f, mode %s, block_state %d, route_set %d"), head.enabled,
                               mode_names[std::clamp(static_cast<int>(head.mode), 0, 3)], signals->block_state(h), signals->route_set(h));
            if (head.placed) {
                ImGui::TextWrapped(tr("Track item %u: node %u at %.1f m, protects the way %s"), head.track_item, head.node, head.offset,
                                   head.forward ? tr("along the node") : tr("against the node"));
            } else {
                ImGui::TextWrapped(tr("Track item %u: not on the network"), head.track_item);
            }
            for (const auto& [junction, branch] : head.links) {
                ImGui::BulletText(tr("linked junction %u, branch %u"), junction, branch);
            }
            if (const auto* forced = signals->override_of(h)) {
                ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.1f, 1.0f), tr("Override: mode %s, aspect %s"),
                                   forced->mode ? mode_names[static_cast<int>(*forced->mode)] : tr("by the script"),
                                   forced->aspect ? formats::to_string(*forced->aspect) : tr("by the script"));
            }
        }
    }
    ImGui::End();
    if (!open) signal_window_.reset();
#else
    (void)signals;
#endif
}

void TrackControl::draw_panel(TrainView& trains, simulation::SignalSystem* signals) {
#ifdef OPENRTS_HAS_IMGUI
    if (placement_.active) {
        ImGui::SeparatorText(tr("Placing a consist"));
        ImGui::TextWrapped("%s", placement_.layout.name.c_str());
        ImGui::Text(tr("%zu cars, %.0f m"), placement_.layout.lengths.size(), placement_.layout.length);
        if (!placement_.fits) {
            ImGui::TextColored(ImVec4(0.9f, 0.1f, 0.1f, 1.0f), tr("Does not fit: not enough track"));
        } else if (!placement_.overlaps.empty()) {
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.0f, 1.0f), tr("Runs into %s"), placement_.overlaps.c_str());
        }
        ImGui::BeginDisabled(!placement_.fits);
        if (ImGui::Button(tr("Put it here (Enter)"))) finish_placement(trains);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(tr("Turn around (Space)"))) turn_placement(trains);
        if (ImGui::Button(tr("<< 10 m"))) slide_placement(trains, -10.0);
        ImGui::SameLine();
        if (ImGui::Button(tr("< 1 m"))) slide_placement(trains, -1.0);
        ImGui::SameLine();
        if (ImGui::Button(tr("1 m >"))) slide_placement(trains, 1.0);
        ImGui::SameLine();
        if (ImGui::Button(tr("10 m >>"))) slide_placement(trains, 10.0);
        ImGui::Checkbox(tr("As traffic (AI)"), &spawn_ai_);
        ImGui::SameLine();
        if (ImGui::Button(tr("Drop (Esc)"))) {
            placement_ = {};
            message_ = tr("placing dropped");
        }
        spawn_options();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled(tr("Drag a car along the track; click another\ntrack to move it there. Arrows nudge it\n(1 m, Shift 10 m); > is the way it faces."));
        ImGui::PopTextWrapPos();
    }
    ImGui::TextUnformatted(tr("Trains"));
    const auto summaries = trains.summaries();
    if (ImGui::BeginListBox("##trains", ImVec2(-1.0f, 150.0f))) {
        for (const auto& summary : summaries) {
            char label[200];
            std::snprintf(label, sizeof label, tr("%s  [%s]  %.0f km/h%s##%llu"), summary.name.c_str(),
                          summary.asleep ? "STATIC" : role_name(summary.role), std::abs(summary.speed) * 3.6,
                          summary.derailed ? tr("  DERAILED") : "", static_cast<unsigned long long>(summary.id));
            if (ImGui::Selectable(label, summary.id == selected_)) selected_ = summary.id;
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) trains.follow_train(summary.id);
        }
        ImGui::EndListBox();
    }
    const auto selected = std::find_if(summaries.begin(), summaries.end(), [&](const auto& s) { return s.id == selected_; });
    if (selected != summaries.end()) {
        ImGui::SeparatorText(selected->name.c_str());
        ImGui::Text(tr("%s%s, %zu cars, %.0f m"), role_name(selected->role), selected->asleep ? tr(" (STATIC: not simulated until moved)") : "",
                    selected->cars, selected->length);
        if (selected->target >= 0.0f) ImGui::Text(tr("Test driver aims at %.0f km/h"), selected->target * 3.6f);
        if (signals) {
            const auto settings = signals->train_settings(selected->id);
            if (settings.mode || settings.obey_shunting || settings.obey_distance || settings.obey_info) {
                ImGui::Text(tr("Signals: %s%s%s%s"), !settings.mode ? tr("as they are") : settings.mode == simulation::SignalMode::shunt ? "SHUNT" : "NORMAL",
                            settings.obey_shunting ? tr(", obeys shunting") : "", settings.obey_distance ? tr(", obeys distance") : "",
                            settings.obey_info ? tr(", obeys info") : "");
            }
        }
        ImGui::TextWrapped("%s", tr(trains.path_status(selected->id).c_str()));
        if (const auto* train = trains.train_by_id(selected->id); train && train->placed()) {
            const auto limit = trains.speed_limits().evaluate(*train, simulation::TrainCategory::passenger);
            if (limit.current > 0.0f) {
                if (limit.ahead > 0.0f) {
                    ImGui::Text(tr("Speed limit %.0f km/h, next %.0f km/h in %.0f m"), limit.current * 3.6f, limit.ahead * 3.6f, limit.ahead_distance);
                } else {
                    ImGui::Text(tr("Speed limit %.0f km/h"), limit.current * 3.6f);
                }
            }
        }
        if (ImGui::Button(tr("Jump to"))) trains.follow_train(selected->id);
        ImGui::SameLine();
        if (editing_ && ImGui::Button(tr("Stop editing"))) {
            editing_ = false;
        } else if (!editing_ && ImGui::Button(trains.dispatcher().plan(selected->id) ? tr("Edit path") : tr("Create path"))) {
            start_editing(trains, selected->id);
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel path"))) {
            trains.clear_path(selected->id);
            editing_ = false;
        }
        if (ImGui::Button(tr("Drive"))) message_ = trains.set_role(selected->id, TrainView::Role::driven);
        ImGui::SameLine();
        if (ImGui::Button(tr("Test driver"))) message_ = trains.set_role(selected->id, TrainView::Role::ai);
        ImGui::SameLine();
        if (ImGui::Button(tr("Stand"))) message_ = trains.set_role(selected->id, TrainView::Role::standing);
        if (editing_) {
            ImGui::TextWrapped(tr("Editing: right-click the track for start, waiting, reverse and end points; a junction to switch it "
                               "on the path; a point to remove it."));
        }
    }
    if (selected_signal_ && signals && *selected_signal_ < signals->objects().size()) {
        const auto& object = signals->objects()[*selected_signal_];
        ImGui::SeparatorText(tr("Signal"));
        const auto name = map_.signal_name ? map_.signal_name(*selected_signal_) : std::string{};
        ImGui::TextWrapped(tr("%s%sUiD %u, %zu head(s)"), name.c_str(), name.empty() ? "" : ", ", object.placement.uid, object.heads.size());
        for (const auto h : object.heads) {
            const auto& head = signals->heads()[h];
            constexpr const char* mode_names[] = {"NORMAL", "SHUNT", "FAILED", "EXCL"};
            const int mode = std::clamp(static_cast<int>(head.mode), 0, 3);
            // Wrapped: the panel is narrow and the lines are long.
            ImGui::Bullet();
            ImGui::TextWrapped(tr("%s: aspect %s, draw state %.0f, enabled %.0f, block %d, route %d, mode %s%s"),
                               head.type ? head.type->name.c_str() : "?", formats::to_string(head.aspect()), head.draw_state,
                               head.enabled, signals->block_state(h), signals->route_set(h), mode_names[mode],
                               signals->override_of(h) ? tr(", OVERRIDE") : "");
        }
        if (ImGui::Button(tr("More..."))) signal_window_ = *selected_signal_;
    }
    ImGui::SeparatorText(tr("Show"));
    // Two columns: the labels are long in Russian.
    if (ImGui::BeginTable("##show", 2)) {
        const auto box = [](const char* label, bool& value) {
            ImGui::TableNextColumn();
            ImGui::Checkbox(label, &value);
        };
        box(tr("Signals"), map_.layers.signals);
        box(tr("Platforms"), map_.layers.platforms);
        box(tr("Sidings"), map_.layers.sidings);
        box(tr("Stations"), map_.layers.station_names);
        box(tr("Signal names"), map_.layers.signal_names);
        box(tr("Junction numbers"), map_.layers.junction_ids);
        box(tr("Every path"), show_all_paths_);
        box(tr("Electrification"), show_electrification_);
        ImGui::EndTable();
    }
    if (show_electrification_) draw_electrification_panel(trains);
    // Messages from the trains come in English; known ones are translated.
    if (!message_.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", tr(message_.c_str()));
    }
    ImGui::Separator();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled(tr("Wheel: zoom. Right drag: move. Right click: menu.\nLeft click: select. Double click a train: follow it."));
    ImGui::TextDisabled(tr("Trains: magenta driven, blue traffic (AI), olive standing.\nJunctions: orange reserved, red occupied."));
    ImGui::PopTextWrapPos();
#else
    (void)trains;
    (void)signals;
#endif
}

void TrackControl::draw_electrification_panel(TrainView& trains) {
#ifdef OPENRTS_HAS_IMGUI
    auto& electro = trains.electrification();
    const auto& data = electro.data();
    using Mode = simulation::Electrification::Mode;
    ImGui::SeparatorText(tr("Electrification"));
    ImGui::PushTextWrapPos(0.0f);
    switch (electro.mode()) {
    case Mode::none:
        ImGui::TextUnformatted(tr("The route is not electrified: electric engines have no voltage."));
        break;
    case Mode::by_engine:
        ImGui::TextUnformatted(tr("No .electrotype: every electric engine gets the middle of its first WorksOnLineVoltages, as RTS."));
        break;
    case Mode::single_type: {
        const auto& t = data.types.supply_types.front();
        ImGui::Text(tr("No .electrodb: %s everywhere (%.0f V, %.0f Hz)."), t.name.c_str(), t.voltage, t.frequency);
        break;
    }
    case Mode::sections:
        break;
    }
    if (electro.mode() != Mode::sections) {
        ImGui::PopTextWrapPos();
        return;
    }
    // Legend: the supply types with their colours.
    for (const auto& t : data.types.supply_types) {
        const auto c = t.colour;
        ImGui::ColorButton(("##supply" + std::to_string(t.id)).c_str(),
                           ImVec4(((c >> 16) & 0xff) / 255.0f, ((c >> 8) & 0xff) / 255.0f, (c & 0xff) / 255.0f, 1.0f),
                           ImGuiColorEditFlags_NoTooltip, ImVec2(14, 14));
        ImGui::SameLine();
        if (t.frequency > 0.0f) {
            ImGui::Text(tr("%s: %.1f kV AC %.0f Hz, drop %.0f V/km"), t.name.c_str(), t.voltage / 1000.0f, t.frequency,
                        t.voltage_drop);
        } else {
            ImGui::Text(tr("%s: %.2f kV DC, drop %.0f V/km"), t.name.c_str(), t.voltage / 1000.0f, t.voltage_drop);
        }
    }
    ImGui::TextDisabled(tr("Grey: wire without voltage (a dead or neutral stretch)."));
    ImGui::Checkbox(tr("A section with several sources feeds what the engine works on"), &electro.automatic_sources);
    ImGui::SameLine();
    ImGui::TextDisabled(tr("(RTS in an activity)"));
    // Sections fed from more than one source: the dispatcher's switch.
    const auto& sections = data.database.sections;
    for (std::size_t i = 0; i < sections.size(); ++i) {
        const auto& section = sections[i];
        if (section.sources.size() < 2) continue;
        const auto describe = [&](const int k) -> std::string {
            if (k < 0) return tr("by the sources' Active flags");
            const auto& source = section.sources[static_cast<std::size_t>(k)];
            const auto* sub = electro.substation(source.substation);
            std::string text = sub ? sub->name : "#" + std::to_string(source.substation);
            if (sub) {
                char buffer[64];
                std::snprintf(buffer, sizeof buffer, " (%.1f kV)", sub->voltage / 1000.0f);
                text += buffer;
            }
            return text;
        };
        const int current = electro.current_source(i);
        ImGui::TextUnformatted(section.name.c_str());
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo(("##source" + std::to_string(i)).c_str(), describe(current).c_str())) {
            for (int k = -1; k < static_cast<int>(section.sources.size()); ++k) {
                if (ImGui::Selectable(describe(k).c_str(), k == current)) electro.set_current_source(i, k);
            }
            ImGui::EndCombo();
        }
    }
    // The substations' load (RTS 0x5B6030: the power the trains drew
    // through each phase over the last 2 s), against the substation's Power.
    const auto loads = electro.substation_loads();
    if (!loads.empty()) {
        ImGui::SeparatorText(tr("Substation load"));
        for (const auto& load : loads) {
            const auto* sub = electro.substation(load.substation);
            const std::string name = sub ? sub->name : "#" + std::to_string(load.substation);
            if (sub && sub->power > 0.0f) {
                // Power is written in MW (the server reads it x 1000 as kW).
                ImGui::Text(tr("%s, phase %u: %.2f MW of %.1f MW"), name.c_str(), load.phase, load.power / 1e6f, sub->power);
                ImGui::ProgressBar(std::clamp(load.power / (sub->power * 1e6f), 0.0f, 1.0f), ImVec2(-1.0f, 0.0f));
            } else {
                ImGui::Text(tr("%s, phase %u: %.2f MW"), name.c_str(), load.phase, load.power / 1e6f);
            }
        }
    }
    ImGui::PopTextWrapPos();
#else
    (void)trains;
#endif
}

} // namespace openrts::viewer
#endif
