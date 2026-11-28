// The inspector's lamps (F12, "Lamps"): a car's lights (its type's, every
// car of it) and a world object's lamps (its shape's), changed live, paused
// too, the chosen one held on to set it by day, drawn in the world as a
// gizmo (its place, its axes, its beam). Kept in a mod: a car's in its
// companion <file>.eng.openrts / .wag.openrts (formats::set_override_lights),
// a shape's in its override <shape>.openrts (formats::ShapeOverride::lights).
// The first step of a car editor.

#include "inspector.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#include <glm/exponential.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <openrts/renderer/model_assets.hpp>
#include <openrts/renderer/model_library.hpp>
#include <openrts/renderer/model_renderer.hpp>
#include <openrts/renderer/photometric.hpp>

#include "train_view.hpp"
#include "ui_text.hpp"

#ifdef OPENRTS_HAS_IMGUI
#include <imgui.h>
#include <ImGuizmo.h>
#include <imgui_internal.h>
#endif

namespace openrts::viewer {
namespace {

constexpr float gizmo_reach = 25.0f; // a beam drawn so far at most, m

// AARRGGBB <-> RGBA floats.
void to_rgba(const std::uint32_t argb, float out[4]) {
    out[0] = static_cast<float>((argb >> 16) & 0xFFu) / 255.0f;
    out[1] = static_cast<float>((argb >> 8) & 0xFFu) / 255.0f;
    out[2] = static_cast<float>(argb & 0xFFu) / 255.0f;
    out[3] = static_cast<float>(argb >> 24) / 255.0f;
}
std::uint32_t to_argb(const float in[4]) {
    const auto byte = [](const float v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return byte(in[3]) << 24 | byte(in[0]) << 16 | byte(in[1]) << 8 | byte(in[2]);
}

[[nodiscard]] bool has_flag(const std::string& flags, const std::string_view word) {
    std::size_t at = 0;
    while (at < flags.size()) {
        auto end = flags.find(' ', at);
        if (end == std::string::npos) end = flags.size();
        if (std::string_view(flags).substr(at, end - at) == word) return true;
        at = end + 1;
    }
    return false;
}
void set_flag(std::string& flags, const std::string_view word, const bool on) {
    std::string out;
    std::size_t at = 0;
    while (at < flags.size()) {
        auto end = flags.find(' ', at);
        if (end == std::string::npos) end = flags.size();
        const auto w = std::string_view(flags).substr(at, end - at);
        if (!w.empty() && w != word) {
            if (!out.empty()) out.push_back(' ');
            out += w;
        }
        at = end + 1;
    }
    if (on) {
        if (!out.empty()) out.push_back(' ');
        out += word;
    }
    flags = out;
}

// Unity's rotation (degrees X, Y, Z: Ry Rx Rz) as a matrix.
glm::mat3 euler_rotation(const glm::vec3& e) {
    const glm::mat4 r = glm::rotate(glm::mat4{1.0f}, glm::radians(e.y), glm::vec3{0.0f, 1.0f, 0.0f})
        * glm::rotate(glm::mat4{1.0f}, glm::radians(e.x), glm::vec3{1.0f, 0.0f, 0.0f})
        * glm::rotate(glm::mat4{1.0f}, glm::radians(e.z), glm::vec3{0.0f, 0.0f, 1.0f});
    return glm::mat3{r};
}

// A rotation (orthonormal columns) as Unity's angles: Y and X from its +z,
// Z from its +x against the unrolled one's.
glm::vec3 euler_of(const glm::mat3& r) {
    const glm::vec3 f = glm::normalize(r[2]);
    glm::vec3 e{glm::degrees(std::asin(std::clamp(-f.y, -1.0f, 1.0f))), glm::degrees(std::atan2(f.x, f.z)), 0.0f};
    const glm::mat3 unrolled = euler_rotation(e);
    e.z = glm::degrees(std::atan2(glm::dot(r[0], unrolled[1]), glm::dot(r[0], unrolled[0])));
    return e;
}

// A lamp's light space for a shape's LightSource: z towards PointsTo, x
// along the shape's x (TrainView::update_lights' cab lamps, the scenery's).
glm::mat4 shape_lamp_place(const formats::ShapeLight& lamp) {
    const glm::vec3 from{lamp.position[0], lamp.position[1], lamp.position[2]};
    glm::vec3 to{lamp.points_to[0], lamp.points_to[1], lamp.points_to[2]};
    if (glm::distance(from, to) < 1e-3f) to = from - glm::vec3{0.0f, 1.0f, 0.0f};
    const glm::vec3 z = glm::normalize(to - from);
    glm::vec3 x = glm::vec3{1.0f, 0.0f, 0.0f} - z * z.x;
    if (glm::length(x) < 1e-3f) x = glm::vec3{0.0f, 0.0f, 1.0f} - z * z.z;
    x = glm::normalize(x);
    const glm::vec3 y = glm::cross(z, x);
    glm::mat4 m{1.0f};
    m[0] = glm::vec4{x, 0.0f};
    m[1] = glm::vec4{y, 0.0f};
    m[2] = glm::vec4{z, 0.0f};
    m[3] = glm::vec4{from, 1.0f};
    return m;
}

// A car's light's own move and turns in its frame (TrainView::car_light_local
// without the last car's turn): Position, then Azimuth about y, Elevation about x.
glm::mat4 car_lamp_local(const formats::LightState& s) {
    glm::mat4 local = glm::translate(glm::mat4{1.0f}, glm::vec3{s.position[0], s.position[1], s.position[2]});
    local = glm::rotate(local, glm::radians(s.azimuth[1]), glm::vec3{0.0f, 1.0f, 0.0f});
    return glm::rotate(local, glm::radians(s.elevation[0]), glm::vec3{1.0f, 0.0f, 0.0f});
}

// A marker: an octahedron of half size `size` at `at`.
void diamond(const glm::vec3& at, const float size, const std::uint32_t colour, std::vector<renderer::WideLine>& lines) {
    const glm::vec3 x{size, 0.0f, 0.0f}, y{0.0f, size, 0.0f}, z{0.0f, 0.0f, size};
    const glm::vec3 ring[4] = {at + x, at + z, at - x, at - z};
    for (int k = 0; k < 4; ++k) {
        lines.push_back({ring[k], ring[(k + 1) % 4], colour, 2.0f});
        lines.push_back({ring[k], at + y, colour, 2.0f});
        lines.push_back({ring[k], at - y, colour, 2.0f});
    }
}

// The lamp's light: a cone (`tangent` of its half angles) to `reach`, or a
// sphere of `reach` (its axes: the ImGuizmo gizmo's, draw_lamp_gizmo).
void lamp_gizmo(const glm::mat4& place, const bool cone, const float tangent_x, const float tangent_y, const float reach,
                const std::uint32_t colour, std::vector<renderer::WideLine>& lines, const float source = 0.0f) {
    const glm::vec3 at{place[3]};
    const auto axis = [&](const int k) { return glm::normalize(glm::vec3{place[k]}); };
    const float d = std::clamp(reach, 0.05f, gizmo_reach);
    constexpr int segments = 32;
    constexpr float two_pi = 6.2831853f;
    if (cone) {
        lines.push_back({at, at + axis(2) * d, colour, 1.5f});
        // From the source's disc (its radius) out to the reach drawn.
        glm::vec3 last{}, last_start{};
        for (int s = 0; s <= segments; ++s) {
            const float a = two_pi * static_cast<float>(s) / segments;
            const glm::vec3 local{std::cos(a) * (tangent_x * d + source), std::sin(a) * (tangent_y * d + source), d};
            const glm::vec3 p{place * glm::vec4{local, 1.0f}};
            const glm::vec3 start{place * glm::vec4{std::cos(a) * source, std::sin(a) * source, 0.0f, 1.0f}};
            if (s > 0) {
                lines.push_back({last, p, colour, 1.5f});
                if (source > 0.0f) lines.push_back({last_start, start, colour, 1.5f});
            }
            if (s % 8 == 0 && s < segments) lines.push_back({source > 0.0f ? start : at, p, colour, 1.0f});
            last = p;
            last_start = start;
        }
        return;
    }
    for (int plane = 0; plane < 3; ++plane) {
        glm::vec3 last{};
        for (int s = 0; s <= segments; ++s) {
            const float a = two_pi * static_cast<float>(s) / segments;
            glm::vec3 local{0.0f};
            local[plane] = std::cos(a) * d;
            local[(plane + 1) % 3] = std::sin(a) * d;
            const glm::vec3 p{place * glm::vec4{local, 1.0f}};
            if (s > 0) lines.push_back({last, p, colour, 1.0f});
            last = p;
        }
    }
}

#ifdef OPENRTS_HAS_IMGUI
// A combo over `names`; true when changed.
template <std::size_t N>
bool pick_one(const char* label, std::uint8_t& value, const char* const (&names)[N]) {
    int v = (std::min)(static_cast<int>(value), static_cast<int>(N) - 1);
    const char* shown[N];
    for (std::size_t i = 0; i < N; ++i) shown[i] = tr(names[i]);
    if (!ImGui::Combo(label, &v, shown, static_cast<int>(N))) return false;
    value = static_cast<std::uint8_t>(v);
    return true;
}
#endif

#ifdef OPENRTS_HAS_IMGUI
// openRTS's label of a light or lamp (ORTSName): only to tell them apart.
bool name_field(std::string& name) {
    char buffer[96];
    std::snprintf(buffer, sizeof buffer, "%s", name.c_str());
    if (!ImGui::InputText(tr("Label (openRTS's, to tell them apart)"), buffer, sizeof buffer)) return false;
    name = buffer;
    // Kept between quotes in the files: no quotes in it.
    std::erase(name, '"');
    return true;
}

// openRTS's photometric light (renderer/photometric.hpp): candela or lumen
// and kelvin, for the experimental path; 0 each: the RTS colour as it is.
bool photometric_fields(float& candela, float& lumens, float& kelvin) {
    bool changed = false;
    ImGui::SeparatorText(tr("Real brightness (new renderer)"));
    ImGui::TextWrapped("%s", tr("Optional: the lamp's brightness in real units instead of its colour's. Works in the new renderer only; 0 in both fields: as in RTS."));
    // A number typed or stepped (the arrows: x10 with Ctrl), and the usual lamps.
    const auto number = [&](const char* label, float& v, const float step, const float big) {
        if (!ImGui::InputFloat(label, &v, step, big, "%.0f")) return false;
        v = (std::max)(v, 0.0f);
        return true;
    };
    changed |= number(tr("Intensity on its axis, cd"), candela, 1000.0f, 50000.0f);
    changed |= number(tr("or its whole light, lm (when no cd)"), lumens, 100.0f, 5000.0f);
    const auto preset = [&](const char* label, const float cd, const float lm, const float k) {
        if (!ImGui::SmallButton(label)) return;
        candela = cd;
        lumens = lm;
        if (k > 0.0f) kelvin = k;
        changed = true;
    };
    preset(tr("headlight 200 000 cd"), 200000.0f, 0.0f, 4500.0f);
    ImGui::SameLine();
    preset(tr("marker lamp 200 cd"), 200.0f, 0.0f, 2700.0f);
    ImGui::SameLine();
    preset(tr("street lamp 15 000 lm"), 0.0f, 15000.0f, 2000.0f);
    ImGui::SameLine();
    preset(tr("platform LED 5 000 lm"), 0.0f, 5000.0f, 4500.0f);
    ImGui::SameLine();
    preset(tr("saloon 1 000 lm"), 0.0f, 1000.0f, 3000.0f);
    ImGui::SameLine();
    preset(tr("as in RTS"), 0.0f, 0.0f, 0.0f);
    changed |= ImGui::InputFloat(tr("Its colour by temperature, K (0: its own colour)"), &kelvin, 100.0f, 1000.0f, "%.0f");
    kelvin = kelvin <= 0.0f ? 0.0f : std::clamp(kelvin, 1000.0f, 12000.0f);
    if (kelvin > 0.0f) {
        const auto c = glm::pow(renderer::kelvin_colour(kelvin) / 1.5f, glm::vec3{1.0f / 2.2f});
        ImGui::SameLine();
        ImGui::ColorButton("##kelvin", {c.x, c.y, c.z, 1.0f}, ImGuiColorEditFlags_NoTooltip, {18.0f, 18.0f});
    }
    ImGui::TextDisabled("%s", tr("Sodium 2000 K, a bulb 2700 K, an LED 4000-5000 K, daylight 6500 K."));
    // Every such lamp at once: brighter or dimmer to the eye (lux_per_unit 20 at x1).
    float factor = 20.0f / (std::max)(renderer::lux_per_unit(), 1e-3f);
    if (ImGui::SliderFloat(tr("Brightness of every such lamp, x"), &factor, 0.01f, 100.0f, "%.2f", ImGuiSliderFlags_Logarithmic))
        renderer::lux_per_unit() = 20.0f / (std::max)(factor, 0.01f);
    return changed;
}
#endif

} // namespace

std::string Inspector::lamp_key() const {
    if (selection_.kind == Selection::Kind::world)
        return "w" + std::to_string(selection_.tile_x) + "," + std::to_string(selection_.tile_z) + "," + std::to_string(selection_.uid);
    if (selection_.kind == Selection::Kind::car) return "c" + std::to_string(selection_.train) + "," + std::to_string(selection_.car);
    return {};
}

std::optional<Inspector::HeldLamp> Inspector::held_lamp() const {
    if (!open_ || !lamp_hold_ || selection_.kind != Selection::Kind::world || lamp_of_ != lamp_key()) return std::nullopt;
    return HeldLamp{selection_.tile_x, selection_.tile_z, selection_.uid, static_cast<std::size_t>((std::max)(lamp_, 0))};
}

void Inspector::draw_lamps(const InspectorWorld& world, const core::VirtualPath& shape) {
#ifdef OPENRTS_HAS_IMGUI
    if (lamp_of_ != lamp_key()) {
        lamp_of_ = lamp_key();
        lamp_ = 0;
        lamp_state_ = 0;
        lamp_status_.clear();
        lamp_dirty_ = false;
        lamp_undo_.clear();
        lamp_redo_.clear();
        lamp_editing_ = false;
    }
    // The gizmo on the chosen lamp: arrows to move it (its way: the yaw and
    // pitch fields; the format has no roll about the beam, so rings to turn
    // it were more confusing than useful, testers 2026-11-27).
    // The tools' own small window, as an editor's: what the gizmo does.
    {
        ImGui::SetNextWindowSize(ImVec2{0.0f, 0.0f});
        if (ImGui::Begin(tr("Tools##lamp_tools"), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
            const auto tool = [&](const char* label, const int operation) {
                const bool on = gizmo_operation_ == operation;
                if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                if (ImGui::Button(label, ImVec2{170.0f, 0.0f})) gizmo_operation_ = operation;
                if (on) ImGui::PopStyleColor();
            };
            tool(tr("Move (arrows)"), 1);
            tool(tr("Rotate (X, Y, Z rings)"), 0);
            ImGui::RadioButton(tr("Local: the lamp's axes"), &gizmo_space_, 0);
            ImGui::SameLine();
            ImGui::RadioButton(tr("World: the world's axes"), &gizmo_space_, 1);
            ImGui::TextDisabled("%s", tr("Shift: by 1 cm / 1 degree; Ctrl: turn slower"));
        }
        ImGui::End();
    }
    draw_lamp_gizmo(world);
    const bool car = selection_.kind == Selection::Kind::car;
    if (world.trains) world.trains->hold_car_light(selection_.train, selection_.car, std::nullopt);
    if (!ImGui::CollapsingHeader(tr("Lamps"), ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::TextWrapped("%s", car ? tr("The lights of this car's type (every car of it changes at once). Kept in the mod as the car's <file>.eng.openrts / .wag.openrts.")
                                 : tr("The lamps of this object's shape (every object of it). Kept in the mod as the shape's <shape>.openrts."));
    ImGui::Checkbox(tr("Hold the chosen lamp on (to set it by day, paused too)"), &lamp_hold_);
    // Undo and redo: the buttons, and Ctrl+Z / Ctrl+Y (Ctrl+Shift+Z) while
    // the inspector has the keyboard (not in a text field: its own).
    ImGui::BeginDisabled(lamp_undo_.empty());
    if (ImGui::Button(tr("Undo (Ctrl+Z)"))) step_lamps(world, shape, false);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(lamp_redo_.empty());
    if (ImGui::Button(tr("Redo (Ctrl+Y)"))) step_lamps(world, shape, true);
    ImGui::EndDisabled();
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput && ImGui::GetIO().KeyCtrl) {
        const bool shift = ImGui::GetIO().KeyShift;
        if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) step_lamps(world, shape, shift);
        else if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) step_lamps(world, shape, true);
    }
    if (car) {
        draw_car_lamps(world);
    } else {
        draw_shape_lamps(world, shape);
    }
    if (!ImGui::IsAnyItemActive()) lamp_editing_ = false;
    if (!lamp_status_.empty()) ImGui::TextWrapped("%s", lamp_status_.c_str());
#else
    (void)world;
    (void)shape;
#endif
}

void Inspector::draw_car_lamps(const InspectorWorld& world) {
#ifdef OPENRTS_HAS_IMGUI
    auto* lights = world.trains ? world.trains->car_lights(selection_.train, selection_.car) : nullptr;
    if (!lights) {
        ImGui::TextDisabled("%s", tr("This car's type is not at hand."));
        return;
    }
    // As they were before this frame's change (undo).
    const LampSnapshot before{.key = lamp_key(), .car = true, .car_lights = *lights, .lamp = lamp_, .state = lamp_state_};
    static const char* const types[] = {"0: glow (sprite)", "1: spotlight", "2: sphere", "3: orthogonal (box)", "4: cone"};
    static const char* const headlight[] = {"any", "off", "dim", "bright", "dim or bright"};
    static const char* const unit[] = {"any", "middle", "first car", "last car", "last, turned", "first, turned"};
    static const char* const yes_no[] = {"any", "no", "yes"};
    static const char* const time_of_day[] = {"any", "day", "night"};
    static const char* const weather[] = {"any", "clear", "rain", "snow"};
    if (lights->empty()) ImGui::TextDisabled("%s", tr("No lights."));
    lamp_ = std::clamp(lamp_, 0, (std::max)(static_cast<int>(lights->size()) - 1, 0));
    const float list_height = (std::min)(ImGui::GetTextLineHeightWithSpacing() * (static_cast<float>(lights->size()) + 0.5f), 160.0f);
    if (ImGui::BeginListBox("##car_lamps", {-1.0f, list_height})) {
        for (std::size_t i = 0; i < lights->size(); ++i) {
            const auto& l = (*lights)[i];
            char label[160];
            if (!l.name.empty()) std::snprintf(label, sizeof label, "%zu: %s (%s)%s", i, l.name.c_str(), tr(types[l.type < 5 ? l.type : 4]), l.states.empty() ? " (-)" : "");
            else std::snprintf(label, sizeof label, "%zu: %s%s, %zu %s", i, tr(types[l.type < 5 ? l.type : 4]),
                               l.states.empty() ? " (-)" : "", l.states.size(), tr("states"));
            if (ImGui::Selectable(label, static_cast<int>(i) == lamp_)) {
                lamp_ = static_cast<int>(i);
                lamp_state_ = 0;
            }
        }
        ImGui::EndListBox();
    }
    bool changed = false;
    if (ImGui::Button(tr("Add a light"))) {
        formats::LightDefinition made = lights->empty() ? formats::LightDefinition{} : (*lights)[static_cast<std::size_t>(lamp_)];
        if (made.states.empty()) made.states.push_back({.position = {0.0f, 3.0f, 0.0f}, .radius = 0.3f, .angle = 10.0f});
        lights->push_back(std::move(made));
        lamp_ = static_cast<int>(lights->size()) - 1;
        changed = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(lights->empty());
    if (ImGui::Button(tr("Remove it"))) {
        // Its place stays (the DLLs switch lights by index): no states, off.
        (*lights)[static_cast<std::size_t>(lamp_)].states.clear();
        changed = true;
    }
    ImGui::EndDisabled();
    if (!lights->empty() && lamp_ < static_cast<int>(lights->size())) {
        auto& l = (*lights)[static_cast<std::size_t>(lamp_)];
        ImGui::SeparatorText(tr("Light"));
        changed |= name_field(l.name);
        changed |= pick_one(tr("Type"), l.type, types);
        changed |= pick_one(tr("Headlight"), l.headlight, headlight);
        changed |= pick_one(tr("Unit (place in the train)"), l.unit, unit);
        changed |= pick_one(tr("Control (driven)"), l.control, yes_no);
        changed |= pick_one(tr("Service"), l.service, yes_no);
        changed |= pick_one(tr("Time of day"), l.time_of_day, time_of_day);
        changed |= pick_one(tr("Weather"), l.weather, weather);
        changed |= ImGui::DragFloat(tr("Fade in, s"), &l.fade_in, 0.01f, 0.0f, 10.0f);
        changed |= ImGui::DragFloat(tr("Fade out, s"), &l.fade_out, 0.01f, 0.0f, 10.0f);
        if (l.type == 0) {
            char texture[128];
            std::snprintf(texture, sizeof texture, "%s", l.texture.c_str());
            if (ImGui::InputText(tr("Texture (TRAINS/TRAINSET/...)"), texture, sizeof texture)) {
                l.texture = texture;
                changed = true;
            }
        } else {
            changed |= ImGui::DragFloat(tr("Fall-off start (share of the reach)"), &l.attenuation[1], 0.005f, 0.0f, 1.0f);
            changed |= ImGui::DragFloat(tr("Fall-off length (share of the reach)"), &l.attenuation[2], 0.005f, 0.01f, 1.0f);
            float focus = l.spot_exponent / 128.0f;
            if (ImGui::SliderFloat(tr("Focus (SpotExponent: 0 even, 1 bright middle)"), &focus, 0.0f, 1.0f)) {
                l.spot_exponent = focus * 128.0f;
                changed = true;
            }
            changed |= photometric_fields(l.candela, l.lumens, l.kelvin);
        }
        if (l.states.empty()) {
            ImGui::TextDisabled("%s", tr("Removed (no states): add one to bring it back."));
            if (ImGui::Button(tr("Add a state"))) {
                l.states.push_back({.position = {0.0f, 3.0f, 0.0f}, .radius = 0.3f, .angle = 10.0f});
                changed = true;
            }
        } else {
            ImGui::SeparatorText(tr("State"));
            lamp_state_ = std::clamp(lamp_state_, 0, static_cast<int>(l.states.size()) - 1);
            if (l.states.size() > 1) {
                ImGui::SliderInt(tr("State"), &lamp_state_, 0, static_cast<int>(l.states.size()) - 1);
                changed |= ImGui::DragFloat(tr("Cycle, s"), &l.cycle, 0.01f, 0.0f, 10.0f);
            }
            auto& s = l.states[static_cast<std::size_t>(lamp_state_)];
            changed |= ImGui::DragFloat3(tr("Position (x right, y up, z ahead), m"), s.position.data(), 0.005f);
            if (l.type != 0) {
                changed |= ImGui::DragFloat(tr("Azimuth (turn), degrees"), &s.azimuth[1], 0.25f, -360.0f, 360.0f);
                changed |= ImGui::DragFloat(tr("Elevation (+ dips), degrees"), &s.elevation[0], 0.1f, -90.0f, 90.0f);
                changed |= ImGui::DragFloat(tr("Reach, m"), &s.radius, 0.1f, 0.0f, 2000.0f);
                changed |= ImGui::DragFloat(tr("Half angle, degrees"), &s.angle, 0.05f, 0.5f, 89.0f);
                changed |= ImGui::DragFloat(tr("Source radius, m"), &s.source_radius, 0.005f, 0.0f, 5.0f);
            } else {
                changed |= ImGui::DragFloat(tr("Azimuth (turn), degrees"), &s.azimuth[1], 0.25f, -360.0f, 360.0f);
                changed |= ImGui::DragFloat(tr("Glow half size, m"), &s.radius, 0.005f, 0.0f, 10.0f);
            }
            float rgba[4];
            to_rgba(s.colour, rgba);
            // A glow's alpha is its brightness; a beam's is its colour's
            // lightness alone (RTS takes no alpha for it).
            if (l.type == 0 ? ImGui::ColorEdit4(tr("Colour (alpha: brightness)"), rgba, ImGuiColorEditFlags_AlphaBar)
                            : ImGui::ColorEdit3(tr("Colour (its lightness: the brightness)"), rgba)) {
                s.colour = to_argb(rgba);
                changed = true;
            }
            if (l.states.size() > 1) changed |= ImGui::DragFloat(tr("Duration, s"), &s.duration, 0.01f, 0.0f, 10.0f);
            if (ImGui::Button(tr("Copy the state"))) {
                l.states.push_back(s);
                lamp_state_ = static_cast<int>(l.states.size()) - 1;
                changed = true;
            }
        }
    }
    if (changed) {
        lamp_dirty_ = true;
        remember_lamps(before, lamp_editing_);
        lamp_editing_ = ImGui::IsAnyItemActive();
    }
    if (lamp_hold_ && !lights->empty())
        world.trains->hold_car_light(selection_.train, selection_.car,
                                     std::pair{static_cast<std::size_t>(lamp_), static_cast<std::size_t>((std::max)(lamp_state_, 0))});
    ImGui::Separator();
    ImGui::BeginDisabled(!lamp_dirty_);
    if (ImGui::Button(tr("Save the lights to the mod"))) {
        lamp_status_ = world.trains->save_car_lights(selection_.train, selection_.car, mod_);
        lamp_dirty_ = false;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("As in the files"))) {
        remember_lamps(before, false);
        world.trains->revert_car_lights(selection_.train, selection_.car);
        lamp_dirty_ = false;
        lamp_status_ = tr("The lights are as the files have them.");
    }
    if (lamp_dirty_) {
        ImGui::SameLine();
        ImGui::TextColored({1.0f, 0.75f, 0.3f, 1.0f}, "%s", tr("not saved"));
    }
#else
    (void)world;
#endif
}

void Inspector::draw_shape_lamps(const InspectorWorld& world, const core::VirtualPath& shape) {
#ifdef OPENRTS_HAS_IMGUI
    auto* model = world.library ? world.library->find(shape) : nullptr;
    if (!model) {
        ImGui::TextDisabled("%s", tr("The shape is not loaded."));
        return;
    }
    auto lamps = model->lights();
    const LampSnapshot before{.key = lamp_key(), .shape_lamps = lamps, .lamp = lamp_, .state = lamp_state_};
    if (lamps.empty()) ImGui::TextDisabled("%s", tr("No lamps (ESD_LightSources): add one."));
    lamp_ = std::clamp(lamp_, 0, (std::max)(static_cast<int>(lamps.size()) - 1, 0));
    static const char* const kinds[] = {"cone", "sphere", "box"};
    const auto kind_of = [](const formats::ShapeLight& l) -> std::uint8_t {
        return has_flag(l.flags, "light_cone") ? 0 : has_flag(l.flags, "light_sphere") ? 1 : 2;
    };
    const float list_height = (std::min)(ImGui::GetTextLineHeightWithSpacing() * (static_cast<float>(lamps.size()) + 0.5f), 160.0f);
    if (ImGui::BeginListBox("##shape_lamps", {-1.0f, list_height})) {
        for (std::size_t i = 0; i < lamps.size(); ++i) {
            char label[160];
            if (!lamps[i].name.empty()) std::snprintf(label, sizeof label, "%zu: %s (%s)", i, lamps[i].name.c_str(), tr(kinds[kind_of(lamps[i])]));
            else std::snprintf(label, sizeof label, "%zu: %s, %s", i, tr(kinds[kind_of(lamps[i])]), lamps[i].flags.c_str());
            if (ImGui::Selectable(label, static_cast<int>(i) == lamp_)) lamp_ = static_cast<int>(i);
        }
        ImGui::EndListBox();
    }
    bool changed = false;
    if (ImGui::Button(tr("Add a lamp"))) {
        formats::ShapeLight made;
        if (lamps.empty()) {
            made.position = {0.0f, 5.0f, 0.0f};
            made.points_to = {0.0f, 0.0f, 0.0f};
            made.radius = {6.0f, 6.0f};
            made.flags = "light_cone night_auto";
            made.colour = 0xFFFFE0B0u;
        } else {
            made = lamps[static_cast<std::size_t>(lamp_)];
        }
        lamps.push_back(made);
        lamp_ = static_cast<int>(lamps.size()) - 1;
        changed = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(lamps.empty());
    // A copy of the chosen lamp, right after it, chosen (where it is: move it).
    if (ImGui::Button(tr("Duplicate it")) && !lamps.empty()) {
        const auto at = static_cast<std::size_t>(lamp_);
        const auto copy = lamps[at];
        lamps.insert(lamps.begin() + static_cast<std::ptrdiff_t>(at) + 1, copy);
        lamp_ = static_cast<int>(at) + 1;
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Remove it"))) {
        lamps.erase(lamps.begin() + lamp_);
        lamp_ = (std::max)(lamp_ - 1, 0);
        changed = true;
    }
    ImGui::EndDisabled();
    if (!lamps.empty()) {
        auto& l = lamps[static_cast<std::size_t>(lamp_)];
        ImGui::SeparatorText(tr("Lamp"));
        changed |= name_field(l.name);
        std::uint8_t kind = kind_of(l);
        if (pick_one(tr("Kind"), kind, kinds)) {
            set_flag(l.flags, "light_cone", kind == 0);
            set_flag(l.flags, "light_sphere", kind == 1);
            changed = true;
        }
        const auto flag = [&](const char* label, const char* word) {
            bool on = has_flag(l.flags, word);
            if (ImGui::Checkbox(label, &on)) {
                set_flag(l.flags, word, on);
                changed = true;
            }
        };
        flag(tr("Lights whatever way a surface faces (POSITIONAL)"), "positional");
        flag(tr("Burns always (RUNNING)"), "running");
        flag(tr("By the light level or at night (NIGHT_AUTO)"), "night_auto");
        flag(tr("Never by day (NIGHT_ONLY)"), "night_only");
        {
            // ORTSShadowPriority: which lamps get the few shadow maps.
            const char* names[6];
            names[0] = tr("0: throws no shadows");
            names[1] = tr("1: ordinary");
            names[2] = tr("2: a station lamp, its shadow into the cab");
            names[3] = tr("3: more wanted");
            names[4] = tr("4: much more wanted");
            names[5] = tr("5: a floodlight over the station");
            int p = std::clamp(l.shadow_priority, 0, 5);
            if (ImGui::Combo(tr("Shadow priority (openRTS: which lamps get the shadow maps)"), &p, names, 6)) {
                l.shadow_priority = p;
                changed = true;
            }
        }
        changed |= ImGui::DragFloat3(tr("Position (the shape's x, y, z), m"), l.position.data(), 0.01f);
        changed |= ImGui::DragFloat3(tr("Points to, m"), l.points_to.data(), 0.01f);
        {
            // The same point as two angles (the distance kept): the turn
            // about the shape's up (0: along +z, 90: along +x) and the tilt
            // (-90 straight down, 90 straight up).
            const glm::vec3 from{l.position[0], l.position[1], l.position[2]};
            glm::vec3 way = glm::vec3{l.points_to[0], l.points_to[1], l.points_to[2]} - from;
            float length = glm::length(way);
            if (length < 1e-3f) {
                way = {0.0f, -1.0f, 0.0f};
                length = 1.0f;
            }
            way /= length;
            auto& euler = lamp_euler_for(way);
            if (ImGui::DragFloat3(tr("Rotation X, Y, Z, degrees (as Unity: X tilt, Y left/right, Z roll: not saved)"), &euler.x, 0.5f, -360.0f,
                                  360.0f, "%.1f")) {
                const glm::vec3 to = from + euler_rotation(euler)[2] * length;
                l.points_to = {to.x, to.y, to.z};
                changed = true;
            }
        }
        changed |= ImGui::DragFloat2(tr("Light spot's radius at the point's distance (across x, across z), m"), l.radius.data(), 0.02f, 0.0f, 500.0f);
        float rgba[4];
        to_rgba(l.colour, rgba);
        if (ImGui::ColorEdit3(tr("Colour (its lightness: the brightness)"), rgba)) {
            l.colour = to_argb(rgba);
            changed = true;
        }
        changed |= ImGui::DragFloat(tr("Fall-off start (share of the reach)"), &l.attenuation[1], 0.005f, 0.0f, 1.0f);
        changed |= ImGui::DragFloat(tr("Fall-off length (share of the reach)"), &l.attenuation[2], 0.005f, 0.01f, 3.0f);
        changed |= ImGui::SliderFloat(tr("Focus (SpotExponent: 0 even, 1 bright middle)"), &l.spot_exponent, 0.0f, 1.0f);
        changed |= ImGui::DragFloat(tr("Source radius, m"), &l.source_radius, 0.005f, 0.0f, 5.0f);
        changed |= ImGui::DragFloat(tr("Lit within this far of the camera, m (0: always)"), &l.distance, 1.0f, 0.0f, 5000.0f);
        changed |= ImGui::DragFloat(tr("Lit while the world is at most this lit (0: at night)"), &l.auto_when_world_lit, 0.005f, 0.0f, 1.0f);
        changed |= ImGui::DragFloat(tr("Fade, s"), &l.light_fade, 0.01f, 0.0f, 10.0f);
        int count = static_cast<int>(l.array_count);
        if (ImGui::DragInt(tr("Lamps in a row (LightArray)"), &count, 0.1f, 0, 500)) {
            l.array_count = static_cast<std::uint32_t>((std::max)(count, 0));
            changed = true;
        }
        if (l.array_count >= 2) changed |= ImGui::DragFloat3(tr("Step of the row, m"), l.array_step.data(), 0.01f);
        changed |= photometric_fields(l.candela, l.lumens, l.kelvin);
    }
    if (changed) {
        remember_lamps(before, lamp_editing_);
        lamp_editing_ = ImGui::IsAnyItemActive();
        apply_shape_lamps(world, shape, std::move(lamps));
    }
    ImGui::Separator();
    ImGui::BeginDisabled(!lamp_dirty_ && !dirty_);
    if (ImGui::Button(tr("Save the lamps to the mod"))) {
        save_edit(world, shape);
        lamp_status_ = status_;
        lamp_dirty_ = false;
    }
    ImGui::EndDisabled();
    // A route's own shape: beside it in the route, to go with the route.
    const bool in_route = shape.str().size() > 7 && core::fold_case(shape.str().substr(0, 7)) == "routes/";
    if (in_route) {
        ImGui::SameLine();
        if (ImGui::Button(tr("Save to the route"))) {
            save_edit_to_route(world, shape);
            lamp_status_ = status_;
            lamp_dirty_ = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Save to the original .sd"))) {
        save_lamps_to_sd(world, shape);
        lamp_status_ = status_;
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("As in the .sd"))) {
        remember_lamps(before, false);
        auto& over = edited(world, shape);
        over.lights.reset();
        renderer::hold_shape_override(shape, over);
        world.library->notify_changes({renderer::shape_override_path(shape)});
        dirty_ = true;
        lamp_dirty_ = true;
        lamp_status_ = tr("The lamps are the .sd's again (save to keep it so).");
    }
    if (lamp_dirty_) {
        ImGui::SameLine();
        ImGui::TextColored({1.0f, 0.75f, 0.3f, 1.0f}, "%s", tr("not saved"));
    }
#else
    (void)world;
    (void)shape;
#endif
}

void Inspector::remember_lamps(LampSnapshot before, const bool continuing) {
    if (continuing) return;
    lamp_undo_.push_back(std::move(before));
    if (lamp_undo_.size() > 200) lamp_undo_.erase(lamp_undo_.begin());
    lamp_redo_.clear();
}

std::optional<Inspector::LampSnapshot> Inspector::lamp_snapshot(const InspectorWorld& world, const core::VirtualPath& shape) const {
    LampSnapshot now{.key = lamp_key(), .lamp = lamp_, .state = lamp_state_};
    if (selection_.kind == Selection::Kind::car) {
        const auto* lights = world.trains ? world.trains->car_lights(selection_.train, selection_.car) : nullptr;
        if (!lights) return std::nullopt;
        now.car = true;
        now.car_lights = *lights;
        return now;
    }
    const auto* model = world.library ? world.library->find(shape) : nullptr;
    if (!model) return std::nullopt;
    now.shape_lamps = model->lights();
    return now;
}

void Inspector::step_lamps(const InspectorWorld& world, const core::VirtualPath& shape, const bool redo) {
    auto& from = redo ? lamp_redo_ : lamp_undo_;
    auto& to = redo ? lamp_undo_ : lamp_redo_;
    if (from.empty() || from.back().key != lamp_key() || drag_) return;
    auto now = lamp_snapshot(world, shape);
    if (!now) return;
    auto back = std::move(from.back());
    from.pop_back();
    to.push_back(std::move(*now));
    if (back.car) {
        if (auto* lights = world.trains ? world.trains->car_lights(selection_.train, selection_.car) : nullptr) *lights = back.car_lights;
    } else {
        apply_shape_lamps(world, shape, back.shape_lamps);
    }
    lamp_ = back.lamp;
    lamp_state_ = back.state;
    lamp_dirty_ = true;
    lamp_editing_ = false;
}

void Inspector::save_edit_to_route(const InspectorWorld& world, const core::VirtualPath& shape) {
    const auto origin = world.vfs ? world.vfs->origin(shape) : std::nullopt;
    if (!origin || origin->archive) {
        status_ = tr("The shape is not in a folder of the route (an archive or not found): saved nowhere.");
        return;
    }
    auto file = origin->location;
    file += ".openrts";
    const auto& over = edited(world, shape);
    std::error_code error;
    if (over.empty()) {
        std::filesystem::remove(file, error);
    } else {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << formats::write_shape_override(over);
        if (!out) {
            const auto name = file.generic_u8string();
            status_ = trf("Could not write %s", std::string(name.begin(), name.end()).c_str());
            return;
        }
    }
    dirty_ = false;
    const auto shown = file.generic_u8string();
    status_ = trf("Saved beside the shape in the route: %s", std::string(shown.begin(), shown.end()).c_str());
    // A mod's copy wins over the route's (the mods are above): say so.
    if (const auto in_effect = world.vfs->origin(renderer::shape_override_path(shape)); in_effect && in_effect->location != file)
        status_ += std::string("\n") + trf("A mod's file of it is above the route's and still wins: %s", in_effect->source_name.c_str());
}

namespace {

// A text file as UTF-8, and whether it was UTF-16 (LE, with its mark).
std::string read_text(const std::filesystem::path& file, bool& utf16) {
    std::ifstream in(file, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    utf16 = bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE;
    if (!utf16) return bytes;
    std::string out;
    for (std::size_t i = 2; i + 1 < bytes.size(); i += 2) {
        std::uint32_t c = static_cast<unsigned char>(bytes[i]) | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i + 1])) << 8);
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < bytes.size()) {
            const std::uint32_t low = static_cast<unsigned char>(bytes[i + 2]) | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i + 3])) << 8);
            c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
            i += 2;
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

// UTF-8 as UTF-16 LE with its mark.
std::string to_utf16(const std::string& text) {
    std::string out{"\xFF\xFE"};
    const auto put = [&](const std::uint32_t u) {
        out += static_cast<char>(u & 0xFF);
        out += static_cast<char>(u >> 8);
    };
    for (std::size_t i = 0; i < text.size();) {
        const auto b = static_cast<unsigned char>(text[i]);
        std::uint32_t c = b;
        int more = 0;
        if (b >= 0xF0) c = b & 0x07, more = 3;
        else if (b >= 0xE0) c = b & 0x0F, more = 2;
        else if (b >= 0xC0) c = b & 0x1F, more = 1;
        ++i;
        for (int k = 0; k < more && i < text.size(); ++k, ++i) c = (c << 6) | (static_cast<unsigned char>(text[i]) & 0x3F);
        if (c >= 0x10000) {
            c -= 0x10000;
            put(0xD800 + (c >> 10));
            put(0xDC00 + (c & 0x3FF));
        } else {
            put(c);
        }
    }
    return out;
}

// The text with its ESD_LightSources block (any case) put in place of the
// one there, or before the Shape block's closing parenthesis.
std::string with_light_sources(std::string text, const std::string& block) {
    std::string lower = text;
    for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (const auto at = lower.find("esd_lightsources"); at != std::string::npos) {
        auto open = text.find('(', at);
        if (open != std::string::npos) {
            int depth = 0;
            bool quoted = false;
            for (auto i = open; i < text.size(); ++i) {
                if (text[i] == '"') quoted = !quoted;
                if (quoted) continue;
                if (text[i] == '(') ++depth;
                if (text[i] == ')' && --depth == 0) {
                    auto end = i + 1;
                    while (end < text.size() && (text[end] == '\r' || text[end] == '\n')) ++end;
                    auto start = at;
                    while (start > 0 && (text[start - 1] == ' ' || text[start - 1] == '\t')) --start;
                    return text.substr(0, start) + block + text.substr(end);
                }
            }
        }
    }
    const auto close = text.find_last_of(')');
    if (close == std::string::npos) return text + block;
    return text.substr(0, close) + block + text.substr(close);
}

} // namespace

void Inspector::save_lamps_to_sd(const InspectorWorld& world, const core::VirtualPath& shape) {
    auto* model = world.library ? world.library->find(shape) : nullptr;
    const auto sd = core::VirtualPath::parse(shape.str() + "d");
    const auto origin = world.vfs && sd ? world.vfs->origin(*sd) : std::nullopt;
    if (!model || !origin || origin->archive) {
        status_ = tr("The shape's .sd is not in a folder (an archive or not found): saved nowhere.");
        return;
    }
    const auto file = origin->location;
    bool utf16 = false;
    const auto text = read_text(file, utf16);
    const auto& lamps = model->lights();
    std::string block = formats::write_shape_lights(lamps, "\t");
    if (const auto at = block.find("LightSources"); at != std::string::npos) block.insert(at, "ESD_");
    if (lamps.empty()) block.clear();
    const auto changed = with_light_sources(text, block);
    std::error_code error;
    auto backup = file;
    backup += ".bak";
    if (!std::filesystem::exists(backup, error)) std::filesystem::copy_file(file, backup, error);
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << (utf16 ? to_utf16(changed) : changed);
        if (!out) {
            const auto name = file.generic_u8string();
            status_ = trf("Could not write %s", std::string(name.begin(), name.end()).c_str());
            return;
        }
    }
    // The .sd has them now: the override's lamps go (saved so), the .sd read again.
    auto& over = edited(world, shape);
    over.lights.reset();
    renderer::hold_shape_override(shape, over);
    save_edit(world, shape);
    world.library->notify_changes({*sd, renderer::shape_override_path(shape)});
    lamp_dirty_ = false;
    const auto shown = file.generic_u8string();
    status_ = trf("The lamps are written into %s (the old one kept as .sd.bak)", std::string(shown.begin(), shown.end()).c_str());
}

void Inspector::apply_shape_lamps(const InspectorWorld& world, const core::VirtualPath& shape, std::vector<formats::ShapeLight> lamps) {
    auto* model = world.library ? world.library->find(shape) : nullptr;
    if (!model) return;
    auto& over = edited(world, shape);
    over.lights = lamps;
    renderer::hold_shape_override(shape, over);
    model->set_lights(std::move(lamps));
    dirty_ = true;
    lamp_dirty_ = true;
}

std::optional<Inspector::LampGeometry> Inspector::lamp_geometry(const InspectorWorld& world, const core::VirtualPath& shape,
                                                                const glm::mat4& transform) const {
    if (lamp_of_ != lamp_key() || lamp_ < 0) return std::nullopt;
    const auto index = static_cast<std::size_t>(lamp_);
    LampGeometry g;
    if (selection_.kind == Selection::Kind::car) {
        const auto* lights = world.trains ? world.trains->car_lights(selection_.train, selection_.car) : nullptr;
        if (!lights || index >= lights->size() || (*lights)[index].states.empty()) return std::nullopt;
        const auto& l = (*lights)[index];
        const auto state = (std::min)(static_cast<std::size_t>((std::max)(lamp_state_, 0)), l.states.size() - 1);
        const auto& s = l.states[state];
        const auto place = world.trains->car_light_place(selection_.train, selection_.car, index, state);
        if (!place) return std::nullopt;
        // The frame its numbers are in: the place without its own move and turns.
        g.frame = *place * glm::inverse(car_lamp_local(s));
        g.place = *place;
        g.cone = l.type != 0;
        const float t = std::tan(glm::radians(std::clamp(s.angle, 0.5f, 89.0f)));
        g.tangent_x = g.tangent_y = t;
        g.reach = s.radius;
        g.size = (std::max)(s.radius, 0.05f);
        g.source = s.source_radius;
    } else {
        auto* model = world.library ? world.library->find(shape) : nullptr;
        if (!model || index >= model->lights().size()) return std::nullopt;
        const auto& lamp = model->lights()[index];
        g.frame = transform;
        g.place = transform * shape_lamp_place(lamp);
        const float reach = glm::distance(glm::vec3{lamp.position[0], lamp.position[1], lamp.position[2]},
                                          glm::vec3{lamp.points_to[0], lamp.points_to[1], lamp.points_to[2]});
        const float rx = (std::max)(lamp.radius[0], 0.01f), rz = (std::max)(lamp.radius[1] > 0.0f ? lamp.radius[1] : lamp.radius[0], 0.01f);
        g.cone = has_flag(lamp.flags, "light_cone");
        g.tangent_x = rx / (std::max)(reach, 0.01f);
        g.tangent_y = rz / (std::max)(reach, 0.01f);
        g.reach = reach;
        g.size = (std::max)(rx, rz);
        g.source = lamp.source_radius;
    }
    g.at = glm::vec3{g.place[3]};
    g.beam = glm::normalize(glm::vec3{g.place[2]});
    return g;
}

void Inspector::add_lamp_lines(const InspectorWorld& world, const core::VirtualPath& shape, const glm::mat4& transform,
                               std::vector<renderer::WideLine>& lines) const {
    handles_.clear();
    if (lamp_of_ != lamp_key()) return;
    const auto scale = [&](const glm::vec3& p) { return (std::max)(0.04f, 0.012f * glm::distance(eye_, p)); };
    const auto colour_of = [&](const std::uint32_t colour) {
        return hovered_ == static_cast<int>(handles_.size()) ? 0xffffffffu : colour;
    };
    // Every lamp: a marker to choose it by.
    std::vector<std::pair<std::size_t, glm::vec3>> lamps;
    if (selection_.kind == Selection::Kind::car && world.trains) {
        if (const auto* lights = world.trains->car_lights(selection_.train, selection_.car)) {
            for (std::size_t i = 0; i < lights->size(); ++i) {
                if ((*lights)[i].states.empty()) continue;
                const auto state = static_cast<int>(i) == lamp_ ? static_cast<std::size_t>((std::max)(lamp_state_, 0)) : 0u;
                if (const auto place = world.trains->car_light_place(selection_.train, selection_.car, i, state))
                    lamps.emplace_back(i, glm::vec3{(*place)[3]});
            }
        }
    } else if (auto* model = world.library ? world.library->find(shape) : nullptr) {
        for (std::size_t i = 0; i < model->lights().size(); ++i) {
            const auto& p = model->lights()[i].position;
            lamps.emplace_back(i, glm::vec3{transform * glm::vec4{p[0], p[1], p[2], 1.0f}});
        }
    }
    const auto geometry = lamp_geometry(world, shape, transform);
    for (const auto& [index, at] : lamps) {
        const bool chosen = geometry && static_cast<int>(index) == lamp_;
        // The chosen one is moved and turned by the ImGuizmo gizmo (draw_lamp_gizmo).
        const Handle handle{.kind = Handle::Kind::lamp, .at = at, .lamp = index};
        diamond(at, scale(at) * (chosen ? 1.6f : 1.0f), colour_of(chosen ? 0xff20e0ffu : 0xffffd040u), lines);
        handles_.push_back(handle);
    }
    if (!geometry) return;
    const auto& g = *geometry;
    // Its moves and turns: the ImGuizmo gizmo's (the arrows and the aimed
    // end drawn here before it are gone, testers 2026-11-27).
    // Its light: the cone (or the sphere), its end to aim it, its rim to open it.
    const float d = g.cone ? std::clamp(g.reach, 0.5f, gizmo_reach) : g.size;
    lamp_gizmo(g.place, g.cone, g.tangent_x, g.tangent_y, d, 0xff40e0ffu, lines, g.cone ? g.source : 0.0f);
    if (g.cone) {
        const glm::vec3 rim{g.place * glm::vec4{g.tangent_x * d + g.source, 0.0f, d, 1.0f}};
        diamond(rim, scale(rim), colour_of(0xffff40ffu), lines);
        handles_.push_back({.kind = Handle::Kind::rim, .at = rim});
        // The source's radius: its field (the handle was in the gizmo's way).
    } else {
        const glm::vec3 rim = g.at + glm::normalize(glm::vec3{g.place[0]}) * d;
        diamond(rim, scale(rim), colour_of(0xffff40ffu), lines);
        handles_.push_back({.kind = Handle::Kind::rim, .at = rim});
    }
    // A row: its other lamps' places.
    if (selection_.kind == Selection::Kind::world) {
        if (auto* model = world.library ? world.library->find(shape) : nullptr; model && static_cast<std::size_t>(lamp_) < model->lights().size()) {
            const auto& lamp = model->lights()[static_cast<std::size_t>(lamp_)];
            for (std::uint32_t n = 1; n < lamp.array_count && n < 200; ++n) {
                const glm::vec3 local{lamp.position[0] + lamp.array_step[0] * n, lamp.position[1] + lamp.array_step[1] * n,
                                      lamp.position[2] + lamp.array_step[2] * n};
                const glm::vec3 p{transform * glm::vec4{local, 1.0f}};
                diamond(p, scale(p) * 0.7f, 0xff40e0ffu, lines);
            }
        }
    }
}

namespace {

// The handle nearest the ray within its reach (a few pixels' worth); -1 none.
int nearest_handle(const std::vector<glm::vec3>& handles, const glm::vec3& eye, const glm::vec3& direction) {
    int best = -1;
    float best_distance = 1e30f;
    for (std::size_t i = 0; i < handles.size(); ++i) {
        const glm::vec3 v = handles[i] - eye;
        const float t = glm::dot(v, direction);
        if (t <= 0.0f) continue;
        const float off = glm::length(v - direction * t);
        if (off > (std::max)(0.06f, 0.02f * t)) continue;
        // The nearer to the ray, then the nearer to the eye.
        const float score = off / (std::max)(0.06f, 0.02f * t) + t * 1e-4f;
        if (score < best_distance) {
            best_distance = score;
            best = static_cast<int>(i);
        }
    }
    return best;
}

// Where the ray meets the plane through `point` facing `normal`.
std::optional<glm::vec3> on_plane(const glm::vec3& eye, const glm::vec3& direction, const glm::vec3& point, const glm::vec3& normal) {
    const float facing = glm::dot(direction, normal);
    if (std::abs(facing) < 1e-5f) return std::nullopt;
    const float t = glm::dot(point - eye, normal) / facing;
    if (t <= 0.0f) return std::nullopt;
    return eye + direction * t;
}

// Where on the line through `at` along `axis` (unit) the ray passes nearest.
std::optional<float> along_line(const glm::vec3& eye, const glm::vec3& direction, const glm::vec3& at, const glm::vec3& axis) {
    const glm::vec3 w = at - eye;
    const float b = glm::dot(axis, direction);
    const float denominator = 1.0f - b * b;
    if (denominator < 1e-6f) return std::nullopt;
    return (b * glm::dot(direction, w) - glm::dot(axis, w)) / denominator;
}

} // namespace

void Inspector::hover(const std::optional<glm::vec3>& eye, const std::optional<glm::vec3>& direction) {
    if (eye) eye_ = *eye;
    if (drag_) return; // the dragged one stays lit
    std::vector<glm::vec3> views;
    for (const auto& h : handles_) views.push_back(h.at);
    hovered_ = eye && direction && open_ ? nearest_handle(views, *eye, *direction) : -1;
}

bool Inspector::begin_drag(const glm::vec3& eye, const glm::vec3& direction, const InspectorWorld& world) {
    if (!open_) return false;
    std::vector<glm::vec3> views;
    for (const auto& h : handles_) views.push_back(h.at);
    const int index = nearest_handle(views, eye, direction);
    if (index < 0) return false;
    const auto handle = handles_[static_cast<std::size_t>(index)];
    if (handle.kind == Handle::Kind::lamp) {
        lamp_ = static_cast<int>(handle.lamp);
        lamp_state_ = 0;
        return true;
    }
    if (!start_drag(handle, eye, direction, world)) return false;
    hovered_ = index;
    return true;
}

// A drag of the chosen lamp by `handle` begun: the lamps as they are, one
// step of the undo.
bool Inspector::start_drag(const Handle& handle, const glm::vec3& eye, const glm::vec3& direction, const InspectorWorld& world) {
    const auto chosen = resolve(world);
    if (!chosen) return false;
    const auto geometry = lamp_geometry(world, chosen->shape, chosen->transform);
    if (!geometry) return false;
    Drag d;
    d.handle = handle;
    d.view = direction;
    d.start = eye + direction * glm::dot(handle.at - eye, direction);
    d.frame = geometry->frame;
    d.lamp_at = geometry->at;
    d.beam = geometry->beam;
    d.tangent = geometry->tangent_x;
    d.size = geometry->size;
    d.source = geometry->source * glm::length(glm::vec3{geometry->frame[0]});
    if (handle.kind == Handle::Kind::axis) {
        const glm::vec3 axis = glm::normalize(glm::vec3{geometry->frame[handle.axis]});
        d.along = along_line(eye, direction, geometry->at, axis).value_or(0.0f);
    }
    if (selection_.kind == Selection::Kind::car) {
        const auto* lights = world.trains ? world.trains->car_lights(selection_.train, selection_.car) : nullptr;
        if (!lights) return false;
        d.car = *lights;
    } else {
        const auto* model = world.library ? world.library->find(chosen->shape) : nullptr;
        if (!model) return false;
        d.shape = model->lights();
    }
    // The drag is one step of the undo.
    remember_lamps(LampSnapshot{.key = lamp_key(), .car = selection_.kind == Selection::Kind::car, .car_lights = d.car,
                                .shape_lamps = d.shape, .lamp = lamp_, .state = lamp_state_},
                   false);
    drag_ = std::move(d);
    return true;
}

void Inspector::drag(const glm::vec3& eye, const glm::vec3& direction, const InspectorWorld& world) {
    if (!drag_ || lamp_ < 0) return;
    if (gizmo_using_) return; // the ImGuizmo gizmo drives it (draw_lamp_gizmo)
    const auto& d = *drag_;
    const glm::mat4 to_numbers = glm::inverse(d.frame);
    const float frame_scale = (std::max)(glm::length(glm::vec3{d.frame[0]}), 1e-4f);
    // The move in the scene (axis, free), the new way (aim), the new cone or size (rim).
    std::optional<glm::vec3> moved, way;
    std::optional<float> tangent, size, source;
    switch (d.handle.kind) {
    case Handle::Kind::axis: {
        const glm::vec3 axis = glm::normalize(glm::vec3{d.frame[d.handle.axis]});
        if (const auto s = along_line(eye, direction, d.lamp_at, axis)) moved = axis * (*s - d.along);
        break;
    }
    case Handle::Kind::free:
        if (const auto p = on_plane(eye, direction, d.start, d.view)) moved = *p - d.start;
        break;
    case Handle::Kind::aim:
        if (const auto p = on_plane(eye, direction, d.start, d.view); p && glm::distance(*p, d.lamp_at) > 0.05f)
            way = glm::normalize(*p - d.lamp_at);
        break;
    case Handle::Kind::rim:
        if (const auto p = on_plane(eye, direction, d.start, d.view)) {
            const glm::vec3 rel = *p - d.lamp_at;
            const float along = glm::dot(rel, d.beam);
            const float radial = glm::length(rel - d.beam * along);
            tangent = along > 0.05f ? (std::max)(radial - d.source, 0.0f) / along : std::optional<float>{};
            size = glm::length(rel);
        }
        break;
    case Handle::Kind::source:
        if (const auto p = on_plane(eye, direction, d.start, d.view)) {
            const glm::vec3 rel = *p - d.lamp_at;
            source = glm::length(rel - d.beam * glm::dot(rel, d.beam)) / frame_scale;
            if (*source < 0.01f) source = 0.0f;
        }
        break;
    case Handle::Kind::lamp: break;
    }
    apply_drag(world, moved, way, tangent, size, source);
}

// The drag's change on the lamps as they were at its start: a move and a new
// way in the scene, the cone's tangent, a size, the source's radius.
void Inspector::apply_drag(const InspectorWorld& world, const std::optional<glm::vec3> moved, const std::optional<glm::vec3> way,
                           const std::optional<float> tangent, const std::optional<float> size, const std::optional<float> source) {
    if (!drag_ || lamp_ < 0) return;
    const auto& d = *drag_;
    const auto index = static_cast<std::size_t>(lamp_);
    const glm::mat4 to_numbers = glm::inverse(d.frame);
    const float frame_scale = (std::max)(glm::length(glm::vec3{d.frame[0]}), 1e-4f);
    if (!moved && !way && !tangent && !size && !source) return;
    const glm::vec3 local_move = moved ? glm::vec3{to_numbers * glm::vec4{*moved, 0.0f}} : glm::vec3{0.0f};
    const glm::vec3 local_way = way ? glm::normalize(glm::vec3{to_numbers * glm::vec4{*way, 0.0f}}) : glm::vec3{0.0f, 0.0f, 1.0f};
    if (selection_.kind == Selection::Kind::car) {
        auto* lights = world.trains ? world.trains->car_lights(selection_.train, selection_.car) : nullptr;
        if (!lights || index >= lights->size() || index >= d.car.size() || d.car[index].states.empty()) return;
        auto& l = (*lights)[index];
        const auto state = (std::min)(static_cast<std::size_t>((std::max)(lamp_state_, 0)), l.states.size() - 1);
        auto& s = l.states[state];
        const auto& was = d.car[index].states[state];
        if (moved) {
            for (int k = 0; k < 3; ++k) s.position[static_cast<std::size_t>(k)] = was.position[static_cast<std::size_t>(k)] + local_move[k];
        }
        if (way) {
            // Its forward is Ry(azimuth) Rx(elevation) (0, 0, 1) = (cos e sin a, -sin e, cos e cos a).
            s.azimuth[1] = glm::degrees(std::atan2(local_way.x, local_way.z));
            s.elevation[0] = glm::degrees(std::asin(std::clamp(-local_way.y, -1.0f, 1.0f)));
        }
        if (source) s.source_radius = *source;
        if (d.handle.kind == Handle::Kind::rim) {
            if (l.type != 0 && tangent) s.angle = std::clamp(glm::degrees(std::atan(*tangent)), 0.5f, 89.0f);
            if (l.type == 0 && size) s.radius = (std::max)(*size / frame_scale, 0.01f);
        }
        lamp_dirty_ = true;
        return;
    }
    const auto chosen = resolve(world);
    if (!chosen || index >= d.shape.size()) return;
    auto lamps = d.shape;
    auto& l = lamps[index];
    const auto& was = d.shape[index];
    if (moved) {
        for (std::size_t k = 0; k < 3; ++k) {
            l.position[k] = was.position[k] + local_move[static_cast<int>(k)];
            l.points_to[k] = was.points_to[k] + local_move[static_cast<int>(k)];
        }
    }
    if (way) {
        const glm::vec3 from{was.position[0], was.position[1], was.position[2]};
        const float length = (std::max)(glm::distance(from, glm::vec3{was.points_to[0], was.points_to[1], was.points_to[2]}), 0.1f);
        const glm::vec3 to = from + local_way * length;
        l.points_to = {to.x, to.y, to.z};
    }
    if (source) l.source_radius = *source;
    if (d.handle.kind == Handle::Kind::rim) {
        const float rx = (std::max)(was.radius[0], 0.01f), rz = (std::max)(was.radius[1] > 0.0f ? was.radius[1] : was.radius[0], 0.01f);
        float ratio = 1.0f;
        if (has_flag(was.flags, "light_cone") && tangent && d.tangent > 1e-4f) ratio = *tangent / d.tangent;
        if (!has_flag(was.flags, "light_cone") && size && d.size > 1e-4f) ratio = *size / d.size;
        ratio = std::clamp(ratio, 0.01f, 100.0f);
        l.radius = {rx * ratio, was.radius[1] > 0.0f ? rz * ratio : 0.0f};
    }
    apply_shape_lamps(world, chosen->shape, std::move(lamps));
}

glm::vec3& Inspector::lamp_euler_for(const glm::vec3& beam) {
    const auto key = lamp_key() + "#" + std::to_string(lamp_);
    const glm::vec3 b = glm::normalize(beam);
    if (key != lamp_euler_key_) {
        lamp_euler_key_ = key;
        lamp_euler_ = glm::vec3{0.0f};
    }
    if (glm::dot(euler_rotation(lamp_euler_)[2], b) < 0.99999f) {
        const float roll = lamp_euler_.z;
        lamp_euler_ = glm::vec3{glm::degrees(std::asin(std::clamp(-b.y, -1.0f, 1.0f))), glm::degrees(std::atan2(b.x, b.z)), roll};
    }
    return lamp_euler_;
}

namespace {
int gizmo_frame = -10;     // the ImGui frame the gizmo was last drawn in
bool gizmo_mouse = false;  // over it or held then
bool gizmo_square = false; // the mouse in its window's square then
} // namespace

bool lamp_gizmo_square_hovered() {
#ifdef OPENRTS_HAS_IMGUI
    return gizmo_square && ImGui::GetFrameCount() - gizmo_frame <= 1;
#else
    return false;
#endif
}

bool lamp_gizmo_wants_mouse() {
#ifdef OPENRTS_HAS_IMGUI
    return gizmo_mouse && ImGui::GetFrameCount() - gizmo_frame <= 1;
#else
    return false;
#endif
}

// The chosen lamp's ImGuizmo gizmo, over the scene: rings to turn it (its
// beam), arrows to move it; a use one step of the undo (start_drag), each
// frame of it applied as a drag (apply_drag).
void Inspector::draw_lamp_gizmo(const InspectorWorld& world) {
#ifdef OPENRTS_HAS_IMGUI
    gizmo_over_ = false;
    const auto chosen = resolve(world);
    if (!chosen) {
        gizmo_using_ = false;
        return;
    }
    const auto geometry = lamp_geometry(world, chosen->shape, chosen->transform);
    if (!geometry) {
        gizmo_using_ = false;
        return;
    }
    ImGuizmo::SetOrthographic(false);
    const auto& io = ImGui::GetIO();
    // ImGuizmo's projection: OpenGL's (-1..1 depth), the camera's angle.
    const glm::mat4 projection = glm::perspective(camera_fov_, camera_aspect_, 0.05f, 10000.0f);
    // Its own window, a square round the gizmo taking the mouse: ImGuizmo
    // takes the mouse only while its draw list's window is the hovered one;
    // over the background's (or a whole-screen one that takes none) another
    // window under the mouse, the scene's overlay, kept it from the rings
    // (testers 2026-11-27: taken only here and there). Outside the square
    // the scene has the mouse as before.
    const glm::vec4 clip = projection * camera_view_ * glm::vec4{geometry->at, 1.0f};
    if (clip.w <= 0.01f) {
        gizmo_using_ = false;
        return;
    }
    const ImVec2 centre{(clip.x / clip.w * 0.5f + 0.5f) * io.DisplaySize.x, (0.5f - clip.y / clip.w * 0.5f) * io.DisplaySize.y};
    const float half = 0.17f * io.DisplaySize.y;
    ImGui::SetNextWindowPos(ImVec2{centre.x - half, centre.y - half});
    ImGui::SetNextWindowSize(ImVec2{half * 2.0f, half * 2.0f});
    ImGui::Begin("##lamp_gizmo", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings
                     | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove
                     | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus);
    // Behind every panel: a panel over it hides it and has its clicks.
    ImGui::BringWindowToDisplayBack(ImGui::GetCurrentWindow());
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y);
    struct EndWindow {
        ~EndWindow() { ImGui::End(); }
    } end_window;
    ImGui::GetWindowDrawList()->PushClipRectFullScreen(); // arrows past the square drawn whole
    gizmo_square = ImGui::IsWindowHovered();
    // A shape's lamp: in the shape's axes (scale out), turned by its angles.
    const formats::ShapeLight* shape_lamp = nullptr;
    if (selection_.kind == Selection::Kind::world) {
        if (const auto* model = world.library ? world.library->find(chosen->shape) : nullptr;
            model && static_cast<std::size_t>(lamp_) < model->lights().size())
            shape_lamp = &model->lights()[static_cast<std::size_t>(lamp_)];
    }
    glm::mat4 axes{1.0f};
    for (int k = 0; k < 3; ++k) axes[k] = glm::vec4{glm::normalize(glm::vec3{geometry->frame[k]}), 0.0f};
    axes[3] = geometry->frame[3];
    if (!gizmo_using_ && shape_lamp) {
        const glm::vec3 from{shape_lamp->position[0], shape_lamp->position[1], shape_lamp->position[2]};
        glm::vec3 to{shape_lamp->points_to[0], shape_lamp->points_to[1], shape_lamp->points_to[2]};
        if (glm::distance(from, to) < 1e-3f) to = from - glm::vec3{0.0f, 1.0f, 0.0f};
        const glm::mat3 r = euler_rotation(lamp_euler_for(to - from));
        glm::mat4 local{r};
        local[3] = glm::vec4{glm::vec3{glm::inverse(axes) * glm::vec4{geometry->at, 1.0f}}, 1.0f};
        gizmo_place_ = axes * local;
    } else if (!gizmo_using_) {
        // The lamp's place in the scene: its axes, z its beam.
        gizmo_place_ = glm::mat4{1.0f};
        const glm::vec3 z = glm::normalize(geometry->beam);
        glm::vec3 x = glm::normalize(glm::vec3{geometry->place[0]});
        x = glm::normalize(x - z * glm::dot(x, z));
        gizmo_place_[0] = glm::vec4{x, 0.0f};
        gizmo_place_[1] = glm::vec4{glm::cross(z, x), 0.0f};
        gizmo_place_[2] = glm::vec4{z, 0.0f};
        gizmo_place_[3] = glm::vec4{geometry->at, 1.0f};
    }
    const auto operation = gizmo_operation_ == 0 ? ImGuizmo::OPERATION(ImGuizmo::ROTATE_X | ImGuizmo::ROTATE_Y | ImGuizmo::ROTATE_Z) : ImGuizmo::TRANSLATE; // no screen ring (the white one)
    // Shift: in steps (1 degree, 1 cm). Ctrl: a turn a quarter as fast
    // (ImGuizmo turns by each frame's angle, so a quarter of each frame's
    // turn is a quarter of the whole).
    const bool steps = io.KeyShift, slow = io.KeyCtrl;
    const float snap[3] = {operation == ImGuizmo::TRANSLATE ? 0.01f : 1.0f, 0.01f, 0.01f};
    const glm::mat4 before = gizmo_place_;
    const bool changed = ImGuizmo::Manipulate(&camera_view_[0][0], &projection[0][0], operation,
                                              gizmo_space_ == 1 ? ImGuizmo::WORLD : ImGuizmo::LOCAL, &gizmo_place_[0][0],
                                              nullptr, steps ? snap : nullptr);
    if (changed && slow && !steps && operation != ImGuizmo::TRANSLATE) {
        const glm::quat from = glm::quat_cast(glm::mat3{before});
        const glm::quat to = glm::quat_cast(glm::mat3{gizmo_place_});
        const glm::mat3 r = glm::mat3_cast(glm::slerp(from, to, 0.25f));
        for (int k = 0; k < 3; ++k) gizmo_place_[k] = glm::vec4{r[k] * glm::length(glm::vec3{gizmo_place_[k]}), 0.0f};
    }
    const bool using_now = ImGuizmo::IsUsing();
    gizmo_over_ = ImGuizmo::IsOver();
    gizmo_frame = ImGui::GetFrameCount();
    gizmo_mouse = gizmo_over_ || using_now;
    if (using_now && !gizmo_using_) {
        drag_.reset();
        if (!start_drag(Handle{.kind = Handle::Kind::free, .at = geometry->at}, eye_, geometry->beam, world)) return;
    }
    gizmo_using_ = using_now;
    if (!using_now) {
        if (drag_ && drag_->handle.kind == Handle::Kind::free && drag_->view == geometry->beam) drag_.reset();
        return;
    }
    if (changed && drag_) {
        const glm::vec3 at{gizmo_place_[3]};
        const glm::vec3 beam = glm::normalize(glm::vec3{gizmo_place_[2]});
        if (operation == ImGuizmo::TRANSLATE) {
            apply_drag(world, at - drag_->lamp_at, std::nullopt, std::nullopt, std::nullopt, std::nullopt);
        } else if (shape_lamp) {
            // The angles from the turned axes, in the shape's; the beam their +z.
            const glm::mat4 local = glm::inverse(axes) * gizmo_place_;
            glm::mat3 r{local};
            for (int k = 0; k < 3; ++k) r[k] = glm::normalize(r[k]);
            lamp_euler_ = euler_of(r);
            apply_drag(world, std::nullopt, glm::normalize(glm::vec3{axes * glm::vec4{r[2], 0.0f}}), std::nullopt, std::nullopt, std::nullopt);
            // The beam just set: the angles are these (lamp_euler_for keeps them).
        } else {
            apply_drag(world, std::nullopt, beam, std::nullopt, std::nullopt, std::nullopt);
        }
    }
#else
    static_cast<void>(world);
#endif
}

} // namespace openrts::viewer
