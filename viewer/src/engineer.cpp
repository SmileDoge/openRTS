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
constexpr float eye_share = 0.85f;  // of engineer.Height, above his feet (on the cab shape, outside)
constexpr float step_share = 0.4f;  // of engineer.Height: the highest plane above his feet he steps onto (RTS 0x434C58)
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

// A door out onto the car's own outside (EXT_WAG: the ChME3's cab doors to
// its walkways).
bool to_car_outside(const formats::CabDoor& door) {
    return lower(door.from) == "ext_wag" || lower(door.to) == "ext_wag";
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

// The cab's floor for walking, RTS's (0x434C58, each step from 0x4322F0):
// its InteriorPlanes and the InteriorPlane of each open door. The cab
// loader (0x51CD88) keeps a plane's corners as written, X negated as every
// .sd position: it holds the points from the first corner's X to the
// second's and from the first's Z down to the second's, so a plane written
// the other way round holds nobody (the 2TE116's three in its cabs). Its
// height goes from the first corner's at its Z to the second's at its
// (the ER9P's door steps).
bool holds(const formats::CabPlane& p, const float x, const float z) {
    return p.a[2] > p.b[2] && p.a[0] <= x && x <= p.b[0] && p.b[2] <= z && z <= p.a[2];
}

float plane_height(const formats::CabPlane& p, const float z) {
    return p.a[1] + (p.b[1] - p.a[1]) * (p.a[2] - z) / (p.a[2] - p.b[2]);
}

bool walks_on_planes(const CabView& cab) {
    return !cab.description().planes.empty();
}

struct Footing {
    enum class Kind : std::uint8_t { none, plane, door };
    Kind kind = Kind::none;
    std::size_t index{};
    float y{};
};

// What holds a step at (x, z): the highest plane there under `below` (RTS:
// his feet + 0.4 Height), or an open door's plane above every such plane.
Footing footing(const CabView& cab, const float x, const float z, const float below) {
    const auto& sd = cab.description();
    Footing best;
    float top = -100.0f;
    for (std::size_t i = 0; i < sd.planes.size(); ++i) {
        if (!holds(sd.planes[i], x, z)) continue;
        const float y = plane_height(sd.planes[i], z);
        if (y > top && y < below) {
            best = {Footing::Kind::plane, i, y};
            top = y;
        }
    }
    // RTS compares a door's plane with the planes' best only.
    for (std::size_t i = 0; i < sd.doors.size(); ++i) {
        const auto& door = sd.doors[i];
        if (!has_plane(door) || !cab.door_open(i) || !holds(door.plane, x, z)) continue;
        const float y = plane_height(door.plane, z);
        if (y > top && y < below) best = {Footing::Kind::door, i, y};
    }
    return best;
}

// openRTS: the plane (or open door's plane) nearest (x, z), within
// `within` metres, under `below` there: the point brought into it. 43 of
// the chairs' Pos3 lie up to 0.3 m off every plane (the VL10k's 0.034 m),
// and where one climbs in is not RTS's (it runs the door's Trajectory).
std::optional<glm::vec3> nearest_plane_point(const CabView& cab, const float x, const float z, const float below,
                                             const float within) {
    const auto& sd = cab.description();
    std::optional<glm::vec3> best;
    float nearest = within;
    auto consider = [&](const formats::CabPlane& p) {
        if (p.a[2] <= p.b[2] || p.a[0] > p.b[0]) return;
        const glm::vec3 in{std::clamp(x, p.a[0], p.b[0]), 0.0f, std::clamp(z, p.b[2], p.a[2])};
        const float d = std::hypot(in.x - x, in.z - z);
        const float y = plane_height(p, in.z);
        if (d <= nearest && y < below) {
            nearest = d;
            best = glm::vec3{in.x, y, in.z};
        }
    };
    for (const auto& p : sd.planes) consider(p);
    for (std::size_t i = 0; i < sd.doors.size(); ++i)
        if (has_plane(sd.doors[i]) && cab.door_open(i)) consider(sd.doors[i].plane);
    return best;
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
    seat_.reset();
    up_ = false;
    lean_ = lean_goal_ = glm::vec3{0.0f};
    blend_ = 1.0f;
    camera_set_ = false;
    head_out_ = false;
}

std::optional<std::size_t> Engineer::chair_window(const CabView& cab) const {
    const auto& chairs = cab.description().chairs;
    const auto& seats = cab.description().seats;
    // A salon's seat's window, or the chair's.
    const std::string* name = nullptr;
    if (seat_) {
        if (*seat_ < seats.size()) name = &seats[*seat_].head_out_window;
    } else if (chair_ < chairs.size()) {
        name = &chairs[chair_].head_out_window;
    }
    if (!name || name->empty()) return std::nullopt;
    const auto window = cab.find_window(*name);
    if (!window || !cab.description().windows[*window].can_open) return std::nullopt;
    return window;
}

void Engineer::stand_by(const CabView& cab, const std::size_t chair) {
    const auto& chairs = cab.description().chairs;
    if (chair >= chairs.size()) {
        sit(chair);
        return;
    }
    chair_ = chair;
    feet_ = stand_at(cab, vec(chairs[chair].window));
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
    passage_.stop();
    fall_ = 0.0f;
    box_top_.reset();
}

void Engineer::off() noexcept {
    mode_ = Mode::off;
    seat_.reset();
    camera_set_ = false;
    prompt_.clear();
    passage_.stop();
    fall_ = 0.0f;
}

glm::vec3 Engineer::local_eye(const CabView& cab) const {
    if (mode_ == Mode::seated && head_out_) {
        if (const auto window = chair_window(cab)) return vec(cab.description().windows[*window].head_out);
    }
    if (mode_ == Mode::seated && up_) return up_feet_ + glm::vec3{0.0f, rise(cab), 0.0f} + lean_;
    if (mode_ == Mode::seated && seat_ && *seat_ < cab.description().seats.size()) {
        // RTS 0x4372B8: down at Pos2, the eye Height1 above it.
        return vec(cab.description().seats[*seat_].seated) + glm::vec3{0.0f, seated_eye_, 0.0f} + lean_;
    }
    if (mode_ == Mode::seated) {
        const auto& chairs = cab.description().chairs;
        const glm::vec3 seat = chair_ < chairs.size() ? vec(chairs[chair_].seated) : cab.eye();
        return seat + glm::vec3{0.0f, seated_eye_, 0.0f} + lean_;
    }
    return feet_ + glm::vec3{0.0f, rise(cab), 0.0f};
}

float Engineer::rise(const CabView& cab) const noexcept {
    return walks_on_planes(cab) ? height_ : eye_share * height_;
}

glm::vec3 Engineer::stand_at(const CabView& cab, glm::vec3 spot) const {
    if (walks_on_planes(cab)) {
        // RTS stands him at the point itself (0x436CA8); the plane there
        // gives him its height at once, not at the first step.
        const float below = spot.y + step_share * height_;
        if (const auto at = footing(cab, spot.x, spot.z, below); at.kind != Footing::Kind::none) {
            spot.y = at.y;
        } else if (const auto near = nearest_plane_point(cab, spot.x, spot.z, below, 0.3f)) {
            spot.y = near->y;
        }
        return spot;
    }
    if (const auto* surfaces = cab.collision()) {
        auto floor = surfaces->floor(spot.x, spot.z, spot.y - 1.0f, spot.y + 0.5f, cab.skip());
        if (!floor) floor = surfaces->floor(spot.x, spot.z, spot.y - 1.0f, spot.y + 1.0f, cab.skip());
        if (floor) spot.y = *floor;
    }
    return spot;
}

void Engineer::walk_planes(const CabView& cab, const glm::vec3& step) {
    blocked_ = false;
    const float below = feet_.y + step_share * height_;
    const glm::vec3 to{feet_.x + step.x, feet_.y, feet_.z + step.z};
    // RTS 0x4322F0: where a plane holds the step, he goes there, at its height.
    if (const auto at = footing(cab, to.x, to.z, below); at.kind != Footing::Kind::none) {
        feet_ = {to.x, at.y, to.z};
        return;
    }
    // Off the planes: from a plane, along its edge (0x4350E0 brings the point
    // into its rectangle); from a door's plane he stays (an outer door's or a
    // passage's: out through it, update()); from nowhere (a Pos3 off the
    // planes, openRTS) onto the nearest plane.
    glm::vec3 next = feet_;
    const auto from = footing(cab, feet_.x, feet_.z, below);
    if (from.kind == Footing::Kind::door) {
        // RTS 0x4322F0 -> 0x431E00: off an open door's plane to the car's
        // outside (EXT_WAG), outside every InteriorArea: onto it, where it
        // holds him (the scene's to tell; the ChME3's cab doors). openRTS
        // looks 0.1 m on: the ChME3's door plane ends 0.036 m short of its
        // area's edge, more than a step at a walk (RTS's steps are longer).
        const auto& door = cab.description().doors[from.index];
        if (to_car_outside(door)) {
            const glm::vec2 along{step.x, step.z};
            const glm::vec2 on = glm::length(along) > 1e-6f ? glm::vec2{to.x, to.z} + glm::normalize(along) * 0.1f : glm::vec2{to.x, to.z};
            bool in_area = false;
            for (const auto& area : cab.description().areas)
                in_area = in_area || (on.x >= (std::min)(area.x1, area.x2) && on.x <= (std::max)(area.x1, area.x2)
                                      && on.y >= (std::min)(area.z1, area.z2) && on.y <= (std::max)(area.z1, area.z2));
            if (!in_area) event_ = {.kind = Event::Kind::onto_car, .door = from.index, .at = to};
        }
    }
    if (from.kind == Footing::Kind::plane) {
        const auto& p = cab.description().planes[from.index];
        next = {std::clamp(to.x, p.a[0], p.b[0]), 0.0f, std::clamp(to.z, p.b[2], p.a[2])};
        next.y = plane_height(p, next.z);
    } else if (from.kind == Footing::Kind::none) {
        if (const auto near = nearest_plane_point(cab, to.x, to.z, below, 0.3f)) next = *near;
    }
    if (distance_xz(next, feet_) < 1e-5f) {
        blocked_ = true;
        return;
    }
    feet_ = next;
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
    if (walks_on_planes(cab)) {
        walk_planes(cab, step);
        return;
    }
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

void Engineer::settle(const float support, const float elapsed) {
    if (support >= feet_.y) {
        feet_.y = support;
        fall_ = 0.0f;
        return;
    }
    // RTS 0x4121BC: down at 9.8 m/s² to what holds him.
    fall_ += 9.8f * elapsed;
    feet_.y = (std::max)(support, feet_.y - fall_ * elapsed);
    if (feet_.y <= support) fall_ = 0.0f;
}

void Engineer::walk_outside(const glm::vec3& step, const Outdoors& outdoors, const float elapsed) {
    if (glm::length(glm::vec2{step.x, step.z}) < 1e-6f) {
        // Standing: only falling on (RTS tests a step while he moves or falls).
        if (fall_ <= 0.0f) return;
        std::optional<float> support = box_top_;
        if (!support && outdoors.floor) support = outdoors.floor(feet_);
        if (support) settle(*support, elapsed);
        return;
    }
    auto blocked = [&](const glm::vec3& to) { return outdoors.blocked && outdoors.blocked(feet_, to); };
    glm::vec3 to = feet_ + step;
    // The world's objects' boxes (RTS 0x4322F0): pushed out of walls, onto
    // tops, or a climb that runs as a passage.
    std::optional<float> on_box;
    if (outdoors.boxes) {
        auto among = outdoors.boxes(feet_, to, height_);
        if (among.climb) {
            passage_.start(std::move(*among.climb), feet_, false);
            passage_end_.clear();
            passage_door_ = SIZE_MAX;
            fall_ = 0.0f;
            return;
        }
        to.x = among.to.x;
        to.z = among.to.z;
        if (among.on_box) on_box = among.to.y;
    }
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
    box_top_ = on_box;
    std::optional<float> support = on_box;
    if (!support && outdoors.floor) support = outdoors.floor(feet_);
    if (support) settle(*support, elapsed);
}

void Engineer::shift(const glm::vec3& delta) noexcept {
    if (mode_ != Mode::outside) return;
    feet_ += delta;
    if (box_top_) *box_top_ += delta.y;
    passage_.shift(delta);
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
    if (walks_on_planes(cab)) {
        // On the plane inside the doorway, up to 1.6 m above the doorway's
        // (RTS runs the door's Trajectory up the steps to it): the ER9P's
        // outer doors' planes go from 1.0 down to 0.0, the vestibule's is
        // at 1.0. The CHS4t's point inside lies 0.013 m off its plane.
        const float below = plane + 1.6f;
        if (const auto at = footing(cab, inner.x, inner.z, below); at.kind != Footing::Kind::none) {
            inner.y = at.y;
        } else if (const auto near = nearest_plane_point(cab, inner.x, inner.z, below, 0.6f)) {
            inner = *near;
        }
    } else if (const auto* surfaces = cab.collision()) {
        // Up the steps when the doorway's floor is the lowest step: the
        // ER9PK's cab doors (cabklub.sd DOOR1 / DOOR2) have their plane at
        // 0.50, the vestibule's floor at 1.39; he stood under it, and no
        // step of his reached it (2026-11-11).
        auto floor = surfaces->floor(inner.x, inner.z, plane - 0.6f, plane + 0.8f, cab.skip());
        if (!floor) floor = surfaces->floor(inner.x, inner.z, plane - 0.6f, plane + 1.6f, cab.skip());
        if (floor) inner.y = *floor;
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
        // A salon's seat or a window he looks at (RTS picks the nearest on
        // the screen within its Radius, 0x414D24; among a salon's hundreds
        // of seats openRTS takes the one most in the middle of the view,
        // within 2.2 m).
        Choice looked;
        float middle = 0.75f;
        const auto consider = [&](const glm::vec3& at, const float reach, const Target target, const std::size_t index) {
            const glm::vec3 to = at - eye_cab_;
            const float d = glm::length(to);
            if (d > (std::min)(reach, 2.2f) || d < 1e-3f) return;
            const float facing = glm::dot(to / d, look_cab_);
            if (facing > middle) {
                middle = facing;
                looked = {target, index};
            }
        };
        for (std::size_t i = 0; i < sd.seats.size(); ++i) consider(vec(sd.seats[i].position), sd.seats[i].radius, Target::seat, i);
        for (std::size_t i = 0; i < sd.windows.size(); ++i)
            if (sd.windows[i].has_position && sd.windows[i].can_open && sd.windows[i].radius > 0.0f)
                consider(vec(sd.windows[i].position), sd.windows[i].radius, Target::window, i);
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
        // A chair or a door by him, unless what he looks at is nearer the
        // middle of the view (in a salon the tambour's door is by the first
        // seats).
        if (best.target == Target::none) return looked;
        if (looked.target == Target::none) return best;
        const glm::vec3 at = best.target == Target::sit ? vec(sd.chairs[best.index].position) : door_point(sd.doors[best.index]);
        const glm::vec3 to = at - eye_cab_;
        const float best_facing = glm::length(to) > 1e-3f ? glm::dot(glm::normalize(to), look_cab_) : 1.0f;
        return middle > best_facing ? looked : best;
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

glm::vec3 Engineer::walk_step(const EngineerControls& controls, const float yaw, const float elapsed) const {
    const float sign_forward = (controls.forward ? 1.0f : 0.0f) - (controls.back ? 1.0f : 0.0f);
    const float sign_right = (controls.right ? 1.0f : 0.0f) - (controls.left ? 1.0f : 0.0f);
    if (sign_forward == 0.0f && sign_right == 0.0f) return {};
    const glm::vec3 forward{-std::sin(yaw), 0.0f, -std::cos(yaw)};
    const glm::vec3 right{forward.z, 0.0f, -forward.x};
    // RTS 0x4121BC: Shift runs, End alone a fifth of the walk, Shift+End twice the run.
    const float pace = controls.run ? run_speed_ * (controls.slow ? 2.0f : 1.0f) : walk_speed_ * (controls.slow ? 0.2f : 1.0f);
    return glm::normalize(forward * sign_forward + right * sign_right) * pace * std::min(elapsed, 0.1f);
}

void Engineer::move_outside(const EngineerControls& controls, const Outdoors& outdoors, const float elapsed,
                            const ViewerCamera& camera) {
    const float dt = (std::min)(elapsed, 0.1f);
    if (passage_.active()) {
        // The climb onto a box (RTS: the keys wait while he goes).
        if (!passage_.step(feet_, dt, walk_speed_)) end_passage(nullptr, outdoors);
        return;
    }
    walk_outside(walk_step(controls, camera.yaw, elapsed), outdoors, dt);
}

void Engineer::update_on_foot(const EngineerControls& controls, const Outdoors& outdoors, const float elapsed,
                              ViewerCamera& camera) {
    prompt_.clear();
    if (mode_ != Mode::outside) return;
    look_pitch_ = camera.pitch;
    move_outside(controls, outdoors, elapsed, camera);
    blend_ = 1.0f;
    camera.fly = true;
    camera.position = feet_ + glm::vec3{0.0f, height_, 0.0f};
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

    // Where he looks from and to, in the cab (a seat or a window is picked so).
    eye_cab_ = glm::vec3{to_car * glm::vec4{camera.position, 1.0f}};
    if (const glm::vec3 l{to_car * glm::vec4{camera.forward(), 0.0f}}; glm::length(l) > 1e-4f) look_cab_ = glm::normalize(l);

    // Enter.
    const auto choice = choose(cab, car);
    if (controls.action && !head_out_) {
        const auto& sd = cab.description();
        switch (choice.target) {
        case Target::stand: {
            const auto& chairs = sd.chairs;
            const glm::vec3 eye = local_eye(cab);
            lean_ = lean_goal_ = glm::vec3{0.0f};
            if (seat_) {
                // RTS 0x4372B8 action 2: up and back to the aisle (Pos1).
                const auto& seats = sd.seats;
                feet_ = stand_at(cab, *seat_ < seats.size() ? vec(seats[*seat_].aisle) : feet_);
                seat_.reset();
                up_ = false;
                mode_ = Mode::standing;
                blend_from_ = eye;
                blend_ = 0.0f;
                break;
            }
            if (!controls.action_ctrl) {
                // RTS 0x4121BC, chair action 3 (0x436CA8): Enter rises to
                // the chair's Pos2 and sits back down, at the chair all the
                // time; only Ctrl+Enter (action 2) leaves it, to Pos3.
                if (!up_) {
                    // RTS (0x43778C): risen, his feet are at Pos2.y, the
                    // eye Height above. On the cab shape: on its floor.
                    glm::vec3 spot = chair_ < chairs.size() ? vec(chairs[chair_].standing) : cab.eye();
                    const auto* surfaces = walks_on_planes(cab) ? nullptr : cab.collision();
                    if (surfaces) {
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
            feet_ = stand_at(cab, chair_ < chairs.size() ? vec(chairs[chair_].window) : cab.eye());
            mode_ = Mode::standing;
            blend_from_ = eye;
            blend_ = 0.0f;
            break;
        }
        case Target::sit: {
            blend_from_ = local_eye(cab);
            blend_ = 0.0f;
            chair_ = choice.index;
            seat_.reset();
            mode_ = Mode::seated;
            break;
        }
        case Target::seat: {
            // RTS 0x4372B8 action 1: along the aisle (Pos1) and down (Pos2).
            blend_from_ = local_eye(cab);
            blend_ = 0.0f;
            seat_ = choice.index;
            up_ = false;
            head_out_ = false;
            mode_ = Mode::seated;
            break;
        }
        case Target::window:
            // RTS type 6 (0x4121BC -> 0x438708): Enter opens or shuts it.
            cab.toggle_window(choice.index);
            break;
        case Target::door:
            if (controls.action_ctrl && to_car_outside(sd.doors[choice.index])) {
                // RTS 0x4121BC -> 0x431270: Ctrl+Enter opens the door and
                // goes out along the car's ExteriorDoor of the same name.
                if (!cab.door_open(choice.index)) cab.toggle_door(choice.index);
                event_ = {.kind = Event::Kind::onto_car, .door = choice.index, .at = feet_, .by_path = true};
            } else {
                cab.toggle_door(choice.index);
            }
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
                if (blocked_ || event_.kind != Event::Kind::none) break;
            }
            if (blocked_ && event_.kind == Event::Kind::none) {
                // Out through an open outer door, or on into the next car.
                const glm::vec3 going{local.x, 0.0f, local.z};
                if (const auto door = open_door_near(cab, true)) {
                    if (glm::dot(going, outward(cab.description().doors[*door])) > 0.0f) get_off(cab, *door, car, outdoors);
                } else if (const auto passage = open_door_near(cab, false)) {
                    if (glm::dot(going, outward(cab.description().doors[*passage])) > 0.0f) pass_ = passage;
                }
            }
        }
    }
    if (mode_ == Mode::outside) move_outside(controls, outdoors, elapsed, camera);

    // What Enter would do now.
    switch (choose(cab, car).target) {
    case Target::stand: {
        prompt_ = seat_ ? tr("Enter: stand up")
                  : up_ ? tr("Enter: sit down, Ctrl+Enter: step out to walk")
                        : tr("Enter: stand up at the desk, Ctrl+Enter: step out to walk");
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
    case Target::seat: prompt_ = tr("Enter: sit down"); break;
    case Target::window:
        prompt_ = cab.window_open(choose(cab, car).index) ? tr("Enter: close the window") : tr("Enter: open the window");
        break;
    case Target::door: {
        const auto c = choose(cab, car);
        const auto& door = cab.description().doors[c.index];
        prompt_ = to_car_outside(door)   ? (cab.door_open(c.index) ? tr("Enter: close the door, Ctrl+Enter: go out")
                                                                   : tr("Enter: open the door, Ctrl+Enter: go out"))
                  : cab.door_open(c.index) ? tr("Enter: close the door")
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
        camera.position = feet_ + glm::vec3{0.0f, height_, 0.0f}; // RTS: Height above his feet
    }
    camera.pitch = std::clamp(look_pitch_, -1.4f, 1.4f);
    last_yaw_ = camera.yaw;
    camera_set_ = true;
}

// ---------------------------------------------------------------------------
// A car's outside (RTS mode 6): walkways, steps, ladders, ledges, its doors.

namespace {

// In view (RTS 0x422EBC: the Position on the screen) and within `reach`
// of the eye: the distance, else none.
std::optional<float> in_reach(const glm::vec3& point, const float reach, const glm::vec3& eye, const glm::vec3& forward) {
    const glm::vec3 to = point - eye;
    const float d = glm::length(to);
    if (d > reach) return std::nullopt;
    if (d > 0.3f && glm::dot(to / d, forward) < 0.45f) return std::nullopt; // off the screen
    return d;
}

// The ExteriorDoor of `car` within reach: one he works (with a Position),
// the nearest.
std::optional<std::size_t> outer_door_in_reach(const CarOutside& car, const glm::vec3& eye, const glm::vec3& forward) {
    std::optional<std::size_t> best;
    float nearest = 1e9f;
    const auto& doors = car.details->door_details;
    for (std::size_t i = 0; i < doors.size(); ++i) {
        if (!doors[i].has_position || doors[i].parent >= 0) continue;
        const glm::vec3 at{car.transform * glm::vec4{vec(doors[i].position), 1.0f}};
        if (const auto d = in_reach(at, doors[i].radius, eye, forward); d && *d < nearest) {
            nearest = *d;
            best = i;
        }
    }
    return best;
}

// RTS 0x431F88: from the ground up the way it leads (when it leads
// somewhere but the ground), else from the end nearer him.
bool ladder_reverse(const formats::ExteriorLadder& ladder, const glm::vec3& feet, const bool from_ground) {
    if (ladder.trajectory.empty()) return false;
    if (from_ground && !ladder.to.empty() && ladder.to != "exterior") return false;
    const float first = glm::length(feet - vec(ladder.trajectory.front().at));
    const float last = glm::length(feet - vec(ladder.trajectory.back().at));
    return last < first;
}

} // namespace

std::optional<std::pair<std::size_t, float>> Engineer::ladder_in_reach(const CarOutside& car, const glm::vec3& eye,
                                                                       const glm::vec3& forward) {
    if (!car.details) return std::nullopt;
    std::optional<std::pair<std::size_t, float>> best;
    const auto& ladders = car.details->ladders;
    for (std::size_t i = 0; i < ladders.size(); ++i) {
        if (ladders[i].trajectory.empty()) continue;
        const glm::vec3 at{car.transform * glm::vec4{vec(ladders[i].position), 1.0f}};
        if (const auto d = in_reach(at, ladders[i].radius, eye, forward); d && (!best || *d < best->second)) best = {{i, *d}};
    }
    return best;
}

std::string Engineer::ladder_prompt(const CarOutside& car, const std::size_t ladder, const glm::vec3& feet_car) {
    if (!car.details || ladder >= car.details->ladders.size()) return {};
    const auto& points = car.details->ladders[ladder].trajectory;
    if (points.empty()) return {};
    // Where it takes him: the far end.
    const float first = glm::length(feet_car - vec(points.front().at));
    const float last = glm::length(feet_car - vec(points.back().at));
    const float end = (last < first ? points.front() : points.back()).at[1];
    return end < feet_car.y - 0.3f ? tr("Ctrl+Enter: climb down") : tr("Ctrl+Enter: climb up");
}

void Engineer::climb(const CarOutside& car, const std::size_t ladder) {
    if (!car.details || ladder >= car.details->ladders.size()) return;
    const auto& l = car.details->ladders[ladder];
    const bool from_ground = mode_ == Mode::outside;
    if (from_ground) feet_ = glm::vec3{glm::inverse(car.transform) * glm::vec4{feet_, 1.0f}};
    const bool reverse = ladder_reverse(l, feet_, from_ground);
    mode_ = Mode::on_car;
    passage_.start(l.trajectory, feet_, reverse);
    // RTS 0x435654: the end leads to the Connects word on that side.
    passage_end_ = reverse ? l.from : l.to;
    passage_door_ = SIZE_MAX;
    last_ = {};
    fall_ = 0.0f;
    box_top_.reset();
    if (from_ground) rebase_ = true;
    camera_set_ = true;
}

void Engineer::step_onto_car(const CarOutside& car, const glm::vec3& at, const std::optional<std::size_t> door) {
    if (!car.details) return;
    mode_ = Mode::on_car;
    feet_ = at;
    fall_ = 0.0f;
    up_ = false;
    head_out_ = false;
    blend_ = 1.0f;
    rebase_ = true;
    camera_set_ = true;
    last_ = outside_footing(*car.details, car.door_open, feet_);
    if (door && *door < car.details->door_details.size() && !car.details->door_details[*door].trajectory.empty()) {
        // RTS 0x431270: the car's door's way backwards, out to its outside.
        passage_.start(car.details->door_details[*door].trajectory, feet_, true);
        passage_end_ = "ext_wag";
        passage_door_ = SIZE_MAX;
    } else {
        passage_.stop();
    }
}

void Engineer::enter_cab(const CabView& cab, const std::size_t door, const glm::vec3& at) {
    (void)door;
    mode_ = Mode::standing;
    passage_.stop();
    fall_ = 0.0f;
    up_ = false;
    head_out_ = false;
    blend_ = 1.0f;
    rebase_ = true;
    camera_set_ = true;
    feet_ = at;
    if (walks_on_planes(cab)) {
        const float below = at.y + step_share * height_;
        if (const auto f = footing(cab, at.x, at.z, below); f.kind != Footing::Kind::none) {
            feet_.y = f.y;
        } else if (const auto near = nearest_plane_point(cab, at.x, at.z, below, 0.6f)) {
            feet_ = *near;
        }
    } else {
        feet_ = stand_at(cab, at);
    }
}

bool Engineer::cab_holds(const CabView& cab, const glm::vec3& at, const float height) {
    return walks_on_planes(cab) && footing(cab, at.x, at.z, at.y + step_share * height).kind != Footing::Kind::none;
}

void Engineer::get_off_car(const CarOutside& car, const glm::vec3& at) {
    const glm::vec3 scene{car.transform * glm::vec4{at, 1.0f}};
    mode_ = Mode::outside;
    passage_.stop();
    feet_ = scene;
    box_top_.reset();
    fall_ = 1e-4f; // down to the ground (RTS 0x4369F4, then falling)
    camera_set_ = true;
}

void Engineer::look_relative(const float base, ViewerCamera& camera) {
    if (rebase_) {
        look_yaw_ = camera.yaw - base; // the same heading as before
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
}

void Engineer::end_passage(const CarOutside* car, const Outdoors& outdoors) {
    (void)outdoors;
    const std::string word = std::exchange(passage_end_, std::string{});
    const std::size_t door = std::exchange(passage_door_, SIZE_MAX);
    fall_ = 0.0f;
    if (mode_ != Mode::on_car || !car || !car->details) return;
    if (word == "exterior") {
        get_off_car(*car, feet_); // RTS -1: on the ground
    } else if (word == "interior") {
        event_ = {.kind = Event::Kind::into_cab, .door = door, .at = feet_}; // RTS -3: the cab
    } else if (word == "wagon_front" || word == "wagon_back") {
        event_ = {.kind = Event::Kind::next_car, .side = word == "wagon_front" ? 1 : -1, .at = feet_};
    } else {
        // EXT_WAG, none (a roof's ladder) or an area: he stays on the outside.
        last_ = outside_footing(*car->details, car->door_open, feet_);
    }
}

void Engineer::walk_on_car(const CarOutside& car, const glm::vec3& step, const Outdoors& outdoors) {
    (void)outdoors;
    blocked_ = false;
    const auto& d = *car.details;
    const glm::vec3 to{feet_.x + step.x, feet_.y, feet_.z + step.z};
    const auto held = outside_footing(d, car.door_open, to);
    if (held.kind != OutsideFooting::Kind::none) {
        // RTS 0x4322F0 (mode 6): there, at the plane's height, onto the
        // ledge, or (a door's plane) as he is.
        last_ = held;
        feet_.x = held.at.x;
        feet_.z = held.at.z;
        if (held.kind != OutsideFooting::Kind::door) {
            if (held.at.y >= feet_.y) {
                feet_.y = held.at.y;
                fall_ = 0.0f;
            } else {
                fall_ = (std::max)(fall_, 1e-4f);
                fall_target_ = held.at.y;
            }
        }
        return;
    }
    if (last_.kind == OutsideFooting::Kind::plane && last_.index < d.planes.size()) {
        const auto& plane = d.planes[last_.index];
        if (plane.front || plane.back) {
            // Over to the car at that end (RTS 0x4313FC).
            event_ = {.kind = Event::Kind::next_car, .side = plane.front ? 1 : -1, .at = to, .or_off = plane.exit};
            blocked_ = true;
            return;
        }
        if (!plane.exit) {
            blocked_ = true; // he stays where he was
            return;
        }
    } else if (last_.kind == OutsideFooting::Kind::door && last_.index < d.door_details.size()) {
        const auto& door = d.door_details[last_.index];
        if (door.to == "interior" && car.cab_holds && car.cab_holds(to)) {
            // Into the cab along the door's way (RTS 0x4310B0).
            passage_.start(door.trajectory.empty() ? std::vector<formats::TrajectoryPoint>{{{to.x, to.y, to.z}, 0}} : door.trajectory,
                           feet_, false);
            passage_end_ = door.to;
            passage_door_ = last_.index;
            return;
        }
        if (door.to == "wagon_front" || door.to == "wagon_back") {
            event_ = {.kind = Event::Kind::next_car, .side = door.to == "wagon_front" ? 1 : -1, .at = to};
            blocked_ = true;
            return;
        }
    }
    // At the side down to the ground; at an end over to the next car, or down.
    if (std::abs(to.x) >= 0.4f * car.width) {
        get_off_car(car, to);
        return;
    }
    event_ = {.kind = Event::Kind::next_car, .side = to.z > 0.0f ? 1 : -1, .at = to, .or_off = true};
    blocked_ = true;
}

void Engineer::update_on_car(const CarOutside& car, const EngineerControls& controls, const Outdoors& outdoors,
                             const float elapsed, ViewerCamera& camera) {
    prompt_.clear();
    if (mode_ != Mode::on_car || !car.details) return;
    const float dt = (std::min)(elapsed, 0.1f);
    const glm::vec4 ahead = car.transform * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
    const float base = std::atan2(ahead.x, ahead.z) + pi;
    look_relative(base, camera);
    const glm::mat4 to_car = glm::inverse(car.transform);

    if (passage_.active()) {
        if (!passage_.step(feet_, dt, walk_speed_)) end_passage(&car, outdoors);
    } else {
        const glm::vec3 eye{car.transform * glm::vec4{feet_ + glm::vec3{0.0f, height_, 0.0f}, 1.0f}};
        const glm::vec3 forward = camera.forward();
        const auto ladder = ladder_in_reach(car, eye, forward);
        const auto door = outer_door_in_reach(car, eye, forward);
        float door_distance = 1e9f;
        if (door) door_distance = glm::length(glm::vec3{car.transform * glm::vec4{vec(car.details->door_details[*door].position), 1.0f}} - eye);
        const bool at_ladder = ladder && (!door || ladder->second <= door_distance);
        if (at_ladder) {
            prompt_ = ladder_prompt(car, ladder->first, feet_);
            if (controls.action && controls.action_ctrl) climb(car, ladder->first);
        } else if (door) {
            const auto& d = car.details->door_details[*door];
            const bool open = car.door_open && car.door_open(*door);
            prompt_ = !d.can_be_opened ? tr("Ctrl+Enter: go through")
                      : open           ? tr("Enter: close the door, Ctrl+Enter: go through")
                                       : tr("Enter: open the door, Ctrl+Enter: go through");
            if (controls.action) {
                if (controls.action_ctrl) {
                    // RTS 0x4121BC: open it (0x438708), then through along its way (0x4310B0).
                    if (!open && d.can_be_opened) event_ = {.kind = Event::Kind::outer_door, .door = *door};
                    passage_.start(d.trajectory.empty() ? std::vector<formats::TrajectoryPoint>{{{feet_.x, feet_.y, feet_.z}, 0}}
                                                        : d.trajectory,
                                   feet_, false);
                    passage_end_ = d.to;
                    passage_door_ = *door;
                } else if (d.can_be_opened) {
                    event_ = {.kind = Event::Kind::outer_door, .door = *door};
                }
            }
        }
        if (!passage_.active() && mode_ == Mode::on_car) {
            const glm::vec3 step = walk_step(controls, camera.yaw, elapsed);
            const glm::vec3 local{to_car * glm::vec4{step, 0.0f}};
            const float length = glm::length(glm::vec2{local.x, local.z});
            if (length > 1e-6f) {
                const int pieces = (std::max)(1, static_cast<int>(std::ceil(length / inside_piece)));
                for (int p = 0; p < pieces && mode_ == Mode::on_car; ++p) {
                    walk_on_car(car, local / static_cast<float>(pieces), outdoors);
                    if (blocked_ || passage_.active() || event_.kind == Event::Kind::next_car) break;
                }
            }
            // Falling to what holds him.
            if (mode_ == Mode::on_car && fall_ > 0.0f) {
                fall_ += 9.8f * dt;
                feet_.y = (std::max)(fall_target_, feet_.y - fall_ * dt);
                if (feet_.y <= fall_target_) fall_ = 0.0f;
            }
        }
    }

    blend_ = 1.0f;
    camera.fly = true;
    if (mode_ == Mode::on_car) {
        camera.position = glm::vec3{car.transform * glm::vec4{feet_ + glm::vec3{0.0f, height_, 0.0f}, 1.0f}};
        camera.yaw = base + look_yaw_;
    } else {
        camera.position = feet_ + glm::vec3{0.0f, height_, 0.0f};
    }
    camera.pitch = std::clamp(look_pitch_, -1.4f, 1.4f);
    last_yaw_ = camera.yaw;
    camera_set_ = true;
}

} // namespace openrts::viewer
