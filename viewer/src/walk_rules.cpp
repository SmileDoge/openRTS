#include "walk_rules.hpp"

#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>
#include <glm/mat3x3.hpp>

namespace openrts::viewer {

namespace {

constexpr float box_reach = 0.3f; // RTS 0x5E04E4

glm::vec3 vec(const std::array<float, 3>& a) {
    return {a[0], a[1], a[2]};
}

float horizontal(const glm::vec3& a, const glm::vec3& b) {
    return std::hypot(a.x - b.x, a.z - b.z);
}

// RTS 0x455D08: the distance from `p` to the segment a..b where p lies
// across it; -1 beyond its ends.
float segment_distance(const glm::vec3& a, const glm::vec3& b, const glm::vec3& p) {
    const glm::vec3 d = b - a;
    const float along = glm::dot(d, p - a);
    const float length2 = glm::dot(d, d);
    if (along < 0.0f || length2 < along) return -1.0f;
    return glm::length(a + d * (along / length2) - p);
}

// RTS 0x455E48: `p` onto the segment a..b.
glm::vec3 onto_segment(const glm::vec3& a, const glm::vec3& b, const glm::vec3& p) {
    const glm::vec3 d = b - a;
    const float along = glm::dot(d, p - a);
    const float length2 = glm::dot(d, d);
    if (along <= 0.0f) return a;
    if (length2 <= along) return b;
    return a + d * (along / length2);
}

// A plane between two corners, sorted along X and Z (RTS 0x45B750; Y as written).
struct Rect {
    float x0, x1, z0, z1, y1, y2;
};
Rect sorted(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return {(std::min)(a[0], b[0]), (std::max)(a[0], b[0]), (std::min)(a[2], b[2]), (std::max)(a[2], b[2]), a[1], b[1]};
}
bool holds(const Rect& r, const glm::vec3& p) {
    const float feet = p.y + 0.05f;
    return p.x > r.x0 && p.x < r.x1 && p.z > r.z0 && p.z < r.z1 && (feet >= r.y1 || feet >= r.y2);
}
// RTS 0x4322F0 (mode 6): y2 at the low Z, by (y2 - y1) over the length.
float height(const Rect& r, const float z) {
    if (r.z1 - r.z0 < 1e-6f) return r.y2;
    return r.y2 + (r.y2 - r.y1) * (z - r.z0) / (r.z1 - r.z0);
}

// A box's own frame in its shape's: Rx(rx) Ry(ry) Rz(rz), degrees, then the
// move (RTS 0x45EC40 in its frame, X negated: the same in the file's).
glm::mat3 box_rotation(const formats::WalkBox& box) {
    const float a = glm::radians(box.rotation[0]), b = glm::radians(box.rotation[1]), c = glm::radians(box.rotation[2]);
    const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b), cc = std::cos(c), sc = std::sin(c);
    // Columns (glm is column-major): rows of Rx Ry Rz are
    // [cb cc, -cb sc, sb], [sa sb cc + ca sc, -sa sb sc + ca cc, -sa cb],
    // [-ca sb cc + sa sc, ca sb sc + sa cc, ca cb].
    return glm::mat3{glm::vec3{cb * cc, sa * sb * cc + ca * sc, -ca * sb * cc + sa * sc},
                     glm::vec3{-cb * sc, -sa * sb * sc + ca * cc, ca * sb * sc + sa * cc},
                     glm::vec3{sb, -sa * cb, ca * cb}};
}
bool turned_or_moved(const formats::WalkBox& box) {
    return box.rotation != std::array<float, 3>{} || box.translation != std::array<float, 3>{};
}

// Out of a box across its side nearer `by` (RTS: across X when that side is
// nearer than Z's), returning the X or Z to put the point at.
struct Out {
    bool along_x;
    float value;
};
Out nearer_side(const formats::WalkBox& box, const glm::vec3& by) {
    float dx = box.high[0] - by.x;
    float x = box.high[0] + box_reach;
    if (by.x - box.low[0] < dx) {
        dx = by.x - box.low[0];
        x = box.low[0] - box_reach;
    }
    float dz = by.z - box.low[2];
    float z = box.low[2] - box_reach;
    if (box.high[2] - by.z <= dz) {
        dz = box.high[2] - by.z;
        z = box.high[2] + box_reach;
    }
    return dz >= dx ? Out{true, x} : Out{false, z};
}

} // namespace

void Passage::start(std::vector<formats::TrajectoryPoint> points, const glm::vec3& feet, const bool reverse) {
    points_ = std::move(points);
    start_ = feet;
    reverse_ = reverse;
    active_ = !points_.empty();
    speed_ = -1.0f;
    index_ = 0;
    longest_ = 0.0f;
    // RTS 0x4352B0: from the point nearest him (within 100 m).
    float nearest = 100.0f;
    for (std::size_t i = 0; i < points_.size(); ++i) {
        const float d = glm::length(vec(points_[i].at) - feet);
        if (d < nearest) {
            nearest = d;
            index_ = static_cast<int>(i);
            longest_ = d;
        }
    }
}

void Passage::shift(const glm::vec3& delta) noexcept {
    start_ += delta;
    for (auto& p : points_) {
        p.at[0] += delta.x;
        p.at[1] += delta.y;
        p.at[2] += delta.z;
    }
}

bool Passage::step(glm::vec3& feet, const float elapsed, const float pace) {
    if (!active_) return false;
    const int count = static_cast<int>(points_.size());
    const auto& target = points_[static_cast<std::size_t>(index_)];
    const glm::vec3 to = vec(target.at);
    const int before = reverse_ ? index_ + 1 : index_ - 1;
    const glm::vec3 previous = before < 0 || before >= count ? start_ : vec(points_[static_cast<std::size_t>(before)].at);
    const glm::vec3 delta = to - feet;
    float d = glm::length(delta);
    longest_ = (std::max)(longest_, d);
    if (speed_ < 0.0f) speed_ = pace;
    if (d >= 0.02f) {
        const float l = (std::max)(longest_, 1e-4f);
        switch (target.kind) {
        case 1:
            if (l * 0.3f >= d) speed_ = 0.7f * pace * (d / (l * 0.3f)) + 0.01f;
            break;
        case 2:
            speed_ = pace;
            if (l * 0.3f < d) {
                if (l * 0.6f < d) speed_ = 0.7f * pace * ((l - d) / (l * 0.4f)) + 0.1f;
            } else {
                speed_ = 0.7f * pace * (d / (l * 0.3f)) + 0.1f;
            }
            break;
        case 3:
            speed_ = pace;
            if (l * 0.6f < d) speed_ = 0.7f * pace * ((l - d) / (l * 0.4f)) + 0.1f;
            break;
        case 4:
            if (l * 0.9f > d) {
                if (l * 0.75f >= d) {
                    speed_ = l * 0.5f >= d ? pace * 0.4f : 0.4f * pace * ((d - l * 0.5f) * 1.9f / (l * 0.25f) + 0.1f);
                } else {
                    speed_ = 0.4f * pace * (2.01f - (d - l * 0.75f) / (l * 0.075f));
                }
            } else {
                speed_ = pace * 0.4f;
            }
            break;
        case 5:
            speed_ = 100000.0f;
            break;
        case 6: {
            longest_ = horizontal(to, previous);
            const float across = horizontal(to, feet);
            speed_ = across < longest_ - 0.08f ? (std::max)(longest_ * 0.5f, pace * 2.0f) : 0.01f;
            d = (std::max)(across, 0.02f);
            break;
        }
        default:
            speed_ = pace;
            break;
        }
        const float share = (std::min)(1.0f, elapsed * speed_ / d);
        feet += delta * share;
        return true;
    }
    // At the point: on to the next one.
    const int reached = target.kind;
    const glm::vec3 here = to;
    index_ += reverse_ ? -1 : 1;
    if (index_ >= 0 && index_ < count) {
        const glm::vec3 next = vec(points_[static_cast<std::size_t>(index_)].at);
        if (reached == 6) {
            longest_ = horizontal(next, here);
            speed_ = 0.0f;
        } else if (reached != 0) {
            longest_ = glm::length(next - here);
            speed_ = pace;
        } else {
            speed_ = pace;
        }
        return true;
    }
    feet.y = here.y;
    active_ = false;
    return false;
}

OutsideFooting outside_footing(const formats::ExteriorDetails& details, const std::function<bool(std::size_t)>& door_open,
                               const glm::vec3& at) {
    for (std::size_t i = 0; i < details.planes.size(); ++i) {
        const auto r = sorted(details.planes[i].a, details.planes[i].b);
        if (!holds(r, at)) continue;
        return {OutsideFooting::Kind::plane, i, {at.x, height(r, at.z), at.z}};
    }
    std::optional<std::pair<std::size_t, std::size_t>> ledge;
    for (std::size_t j = 0; j < details.ledges.size(); ++j) {
        const auto& points = details.ledges[j].points;
        for (std::size_t k = 0; k + 1 < points.size(); ++k) {
            const float d = segment_distance(vec(points[k].at), vec(points[k + 1].at), at);
            if (d >= 0.0f && d < 0.3f) ledge = {j, k};
        }
    }
    if (ledge) {
        const auto& points = details.ledges[ledge->first].points;
        return {OutsideFooting::Kind::ledge, ledge->first,
                onto_segment(vec(points[ledge->second].at), vec(points[ledge->second + 1].at), at)};
    }
    for (std::size_t i = 0; i < details.door_details.size(); ++i) {
        const auto& door = details.door_details[i];
        if (!door.has_plane || !door_open || !door_open(i)) continue;
        if (holds(sorted(door.plane.a, door.plane.b), at)) return {OutsideFooting::Kind::door, i, at};
    }
    return {};
}

BoxStep step_among_boxes(const std::vector<PlacedBoxes>& objects, const glm::vec3& from, const glm::vec3& to,
                         const float height) {
    BoxStep out;
    out.to = to;
    for (const auto& object : objects) {
        if (!object.boxes || object.boxes->empty()) continue;
        glm::vec3 to_o{object.inverse * glm::vec4{out.to, 1.0f}};
        const glm::vec3 from_o{object.inverse * glm::vec4{from, 1.0f}};
        const glm::vec3 head_o{object.inverse * glm::vec4{out.to + glm::vec3{0.0f, height, 0.0f}, 1.0f}};
        bool moved = false;
        for (const auto& box : *object.boxes) {
            const bool framed = turned_or_moved(box);
            const glm::mat3 r = framed ? box_rotation(box) : glm::mat3{1.0f};
            const glm::vec3 t = vec(box.translation);
            const auto into = [&](const glm::vec3& p) { return framed ? glm::transpose(r) * (p - t) : p; };
            glm::vec3 p = into(to_o);
            const glm::vec3 f = into(from_o);
            const glm::vec3 h = into(head_o);
            bool changed = false;
            const auto& lo = box.low;
            const auto& hi = box.high;
            // His head would be in it: out across its nearer side.
            if (h.x - box_reach < hi[0] && h.x + box_reach > lo[0] && h.z - box_reach < hi[2] && h.z + box_reach > lo[2]
                && h.y - box_reach <= hi[1] && h.y + box_reach > lo[1]) {
                const auto side = nearer_side(box, h);
                if (side.along_x) {
                    p.x += side.value - h.x;
                } else {
                    p.z += side.value - h.z;
                }
                changed = true;
            }
            const bool over = !(p.x - box_reach >= hi[0] || p.x + box_reach <= lo[0] || p.z - box_reach >= hi[2]
                                || p.z + box_reach <= lo[2]);
            if (over) {
                if (p.y >= hi[1] && p.y - box_reach <= hi[1]) {
                    // On its top.
                    if (!out.on_box) {
                        p.y = hi[1];
                        changed = true;
                        out.on_box = true;
                    }
                } else if ((p.y + height + box_reach >= lo[1] || f.y + height + box_reach >= lo[1]) && p.y - box_reach <= hi[1]) {
                    if (height * 0.4f <= hi[1] - p.y && hi[1] > f.y && height * 0.4f <= hi[1] - f.y) {
                        if (height * 0.9f > hi[1] - f.y || height * 0.9f > hi[1] - p.y) {
                            // Climbs it: half way up, up, over (RTS's path, kinds 2, 2, 0).
                            glm::vec3 top{p.x, hi[1], p.z};
                            if (framed) top = r * top;
                            const float rise = top.y + t.y - from_o.y;
                            out.climb = std::vector<formats::TrajectoryPoint>{
                                {{from.x, from.y + rise * 0.5f, from.z}, 2},
                                {{from.x, from.y + rise, from.z}, 2},
                                {{to.x, from.y + rise, to.z}, 0},
                            };
                            out.to = from;
                            return out;
                        }
                        // Too high: a wall, out across the side nearer his feet.
                        const auto side = nearer_side(box, f);
                        if (side.along_x) {
                            p.x = side.value;
                        } else {
                            p.z = side.value;
                        }
                        changed = true;
                    } else if (p.x < hi[0] && p.x > lo[0] && p.z < hi[2] && p.z > lo[2]) {
                        // A step: onto it.
                        p.y = hi[1];
                        changed = true;
                        out.on_box = true;
                    }
                }
            }
            if (changed) {
                to_o = framed ? r * p + t : p;
                moved = true;
            }
        }
        if (moved) {
            out.to = glm::vec3{object.transform * glm::vec4{to_o, 1.0f}};
            // A steep rise counts as standing on something (RTS).
            if (!out.on_box && out.to.y > from.y && horizontal(out.to, from) * 0.8f <= out.to.y - from.y) out.on_box = true;
        }
    }
    return out;
}

} // namespace openrts::viewer
