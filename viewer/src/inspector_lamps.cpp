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

#include <glm/exponential.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <openrts/renderer/model_assets.hpp>
#include <openrts/renderer/model_library.hpp>
#include <openrts/renderer/model_renderer.hpp>
#include <openrts/renderer/photometric.hpp>

#include "train_view.hpp"
#include "ui_text.hpp"

#ifdef OPENRTS_HAS_IMGUI
#include <imgui.h>
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

// The gizmo: the lamp's place and axes (x red, y green, z blue: the way it
// shines), and its light: a cone (`tangent` of its half angles) to `reach`,
// or a sphere of `reach`.
void lamp_gizmo(const glm::mat4& place, const bool cone, const float tangent_x, const float tangent_y, const float reach,
                const std::uint32_t colour, std::vector<renderer::WideLine>& lines) {
    const glm::vec3 at{place[3]};
    const auto axis = [&](const int k) { return glm::normalize(glm::vec3{place[k]}); };
    lines.push_back({at, at + axis(0) * 0.4f, 0xff3040ffu, 3.0f});
    lines.push_back({at, at + axis(1) * 0.4f, 0xff40ff40u, 3.0f});
    lines.push_back({at, at + axis(2) * 0.8f, 0xffff8040u, 3.0f});
    const float d = std::clamp(reach, 0.05f, gizmo_reach);
    constexpr int segments = 32;
    constexpr float two_pi = 6.2831853f;
    if (cone) {
        lines.push_back({at, at + axis(2) * d, colour, 1.5f});
        glm::vec3 last{};
        for (int s = 0; s <= segments; ++s) {
            const float a = two_pi * static_cast<float>(s) / segments;
            const glm::vec3 local{std::cos(a) * tangent_x * d, std::sin(a) * tangent_y * d, d};
            const glm::vec3 p{place * glm::vec4{local, 1.0f}};
            if (s > 0) lines.push_back({last, p, colour, 1.5f});
            if (s % 8 == 0 && s < segments) lines.push_back({at, p, colour, 1.0f});
            last = p;
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
    ImGui::SeparatorText(tr("Photometric (PBR / HDR)"));
    ImGui::TextDisabled("%s", tr("Real units for the experimental path: the light falls off as 1 / d², its colour by its temperature. 0: as RTS."));
    changed |= ImGui::DragFloat(tr("Intensity, cd (on its axis)"), &candela, 50.0f, 0.0f, 2000000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
    changed |= ImGui::DragFloat(tr("Luminous flux, lm (if no cd)"), &lumens, 20.0f, 0.0f, 500000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
    changed |= ImGui::DragFloat(tr("Colour temperature, K (0: its colour)"), &kelvin, 10.0f, 0.0f, 12000.0f, "%.0f");
    if (kelvin > 0.0f) {
        const auto c = glm::pow(renderer::kelvin_colour(kelvin) / 1.5f, glm::vec3{1.0f / 2.2f});
        ImGui::SameLine();
        ImGui::ColorButton("##kelvin", {c.x, c.y, c.z, 1.0f}, ImGuiColorEditFlags_NoTooltip, {18.0f, 18.0f});
    }
    ImGui::TextDisabled("%s", tr("A locomotive headlight 100000-300000 cd, a marker lamp 50-500 cd; a street lamp 5000-20000 lm, a platform LED 3000-8000 lm, a saloon lamp 500-1500 lm; sodium 2000 K, a bulb 2700 K, an LED 4000-5000 K."));
    float lux = renderer::lux_per_unit();
    if (ImGui::DragFloat(tr("Lux a unit of the scene's light (every lamp)"), &lux, 0.25f, 1.0f, 500.0f, "%.1f")) renderer::lux_per_unit() = lux;
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
        changed |= ImGui::DragFloat3(tr("Position (the shape's x, y, z), m"), l.position.data(), 0.01f);
        changed |= ImGui::DragFloat3(tr("Points to, m"), l.points_to.data(), 0.01f);
        changed |= ImGui::DragFloat2(tr("Radius at the points-to plane (x, z), m"), l.radius.data(), 0.02f, 0.0f, 500.0f);
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
        const Handle handle{.kind = chosen ? Handle::Kind::free : Handle::Kind::lamp, .at = at, .lamp = index};
        diamond(at, scale(at) * (chosen ? 1.6f : 1.0f), colour_of(chosen ? 0xff20e0ffu : 0xffffd040u), lines);
        handles_.push_back(handle);
    }
    if (!geometry) return;
    const auto& g = *geometry;
    // Its arrows along the frame's x, y, z: dragged, the lamp moves along them.
    const float arrow = scale(g.at) * 14.0f;
    static constexpr std::uint32_t axis_colours[3] = {0xff3040ffu, 0xff40ff40u, 0xffff8040u};
    for (int k = 0; k < 3; ++k) {
        const glm::vec3 tip = g.at + glm::normalize(glm::vec3{g.frame[k]}) * arrow;
        const auto c = colour_of(axis_colours[k]);
        lines.push_back({g.at, tip, c, 3.0f});
        diamond(tip, scale(tip) * 0.9f, c, lines);
        handles_.push_back({.kind = Handle::Kind::axis, .at = tip, .axis = k});
    }
    // Its light: the cone (or the sphere), its end to aim it, its rim to open it.
    const float d = g.cone ? std::clamp(g.reach, 0.5f, gizmo_reach) : g.size;
    lamp_gizmo(g.place, g.cone, g.tangent_x, g.tangent_y, d, 0xff40e0ffu, lines);
    if (g.cone) {
        const glm::vec3 end = g.at + g.beam * d;
        diamond(end, scale(end) * 1.3f, colour_of(0xff40e0ffu), lines);
        handles_.push_back({.kind = Handle::Kind::aim, .at = end});
        const glm::vec3 rim{g.place * glm::vec4{g.tangent_x * d, 0.0f, d, 1.0f}};
        diamond(rim, scale(rim), colour_of(0xffff40ffu), lines);
        handles_.push_back({.kind = Handle::Kind::rim, .at = rim});
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
    hovered_ = index;
    return true;
}

void Inspector::drag(const glm::vec3& eye, const glm::vec3& direction, const InspectorWorld& world) {
    if (!drag_ || lamp_ < 0) return;
    const auto& d = *drag_;
    const auto index = static_cast<std::size_t>(lamp_);
    const glm::mat4 to_numbers = glm::inverse(d.frame);
    const float frame_scale = (std::max)(glm::length(glm::vec3{d.frame[0]}), 1e-4f);
    // The move in the scene (axis, free), the new way (aim), the new cone or size (rim).
    std::optional<glm::vec3> moved, way;
    std::optional<float> tangent, size;
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
            tangent = along > 0.05f ? radial / along : std::optional<float>{};
            size = glm::length(rel);
        }
        break;
    case Handle::Kind::lamp: break;
    }
    if (!moved && !way && !tangent && !size) return;
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

} // namespace openrts::viewer
