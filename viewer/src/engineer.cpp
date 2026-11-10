#include "engineer.hpp"
#include "rts_config.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>

#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>

#include "key_layout.hpp"
#include "ui_text.hpp"

namespace openrts::viewer {

namespace {

constexpr float pi = 3.14159265358979f;
// Inside the train a long step (RTS paces reach 120 m/s with Shift) goes in
// pieces, each tried against the cab's surfaces.
constexpr float inside_piece = 0.2f; // m
constexpr float radius = 0.22f;     // how close he comes to a wall
constexpr float eye_share = 0.85f;  // of engineer.Height, above his feet
constexpr float blend_time = 0.7f;  // sitting down, standing up

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool is_exterior(const formats::CabDoor& door) {
    return lower(door.from) == "exterior" || lower(door.to) == "exterior";
}

// A door to the car coupled at that end (WAGON_FRONT / WAGON_BACK).
bool is_passage(const formats::CabDoor& door) {
    return lower(door.from).rfind("wagon", 0) == 0 || lower(door.to).rfind("wagon", 0) == 0;
}

bool has_plane(const formats::CabDoor& door) {
    return door.plane.a != std::array<float, 3>{} || door.plane.b != std::array<float, 3>{};
}

// Where a door is: the middle of its InteriorPlane (the doorway's floor)
// when it has one, else its Position. The ED9M salon's (pricep9.sd)
// passenger doors have their Positions by the car's middle and their
// planes at the doors; RTS opens doors by clicking their nodes.
glm::vec3 door_point(const formats::CabDoor& door) {
    if (has_plane(door)) {
        return {(door.plane.a[0] + door.plane.b[0]) * 0.5f, (door.plane.a[1] + door.plane.b[1]) * 0.5f,
                (door.plane.a[2] + door.plane.b[2]) * 0.5f};
    }
    return {door.position[0], door.position[1], door.position[2]};
}

bool has_point(const formats::CabDoor& door) { return door.has_position || has_plane(door); }

// A way in from outside: an outer door the player opens himself, or one
// whose doorway's floor is given (InteriorPlane). A passenger door of the
// train without it is not: its Position may be anywhere in the car (the
// ED9M's lie by the car's middle, their planes at the doors).
bool get_on_door(const formats::CabDoor& door) {
    return is_exterior(door) && has_point(door) && (door.can_be_opened || has_plane(door));
}

// Out of the car through a door: across the side for a side door, along the
// car for an end door.
glm::vec3 outward(const formats::CabDoor& door) {
    const auto at = door_point(door);
    if (std::abs(at.x) > 0.3f) return {at.x > 0.0f ? 1.0f : -1.0f, 0.0f, 0.0f};
    return {0.0f, 0.0f, at.z >= 0.0f ? 1.0f : -1.0f};
}

float distance_xz(const glm::vec3& a, const glm::vec3& b) {
    return std::hypot(a.x - b.x, a.z - b.z);
}

// The open outer door whose face a step out reaches: from `feet` by `going`
// (cab coordinates) his front comes to the door's plane, inside its width.
std::optional<std::size_t> doorway_reached(const CabView& cab, const glm::vec3& feet, const glm::vec3& going) {
    const glm::vec2 g{going.x, going.z};
    if (glm::length(g) < 1e-6f) return std::nullopt;
    const auto& doors = cab.description().doors;
    for (std::size_t i = 0; i < doors.size(); ++i) {
        const auto& d = doors[i];
        if (!is_exterior(d) || !has_point(d) || !cab.door_open(i)) continue;
        const glm::vec3 out3 = outward(d);
        const glm::vec2 out{out3.x, out3.z};
        if (glm::dot(glm::normalize(g), out) <= 0.2f) continue; // not walking out
        const glm::vec3 mid3 = door_point(d);
        const glm::vec2 from_mid = glm::vec2{feet.x + going.x, feet.z + going.z} - glm::vec2{mid3.x, mid3.z};
        const float ahead = glm::dot(from_mid, out); // < 0: inside the face
        if (ahead < -(radius + 0.05f) || ahead > 1.0f) continue;
        const glm::vec2 across{-out.y, out.x};
        float half = 0.45f; // a door with only a point: about a doorway
        if (has_plane(d)) half = std::abs(glm::dot(glm::vec2{d.plane.a[0] - d.plane.b[0], d.plane.a[2] - d.plane.b[2]}, across)) * 0.5f;
        if (std::abs(glm::dot(from_mid, across)) > half + 0.1f) continue;
        return i;
    }
    return std::nullopt;
}

glm::vec3 vec(const std::array<float, 3>& a) {
    return {a[0], a[1], a[2]};
}

} // namespace

std::vector<std::pair<std::size_t, glm::vec3>> Engineer::get_on_points(const CabView& cab) {
    std::vector<std::pair<std::size_t, glm::vec3>> out;
    const auto& doors = cab.description().doors;
    for (std::size_t i = 0; i < doors.size(); ++i) {
        const auto& door = doors[i];
        if (!get_on_door(door)) continue;
        const auto at = door_point(door);
        out.emplace_back(i, glm::vec3{at.x, 0.0f, at.z} + outward(door) * 0.9f);
    }
    return out;
}

// The keys are RTS's at every door (0x4121BC): Enter works the door,
// Ctrl+Enter goes in through it.
std::string Engineer::get_on_prompt(const CabView& cab, const std::size_t door) {
    if (door >= cab.description().doors.size()) return {};
    if (!cab.description().doors[door].can_be_opened) {
        return cab.door_open(door) ? tr("Ctrl+Enter: climb in") : tr("The passenger doors are opened from the driver's cab.");
    }
    if (cab.doorway(door)) return tr("Ctrl+Enter: climb in");
    return cab.door_open(door) ? tr("Enter: close the door, Ctrl+Enter: climb in")
                               : tr("Enter: open the door, Ctrl+Enter: climb in");
}

void Engineer::configure(const std::string_view config) {
    const auto value = [&](const std::string& key) { return rts_config_value(config, key); };
    if (const auto h = value("engineer.height")) height_ = std::clamp(*h, 1.2f, 2.3f);
    if (const auto h = value("engineer.height1")) seated_eye_ = std::clamp(*h, 0.4f, 1.4f);
    // RTS read_config: engineer.velocity / engineer.velocity1, m/s.
    const auto walk = value("engineer.velocity");
    const auto run = value("engineer.velocity1");
    set_speeds(walk ? *walk : walk_speed_, run ? *run : run_speed_);
}

void Engineer::set_speeds(const float walk, const float run) noexcept {
    walk_speed_ = std::clamp(walk, 0.1f, 300.0f);
    run_speed_ = std::clamp(run, 0.1f, 300.0f);
}

void Engineer::lean(const glm::vec3& along, const float notches) {
    if (mode_ != Mode::seated || head_out_ || notches == 0.0f) return;
    constexpr float step = 0.08f, most = 0.6f;
    if (notches > 0.0f) {
        if (glm::length(along) < 1e-4f) return;
        lean_goal_ += glm::normalize(along) * step * notches;
        if (const float length = glm::length(lean_goal_); length > most) lean_goal_ *= most / length;
    } else if (const float length = glm::length(lean_goal_); length > 1e-4f) {
        lean_goal_ *= (std::max)(length + step * notches, 0.0f) / length;
    }
}

void Engineer::sit(const std::size_t chair) {
    mode_ = Mode::seated;
    chair_ = chair;
    up_ = false;
    lean_ = lean_goal_ = glm::vec3{0.0f};
    blend_ = 1.0f;
    camera_set_ = false;
    head_out_ = false;
}

std::optional<std::size_t> Engineer::chair_window(const CabView& cab) const {
    const auto& chairs = cab.description().chairs;
    if (chair_ >= chairs.size() || chairs[chair_].head_out_window.empty()) return std::nullopt;
    const auto window = cab.find_window(chairs[chair_].head_out_window);
    if (!window || !cab.description().windows[*window].can_open) return std::nullopt;
    return window;
}

void Engineer::stand_by(const CabView& cab, const std::size_t chair) {
    const auto& chairs = cab.description().chairs;
    if (chair >= chairs.size()) {
        sit(chair);
        return;
    }
    glm::vec3 spot = vec(chairs[chair].window);
    if (const auto* surfaces = cab.collision()) {
        auto floor = surfaces->floor(spot.x, spot.z, spot.y - 1.0f, spot.y + 0.5f, cab.skip());
        if (!floor) floor = surfaces->floor(spot.x, spot.z, spot.y - 1.0f, spot.y + 1.0f, cab.skip());
        if (floor) spot.y = *floor;
    }
    chair_ = chair;
    feet_ = spot;
    mode_ = Mode::standing;
    blend_ = 1.0f;
    camera_set_ = false;
    head_out_ = false;
}

void Engineer::stand_outside(const glm::vec3& feet) {
    feet_ = feet;
    mode_ = Mode::outside;
    blend_ = 1.0f;
    head_out_ = false;
    camera_set_ = true;
}

void Engineer::off() noexcept {
    mode_ = Mode::off;
    camera_set_ = false;
    prompt_.clear();
}

glm::vec3 Engineer::local_eye(const CabView& cab) const {
    if (mode_ == Mode::seated && head_out_) {
        if (const auto window = chair_window(cab)) return vec(cab.description().windows[*window].head_out);
    }
    if (mode_ == Mode::seated && up_) return up_feet_ + glm::vec3{0.0f, eye_share * height_, 0.0f} + lean_;
    if (mode_ == Mode::seated) {
        const auto& chairs = cab.description().chairs;
        const glm::vec3 seat = chair_ < chairs.size() ? vec(chairs[chair_].seated) : cab.eye();
        return seat + glm::vec3{0.0f, seated_eye_, 0.0f} + lean_;
    }
    return feet_ + glm::vec3{0.0f, eye_share * height_, 0.0f};
}

bool Engineer::passable(const CabView& cab, const glm::vec3& from, glm::vec3& to) const {
    const auto* surfaces = cab.collision();
    if (!surfaces || surfaces->empty()) return false;
    const auto& skip = cab.skip();
    glm::vec3 direction = to - from;
    direction.y = 0.0f;
    const float length = glm::length(direction);
    if (length < 1e-5f) return true;
    const glm::vec3 ahead = direction / length;
    // Knees (above a step), waist, head.
    for (const float h : {0.45f, 0.95f, 1.45f}) {
        const glm::vec3 lift{0.0f, h, 0.0f};
        if (surfaces->crosses(from + lift, to + ahead * radius + lift, skip)) return false;
    }
    const auto floor = surfaces->floor(to.x, to.z, from.y - 0.6f, from.y + 0.4f, skip);
    if (!floor) return false;
    to.y = *floor;
    return true;
}

void Engineer::walk_inside(const CabView& cab, const glm::vec3& step) {
    blocked_ = false;
    glm::vec3 to = feet_ + step;
    if (passable(cab, feet_, to)) {
        feet_ = to;
        return;
    }
    // Along the wall.
    for (const glm::vec3 part : {glm::vec3{step.x, 0.0f, 0.0f}, glm::vec3{0.0f, 0.0f, step.z}}) {
        if (glm::length(part) < 1e-5f) continue;
        to = feet_ + part;
        if (passable(cab, feet_, to)) {
            feet_ = to;
            return;
        }
    }
    blocked_ = true;
}

std::optional<std::size_t> Engineer::open_door_near(const CabView& cab, const bool exterior, const float reach) const {
    const auto& doors = cab.description().doors;
    std::optional<std::size_t> best;
    float nearest = reach; // 1.4: the doorway reaches past the door to the car end
    for (std::size_t i = 0; i < doors.size(); ++i) {
        if (!has_point(doors[i]) || !cab.door_open(i)) continue;
        if (exterior ? !is_exterior(doors[i]) : !is_passage(doors[i])) continue;
        const float d = distance_xz(feet_, door_point(doors[i]));
        if (d <= nearest) {
            nearest = d;
            best = i;
        }
    }
    return best;
}

void Engineer::get_off(const CabView& cab, const std::size_t door, const glm::mat4& car, const Outdoors& outdoors) {
    const auto& d = cab.description().doors[door];
    const glm::vec3 out = outward(d);
    // Down from the doorway's floor: onto a high platform when there is one.
    const float sill = d.plane.a[1] != 0.0f || d.plane.b[1] != 0.0f ? (d.plane.a[1] + d.plane.b[1]) * 0.5f : 1.5f;
    const glm::vec3 local = glm::vec3{door_point(d).x, sill, door_point(d).z} + out * 0.9f;
    glm::vec3 scene = glm::vec3(car * glm::vec4(local, 1.0f));
    const auto floor = outdoors.floor ? outdoors.floor(scene) : std::nullopt;
    scene.y = floor ? *floor : glm::vec3(car * glm::vec4(local.x, 0.0f, local.z, 1.0f)).y;
    feet_ = scene;
    mode_ = Mode::outside;
}

void Engineer::walk_outside(const glm::vec3& step, const Outdoors& outdoors) {
    auto blocked = [&](const glm::vec3& to) { return outdoors.blocked && outdoors.blocked(feet_, to); };
    glm::vec3 to = feet_ + step;
    bool moved = !blocked(to);
    if (!moved) {
        // Along the wall.
        for (const glm::vec3 part : {glm::vec3{step.x, 0.0f, 0.0f}, glm::vec3{0.0f, 0.0f, step.z}}) {
            if (glm::length(part) < 1e-5f) continue;
            to = feet_ + part;
            if (!blocked(to)) {
                moved = true;
                break;
            }
        }
    }
    if (!moved) {
        // Boxed in on every side (he stands inside something): let him out.
        int ways = 0;
        for (const glm::vec3 d : {glm::vec3{0.3f, 0, 0}, glm::vec3{-0.3f, 0, 0}, glm::vec3{0, 0, 0.3f}, glm::vec3{0, 0, -0.3f}})
            ways += blocked(feet_ + d) ? 0 : 1;
        if (ways == 0) {
            to = feet_ + step;
            moved = true;
        }
    }
    blocked_ = !moved;
    if (!moved) return;
    feet_.x = to.x;
    feet_.z = to.z;
    if (outdoors.floor) {
        if (const auto floor = outdoors.floor(feet_)) feet_.y = *floor;
    }
}

void Engineer::shift(const glm::vec3& delta) noexcept {
    if (mode_ == Mode::outside) feet_ += delta;
}

void Engineer::arrive(CabView& cab, const std::size_t door) {
    get_on(cab, door);
    rebase_ = true;
    camera_set_ = true;
}

void Engineer::get_on(CabView& cab, const std::size_t door) {
    const auto& d = cab.description().doors[door];
    // A passenger door (CanBeOpened 0) is opened by the train, not by him.
    if (d.can_be_opened) cab.open_door(door);
    glm::vec3 inner = door_point(d) - outward(d) * 0.45f;
    const float plane = d.plane.a[1] != 0.0f || d.plane.b[1] != 0.0f ? (d.plane.a[1] + d.plane.b[1]) * 0.5f : d.position[1] - 1.0f;
    inner.y = plane;
    if (const auto* surfaces = cab.collision()) {
        if (const auto floor = surfaces->floor(inner.x, inner.z, plane - 0.6f, plane + 0.8f, cab.skip())) inner.y = *floor;
    }
    feet_ = inner;
    mode_ = Mode::standing;
    blend_ = 1.0f;
    head_out_ = false;
}

Engineer::Choice Engineer::choose(const CabView& cab, const glm::mat4& car) const {
    Choice best;
    float nearest = 1e9f;
    const auto& sd = cab.description();
    if (mode_ == Mode::seated) return {Target::stand, chair_};
    if (mode_ == Mode::standing) {
        for (std::size_t i = 0; i < sd.chairs.size(); ++i) {
            const float d = distance_xz(feet_, vec(sd.chairs[i].position));
            if (d <= std::min(sd.chairs[i].radius, 1.0f) && d < nearest) {
                nearest = d;
                best = {Target::sit, i};
            }
        }
        for (std::size_t i = 0; i < sd.doors.size(); ++i) {
            // A passenger door (CanBeOpened 0) opens with the train's doors;
            // open, he walks out through it. One that follows another
            // (ParentObject: a leaf, a handle) is worked through that one:
            // the EP2K's DOOR_CR1, nearer the doorway than DOOR_C's own
            // point, was opened instead of the door.
            if (!has_point(sd.doors[i]) || !sd.doors[i].can_be_opened || sd.doors[i].parent >= 0) continue;
            const float d = distance_xz(feet_, door_point(sd.doors[i]));
            if (d <= std::min(sd.doors[i].radius, 1.3f) && d < nearest) {
                nearest = d;
                best = {Target::door, i};
            }
        }
        return best;
    }
    if (mode_ == Mode::outside) {
        for (std::size_t i = 0; i < sd.doors.size(); ++i) {
            if (!get_on_door(sd.doors[i])) continue;
            const glm::vec3 local = glm::vec3{door_point(sd.doors[i]).x, 0.0f, door_point(sd.doors[i]).z} + outward(sd.doors[i]) * 0.9f;
            const glm::vec3 scene = glm::vec3(car * glm::vec4(local, 1.0f));
            const float d = distance_xz(feet_, scene);
            if (d <= 2.5f && d < nearest) {
                nearest = d;
                best = {Target::get_on, i};
            }
        }
    }
    return best;
}

void Engineer::update_on_foot(const EngineerControls& controls, const Outdoors& outdoors, const float elapsed,
                              ViewerCamera& camera) {
    prompt_.clear();
    if (mode_ != Mode::outside) return;
    look_pitch_ = camera.pitch;
    const float sign_forward = (controls.forward ? 1.0f : 0.0f) - (controls.back ? 1.0f : 0.0f);
    const float sign_right = (controls.right ? 1.0f : 0.0f) - (controls.left ? 1.0f : 0.0f);
    if (sign_forward != 0.0f || sign_right != 0.0f) {
        const glm::vec3 forward{-std::sin(camera.yaw), 0.0f, -std::cos(camera.yaw)};
        const glm::vec3 right{forward.z, 0.0f, -forward.x};
        // RTS 0x4121BC: Shift runs, End alone a fifth of the walk, Shift+End twice the run.
        const float pace = controls.run ? run_speed_ * (controls.slow ? 2.0f : 1.0f)
                                        : walk_speed_ * (controls.slow ? 0.2f : 1.0f);
        walk_outside(glm::normalize(forward * sign_forward + right * sign_right) * pace * std::min(elapsed, 0.1f), outdoors);
    }
    blend_ = 1.0f;
    camera.fly = true;
    camera.position = feet_ + glm::vec3{0.0f, eye_share * height_, 0.0f};
    camera.pitch = std::clamp(look_pitch_, -1.4f, 1.4f);
    last_yaw_ = camera.yaw;
    camera_set_ = true;
}

void Engineer::update(CabView& cab, const EngineerControls& controls, const glm::mat4& car, const Outdoors& outdoors,
                      const float elapsed, ViewerCamera& camera) {
    prompt_.clear();
    if (mode_ == Mode::off) {
        camera_set_ = false;
        return;
    }
    cab.poll();
    cab.step_doors(elapsed);
    const glm::vec4 ahead = car * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
    const float base = std::atan2(ahead.x, ahead.z) + pi;
    const glm::mat4 to_car = glm::inverse(car);

    // The head: relative to the car inside, to the world outside.
    if (inside()) {
        if (rebase_) {
            look_yaw_ = camera.yaw - base; // the same heading in the next car
            look_pitch_ = camera.pitch;
            rebase_ = false;
        } else if (camera_set_) {
            look_yaw_ += camera.yaw - last_yaw_;
            look_pitch_ = camera.pitch;
        } else {
            look_yaw_ = 0.0f;
            look_pitch_ = -0.05f;
        }
        camera.yaw = base + look_yaw_;
    } else {
        look_pitch_ = camera.pitch;
    }

    // Num0 / Num1 in a chair (RTS 0x4121BC; other keys in the openRTS layout,
    // key_layout.hpp): the chair's window, the head out of it.
    if (mode_ == Mode::seated) {
        const auto window = chair_window(cab);
        if (controls.window && window && !head_out_) cab.toggle_window(*window);
        if (controls.head_out && window && (head_out_ || cab.window_open(*window))) {
            blend_from_ = local_eye(cab);
            blend_ = 0.0f;
            head_out_ = !head_out_;
        }
    }

    // Enter.
    const auto choice = choose(cab, car);
    if (controls.action && !head_out_) {
        const auto& sd = cab.description();
        switch (choice.target) {
        case Target::stand: {
            const auto& chairs = sd.chairs;
            const glm::vec3 eye = local_eye(cab);
            lean_ = lean_goal_ = glm::vec3{0.0f};
            if (!controls.action_ctrl) {
                // RTS 0x4121BC, chair action 3 (0x436CA8): Enter rises to
                // the chair's Pos2 and sits back down, at the chair all the
                // time; only Ctrl+Enter (action 2) leaves it, to Pos3.
                if (!up_) {
                    glm::vec3 spot = chair_ < chairs.size() ? vec(chairs[chair_].standing) : cab.eye();
                    if (const auto* surfaces = cab.collision()) {
                        auto floor = surfaces->floor(spot.x, spot.z, spot.y - 1.0f, spot.y + 0.5f, cab.skip());
                        if (!floor) floor = surfaces->floor(spot.x, spot.z, spot.y - 1.0f, spot.y + 1.0f, cab.skip());
                        if (floor) spot.y = *floor;
                    }
                    up_feet_ = spot;
                }
                up_ = !up_;
                blend_from_ = eye;
                blend_ = 0.0f;
                break;
            }
            up_ = false;
            glm::vec3 spot = chair_ < chairs.size() ? vec(chairs[chair_].window) : cab.eye();
            if (const auto* surfaces = cab.collision()) {
                auto floor = surfaces->floor(spot.x, spot.z, spot.y - 1.0f, spot.y + 0.5f, cab.skip());
                if (!floor) floor = surfaces->floor(spot.x, spot.z, spot.y - 1.0f, spot.y + 1.0f, cab.skip());
                if (floor) spot.y = *floor;
            }
            feet_ = spot;
            mode_ = Mode::standing;
            blend_from_ = eye;
            blend_ = 0.0f;
            break;
        }
        case Target::sit: {
            blend_from_ = local_eye(cab);
            blend_ = 0.0f;
            chair_ = choice.index;
            mode_ = Mode::seated;
            break;
        }
        case Target::door:
            cab.toggle_door(choice.index);
            break;
        case Target::get_on: {
            // RTS (0x4121BC), the same at every door: Enter opens or closes
            // it (0x438708; a passenger door is the train's), Ctrl+Enter
            // (or Shift+Enter) goes in, opening it (0x4310B0).
            const bool by_hand = cab.description().doors[choice.index].can_be_opened;
            if (!controls.action_ctrl && !controls.run) {
                if (by_hand && !cab.doorway(choice.index)) cab.toggle_door(choice.index);
                break;
            }
            if (!by_hand && !cab.door_open(choice.index)) break; // shut: the driver opens it
            get_on(cab, choice.index);
            look_yaw_ = camera.yaw - base;
            break;
        }
        default:
            break;
        }
    }

    // The arrow keys: where the head looks, along the ground.
    const float sign_forward = (controls.forward ? 1.0f : 0.0f) - (controls.back ? 1.0f : 0.0f);
    const float sign_right = (controls.right ? 1.0f : 0.0f) - (controls.left ? 1.0f : 0.0f);
    if ((sign_forward != 0.0f || sign_right != 0.0f) && mode_ != Mode::seated) {
        const glm::vec3 forward{-std::sin(camera.yaw), 0.0f, -std::cos(camera.yaw)};
        const glm::vec3 right{forward.z, 0.0f, -forward.x};
        glm::vec3 step = forward * sign_forward + right * sign_right;
        // RTS 0x4121BC: Shift runs, End alone a fifth of the walk, Shift+End twice the run.
        const float pace = controls.run ? run_speed_ * (controls.slow ? 2.0f : 1.0f)
                                        : walk_speed_ * (controls.slow ? 0.2f : 1.0f);
        step = glm::normalize(step) * pace * std::min(elapsed, 0.1f);
        const glm::vec3 local_step = glm::vec3(to_car * glm::vec4(step, 0.0f));
        const auto at_door = mode_ == Mode::standing ? doorway_reached(cab, feet_, local_step) : std::nullopt;
        if (at_door) {
            // Out as soon as he walks into the face of an open outer door;
            // before, only where the wall stopped him dead (sliding along
            // it he could not get out, and had to search the doorway).
            get_off(cab, *at_door, car, outdoors);
        } else if (mode_ == Mode::standing) {
            const glm::vec3 local = local_step;
            const float length = glm::length(local);
            const int pieces = (std::max)(1, static_cast<int>(std::ceil(length / inside_piece)));
            for (int p = 0; p < pieces; ++p) {
                walk_inside(cab, local / static_cast<float>(pieces));
                if (blocked_) break;
            }
            if (blocked_) {
                // Out through an open outer door, or on into the next car.
                const glm::vec3 going{local.x, 0.0f, local.z};
                if (const auto door = open_door_near(cab, true)) {
                    if (glm::dot(going, outward(cab.description().doors[*door])) > 0.0f) get_off(cab, *door, car, outdoors);
                } else if (const auto passage = open_door_near(cab, false)) {
                    if (glm::dot(going, outward(cab.description().doors[*passage])) > 0.0f) pass_ = passage;
                }
            }
        } else if (mode_ == Mode::outside) {
            walk_outside(step, outdoors);
        }
    }

    // What Enter would do now.
    switch (choose(cab, car).target) {
    case Target::stand: {
        prompt_ = up_ ? tr("Enter: sit down, Ctrl+Enter: step out to walk") : tr("Enter: stand up at the desk, Ctrl+Enter: step out to walk");
        if (const auto window = chair_window(cab)) {
            prompt_ += "   ";
            if (head_out_) {
                prompt_ = trf("%s: back into the cab", head_out_key_name());
            } else if (cab.window_open(*window)) {
                prompt_ += trf("%s: close the window, %s: head out", window_key_name(), head_out_key_name());
            } else {
                prompt_ += trf("%s: open the window", window_key_name());
            }
        }
        break;
    }
    case Target::sit: prompt_ = tr("Enter: sit down"); break;
    case Target::door: {
        const auto c = choose(cab, car);
        const auto& door = cab.description().doors[c.index];
        prompt_ = cab.door_open(c.index) ? tr("Enter: close the door")
                  : is_exterior(door)    ? tr("Enter: open the door (then walk out)")
                  : is_passage(door)     ? tr("Enter: open the door (then on to the next section)")
                                         : tr("Enter: open the door");
        break;
    }
    case Target::get_on:
        prompt_ = get_on_prompt(cab, choose(cab, car).index);
        break;
    default: break;
    }

    // The wheel's lean, eased in (2.5 m/s).
    if (const glm::vec3 to = lean_goal_ - lean_; glm::length(to) > 1e-5f) {
        const float most = 2.5f * (std::min)(elapsed, 0.1f);
        lean_ = glm::length(to) <= most ? lean_goal_ : lean_ + glm::normalize(to) * most;
    }

    // The eye.
    blend_ = std::min(1.0f, blend_ + elapsed / blend_time);
    camera.fly = true;
    if (inside()) {
        const glm::vec3 target = local_eye(cab);
        const float t = blend_ * blend_ * (3.0f - 2.0f * blend_);
        const glm::vec3 eye = blend_ < 1.0f ? blend_from_ + (target - blend_from_) * t : target;
        camera.position = glm::vec3(car * glm::vec4(eye, 1.0f));
        camera.yaw = base + look_yaw_;
    } else {
        camera.position = feet_ + glm::vec3{0.0f, eye_share * height_, 0.0f};
    }
    camera.pitch = std::clamp(look_pitch_, -1.4f, 1.4f);
    last_yaw_ = camera.yaw;
    camera_set_ = true;
}

} // namespace openrts::viewer
