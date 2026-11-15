#include "players_panel.hpp"

#include <cmath>
#include <cstdio>
#include <string>

#ifdef OPENRTS_HAS_IMGUI
#include <imgui.h>
#endif

#include <openrts/formats/route_world.hpp>

#include "net_session.hpp"
#include "train_view.hpp"
#include "ui_text.hpp"

namespace openrts::viewer {

namespace {

glm::vec3 scene_of(const formats::RoutePosition& at, const formats::RoutePosition& origin) {
    constexpr double tile = formats::route_tile_size;
    return {static_cast<float>((at.tile_x - origin.tile_x) * tile + at.x - origin.x), static_cast<float>(at.y - origin.y),
            static_cast<float>((at.tile_z - origin.tile_z) * tile + at.z - origin.z)};
}

std::string distance_text(const float metres) {
    char text[32];
    if (metres >= 1000.0f) {
        std::snprintf(text, sizeof text, tr("%.1f km"), metres / 1000.0f);
    } else {
        std::snprintf(text, sizeof text, tr("%.0f m"), metres);
    }
    return text;
}

} // namespace

void collect_player_labels(const NetSession& net, std::vector<WorldLabel>& labels, const glm::vec3& eye,
                           const formats::RoutePosition& origin, const float range) {
    for (const auto& p : net.players()) {
        const auto at = scene_of(p.at, origin);
        const float d = glm::length(at - eye);
        if (d > range || d < 1.5f) continue;
        // A little over his eye; through walls too: he is to be found.
        labels.push_back({.position = at + glm::vec3{0.0f, 0.45f, 0.0f},
                          .text = p.nick + "  " + distance_text(d),
                          .colour = 0xff80ffc0u,
                          .occludable = false,
                          .key = 0x504C0000u + p.user});
    }
}

bool draw_players_section(NetSession& net, const TrainView& trains, const glm::vec3& eye, const formats::RoutePosition& origin,
                          bool& names) {
    bool changed = false;
#ifdef OPENRTS_HAS_IMGUI
    (void)trains;
    ImGui::SeparatorText(tr("Players"));
    changed = ImGui::Checkbox(tr("Names of the players over the world"), &names);
    const auto& help = net.help();
    const bool helping = help && help->state == net::HelperState::helping;
    const bool waiting = help && help->state == net::HelperState::waiting;
    if (helping) {
        ImGui::TextWrapped("%s", trf("You are %s's helper (train %s).", help->nick.c_str(), net.train_label(help->train).c_str()).c_str());
        if (ImGui::Button(tr("Stop helping"))) net.stop_helping();
    } else if (waiting) {
        ImGui::TextWrapped("%s", trf("Asked %s to take you as the helper: waiting for the answer.", help->nick.c_str()).c_str());
        if (ImGui::Button(tr("Cancel"))) net.stop_helping();
    } else if (help && help->state == net::HelperState::refused) {
        ImGui::TextWrapped("%s", trf("%s did not take you as the helper.", help->nick.c_str()).c_str());
    } else if (help && help->state == net::HelperState::cannot) {
        ImGui::TextWrapped("%s", tr("Cannot be the helper there: nobody drives that train with openRTS."));
    }
    const auto players = net.players();
    if (players.empty()) ImGui::TextDisabled("%s", tr("No other players heard yet."));
    for (const auto& p : players) {
        const float d = glm::length(scene_of(p.at, origin) - eye);
        if (p.train != 0) {
            ImGui::TextUnformatted(
                trf("%s: in train %s, %s", p.nick.c_str(), net.train_label(p.train).c_str(), distance_text(d).c_str()).c_str());
            // In his train: ask to help him (an openRTS server only).
            if (net.openrts_server() && !helping && !waiting) {
                ImGui::SameLine();
                const std::string label = std::string(tr("Ask to be the helper")) + "##ask" + std::to_string(p.user);
                if (ImGui::SmallButton(label.c_str())) net.ask_to_help(p.train);
            }
        } else {
            ImGui::TextUnformatted(trf("%s: on foot or the free camera, %s", p.nick.c_str(), distance_text(d).c_str()).c_str());
        }
    }
    if (!net.openrts_server() && !players.empty()) ImGui::TextDisabled("%s", tr("Helpers are possible on an openRTS server only."));
    if (!net.helpers().empty()) {
        ImGui::TextUnformatted(tr("Your helpers:"));
        for (const auto& h : net.helpers()) {
            ImGui::BulletText("%s", h.nick.c_str());
            ImGui::SameLine();
            const std::string label = std::string(tr("Let go")) + "##drop" + std::to_string(h.user);
            if (ImGui::SmallButton(label.c_str())) net.drop_helper(h.user);
        }
    }
#else
    (void)net;
    (void)trains;
    (void)eye;
    (void)origin;
    (void)names;
#endif
    return changed;
}

void draw_helper_windows(NetSession& net) {
#ifdef OPENRTS_HAS_IMGUI
    if (net.asking().empty()) return;
    const auto display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({display.x * 0.5f, display.y * 0.2f}, ImGuiCond_Appearing, {0.5f, 0.0f});
    ImGui::SetNextWindowBgAlpha(0.85f);
    if (ImGui::Begin(tr("Engineer's helper###helper_ask"), nullptr,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        // A copy: answering takes the player off the list.
        const auto asking = net.asking();
        for (const auto& a : asking) {
            ImGui::TextUnformatted(trf("%s asks to be your helper (train %s).", a.nick.c_str(), net.train_label(a.train).c_str()).c_str());
            const std::string take = std::string(tr("Take")) + "##take" + std::to_string(a.user);
            const std::string refuse = std::string(tr("Refuse")) + "##refuse" + std::to_string(a.user);
            if (ImGui::Button(take.c_str())) net.answer(a.user, true);
            ImGui::SameLine();
            if (ImGui::Button(refuse.c_str())) net.answer(a.user, false);
        }
    }
    ImGui::End();
#else
    (void)net;
#endif
}

} // namespace openrts::viewer
