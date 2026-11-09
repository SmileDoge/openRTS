#include "console_panel.hpp"

#include <openrts/formats/rolling_stock.hpp>

namespace openrts::viewer {

std::optional<net::ConsolePlace> consist_place(const core::Vfs& vfs, const std::string& consist, std::string& error) {
    const auto definition = formats::load_consist(vfs, consist);
    if (!definition.ok()) {
        error = definition.error;
        return std::nullopt;
    }
    net::ConsolePlace place;
    auto name = consist;
    if (const auto slash = name.find_last_of("/\\"); slash != std::string::npos) name = name.substr(slash + 1);
    if (name.size() > 4 && core::fold_case(name.substr(name.size() - 4)) == ".con") name.resize(name.size() - 4);
    place.consist = net::from_utf8(name);
    place.max_velocity = definition.max_velocity;
    for (const auto& entry : definition.entries) {
        place.cars.push_back({.folder = net::from_utf8(entry.folder), .name = net::from_utf8(entry.name), .engine = entry.engine,
                              .flipped = entry.flipped, .cold = entry.cold, .handbrake = entry.handbrake ? *entry.handbrake : -1.0f});
    }
    if (place.cars.empty()) {
        error = "the consist has no cars";
        return std::nullopt;
    }
    return place;
}

} // namespace openrts::viewer

#ifdef OPENRTS_HAS_IMGUI
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <string_view>

#include <imgui.h>

#include <openrts/core/virtual_path.hpp>
#include <openrts/net/wire.hpp>

#include "net_session.hpp"
#include "track_control.hpp"
#include "train_view.hpp"
#include "ui_text.hpp"

namespace openrts::viewer {

namespace {

const char* kind_name(const std::uint16_t kind) {
    switch (kind) {
    case 0: return tr("standing (STATIC)");
    case 1: return tr("a player's");
    case 2: return tr("the server's (TRAFFIC)");
    case 3: return tr("rolling (LOOSE)");
    default: return "?";
    }
}

std::string clock_text(const double seconds) {
    const auto s = static_cast<int>(seconds);
    char text[16];
    std::snprintf(text, sizeof text, "%02d:%02d:%02d", s / 3600 % 24, s / 60 % 60, s % 60);
    return text;
}

// A command word with spaces in quotes.
std::string in_quotes(const std::string& text) { return "\"" + text + "\""; }

// The consist's name from its virtual path (TRAINS/CONSISTS_RTS/<name>.con).
std::string consist_name(const std::string& path) {
    auto name = path;
    if (const auto slash = name.find_last_of("/\\"); slash != std::string::npos) name = name.substr(slash + 1);
    if (name.size() > 4 && core::fold_case(name.substr(name.size() - 4)) == ".con") name.resize(name.size() - 4);
    return name;
}

} // namespace

void ConsolePanel::draw(NetSession& net, TrainView& trains, TrackControl* map, const core::Vfs& vfs) {
    ImGui::SetNextWindowSize(ImVec2(520.0f, 640.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(tr("Dispatcher's console###console_panel"))) {
        ImGui::End();
        return;
    }
    const auto& state = net.console_state();
    // A train picked on the map is picked here too.
    if (map && map->selected() != map_selected_) {
        map_selected_ = map->selected();
        if (const auto id = net.server_train(map_selected_)) selected_train_ = id;
    }
    const auto user_name = [&](const std::uint32_t id) -> std::string {
        if (!state || id == 0) return {};
        for (const auto& u : state->users) {
            if (u.id == id) return net::to_utf8(u.nick);
        }
        return {};
    };

    // The time and the pause.
    if (state) {
        ImGui::Text(tr("Time %s"), clock_text(state->time).c_str());
        ImGui::SameLine();
        if (state->paused) {
            ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.1f, 1.0f), "%s", tr("paused"));
            ImGui::SameLine();
            if (ImGui::Button(tr("Resume"))) net.send_command("resume");
        } else if (ImGui::Button(tr("Pause"))) {
            net.send_command("pause");
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        ImGui::InputTextWithHint("##time", tr("HH:MM"), time_, sizeof time_);
        ImGui::SameLine();
        if (ImGui::Button(tr("Set the time")) && time_[0]) net.send_command("time " + std::string(time_));
    } else {
        ImGui::TextDisabled("%s", tr("waiting for the server's state..."));
    }

    // The trains.
    if (ImGui::CollapsingHeader(tr("Trains"), ImGuiTreeNodeFlags_DefaultOpen) && state) {
        if (ImGui::BeginTable("##trains", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 160.0f))) {
            ImGui::TableSetupColumn(tr("No."));
            ImGui::TableSetupColumn(tr("Kind"));
            ImGui::TableSetupColumn(tr("Driver"));
            ImGui::TableSetupColumn(tr("Speed"));
            ImGui::TableSetupColumn(tr("Route"));
            ImGui::TableHeadersRow();
            for (const auto& t : state->trains) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                char label[64];
                std::snprintf(label, sizeof label, "%u (%u)##train%u", t.number, t.id, t.id);
                if (ImGui::Selectable(label, selected_train_ == t.id, ImGuiSelectableFlags_SpanAllColumns)) {
                    selected_train_ = t.id;
                    if (map) {
                        if (const auto view = net.view_train(t.id)) map->show_train(trains, view);
                    }
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(kind_name(t.kind));
                ImGui::TableNextColumn();
                const auto owner = user_name(t.owner);
                ImGui::TextUnformatted(owner.empty() ? "-" : owner.c_str());
                ImGui::TableNextColumn();
                if (t.stands) {
                    ImGui::TextDisabled("%s", tr("stands"));
                } else {
                    ImGui::Text(tr("%.0f km/h"), std::abs(t.speed) * 3.6);
                }
                ImGui::TableNextColumn();
                if (t.route_junctions) {
                    ImGui::Text(tr("%u junctions"), t.route_junctions);
                } else {
                    ImGui::TextDisabled("-");
                }
            }
            ImGui::EndTable();
        }
        const auto* chosen = [&]() -> const net::ConsoleTrain* {
            for (const auto& t : state->trains) {
                if (t.id == selected_train_) return &t;
            }
            return nullptr;
        }();
        if (chosen) {
            const auto id = std::to_string(chosen->id);
            ImGui::Text(tr("Train %u, %u cars"), chosen->number, chosen->cars);
            // Who drives it.
            ImGui::SetNextItemWidth(200.0f);
            const auto current = user_name(selected_user_);
            if (ImGui::BeginCombo("##assign_to", current.empty() ? tr("a player...") : current.c_str())) {
                for (const auto& u : state->users) {
                    if (u.console || !u.connected) continue;
                    const auto nick = net::to_utf8(u.nick);
                    if (ImGui::Selectable(nick.c_str(), u.id == selected_user_)) selected_user_ = u.id;
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(current.empty());
            if (ImGui::Button(tr("Give to the player"))) net.send_command("assign " + id + " " + in_quotes(current));
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(chosen->owner == 0);
            if (ImGui::Button(tr("Take back"))) net.send_command("unassign " + id);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button(tr("Remove"))) net.send_command("remove " + id);
            // Its route to a signal.
            ImGui::SetNextItemWidth(200.0f);
            ImGui::InputTextWithHint("##route_to", tr("signal (litera), e.g. NM1"), signal_, sizeof signal_);
            ImGui::SameLine();
            if (ImGui::Button(tr("Set the route")) && signal_[0]) net.send_command("route " + id + " " + std::string(signal_));
            ImGui::SameLine();
            ImGui::BeginDisabled(chosen->route_junctions == 0);
            if (ImGui::Button(tr("Cancel the route"))) net.send_command("route " + id + " cancel");
            ImGui::EndDisabled();
            ImGui::TextDisabled("%s", tr("Several signal names in turn give a route through them. The junctions stay set until the train's rear has passed them."));
        } else {
            ImGui::TextDisabled("%s", tr("Pick a train in the list: the map shows it."));
        }
    }

    // A new train.
    if (ImGui::CollapsingHeader(tr("New train"))) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##consist_filter", tr("filter the consists"), consist_filter_, sizeof consist_filter_);
        std::string filter = consist_filter_;
        for (auto& c : filter) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ImGui::BeginListBox("##consists", ImVec2(-FLT_MIN, 130.0f))) {
            for (const auto& path : trains.consist_list()) {
                const auto name = consist_name(path);
                std::string lower = name;
                for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (!filter.empty() && lower.find(filter) == std::string::npos) continue;
                if (ImGui::Selectable((name + "##" + path).c_str(), path == consist_)) consist_ = path;
            }
            ImGui::EndListBox();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##path", path_.empty() ? tr("the path whose start it stands at...") : path_.c_str())) {
            for (const auto& p : trains.route_paths()) {
                if (ImGui::Selectable(p.file.c_str(), p.file == path_)) path_ = p.file;
            }
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(100.0f);
        ImGui::InputTextWithHint("##number", tr("number"), number_, sizeof number_);
        ImGui::SameLine();
        ImGui::BeginDisabled(consist_.empty() || path_.empty());
        if (ImGui::Button(tr("Put it on the track"))) {
            // By its cars: the consist is ours, the server needs only the cars.
            std::string error;
            if (auto place = consist_place(vfs, consist_, error)) {
                place->path = net::from_utf8(path_);
                place->number = number_[0] ? static_cast<std::uint32_t>(std::strtoul(number_, nullptr, 10)) : 0u;
                net.send_place(*place);
            } else {
                net.note(consist_name(consist_) + ": " + error);
            }
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled("%s", tr("It stands with its rear at the path's start; then give it to a player."));
    }

    // The players.
    if (ImGui::CollapsingHeader(tr("Players"), ImGuiTreeNodeFlags_DefaultOpen) && state) {
        if (ImGui::BeginTable("##users", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
            ImGui::TableSetupColumn(tr("Nick"));
            ImGui::TableSetupColumn(tr("Status"));
            ImGui::TableSetupColumn(tr("Trains"));
            ImGui::TableHeadersRow();
            for (const auto& u : state->users) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const auto nick = net::to_utf8(u.nick);
                if (ImGui::Selectable((nick + "##user" + std::to_string(u.id)).c_str(), selected_user_ == u.id, ImGuiSelectableFlags_SpanAllColumns)) {
                    selected_user_ = u.id;
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(u.console ? tr("dispatcher") : !u.connected ? tr("away") : u.playing ? tr("playing") : tr("in the menu"));
                ImGui::TableNextColumn();
                std::string list;
                for (const auto t : u.trains) {
                    for (const auto& train : state->trains) {
                        if (train.id == t) list += (list.empty() ? "" : ", ") + std::to_string(train.number);
                    }
                }
                ImGui::TextUnformatted(list.empty() ? "-" : list.c_str());
            }
            ImGui::EndTable();
        }
        const auto who = user_name(selected_user_);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##message", who.empty() ? tr("a message to everyone") : trf("a message to %s", who.c_str()).c_str(), message_,
                                 sizeof message_);
        if (ImGui::Button(tr("Send")) && message_[0]) {
            net.send_command("say " + (who.empty() ? std::string("*") : in_quotes(who)) + " " + std::string(message_));
            message_[0] = 0;
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("To everyone"))) selected_user_ = 0;
        ImGui::SameLine();
        ImGui::BeginDisabled(who.empty());
        if (ImGui::Button(tr("Drop the player"))) net.send_command("kick " + in_quotes(who));
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (state->admits) {
            if (ImGui::Button(tr("Take no new players"))) net.send_command("admit off");
        } else if (ImGui::Button(tr("Take new players"))) {
            net.send_command("admit on");
        }
    }

    // What the server answered, and any command.
    if (ImGui::CollapsingHeader(tr("Log and commands"), ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto& log = net.console_log();
        if (ImGui::BeginChild("##log", ImVec2(0.0f, 140.0f), ImGuiChildFlags_Borders)) {
            for (const auto& line : log) ImGui::TextUnformatted(line.c_str());
            if (log.size() != log_seen_) {
                ImGui::SetScrollHereY(1.0f);
                log_seen_ = log.size();
            }
        }
        ImGui::EndChild();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextWithHint("##command", tr("a server command (help)"), command_, sizeof command_, ImGuiInputTextFlags_EnterReturnsTrue) &&
            command_[0]) {
            net.send_command(command_);
            command_[0] = 0;
            ImGui::SetKeyboardFocusHere(-1);
        }
    }
    ImGui::End();
}

} // namespace openrts::viewer
#endif
