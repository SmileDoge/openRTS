#include <openrts/formats/cab_description.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <cstdlib>
#include <optional>

#include "text_tree.hpp"
#include "text_tokenizer.hpp"

namespace openrts::formats {
namespace {

using detail::TextNode;

// cp1251 0x80..0xFF to Unicode (the ANSI code page of RTS content).
constexpr char16_t cp1251[128] = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, 0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C,
    0x040B, 0x040F, 0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x0098, 0x2122, 0x0459, 0x203A,
    0x045A, 0x045C, 0x045B, 0x045F, 0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7, 0x0401, 0x00A9,
    0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407, 0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7,
    0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457, 0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415,
    0x0416, 0x0417, 0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F, 0x0420, 0x0421, 0x0422, 0x0423,
    0x0424, 0x0425, 0x0426, 0x0427, 0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F, 0x0430, 0x0431,
    0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437, 0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F,
    0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447, 0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D,
    0x044E, 0x044F,
};

std::string ansi_to_utf8(std::string_view bytes) {
    std::string out;
    out.reserve(bytes.size());
    for (const char ch : bytes) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x80) {
            out.push_back(ch);
            continue;
        }
        const char16_t u = cp1251[c - 0x80];
        if (u < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (u >> 6)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (u >> 12)));
            out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        }
    }
    return out;
}

std::string lower(std::string_view s) {
    std::string r(s);
    for (auto& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

std::uint32_t to_uint(const std::string& s) { return static_cast<std::uint32_t>(std::strtoul(s.c_str(), nullptr, 10)); }
float to_float(const std::string& s) { return std::strtof(s.c_str(), nullptr); }

float number(const TextNode* node, std::size_t index, float fallback = 0.0f) {
    if (!node || index >= node->values.size()) return fallback;
    return to_float(node->values[index]);
}

std::string text(const TextNode* node, std::size_t index = 0) {
    if (!node || index >= node->values.size()) return {};
    return node->values[index];
}

std::array<float, 3> vec3(const TextNode* node) {
    return {number(node, 0), number(node, 1), number(node, 2)};
}

std::string joined(const TextNode* node) {
    std::string r;
    if (!node) return r;
    for (const auto& v : node->values) {
        if (!r.empty()) r += ' ';
        r += v;
    }
    return r;
}

// Finds `key` anywhere below `node` (the first in document order).
const TextNode* find(const TextNode& node, std::string_view key) {
    for (const auto& child : node.children) {
        if (child.name == key) return &child;
        if (const auto* deeper = find(child, key)) return deeper;
    }
    return nullptr;
}

CabSwitch read_switch(const TextNode& node) {
    CabSwitch s;
    s.id = static_cast<std::uint32_t>(number(node.child("id"), 0));
    s.name = text(node.child("name"));
    s.hint = text(node.child("hint"));
    if (const auto* hints = node.child("statehints")) {
        // StateHints ( n "a" "b" ... ): the count first.
        for (std::size_t i = 1; i < hints->values.size(); ++i) s.state_hints.push_back(hints->values[i]);
    }
    s.position = vec3(node.child("position"));
    s.radius = number(node.child("radius"), 0);
    if (const auto* parent = node.child("parentobject")) s.parent = static_cast<std::int32_t>(number(parent, 0, -1.0f));
    if (const auto* states = node.child("states")) {
        s.num_states = static_cast<std::uint32_t>((std::max)(1.0f, number(states, 0, 2)));
        s.initial_state = static_cast<std::uint32_t>(number(states, 1, 0));
        if (s.initial_state >= s.num_states) s.initial_state = 0;
    }
    s.flags = cab_flags(joined(node.child("flags")));
    s.fixed_state = static_cast<std::uint32_t>(number(node.child("fixedstate"), 0));
    if (const auto* keys = node.child("assignkey")) s.keys = keys->values;
    s.anim_time = number(node.child("animtime"), 0);
    s.precision = {number(node.child("switchprecision"), 0), number(node.child("switchprecision"), 1)};
    return s;
}

// UTF-8 to code points (alphabets of screen labels).
std::u32string utf32(std::string_view s) {
    std::u32string r;
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        char32_t cp = c;
        std::size_t n = 1;
        if (c >= 0xF0 && i + 3 < s.size()) {
            cp = (c & 0x07u) << 18 | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12 |
                 (static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6 | (static_cast<unsigned char>(s[i + 3]) & 0x3Fu);
            n = 4;
        } else if (c >= 0xE0 && i + 2 < s.size()) {
            cp = (c & 0x0Fu) << 12 | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6 |
                 (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
            n = 3;
        } else if (c >= 0xC0 && i + 1 < s.size()) {
            cp = (c & 0x1Fu) << 6 | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
            n = 2;
        }
        r.push_back(cp);
        i += n;
    }
    return r;
}

std::array<float, 4> vec4(const TextNode* node) {
    return {number(node, 0), number(node, 1), number(node, 2), number(node, 3)};
}

std::uint32_t named_shader(std::string_view name) {
    static constexpr std::string_view names[]{"tex", "texdiff", "blendatex", "blendatexdiff", "gloss", "addatex", "addatexdiff"};
    const auto l = lower(name);
    for (std::uint32_t i = 0; i < std::size(names); ++i)
        if (l == names[i]) return i + 1;
    return 0;
}

std::uint16_t screen_alignment(std::string_view word) {
    const auto l = lower(word);
    return l == "center" ? 1 : l == "right" ? 2 : l == "justified" ? 3 : l == "bottom" ? 4 : l == "top" ? 5 : 0;
}

// RTS 0x519B8C.
ScreenObject read_screen_object(const TextNode& node) {
    ScreenObject o;
    const auto type = lower(text(node.child("type")));
    o.type = type == "arrow"    ? ScreenObjectType::arrow
        : type == "link"        ? ScreenObjectType::link
        : type == "label"       ? ScreenObjectType::label
        : type == "bar"         ? ScreenObjectType::bar
        : type == "grid"        ? ScreenObjectType::grid
        : type == "sector"      ? ScreenObjectType::sector
                                : ScreenObjectType::frame;
    if (const auto* p = node.child("parentsub")) o.parent = static_cast<std::int32_t>(number(p, 0));
    o.hint = text(node.child("hint"));
    if (const auto* a = node.child("area")) {
        o.flags |= screen_has_area;
        o.area = vec4(a);
    }
    if (const auto* uv = node.child("uv")) {
        o.flags |= screen_has_uv;
        o.uv = vec4(uv);
    }
    if (const auto* a = node.child("activation")) {
        o.flags |= screen_has_activation;
        o.activation = vec4(a);
    }
    if (const auto* t = node.child("textures")) {
        const auto n = static_cast<std::size_t>(number(t, 0));
        for (std::size_t i = 1; i <= n && i < t->values.size(); ++i) o.textures.push_back(t->values[i]);
    }
    o.shader = named_shader(text(node.child("namedshader")));
    o.initial_state = static_cast<std::int32_t>(number(node.child("initialstate"), 0));
    if (const auto* f = node.child("flags")) {
        const auto words = lower(joined(f));
        o.flags |= cab_flags(words) & 0xFFFFFF1Fu;
        if (words.find("controllery") != std::string::npos) o.flags |= screen_controller_y;
        if (words.find("precise") != std::string::npos) o.flags |= screen_precise;
    }
    o.anim_flags = static_cast<std::uint32_t>(number(node.child("animflags"), 0));
    o.anim_time = number(node.child("animtime"), 0);
    if (const auto* f = node.child("subfade")) {
        o.fade = (std::max)(number(f, 0), 0.0f);
        o.blink = (std::max)(number(f, 1), 0.0f);
    }
    if (const auto* c = node.child("subcolour")) {
        o.colour = static_cast<std::uint32_t>(std::strtoul(text(c).c_str(), nullptr, 16));
        o.flags |= screen_has_colour;
    }
    const auto* range = node.child("valuesrange");
    auto read_range = [&] {
        if (range) o.range = vec4(range);
        if (o.range[0] >= o.range[1]) o.range[1] = o.range[0] + 1.0f;
    };
    switch (o.type) {
    case ScreenObjectType::arrow:
        if (const auto* c = node.child("center")) o.center = {number(c, 0), number(c, 1)};
        read_range();
        break;
    case ScreenObjectType::link:
        if (const auto* l = node.child("linkto")) o.link = static_cast<std::int32_t>(number(l, 0));
        break;
    case ScreenObjectType::label: {
        auto pair = [&](const char* key, std::array<float, 2>& to) {
            if (const auto* v = node.child(key)) to = {number(v, 0), number(v, 1)};
        };
        pair("characterwidth", o.char_width);
        pair("characterheight", o.char_height);
        pair("characterspacing", o.char_spacing);
        pair("linespacing", o.line_spacing);
        if (o.char_height[1] <= 0.0f) o.char_width[1] = 0.0f;
        if (const auto* a = node.child("alignment")) {
            o.h_align = screen_alignment(text(a, 0));
            o.v_align = screen_alignment(text(a, 1));
        }
        if (o.char_width[1] > 0.0f) {
            // A texture area given right to left / bottom to top runs the font backwards.
            if (o.uv[2] < o.uv[0]) o.char_width[1] = -o.char_width[1];
            if (o.uv[3] < o.uv[1]) o.char_height[1] = -o.char_height[1];
            const float step = o.char_width[1] + o.char_spacing[1];
            const float per_row = step != 0.0f ? std::floor((o.uv[2] - o.uv[0]) / step) : 0.0f;
            o.chars_per_row = per_row > 0.0f && per_row < 65536.0f ? static_cast<std::uint16_t>(per_row) : 0;
            if (o.chars_per_row == 0) o.char_width[1] = 0.0f;
            o.alphabet = utf32(text(node.child("alphabet")));
            const auto length = static_cast<std::uint32_t>(number(node.child("maxstringlength"), 0));
            if (length > 0 && length < 0x2000) o.max_length = static_cast<std::uint16_t>(length);
        }
        break;
    }
    case ScreenObjectType::bar: {
        const auto style = lower(text(node.child("barstyle")));
        o.bar_style = style == "h_left" ? 1 : style == "h_right" ? 4 : style == "v_up" ? 2 : style == "v_down" ? 3 : 0;
        read_range();
        break;
    }
    case ScreenObjectType::grid:
        if (const auto* r = node.child("gridnumrows"))
            o.grid_rows = {static_cast<std::uint32_t>(number(r, 0)), static_cast<std::uint32_t>(number(r, 1))};
        if (const auto* c = node.child("gridnumcols"))
            o.grid_columns = {static_cast<std::uint32_t>(number(c, 0)), static_cast<std::uint32_t>(number(c, 1))};
        break;
    case ScreenObjectType::sector: {
        const auto shape = lower(text(node.child("sectorshape")));
        o.sector_shape = shape == "rect" ? 1 : 0;
        const auto fill = lower(text(node.child("sectorfilltype")));
        o.sector_fill = fill == "startpos" ? 1 : fill == "endpos" ? 2 : 0;
        o.sector_granularity = number(node.child("sectorgranularity"), 0);
        read_range();
        break;
    }
    case ScreenObjectType::frame:
        break;
    }
    if (const auto* frames = node.child("animationframes")) {
        for (const auto& f : frames->children) {
            if (f.name != "frame") continue;
            ScreenFrame frame;
            if (const auto* a = f.child("area")) {
                frame.has |= 1;
                frame.area = vec4(a);
            }
            if (const auto* a = f.child("activation")) {
                frame.has |= 2;
                frame.activation = vec4(a);
            }
            if (const auto* uv = f.child("uv")) {
                frame.has |= 4;
                frame.uv = vec4(uv);
            }
            if (const auto* t = f.child("texid")) {
                frame.has |= 8;
                frame.texture = static_cast<std::uint32_t>(number(t, 0));
            }
            o.frames.push_back(frame);
        }
    }
    if (o.frames.size() > 2) {
        if (const auto* scale = node.child("animtimescale")) {
            for (std::size_t i = 0; i + 1 < o.frames.size(); ++i) o.anim_time_scale.push_back((std::max)(number(scale, i), 0.0f));
        }
    }
    return o;
}

CabScreen read_screen(const TextNode& node) {
    CabScreen s;
    const auto* tl = node.child("topleft");
    const auto* tr = node.child("topright");
    const auto* br = node.child("bottomright");
    if (!tl || !tr || !br) return s;
    s.top_left = vec3(tl);
    s.top_right = vec3(tr);
    s.bottom_right = vec3(br);
    s.present = true;
    if (const auto* r = node.child("referencescreen")) {
        s.reference_screen = static_cast<std::int32_t>(number(r, 0));
        return s;
    }
    if (const auto* subs = node.child("subobjects")) {
        const auto declared = static_cast<std::size_t>(number(subs, 0));
        for (const auto& c : subs->children) {
            if (c.name != "subobject") continue;
            if (s.objects.size() >= declared) break;
            s.objects.push_back(read_screen_object(c));
        }
    }
    return s;
}

CabDisplay read_display(const TextNode& node) {
    CabDisplay d;
    d.id = static_cast<std::uint32_t>(number(node.child("id"), 0));
    d.name = text(node.child("name"));
    d.hint = text(node.child("hint"));
    d.position = vec3(node.child("position"));
    d.radius = number(node.child("radius"), 0);
    const auto type = lower(text(node.child("displaytype")));
    d.type = type == "arrow" ? CabDisplayType::arrow
        : type == "display"  ? CabDisplayType::display
        : type == "screen"   ? CabDisplayType::screen
                             : CabDisplayType::lamp;
    d.flags = cab_flags(joined(node.child("flags")));
    switch (d.type) {
    case CabDisplayType::arrow: d.flags |= cab_display_arrow; break;
    case CabDisplayType::lamp: d.flags |= cab_display_lamp; break;
    case CabDisplayType::display: d.flags |= cab_display_display; break;
    case CabDisplayType::screen: d.flags |= cab_display_screen; break;
    }
    if (const auto* range = node.child("valuesrange")) {
        for (std::size_t i = 0; i < 4; ++i) d.range[i] = number(range, i);
    }
    d.granularity = number(node.child("granularity"), 0);
    d.initial_state = static_cast<std::uint32_t>(number(node.child("initialstate"), 0));
    d.anim_speed = number(node.child("animspeed"), 0);
    if (const auto* fade = node.child("displayfade")) {
        d.fade = (std::max)(number(fade, 0), 0.0f);
        d.blink = number(fade, 1);
    }
    if (const auto* nodes = node.child("statenodes")) {
        // ( n "a" "b" ... ): the count first, then the names (fewer when the list runs short).
        const auto count = static_cast<std::size_t>((std::max)(number(nodes, 0), 0.0f));
        for (std::size_t k = 1; k < nodes->values.size() && d.state_nodes.size() < count; ++k) d.state_nodes.push_back(nodes->values[k]);
    }
    if (const auto* screen = node.child("displayscreen")) {
        if (const auto* subs = screen->child("subobjects")) {
            for (const auto& c : subs->children)
                if (c.name == "subobject") ++d.screen_subobjects;
        }
        if (d.type == CabDisplayType::screen) d.screen = read_screen(*screen);
    }
    return d;
}

} // namespace

std::uint32_t cab_flags(std::string_view keywords) {
    std::uint32_t flags = 0;
    std::size_t at = 0;
    while (at < keywords.size()) {
        while (at < keywords.size() && std::isspace(static_cast<unsigned char>(keywords[at]))) ++at;
        std::size_t end = at;
        while (end < keywords.size() && !std::isspace(static_cast<unsigned char>(keywords[end]))) ++end;
        const auto word = lower(keywords.substr(at, end - at));
        at = end;
        if (word == "controllerx") flags |= cab_controller_x;
        else if (word == "key") flags |= cab_key;
        else if (word == "nonfixed") flags |= cab_non_fixed;
        else if (word == "controllery") flags |= cab_controller_y;
        else if (word == "precise") flags |= cab_precise;
        else if (word == "overscale") flags |= cab_overscale;
        else if (word == "loop") flags |= cab_loop;
        else if (word == "disabled") flags |= cab_disabled;
        else if (word == "checkdistanceonpress" || word == "displaysequence") flags |= cab_check_distance;
        else if (word == "stretch_uv") flags |= cab_stretch_uv;
    }
    return flags;
}

const CabSwitch* CabDescription::find_switch(std::uint32_t id) const {
    for (const auto& s : switches)
        if (s.id == id) return &s;
    return nullptr;
}

std::uint32_t CabDescription::root_switch(std::uint32_t id) const {
    for (int guard = 0; guard < 16; ++guard) {
        const auto* s = find_switch(id);
        if (!s || s->parent < 0 || static_cast<std::uint32_t>(s->parent) == id) break;
        id = static_cast<std::uint32_t>(s->parent);
    }
    return id;
}

const CabSwitch* CabDescription::find_switch(std::string_view name) const {
    for (const auto& s : switches)
        if (lower(s.name) == lower(name)) return &s;
    return nullptr;
}

namespace {

std::optional<std::string> decode(std::string_view bytes, std::string& error) {
    const bool utf16 = bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF
        && static_cast<unsigned char>(bytes[1]) == 0xFE;
    auto decoded = detail::decode_text(bytes, error);
    if (!decoded) return std::nullopt;
    return utf16 ? std::move(*decoded) : ansi_to_utf8(*decoded);
}

CabAnimation read_animation(const TextNode& node) {
    CabAnimation a;
    a.name = text(node.child("name"));
    a.anim_time = number(node.child("animtime"), 0);
    a.key_frames = static_cast<std::uint32_t>(number(node.child("numkeyframes"), 0));
    a.flags = joined(node.child("flags"));
    if (const auto* nodes = node.child("childnodes"))
        for (std::size_t i = 1; i < nodes->values.size(); ++i) a.nodes.push_back(nodes->values[i]);
    return a;
}

} // namespace

// `node`'s child `key`, else the first one nested deeper (breadth first), as
// RTS finds a block: its cab loader (0x51CD88) looks each keyword up in the
// parent block's text (0x4578C0), at any depth, and counts only ( and )
// (the pair at 0x5F9BA6) to find where the block ends. So a block left open
// or closed with a } does not hide the ones after it: the VL80T's and
// EP1M's .sd close Animations with } (26 cab files of MaDaDev and rz do),
// the 2TE116's cabEDTKLUB.sd leaves InteriorDoors open over
// InteriorWindows and InteriorChairs, the draisine's drezina_cab2.sd
// LocomotiveSignalization over InteriorGeometryInfo.
const TextNode* child_or_nested(const TextNode& node, const std::string_view key) {
    if (const auto* direct = node.child(key)) return direct;
    std::vector<const TextNode*> queue{&node};
    for (std::size_t i = 0; i < queue.size(); ++i) {
        for (const auto& child : queue[i]->children) {
            if (child.name == key) return &child;
            queue.push_back(&child);
        }
    }
    return nullptr;
}

ExteriorDetails parse_exterior_details(std::string_view bytes) {
    ExteriorDetails out;
    std::string error;
    const auto source = decode(bytes, error);
    if (!source) return out;
    TextNode root;
    detail::read_text_tree(*source, root);
    const TextNode* exterior = find(root, "esd_exterior_details");
    if (!exterior) return out;
    if (const auto* animations = exterior->child("animations"))
        for (const auto& node : animations->children)
            if (node.name == "animnode") out.animations.push_back(read_animation(node));
    if (const auto* doors = exterior->child("exteriordoors"))
        for (const auto& node : doors->children)
            if (node.name == "exteriordoor") {
                ExteriorDoor door;
                door.name = text(node.child("name"));
                door.position = vec3(node.child("position"));
                door.radius = number(node.child("radius"), 0, 2.0f);
                door.anim_time = number(node.child("animtime"), 0, 1.0f);
                door.global_id = static_cast<std::uint32_t>(number(node.child("globalid"), 0, 0.0f));
                door.can_be_opened = number(node.child("canbeopened"), 0, 1.0f) != 0.0f;
                out.doors.push_back(door.name);
                out.door_details.push_back(std::move(door));
            }
    if (const auto* objects = exterior->child("wagonobjects")) {
        for (const auto& node : objects->children) {
            WagonObject object;
            object.kind = node.name;
            // ChildNodes ( n name... ), StateKeyFrames ( n frame... ): the count first.
            if (const auto* children = node.child("childnodes")) {
                for (std::size_t k = 1; k < children->values.size(); ++k) object.nodes.push_back(children->values[k]);
            }
            object.num_states = static_cast<std::uint32_t>(number(node.child("numstates"), 0, 2.0f));
            object.first_state_invisible = number(node.child("firststateinvisible"), 0, 0.0f) != 0.0f;
            if (const auto* frames = node.child("statekeyframes")) {
                for (std::size_t k = 1; k < frames->values.size(); ++k) object.state_key_frames.push_back(static_cast<float>(frames->number(k)));
            }
            object.anim_time = number(node.child("animtime"), 0, 0.0f);
            out.wagon_objects.push_back(std::move(object));
        }
    }
    return out;
}

Repaint parse_repaint(std::string_view bytes) {
    Repaint out;
    std::string error;
    const auto source = decode(bytes, error);
    if (!source) return out;
    TextNode root;
    detail::read_text_tree(*source, root);
    const TextNode* repaint = find(root, "repaint");
    if (!repaint) return out;
    out.shape = text(repaint->child("shape"));
    out.description = text(repaint->child("shapedescription"));
    for (const auto& node : repaint->children)
        if (node.name == "texturereplace" && node.values.size() >= 2) out.textures.emplace_back(node.values[0], node.values[1]);
    return out;
}

core::VirtualPath shape_description(const core::Vfs& vfs, const core::VirtualPath& shape) {
    if (core::fold_case(shape.extension()) != ".repaint") return shape.with_extension(".sd");
    if (const auto text = vfs.read(shape)) {
        const auto repaint = parse_repaint(*text);
        const auto& name = !repaint.description.empty() ? repaint.description : repaint.shape;
        if (const auto target = shape.parent().resolve(name)) {
            return repaint.description.empty() ? target->with_extension(".sd") : *target;
        }
    }
    return shape.with_extension(".sd");
}

CabDescription parse_cab_description(std::string_view bytes) {
    CabDescription cab;
    std::string error;
    const bool utf16 = bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF
        && static_cast<unsigned char>(bytes[1]) == 0xFE;
    auto decoded = detail::decode_text(bytes, error);
    if (!decoded) {
        cab.error = error;
        return cab;
    }
    const std::string source = utf16 ? std::move(*decoded) : ansi_to_utf8(*decoded);
    TextNode root;
    detail::read_text_tree(source, root);
    const TextNode* interior = find(root, "esd_interior");
    if (!interior) {
        cab.error = "no ESD_Interior block";
        return cab;
    }
    if (const auto* extended = find(root, "esd_extended_animation")) {
        for (const auto& node : extended->children) {
            if (node.name != "animationnode") continue;
            CabAnimationNode animation{.name = text(node.child("name"))};
            if (animation.name.empty()) continue;
            for (const auto& shortcut : node.children) {
                if (shortcut.name != "shortcut") continue;
                // Shortcut ( n f1 .. fn ): a set of n frames (none: skipped).
                const auto n = static_cast<std::size_t>(number(&shortcut, 0, 0.0f));
                std::vector<std::uint16_t> frames;
                for (std::size_t k = 0; k < n && k + 1 < shortcut.values.size(); ++k) {
                    frames.push_back(static_cast<std::uint16_t>(number(&shortcut, k + 1, 0.0f)));
                }
                if (frames.size() < 2) continue;
                std::sort(frames.begin(), frames.end());
                animation.shortcuts.push_back(std::move(frames));
            }
            if (!animation.shortcuts.empty()) cab.animation_nodes.push_back(std::move(animation));
        }
    }
    if (const auto* switches = interior->child("switches")) {
        cab.switch_count = static_cast<std::uint32_t>(number(switches, 0, 0));
        for (const auto& node : switches->children)
            if (node.name == "switch") cab.switches.push_back(read_switch(node));
    }
    if (const auto* displays = interior->child("displays")) {
        cab.display_count = static_cast<std::uint32_t>(number(displays, 0, 0));
        for (const auto& node : displays->children)
            if (node.name == "display") cab.displays.push_back(read_display(node));
    }
    if (const auto* lights = interior->child("lightsources")) {
        for (const auto& node : lights->children) {
            if (node.name != "lightsource") continue;
            CabLight l;
            l.name = text(node.child("name"));
            l.position = vec3(node.child("position"));
            l.points_to = vec3(node.child("pointsto"));
            l.radius = {number(node.child("radius"), 0), number(node.child("radius"), 1)};
            l.spot_exponent = number(node.child("spotexponent"), 0);
            l.colour = static_cast<std::uint32_t>(std::strtoul(text(node.child("colour")).c_str(), nullptr, 16));
            l.fade = {number(node.child("lightfade"), 0), number(node.child("lightfade"), 1)};
            l.flags = joined(node.child("flags"));
            l.global_id = static_cast<std::uint32_t>(number(node.child("globalid"), 0, 0.0f));
            l.source_radius = number(node.child("sourceradius"), 0, 0.0f);
            l.auto_when_world_lit = number(node.child("autowhenworldlit"), 0, 0.0f);
            if (const auto* a = node.child("attenuation")) {
                const float start = number(a, 0), length = number(a, 1), kind = number(a, 2);
                if (start >= 0.0f && length > 0.0f) l.attenuation = {kind, start, length};
            }
            cab.lights.push_back(std::move(l));
        }
    }
    if (const auto* changes = interior->child("texturechanges")) {
        for (const auto& node : changes->children) {
            if (node.name != "change") continue;
            CabTextureChange c;
            c.texture = static_cast<std::uint32_t>(number(&node, 0));
            c.file = text(&node, 1);
            c.values = {static_cast<std::uint32_t>(number(&node, 2)), static_cast<std::uint32_t>(number(&node, 3))};
            cab.texture_changes.push_back(std::move(c));
        }
    }
    if (const auto* animations = interior->child("animations")) {
        for (const auto& node : animations->children) {
            if (node.name != "animnode") continue;
            CabAnimation a;
            a.name = text(node.child("name"));
            a.anim_time = number(node.child("animtime"), 0);
            a.key_frames = static_cast<std::uint32_t>(number(node.child("numkeyframes"), 0));
            a.flags = joined(node.child("flags"));
            if (const auto* nodes = node.child("childnodes"))
                for (std::size_t i = 1; i < nodes->values.size(); ++i) a.nodes.push_back(nodes->values[i]);
            cab.animations.push_back(std::move(a));
        }
    }
    if (const auto* signals = interior->child("locomotivesignalization")) {
        auto& s = cab.signalization;
        s.present = true;
        s.block_check_ahead = static_cast<std::uint32_t>(number(signals->child("blockcheckahead"), 0));
        if (const auto* lights = signals->child("signallights"))
            for (const auto& node : lights->children)
                if (node.name == "light") s.lights.push_back(text(&node));
        if (const auto* aspects = signals->child("signalaspects")) {
            for (const auto& node : aspects->children) {
                if (node.name != "aspect") continue;
                // Aspect ( n l1 .. ln ): the count, then the lit lights (RTS 0x523F94).
                std::vector<std::uint32_t> lit;
                const std::size_t count = node.values.empty() ? 0 : to_uint(node.values[0]);
                for (std::size_t k = 1; k <= count && k < node.values.size(); ++k) lit.push_back(to_uint(node.values[k]));
                s.aspects.push_back(std::move(lit));
            }
        }
        s.scripted = number(signals->child("signalizationscripted"), 0) != 0.0f;
    }
    if (const auto* geometry = child_or_nested(*interior, "interiorgeometryinfo")) {
        // InteriorPlane ( [name] x1 y1 z1 x2 y2 z2 ): the name is there when
        // there are seven values.
        auto plane = [](const TextNode* node) {
            CabPlane p;
            if (!node) return p;
            const std::size_t first = node->values.size() >= 7 ? 1 : 0;
            if (first) p.name = node->values[0];
            for (std::size_t i = 0; i < 3; ++i) {
                p.a[i] = number(node, first + i);
                p.b[i] = number(node, first + 3 + i);
            }
            return p;
        };
        if (const auto* areas = child_or_nested(*geometry, "interiorareas")) {
            for (const auto& node : areas->children) {
                if (node.name != "interiorarea") continue;
                CabArea a;
                a.name = text(&node);
                a.x1 = number(&node, 1);
                a.z1 = number(&node, 2);
                a.x2 = number(&node, 3);
                a.z2 = number(&node, 4);
                if (const auto* nodes = node.child("childnodes"))
                    for (std::size_t i = 1; i < nodes->values.size(); ++i) a.nodes.push_back(nodes->values[i]);
                cab.areas.push_back(std::move(a));
            }
        }
        if (const auto* nodes = child_or_nested(*geometry, "exteriornodes"))
            for (std::size_t i = 1; i < nodes->values.size(); ++i) cab.exterior_nodes.push_back(nodes->values[i]);
        if (const auto* planes = child_or_nested(*geometry, "interiorplanes"))
            for (const auto& node : planes->children)
                if (node.name == "interiorplane") cab.planes.push_back(plane(&node));
        if (const auto* doors = child_or_nested(*geometry, "interiordoors")) {
            for (const auto& node : doors->children) {
                if (node.name != "interiordoor") continue;
                ++cab.interior_doors;
                CabDoor d;
                d.name = text(node.child("name"));
                d.from = text(node.child("connects"), 0);
                d.to = text(node.child("connects"), 1);
                d.has_position = node.child("position") != nullptr;
                d.position = vec3(node.child("position"));
                d.radius = number(node.child("radius"), 0, 1.5f);
                d.plane = plane(node.child("interiorplane"));
                d.anim_time = number(node.child("animtime"), 0, 1.0f);
                d.global_id = static_cast<std::uint32_t>(number(node.child("globalid"), 0, 0.0f));
                d.can_be_opened = number(node.child("canbeopened"), 0, 1.0f) != 0.0f;
                d.opened = node.child("opened") != nullptr;
                if (const auto* proof = node.child("soundproof")) {
                    d.sound_closed = number(proof, 0);
                    d.sound_open = number(proof, 1);
                }
                if (const auto* sms = node.child("smstriggers")) {
                    d.sms_triggers = {static_cast<std::uint16_t>(number(sms, 0)), static_cast<std::uint16_t>(number(sms, 1))};
                }
                cab.doors.push_back(std::move(d));
            }
        }
        if (const auto* windows = child_or_nested(*geometry, "interiorwindows")) {
            for (const auto& node : windows->children) {
                if (node.name != "interiorwindow") continue;
                CabWindow w;
                w.name = text(node.child("name"));
                w.can_open = number(node.child("canbeopened"), 0) != 0.0f;
                w.opened = node.child("opened") != nullptr;
                w.head_out = vec3(node.child("headoutposition"));
                w.has_position = node.child("position") != nullptr;
                w.position = vec3(node.child("position"));
                w.anim_time = number(node.child("animtime"), 0, 1.0f);
                if (const auto* proof = node.child("soundproof")) {
                    w.sound_closed = number(proof, 0);
                    w.sound_open = number(proof, 1);
                }
                if (const auto* sms = node.child("smstriggers")) {
                    w.sms_triggers = {static_cast<std::uint16_t>(number(sms, 0)), static_cast<std::uint16_t>(number(sms, 1))};
                }
                cab.windows.push_back(std::move(w));
            }
        }
        if (const auto* chairs = child_or_nested(*geometry, "interiorchairs")) {
            for (const auto& node : chairs->children) {
                if (node.name != "chair") continue;
                CabChair c;
                c.position = vec3(node.child("position"));
                c.radius = number(node.child("radius"), 0, 1.5f);
                c.name = text(node.child("skip"));
                c.seated = node.child("pos1") ? vec3(node.child("pos1")) : c.position;
                c.standing = node.child("pos2") ? vec3(node.child("pos2")) : c.seated;
                c.window = node.child("pos3") ? vec3(node.child("pos3")) : c.seated;
                c.head_out_window = text(node.child("headoutwindow"));
                cab.chairs.push_back(std::move(c));
            }
        }
    }
    return cab;
}

} // namespace openrts::formats
