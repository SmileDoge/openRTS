#include "brake_settings_ui.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <vector>

#include <openrts/ui/text.hpp>

#include "viewer_state.hpp"

#ifdef OPENRTS_HAS_IMGUI
#include <imgui.h>
#endif

namespace openrts::viewer {

using ui::tr;

namespace {

// The presets, the КЛУБ's object line and map aside (sub-settings of the
// КЛУБ the player keeps whichever preset).
constexpr BrakeSettings openrts_brakes{};
constexpr BrakeSettings rts_brakes{.realistic = false, .dll_air = 0, .gauges = false, .rails = 0, .limit = false, .stability = false,
                                   .condition = 0, .pressing = 100, .own_brakes = 0, .brake_numbers = 0, .own_klub = 0,
                                   .no_brakes_dll = false};

[[nodiscard]] bool same_preset(BrakeSettings a, const BrakeSettings& b) {
    a.klub_objects = b.klub_objects;
    a.klub_map = b.klub_map;
    return a == b;
}

[[nodiscard]] int remembered_int(const char* key, const int fallback) {
    const auto value = remembered(key);
    return value && !value->empty() ? std::atoi(value->c_str()) : fallback;
}

// The player's own set, remembered as "brakes_custom": twelve figures
// (2026-11-22); six or eight before (the air's settings only), the devices
// then as openRTS's.
[[nodiscard]] std::string custom_code(const BrakeSettings& s) {
    std::ostringstream out;
    out << (s.realistic ? 1 : 0) << ',' << s.dll_air << ',' << (s.gauges ? 1 : 0) << ',' << s.rails << ',' << (s.limit ? 1 : 0) << ','
        << (s.stability ? 1 : 0) << ',' << s.condition << ',' << s.pressing << ',' << s.own_brakes << ',' << s.brake_numbers << ','
        << s.own_klub << ',' << (s.no_brakes_dll ? 1 : 0);
    return out.str();
}

[[nodiscard]] bool custom_from(const std::string& code, BrakeSettings& s) {
    std::vector<int> v;
    std::istringstream in(code);
    std::string field;
    while (std::getline(in, field, ',')) {
        if (field.empty()) return false;
        v.push_back(std::atoi(field.c_str()));
    }
    if (v.size() != 6 && v.size() != 8 && v.size() != 12) return false;
    v.resize(12, -1);
    s.realistic = v[0] != 0;
    s.dll_air = std::clamp(v[1], 0, 2);
    s.gauges = v[2] != 0;
    s.rails = std::clamp(v[3], 0, 6);
    s.limit = v[4] != 0;
    s.stability = v[5] != 0;
    if (v[6] >= 0) s.condition = std::clamp(v[6], 0, 10);
    if (v[7] >= 0) s.pressing = std::clamp(v[7], 50, 150);
    if (v[8] >= 0) s.own_brakes = std::clamp(v[8], 0, 1);
    if (v[9] >= 0) s.brake_numbers = std::clamp(v[9], 0, 1);
    if (v[10] >= 0) s.own_klub = std::clamp(v[10], 0, 1);
    if (v[11] >= 0) s.no_brakes_dll = v[11] != 0;
    return true;
}

#ifdef OPENRTS_HAS_IMGUI
float label_width = 200.0f; // draw_brake_settings's, for its rows

void row(const char* label) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    const char* id = std::strstr(label, "##");
    ImGui::TextUnformatted(label, id); // not the id after ##
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
}

[[nodiscard]] bool begin_rows(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit)) return false;
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, label_width);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

// One dim line under a control: what the choice made gives, or what the
// game sees now.
void note(const std::string& text) {
    if (text.empty()) return;
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}


// A combo over `labels` whose entries stand for `values`.
bool choice(const char* id, int& value, const std::vector<std::pair<const char*, int>>& options) {
    int shown = 0;
    for (std::size_t k = 0; k < options.size(); ++k)
        if (options[k].second == value) shown = static_cast<int>(k);
    bool changed = false;
    if (ImGui::BeginCombo(id, options[static_cast<std::size_t>(shown)].first)) {
        for (std::size_t k = 0; k < options.size(); ++k) {
            if (ImGui::Selectable(options[k].first, static_cast<int>(k) == shown) && options[k].second != value) {
                value = options[k].second;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}
#endif

} // namespace

BrakeSettings brake_preset(const BrakePreset preset, const BrakeSettings& now) {
    BrakeSettings out = preset == BrakePreset::rts ? rts_brakes : openrts_brakes;
    if (preset == BrakePreset::own) {
        out = now;
        if (const auto code = remembered("brakes_custom")) custom_from(*code, out);
    }
    out.klub_objects = now.klub_objects;
    out.klub_map = now.klub_map;
    return out;
}

BrakePreset brake_preset_of(const BrakeSettings& s) {
    if (same_preset(s, openrts_brakes)) return BrakePreset::openrts;
    if (same_preset(s, rts_brakes)) return BrakePreset::rts;
    return BrakePreset::own;
}

BrakeSettings load_brake_settings() {
    BrakeSettings s;
    s.realistic = remembered_int("realistic_air", 1) != 0;
    s.dll_air = std::clamp(remembered_int("dll_air", 2), 0, 2);
    s.gauges = remembered_int("gauges_openrts_air", 1) != 0;
    s.rails = std::clamp(remembered_int("rails", 1), 0, 6);
    s.limit = remembered_int("limit_brake_force", 1) != 0;
    s.stability = remembered_int("distributor_stability", 1) != 0;
    s.condition = std::clamp(remembered_int("brake_condition", s.realistic ? 6 : 0), 0, 10);
    s.pressing = std::clamp(remembered_int("brake_pressing", 100), 50, 150);
    s.own_brakes = std::clamp(remembered_int("own_brakes", 1), 0, 1);
    s.brake_numbers = std::clamp(remembered_int("brake_numbers", 0), 0, 1);
    s.own_klub = std::clamp(remembered_int("own_klub", 1), 0, 1);
    s.no_brakes_dll = remembered_int("no_brakes_dll", 1) != 0;
    s.klub_objects = remembered_int("klub_objects", 0) != 0;
    s.klub_map = remembered_int("klub_map", 1) != 0;
    return s;
}

void remember_brake_settings(const BrakeSettings& s) {
    const auto flag = [](const bool on) { return on ? "1" : "0"; };
    remember("realistic_air", flag(s.realistic));
    remember("dll_air", std::to_string(s.dll_air));
    remember("gauges_openrts_air", flag(s.gauges));
    remember("rails", std::to_string(s.rails));
    remember("limit_brake_force", flag(s.limit));
    remember("distributor_stability", flag(s.stability));
    remember("brake_condition", std::to_string(s.condition));
    remember("brake_pressing", std::to_string(s.pressing));
    remember("own_brakes", std::to_string(s.own_brakes));
    remember("brake_numbers", std::to_string(s.brake_numbers));
    remember("own_klub", std::to_string(s.own_klub));
    remember("no_brakes_dll", flag(s.no_brakes_dll));
    remember("klub_objects", flag(s.klub_objects));
    remember("klub_map", flag(s.klub_map));
    if (brake_preset_of(s) == BrakePreset::own) remember("brakes_custom", custom_code(s));
}

bool draw_brake_settings(BrakeSettings& s, const BrakeSettingsStatus* status, const float labels) {
#ifdef OPENRTS_HAS_IMGUI
    label_width = labels;
    const BrakeSettings before = s;
    const BrakeSettingsStatus none;
    const auto& now = status ? *status : none;

    if (capture_panel() >= 0) ImGui::SetScrollHereY(0.0f);
    // The preset and what it gives.
    if (begin_rows("##brake_preset")) {
        row(tr("Brakes##preset_row"));
        int preset = static_cast<int>(brake_preset_of(s));
        const std::vector<std::pair<const char*, int>> presets = {
            {tr("openRTS (recommended)"), static_cast<int>(BrakePreset::openrts)},
            {tr("as in RTS"), static_cast<int>(BrakePreset::rts)},
            {tr("my own"), static_cast<int>(BrakePreset::own)},
        };
        if (choice("##brake_preset_choice", preset, presets)) s = brake_preset(static_cast<BrakePreset>(preset), s);
        const auto shown = brake_preset_of(s);
        note(tr(shown == BrakePreset::openrts ? "openRTS's air, valves and КЛУБ; Brakes.dll is not loaded where it is not needed."
                : shown == BrakePreset::rts   ? "RTS's air model and Brakes.dll, as RTS has them."
                                              : "Set by hand under \"Details\"."));
        if (!now.train.empty()) {
            row(tr("This train"));
            ImGui::TextWrapped("%s", now.train.c_str());
        }
        ImGui::EndTable();
    }

    if (capture_panel() >= 0) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    if (ImGui::TreeNode(tr("Details##brake_details"))) {
        if (begin_rows("##brake_details_rows")) {
            row(tr("openRTS's air"));
            ImGui::Checkbox(tr("on##realistic_air"), &s.realistic);
            if (!s.realistic) note(tr("Off: RTS's air model; the settings that need openRTS's air are hidden."));
            if (s.realistic) {
                row(tr("Trains with DLLs"));
                choice("##dll_air", s.dll_air, {{tr("openRTS's valve"), 2}, {tr("the DLL's valve"), 1}, {tr("RTS's air"), 0}});
                if (s.dll_air != 0) {
                    row(tr("Brake devices"));
                    choice("##own_brakes", s.own_brakes, {{"openRTS", 1}, {"Brakes.dll", 0}});
                    if (s.own_brakes == 1) {
                        row(tr("Their numbers"));
                        choice("##brake_numbers", s.brake_numbers, {{tr("as Brakes.dll's"), 0}, {tr("from the books"), 1}});
                    }
                    row("КЛУБ");
                    choice("##own_klub", s.own_klub, {{"openRTS", 1}, {"Brakes.dll", 0}});
                    if (s.own_brakes == 1 && s.own_klub == 1) {
                        row("Brakes.dll");
                        ImGui::Checkbox(tr("not loaded where it is not needed"), &s.no_brakes_dll);
                        note(tr("From the next train."));
                    }
                    if (s.own_klub == 1) {
                        row(tr("КЛУБ's object line"));
                        ImGui::Checkbox(tr("platforms, crossings and limits"), &s.klub_objects);
                        row(tr("Electronic map (ЭК)"));
                        ImGui::Checkbox(tr("from the route database"), &s.klub_map);
                        if (s.klub_map) note(now.map);
                    }
                }
                row(tr("Cab gauges"));
                ImGui::Checkbox(tr("show openRTS's air"), &s.gauges);
                row(tr("Brake condition"));
                {
                    std::array<std::string, 11> names;
                    for (int c = 0; c <= 10; ++c) names[static_cast<std::size_t>(c)] = std::to_string(c);
                    names[0] = tr("as the files give it");
                    names[1] = tr("1: due for an overhaul");
                    names[6] = tr("6: in service");
                    names[10] = tr("10: from the works");
                    if (ImGui::BeginCombo("##brake_condition", names[static_cast<std::size_t>(s.condition)].c_str())) {
                        for (int c = 0; c <= 10; ++c)
                            if (ImGui::Selectable(names[static_cast<std::size_t>(c)].c_str(), c == s.condition)) s.condition = c;
                        ImGui::EndCombo();
                    }
                }
                note(now.condition);
                row(tr("Shoe pressing"));
                ImGui::SliderInt("##brake_pressing", &s.pressing, 50, 150, "%d %%");
                note(now.pressing);
            } else {
                row(tr("Distributors"));
                ImGui::Checkbox(tr("a slow pipe fall does not brake"), &s.stability);
            }
            row(tr("Rails"));
            choice("##rails", s.rails,
                   {{tr("by the weather"), 1}, {tr("dry"), 2}, {tr("wet"), 3}, {tr("snow"), 4}, {tr("ice"), 5}, {tr("leaves"), 6}, {tr("as RTS (no weather)"), 0}});
            note(now.rails);
            row(tr("Brake force"));
            ImGui::Checkbox(tr("at most real stock's"), &s.limit);
            ImGui::EndTable();
        }
        ImGui::TreePop();
    }
    if (s == before) return false;
    remember_brake_settings(s);
    return true;
#else
    (void)s;
    (void)status;
    (void)labels;
    return false;
#endif
}

} // namespace openrts::viewer
