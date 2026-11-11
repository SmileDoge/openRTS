#include "cab_screens.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>

#include <glm/geometric.hpp>

namespace openrts::viewer {
namespace {

using formats::ScreenObject;
using formats::ScreenObjectType;

constexpr float pi = 3.14159265358979f;

struct Rect {
    float x0{}, y0{}, x1{}, y1{};
};

// The object's state this frame: the DLL's, or the .sd's.
struct Live {
    std::int32_t state{};
    float value{};
    float blink = -1.0f;
    const std::array<float, 4>* area = nullptr; // the DLL's Area
    std::u16string_view text;
};

[[nodiscard]] std::uint32_t rgba(const std::uint32_t argb) {
    const std::uint32_t a = argb >> 24, r = (argb >> 16) & 0xFFu, g = (argb >> 8) & 0xFFu, b = argb & 0xFFu;
    return a << 24 | b << 16 | g << 8 | r;
}

[[nodiscard]] bool blended(const std::uint32_t shader) { return shader == 3 || shader == 4 || shader == 6 || shader == 7; }

[[nodiscard]] glm::vec3 vec(const std::array<float, 3>& v) { return {v[0], v[1], v[2]}; }

class Emitter {
public:
    Emitter(std::vector<ScreenBatch>& out, const glm::vec3& tl, const glm::vec3& tr, const glm::vec3& br)
        : out_(out), origin_(tl), across_(tr - tl), down_(br - tr) {}

    void use(const core::VirtualPath& texture, const bool blend, const std::uint32_t colour) {
        colour_ = colour;
        if (!out_.empty() && out_.back().texture == texture && out_.back().blend == blend) return;
        out_.push_back({texture, blend, {}});
    }
    [[nodiscard]] glm::vec3 at(const float u, const float v) const { return origin_ + across_ * u + down_ * v; }
    void vertex(const float x, const float y, const float u, const float v) {
        out_.back().vertices.push_back({at(x, y), {u, v}, colour_});
    }
    // Screen rect r with texture rect t (x0 y0 x1 y1 each).
    void quad(const Rect& r, const Rect& t) {
        vertex(r.x0, r.y0, t.x0, t.y0);
        vertex(r.x0, r.y1, t.x0, t.y1);
        vertex(r.x1, r.y1, t.x1, t.y1);
        vertex(r.x0, r.y0, t.x0, t.y0);
        vertex(r.x1, r.y1, t.x1, t.y1);
        vertex(r.x1, r.y0, t.x1, t.y0);
    }
    // Four corners (screen, texture) in order.
    void quad(const glm::vec2 (&p)[4], const glm::vec2 (&t)[4]) {
        const int order[6] = {0, 1, 2, 0, 2, 3};
        for (const int i : order) vertex(p[i].x, p[i].y, t[i].x, t[i].y);
    }

private:
    std::vector<ScreenBatch>& out_;
    glm::vec3 origin_, across_, down_;
    std::uint32_t colour_ = 0xFFFFFFFFu;
};

// RTS 0x52AAD4: the frame the state (or, PRECISE, the value) picks sets the
// area, activation, UV and texture; between two frames a PRECISE value blends.
void apply_frames(const ScreenObject& def, const Live& live, std::array<float, 4>& area, std::array<float, 4>& uv,
                  std::uint32_t& texture) {
    const auto n = static_cast<std::int32_t>(def.frames.size());
    if (n == 0) return;
    std::int32_t state = live.state;
    const bool precise = (def.flags & formats::screen_precise) != 0;
    if (precise) state = static_cast<std::int32_t>(live.value);
    const float whole = std::trunc(live.value);
    const formats::ScreenFrame* frame = nullptr;
    if (!precise || whole == live.value || live.value < 0.0f || n - 1 <= state) {
        if (state >= 0 && state < n) {
            frame = &def.frames[static_cast<std::size_t>(state)];
            if (frame->has & 1) area = frame->area;
        }
    } else if (state >= 0) {
        frame = &def.frames[static_cast<std::size_t>(state)];
        const auto& next = def.frames[static_cast<std::size_t>(state + 1)];
        const float t = live.value - whole;
        if ((frame->has & 1) && (next.has & 1)) {
            for (int k = 0; k < 4; ++k) area[k] = frame->area[k] * (1.0f - t) + next.area[k] * t;
        }
    }
    if (frame) {
        if (frame->has & 4) uv = frame->uv;
        if (frame->has & 8) texture = frame->texture;
    }
}

// sub_46B5D4: characters of the line starting at `at` (past the line break there).
[[nodiscard]] int line_length(std::u16string_view text, std::size_t at) {
    int n = 0;
    const std::size_t start = at;
    while (at < text.size()) {
        const char16_t c = text[at];
        if (c == u'\r' || c == u'\n') {
            if (at == 0) return 0;
            if ((text[start] != u'\r' || at > start + 1) && at != start) return n;
            ++at;
        } else {
            if (c == 0) return n;
            ++n;
            ++at;
        }
    }
    return n;
}

// SECTOR outline point for an angle (degrees, 0 up, clockwise), 0..1 in the area.
[[nodiscard]] glm::vec2 sector_point(const std::uint32_t shape, const float degrees) {
    if (shape == 0) {
        const float a = degrees * pi / 180.0f;
        return {std::sin(a) * 0.5f + 0.5f, 0.5f - std::cos(a) * 0.5f};
    }
    float x = std::fmod(degrees, 360.0f);
    if (x > 180.0f) x -= 360.0f;
    else if (x < -180.0f) x += 360.0f;
    const float k = 1.0f / 90.0f;
    if (x >= -135.0f && x <= -45.0f) return {0.0f, (-x - 45.0f) * k};
    if (x >= -45.0f && x <= 45.0f) return {(x + 45.0f) * k, 0.0f};
    if (x >= 45.0f && x <= 135.0f) return {1.0f, (x - 45.0f) * k};
    if (x >= 135.0f) return {(180.0f - x + 45.0f) * k, 1.0f};
    return {(-x - 135.0f) * k, 1.0f};
}

} // namespace

CabScreens::CabScreens(const formats::CabDescription& cab, const core::VirtualPath& cab_file) : cab_(&cab) {
    const auto folder = cab_file.parent();
    for (std::size_t d = 0; d < cab.displays.size(); ++d) {
        const auto& display = cab.displays[d];
        if (display.type != formats::CabDisplayType::screen || !display.screen.present) continue;
        Screen s;
        s.display = d;
        s.top_left = vec(display.screen.top_left);
        s.top_right = vec(display.screen.top_right);
        s.bottom_right = vec(display.screen.bottom_right);
        s.source = &display.screen;
        if (display.screen.reference_screen >= 0) {
            s.source = nullptr;
            for (const auto& other : cab.displays) {
                if (other.id == static_cast<std::uint32_t>(display.screen.reference_screen) && other.screen.present &&
                    other.screen.reference_screen < 0)
                    s.source = &other.screen;
            }
            if (!s.source) continue;
        }
        for (const auto& def : s.source->objects) {
            Object o;
            for (const auto& name : def.textures) {
                if (auto path = folder.resolve(name)) o.textures.push_back(*path);
            }
            o.area = def.area;
            o.uv = def.uv;
            o.dll_area = def.area;
            if (def.type == ScreenObjectType::arrow || def.type == ScreenObjectType::bar)
                o.frame = (std::min)(0.0f, def.range[0]);
            s.objects.push_back(std::move(o));
        }
        screens_.push_back(std::move(s));
    }
}

void CabScreens::build(const dllhost::CarOutput* state, const float elapsed, std::vector<ScreenBatch>& out,
                       const std::vector<bool>* blinked_off) {
    labels_seen_.clear();
    for (auto& screen : screens_) {
        const auto& display = cab_->displays[screen.display];
        if (blinked_off && screen.display < blinked_off->size() && (*blinked_off)[screen.display]) continue;
        // The display itself: State 0 hides it (SetDisplayState).
        const std::uint32_t display_state = state && display.id < state->displays.size() ? state->displays[display.id].state
                                                                                         : display.initial_state;
        if (display_state == 0) continue;
        // A ReferenceScreen shows the other display's objects, as they are.
        std::uint32_t source_id = display.id;
        if (display.screen.reference_screen >= 0) source_id = static_cast<std::uint32_t>(display.screen.reference_screen);
        const dllhost::ScreenOutput* live_screen = nullptr;
        if (state) {
            for (const auto& s : state->screens)
                if (s.display == source_id) live_screen = &s;
        }
        const auto& defs = screen.source->objects;
        const auto n = defs.size();
        std::vector<Live> live(n);
        for (std::size_t k = 0; k < n; ++k) {
            const auto& def = defs[k];
            auto& l = live[k];
            if (live_screen && k < live_screen->objects.size()) {
                const auto& o = live_screen->objects[k];
                l.state = o.state;
                l.value = o.value;
                l.blink = o.fade[2];
                l.area = reinterpret_cast<const std::array<float, 4>*>(o.area);
                if (k < live_screen->texts.size()) l.text = live_screen->texts[k];
            } else {
                l.state = def.initial_state;
                l.value = def.type == ScreenObjectType::arrow || def.type == ScreenObjectType::bar ||
                                  def.type == ScreenObjectType::sector
                    ? (std::min)(0.0f, def.range[0])
                    : 0.0f;
                l.blink = def.blink;
            }
        }
        // The DLL moved an object (SetScreenArea): that is its area now.
        for (std::size_t k = 0; k < n; ++k) {
            auto& o = screen.objects[k];
            if (live[k].area && *live[k].area != o.dll_area) {
                o.dll_area = *live[k].area;
                o.area = o.dll_area;
            }
        }
        // Areas: relative to the parent's (ParentSub), after the frames.
        std::vector<Rect> rects(n);
        std::vector<std::uint8_t> done(n, 0);
        auto rect_of = [&](auto&& self, std::size_t k, int depth) -> Rect {
            auto& o = screen.objects[k];
            const auto& def = defs[k];
            if (!done[k]) {
                apply_frames(def, live[k], o.area, o.uv, o.texture);
                done[k] = 1;
            }
            Rect r{o.area[0], o.area[1], o.area[2], o.area[3]};
            if (def.parent >= 0 && static_cast<std::size_t>(def.parent) < n && static_cast<std::size_t>(def.parent) != k &&
                depth < 16) {
                const Rect p = self(self, static_cast<std::size_t>(def.parent), depth + 1);
                const float w = p.x1 - p.x0, h = p.y1 - p.y0;
                r = {p.x0 + w * o.area[0], p.y0 + h * o.area[1], p.x0 + w * o.area[2], p.y0 + h * o.area[3]};
            }
            return r;
        };
        // RTS 0x52B054: hidden while any parent's State is negative.
        auto parent_hidden = [&](std::size_t k) {
            for (int depth = 0; depth < 16; ++depth) {
                const auto parent = defs[k].parent;
                if (parent < 0 || static_cast<std::size_t>(parent) >= n || static_cast<std::size_t>(parent) == k) return false;
                k = static_cast<std::size_t>(parent);
                if (live[k].state < 0) return true;
            }
            return false;
        };

        Emitter emit(out, screen.top_left, screen.top_right, screen.bottom_right);
        for (std::size_t k = 0; k < n; ++k) {
            const auto& def = defs[k];
            auto& o = screen.objects[k];
            const auto& l = live[k];
            const bool label_text = def.type == ScreenObjectType::label && !l.text.empty();
            if (label_text) labels_seen_.push_back({.display = display.id, .object = k, .text = std::u16string(l.text), .shown = !(l.state < 0 || parent_hidden(k))});
            if (l.state < 0 || parent_hidden(k)) continue;
            const Rect r = rect_of(rect_of, k, 0);
            if (o.texture >= o.textures.size()) continue;
            // SubFade blink: shown the first half of each 2 * blink seconds.
            if (l.blink > 0.0f) {
                o.blink_clock += elapsed;
                if (o.blink_clock >= l.blink * 2.0f) o.blink_clock = std::fmod(o.blink_clock, l.blink * 2.0f);
                if (o.blink_clock > l.blink) continue;
            }
            const std::uint32_t colour = (def.flags & formats::screen_has_colour) ? rgba(def.colour) : 0xFFFFFFFFu;
            emit.use(o.textures[o.texture], blended(def.shader), colour);
            const Rect t{o.uv[0], o.uv[1], o.uv[2], o.uv[3]};
            switch (def.type) {
            case ScreenObjectType::frame:
                emit.quad(r, t);
                break;
            case ScreenObjectType::arrow: {
                // The needle turns about Center by (first - value) * second /
                // (first - min) degrees, the value moving at AnimTime.
                float shown = l.value;
                if (def.anim_time != 0.0f) {
                    const float step = def.range[3] * elapsed / def.anim_time;
                    if (l.value > o.frame) o.frame = (std::min)(o.frame + step, l.value);
                    else if (l.value < o.frame) o.frame = (std::max)(o.frame - step, l.value);
                    shown = o.frame;
                }
                shown = std::clamp(shown, def.range[0], def.range[1]);
                const float span = def.range[2] - def.range[0];
                const float degrees = span != 0.0f ? (def.range[2] - shown) * def.range[3] / span : 0.0f;
                const float a = -degrees * pi / 180.0f;
                const float c = std::cos(a), s = std::sin(a);
                const glm::vec2 centre{def.center[0], def.center[1]};
                // RTS (0x46B668) turns it in the screen's metres: the corners
                // times Width and Height, then glRotatef. Turned in the 0..1
                // units, a needle on a screen wider than high was sheared
                // and its round hub went oval (the EP2D's speedometer).
                const float width = glm::length(screen.top_right - screen.top_left);
                const float height = glm::length(screen.bottom_right - screen.top_right);
                const float sx = width > 0.0f ? width : 1.0f, sy = height > 0.0f ? height : 1.0f;
                auto turn = [&](float x, float y) {
                    const glm::vec2 d{(x - centre.x) * sx, (y - centre.y) * sy};
                    return centre + glm::vec2{(d.x * c - d.y * s) / sx, (d.x * s + d.y * c) / sy};
                };
                const glm::vec2 p[4] = {turn(r.x0, r.y0), turn(r.x0, r.y1), turn(r.x1, r.y1), turn(r.x1, r.y0)};
                const glm::vec2 uv[4] = {{t.x0, t.y0}, {t.x0, t.y1}, {t.x1, t.y1}, {t.x1, t.y0}};
                emit.quad(p, uv);
                break;
            }
            case ScreenObjectType::label: {
                if (def.char_width[1] == 0.0f || def.chars_per_row == 0 || l.text.empty()) break;
                glm::vec2 scale{1.0f, 1.0f};
                if (def.parent >= 0 && static_cast<std::size_t>(def.parent) < n) {
                    const Rect p = rect_of(rect_of, static_cast<std::size_t>(def.parent), 1);
                    scale = {p.x1 - p.x0, p.y1 - p.y0};
                }
                const std::u16string_view text = l.text.substr(0, (std::min<std::size_t>)(l.text.size(), def.max_length));
                std::size_t count = 0;
                int lines = 1;
                while (count < text.size() && text[count]) {
                    if (text[count] == u'\n') ++lines;
                    ++count;
                }
                if (count == 0) break;
                if (!labels_seen_.empty() && labels_seen_.back().object == k) {
                    labels_seen_.back().chars = count;
                    for (std::size_t i = 0; i < count; ++i)
                        if (text[i] != u'\n' && text[i] != u'\r' && def.alphabet.find(static_cast<char32_t>(text[i])) != std::u32string::npos)
                            ++labels_seen_.back().drawn;
                }
                const float cw = def.char_width[0], ch = def.char_height[0];
                const float sp = def.char_spacing[0], ls = def.line_spacing[0];
                float y = r.y0;
                if (def.v_align == 1) {
                    y = (r.y0 + r.y1) * 0.5f;
                    if (lines & 1) y -= ch * scale.y * 0.5f + static_cast<float>((lines - 1) / 2) * (ch + ls) * scale.y;
                    else y -= static_cast<float>(lines / 2) * (ch + ls) * scale.y;
                } else if (def.v_align == 4) {
                    y = r.y1 - (ch + ls) * scale.y - static_cast<float>(lines - 1) * (ch + ls) * scale.y;
                }
                float x = r.x0;
                for (std::size_t i = 0; i < count; ++i) {
                    const char16_t c = text[i];
                    const bool breaks = c == u'\n' || (c == u'\r' && i + 1 < count && text[i + 1] == u'\n');
                    if (i == 0 || breaks) {
                        const int chars = line_length(text.substr(0, count), i);
                        x = r.x0;
                        if (def.h_align == 1) {
                            x = (r.x0 + r.x1) * 0.5f;
                            if (chars & 1) x -= cw * scale.x * 0.5f + static_cast<float>((chars - 1) / 2) * (cw + sp) * scale.x;
                            else x -= static_cast<float>(chars / 2) * (cw + sp) * scale.x;
                        } else if (def.h_align == 2) {
                            x = r.x1 - (cw + sp) * scale.x - static_cast<float>(chars - 1) * (cw + sp) * scale.x;
                        }
                        if (c == u'\r' || c == u'\n') {
                            y += ch + ls; // RTS does not scale the line step
                            if (c == u'\r') ++i;
                            continue;
                        }
                    }
                    const auto found = def.alphabet.find(static_cast<char32_t>(c));
                    if (found != std::u32string::npos) {
                        const auto index = static_cast<std::uint32_t>(found);
                        const float u = static_cast<float>(index % def.chars_per_row) * (def.char_width[1] + def.char_spacing[1]) + t.x0;
                        const float v = static_cast<float>(index / def.chars_per_row) * (def.char_height[1] + def.line_spacing[1]) + t.y0;
                        emit.quad(Rect{x, y, x + cw * scale.x, y + ch * scale.y}, Rect{u, v, u + def.char_width[1], v + def.char_height[1]});
                    }
                    x += (cw + sp) * scale.x;
                }
                break;
            }
            case ScreenObjectType::bar: {
                float shown = l.value;
                if (def.anim_time != 0.0f) {
                    const float step = elapsed / def.anim_time;
                    if (l.value > o.frame) o.frame = (std::min)(o.frame + step, l.value);
                    else if (l.value < o.frame) o.frame = (std::max)(o.frame - step, l.value);
                    shown = o.frame;
                }
                const float f = std::clamp((shown - def.range[0]) / (def.range[1] - def.range[0]), 0.0f, 1.0f);
                Rect b = r, u = t;
                const bool stretch = (def.flags & formats::cab_stretch_uv) != 0;
                switch (def.bar_style) {
                case 1: // H_LEFT: grows leftwards from the right edge
                    b.x0 = b.x1 - (b.x1 - b.x0) * f;
                    if (!stretch) u.x0 = u.x1 - (u.x1 - u.x0) * f;
                    break;
                case 2: // V_UP
                    b.y0 = b.y1 - (b.y1 - b.y0) * f;
                    if (!stretch) u.y0 = u.y1 - (u.y1 - u.y0) * f;
                    break;
                case 3: // V_DOWN
                    b.y1 = b.y0 + (b.y1 - b.y0) * f;
                    if (!stretch) u.y1 = u.y0 + (u.y1 - u.y0) * f;
                    break;
                case 4: // H_RIGHT
                    b.x1 = b.x0 + (b.x1 - b.x0) * f;
                    if (!stretch) u.x1 = u.x0 + (u.x1 - u.x0) * f;
                    break;
                default:
                    break;
                }
                emit.quad(b, u);
                break;
            }
            case ScreenObjectType::grid: {
                // As RTS lays it out: cells of the area by the texture's
                // counts, texture cells by the shown counts; State's bits.
                const int shown_cols = static_cast<int>(def.grid_columns[0]), tex_cols = static_cast<int>(def.grid_columns[1]);
                const int shown_rows = static_cast<int>(def.grid_rows[0]), tex_rows = static_cast<int>(def.grid_rows[1]);
                if (shown_cols <= 0 || tex_cols <= 0 || shown_rows <= 0 || tex_rows <= 0) break;
                const float cell_w = (r.x1 - r.x0) / static_cast<float>(tex_cols);
                const float cell_h = (r.y1 - r.y0) / static_cast<float>(tex_rows);
                const float uv_w = (t.x1 - t.x0) / static_cast<float>(shown_cols);
                const float uv_h = (t.y1 - t.y0) / static_cast<float>(shown_rows);
                int bit = 0;
                for (int row = 0; row < shown_rows; ++row) {
                    for (int col = 0; col < shown_cols; ++col, ++bit) {
                        if (bit >= 32 || !(static_cast<std::uint32_t>(l.state) & (1u << bit))) continue;
                        const float u0 = static_cast<float>(bit % tex_cols) * uv_w + t.x0;
                        const float v0 = static_cast<float>(bit / tex_cols) * uv_h + t.y0;
                        const float x0 = static_cast<float>(col) * cell_w + r.x0;
                        const float y0 = static_cast<float>(row) * cell_h + r.y0;
                        emit.quad(Rect{x0, y0, x0 + cell_w, y0 + cell_h}, Rect{u0, v0, u0 + uv_w, v0 + uv_h});
                    }
                }
                break;
            }
            case ScreenObjectType::sector: {
                // A fan from the centre over the angles the value reaches,
                // in steps of SectorGranularity (5 degrees without it).
                const float lo = def.range[2], hi = def.range[3];
                const float span = def.range[1] - def.range[0];
                if (!(l.value > def.range[0] || def.sector_fill != 1) || span == 0.0f) break;
                const float step = def.sector_granularity > 0.0f ? def.sector_granularity : 5.0f;
                float first = 0.0f, last = 0.0f; // from `last` down to `first`
                if (def.sector_fill == 1) {
                    first = lo;
                    float sweep = (hi - lo) * (l.value - def.range[0]) / span;
                    if (def.sector_granularity > 0.0f) sweep = std::ceil(sweep / def.sector_granularity) * def.sector_granularity;
                    last = (std::min)(lo + sweep, hi);
                } else if (def.sector_fill == 2) {
                    last = hi;
                    float sweep = (hi - lo) * (l.value - def.range[0]) / span;
                    if (def.sector_granularity > 0.0f) sweep = std::floor(sweep / def.sector_granularity) * def.sector_granularity;
                    first = (std::max)(hi - sweep, lo);
                } else {
                    break;
                }
                if (last < first) break;
                std::vector<glm::vec2> outline;
                for (float a = last;; a -= step) {
                    const float clamped = (std::max)(a, first);
                    outline.push_back(sector_point(def.sector_shape, clamped));
                    if (clamped <= first || outline.size() > 4096) break;
                }
                const float w = r.x1 - r.x0, h = r.y1 - r.y0, uw = t.x1 - t.x0, uh = t.y1 - t.y0;
                auto put = [&](glm::vec2 p) { emit.vertex(r.x0 + p.x * w, r.y0 + p.y * h, t.x0 + p.x * uw, t.y0 + p.y * uh); };
                for (std::size_t i = 0; i + 1 < outline.size(); ++i) {
                    put({0.5f, 0.5f});
                    put(outline[i]);
                    put(outline[i + 1]);
                }
                break;
            }
            case ScreenObjectType::link:
                break;
            }
            if (!out.empty() && out.back().vertices.empty()) out.pop_back();
        }
    }
}

} // namespace openrts::viewer
