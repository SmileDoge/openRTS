#include "graphics_presets_ui.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>

#include "ui_text.hpp"

#ifdef OPENRTS_HAS_IMGUI
#include <imgui.h>
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace openrts::viewer {
namespace {

void say(OwnPresetsUi& ui, std::string text, const bool bad = false) {
    ui.status = std::move(text);
    ui.status_bad = bad;
}

// A name not taken by another preset with other settings: "name (2)", ...
[[nodiscard]] std::string free_name(const std::vector<OwnGraphicsPreset>& presets, const OwnGraphicsPreset& preset) {
    const auto clashes = [&](const std::string& name) {
        return std::any_of(presets.begin(), presets.end(), [&](const OwnGraphicsPreset& p) {
            return p.name == name && !(p.settings == preset.settings);
        });
    };
    if (!clashes(preset.name)) return preset.name;
    for (int i = 2; i < 100; ++i) {
        auto name = preset.name + " (" + std::to_string(i) + ")";
        if (!clashes(name)) return name;
    }
    return preset.name;
}

void set_name(OwnPresetsUi& ui, const std::string& name) {
    std::snprintf(ui.name, sizeof ui.name, "%s", name.c_str());
}

} // namespace

bool draw_own_graphics_presets(OwnPresetsUi& ui, GraphicsSettings& settings, const float width) {
#ifdef OPENRTS_HAS_IMGUI
    if (!ui.listed) {
        ui.presets = load_own_graphics_presets();
        ui.listed = true;
        if (ui.chosen >= static_cast<int>(ui.presets.size())) ui.chosen = -1;
    }
    bool changed = false;
    ImGui::PushID("own_presets");

    // The ones in the folder: the one the settings are now shows first.
    int matching = -1;
    for (std::size_t i = 0; i < ui.presets.size(); ++i)
        if (ui.presets[i].settings == settings) matching = static_cast<int>(i);
    if (ui.chosen < 0) ui.chosen = matching;
    const char* shown = ui.chosen >= 0 ? ui.presets[static_cast<std::size_t>(ui.chosen)].name.c_str()
                        : ui.presets.empty() ? tr("(no presets saved yet)")
                                             : tr("(choose a preset)");
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo("##list", shown)) {
        for (std::size_t i = 0; i < ui.presets.size(); ++i) {
            const auto label = ui.presets[i].name + (static_cast<int>(i) == matching ? std::string("  ") + tr("(now)") : std::string()) + "##"
                               + std::to_string(i);
            if (ImGui::Selectable(label.c_str(), static_cast<int>(i) == ui.chosen)) {
                ui.chosen = static_cast<int>(i);
                set_name(ui, ui.presets[i].name);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(ui.chosen < 0);
    if (ImGui::Button(tr("Apply"))) {
        const auto& preset = ui.presets[static_cast<std::size_t>(ui.chosen)];
        settings = preset.settings;
        changed = true;
        say(ui, trf("Preset \"%s\" applied.", preset.name.c_str()));
        std::cout << "graphics preset applied: " << preset.name << std::endl;
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Delete"))) {
        const auto preset = ui.presets[static_cast<std::size_t>(ui.chosen)];
        if (delete_own_graphics_preset(preset)) {
            say(ui, trf("Preset \"%s\" deleted.", preset.name.c_str()));
        } else {
            say(ui, tr("Could not delete the preset's file."), true);
        }
        ui.chosen = -1;
        ui.listed = false;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("Refresh"))) ui.listed = false;

    ImGui::SetNextItemWidth(width);
    ImGui::InputTextWithHint("##name", tr("Name of the preset"), ui.name, sizeof ui.name);
    ImGui::SameLine();
    if (ImGui::Button(tr("Save the current settings as a preset"))) {
        std::string name = ui.name;
        while (!name.empty() && name.back() == ' ') name.pop_back();
        if (name.empty()) {
            say(ui, tr("Give the preset a name."), true);
        } else if (save_own_graphics_preset({.name = name, .settings = settings})) {
            say(ui, trf("Preset \"%s\" saved.", name.c_str()));
            ui.listed = false;
            ui.chosen = -1;
        } else {
            say(ui, tr("Could not write the preset's file."), true);
        }
    }

    if (ImGui::Button(tr("Copy the code of the current settings"))) {
        std::string name = ui.name;
        while (!name.empty() && name.back() == ' ') name.pop_back();
        if (name.empty()) name = tr("My preset");
        const auto code = graphics_preset_code({.name = name, .settings = settings});
        ImGui::SetClipboardText(code.c_str());
        say(ui, trf("The code of \"%s\" is in the clipboard (%zu characters): paste it into a message.", name.c_str(), code.size()));
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Paste a preset from the clipboard"))) {
        const char* clip = ImGui::GetClipboardText();
        auto preset = clip ? read_graphics_preset(clip, settings) : std::nullopt;
        if (!preset) {
            say(ui, tr("There is no openRTS graphics preset in the clipboard."), true);
        } else {
            if (preset->name.empty()) preset->name = tr("A preset from the clipboard");
            if (!ui.listed) ui.presets = load_own_graphics_presets();
            preset->name = free_name(ui.presets, *preset);
            settings = preset->settings;
            changed = true;
            const bool kept = save_own_graphics_preset(*preset);
            say(ui, kept ? trf("Preset \"%s\" taken from the clipboard, applied and saved.", preset->name.c_str())
                         : trf("Preset \"%s\" applied, but its file could not be written.", preset->name.c_str()),
                !kept);
            set_name(ui, preset->name);
            std::cout << "graphics preset from the clipboard: " << preset->name << std::endl;
            ui.listed = false;
            ui.chosen = -1;
        }
    }
#ifdef _WIN32
    ImGui::SameLine();
    if (ImGui::Button(tr("Open the presets folder")))
        ShellExecuteW(nullptr, L"open", graphics_presets_directory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
    if (!ui.status.empty()) {
        if (ui.status_bad) {
            ImGui::TextColored(ImVec4{0.95f, 0.55f, 0.45f, 1.0f}, "%s", ui.status.c_str());
        } else {
            ImGui::TextColored(ImVec4{0.55f, 0.90f, 0.60f, 1.0f}, "%s", ui.status.c_str());
        }
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", tr("To share: \"Copy the code\" and send it in a message (Discord, a forum); the other player copies the message and presses \"Paste a preset from the clipboard\". A preset is also a file in the presets folder, to be sent as a file. A preset holds every graphics setting here; anti-aliasing and HDR from it take effect from the next start."));
    ImGui::PopStyleColor();
    ImGui::PopID();
    return changed;
#else
    (void)ui;
    (void)settings;
    (void)width;
    return false;
#endif
}

} // namespace openrts::viewer
