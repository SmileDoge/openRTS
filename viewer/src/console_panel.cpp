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
#include <set>
#include <string>
#include <string_view>

#include <imgui.h>

#include <openrts/core/virtual_path.hpp>
#include <openrts/net/wire.hpp>
#include <openrts/simulation/signals.hpp>

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

    // Station routes and signals, as the server's window has them.
    if (ImGui::CollapsingHeader(tr("Station routes")) && state) station_routes(net, *state);
    if (ImGui::CollapsingHeader(tr("Signals"))) signals(net, trains);

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

void ConsolePanel::station_routes(NetSession& net, const net::ConsoleState& state) {
    static constexpr const char* states[] = {"waiting", "set", "set part of the way", "the train on it", "left"};
    const auto state_text = [](const net::ConsoleSegment& g) {
        return std::string(tr(g.state < 5 ? states[g.state] : "?")) + ", " + std::to_string(g.thrown) + "/" + std::to_string(g.junctions);
    };
    const auto segment_text = [](const net::ConsoleSegment& g) {
        return net::to_utf8(g.station) + ": " + net::to_utf8(g.name) + (g.reverse ? tr(" (from its end)") : "");
    };
    const auto number_of = [&](const std::uint32_t id) {
        for (const auto& t : state.trains) {
            if (t.id == id) return t.number;
        }
        return id;
    };
    if (!selected_train_) {
        ImGui::TextDisabled("%s", tr("Pick a train in the list: the map shows it."));
    } else {
        ImGui::Text(tr("Train %u"), number_of(selected_train_));
        // Its queue (rts_server.exe's upper list).
        ImGui::SeparatorText(tr("Its queue"));
        std::uint32_t last = 0;
        for (const auto& g : state.segments) {
            if (g.train != selected_train_) continue;
            last = g.id;
            char label[48];
            std::snprintf(label, sizeof label, "%s##cancel%u", tr("Cancel"), g.id);
            if (ImGui::SmallButton(label)) net.send_command("segment cancel " + std::to_string(g.id));
            ImGui::SameLine();
            ImGui::Text("%s, %s", segment_text(g).c_str(), state_text(g).c_str());
        }
        if (last == 0) ImGui::TextDisabled("%s", tr("nothing queued"));
        // Asked again when the train or its queue's end changes, the way is
        // turned, and every 3 s (the train moves on).
        const auto key = std::to_string(selected_train_) + "/" + std::to_string(last);
        if (key != next_key_) {
            next_key_ = key;
            back_ = false;
            picked_.reset();
            next_asked_ = -10.0;
        }
        const double now = ImGui::GetTime();
        if (now - next_asked_ >= 3.0) {
            net.ask_next_routes(selected_train_, back_);
            next_asked_ = now;
        }
        // What can be set next, from there, the way chosen.
        ImGui::SeparatorText(tr("Routes it can take"));
        const auto& next = net.next_routes();
        const bool current = next && next->train == selected_train_ && next->back == back_;
        if (current && next->found) {
            const auto after = net::to_utf8(next->after_name);
            const auto from = next->after != 0 ? (back_ ? trf("From the end of %s, back (it reverses there)", after.c_str()) : trf("From the end of %s", after.c_str()))
                                                : std::string(back_ ? tr("From the train's rear (it goes rear first)") : tr("From the train's head"));
            ImGui::TextUnformatted(from.c_str());
        }
        if (ImGui::Button(back_ ? tr("Direction: back (change)") : tr("Direction: ahead (change)"))) {
            back_ = !back_;
            picked_.reset();
            next_asked_ = -10.0;
        }
        ImGui::TextDisabled("%s", tr("green: main track; lilac: onto a wrong track; [П] a platform, then the track's length"));
        std::string picked_name;
        if (!current) {
            ImGui::TextDisabled("%s", tr("asking the server..."));
        } else if (!next->found) {
            ImGui::TextDisabled("%s", tr("the train is not on the server's track"));
        } else if (ImGui::BeginListBox("##available", ImVec2(-FLT_MIN, 170.0f))) {
            if (next->routes.empty()) ImGui::TextDisabled("%s", tr("none from here this way"));
            for (const auto& c : next->routes) {
                std::string label = net::to_utf8(c.station) + ": " + net::to_utf8(c.name);
                if (c.reverse) label += tr(" (from its end)");
                if (c.platform) label += " [П]";
                if (c.track_length > 0.0f) label += ' ' + trf("%.0f m", c.track_length);
                if (c.distance > 50.0f) label += "  " + trf("in %.1f km", c.distance / 1000.0f);
                const bool is = picked_ && picked_->first == c.route && picked_->second == c.reverse;
                if (is) picked_name = net::to_utf8(c.name);
                label += "##next" + std::to_string(c.route) + (c.reverse ? "r" : "");
                const bool colored = c.wrong_way || c.main_track;
                if (c.wrong_way) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.55f, 1.0f, 1.0f));
                else if (c.main_track) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.95f, 0.45f, 1.0f));
                if (ImGui::Selectable(label.c_str(), is, ImGuiSelectableFlags_AllowDoubleClick)) {
                    picked_ = std::pair{c.route, c.reverse};
                    picked_name = net::to_utf8(c.name);
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) set_picked(net, *next);
                }
                if (colored) ImGui::PopStyleColor();
            }
            ImGui::EndListBox();
        }
        ImGui::Checkbox(tr("hold the next"), &hold_);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        ImGui::InputInt(tr("priority"), &priority_);
        priority_ = (std::max)(priority_, 0);
        ImGui::BeginDisabled(!picked_ || !current);
        const auto set_label = picked_ && !picked_name.empty() ? trf("Set: %s", picked_name.c_str()) : std::string(tr("Set the route picked"));
        if (ImGui::Button(set_label.c_str()) && current) set_picked(net, *next);
        ImGui::EndDisabled();
        if (!picked_) ImGui::TextDisabled("%s", tr("pick a route in the list (a double click sets it)"));
    }
    // Every train's queue (rts_server.exe's lower list).
    ImGui::SeparatorText(tr("Every train's queue"));
    if (state.segments.empty()) {
        ImGui::TextDisabled("%s", tr("nothing queued"));
    } else if (ImGui::BeginTable("##segments", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn(tr("train"));
        ImGui::TableSetupColumn(tr("station route"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("state"));
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (const auto& g : state.segments) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%u", number_of(g.train));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(segment_text(g).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(state_text(g).c_str());
            ImGui::TableNextColumn();
            char label[48];
            std::snprintf(label, sizeof label, "%s##cancel_all%u", tr("Cancel"), g.id);
            if (ImGui::SmallButton(label)) net.send_command("segment cancel " + std::to_string(g.id));
        }
        ImGui::EndTable();
    }
}

// The route picked, queued as the server's "segment" line: from its end,
// holding the next, rear first when the train runs so from there, its
// priority.
void ConsolePanel::set_picked(NetSession& net, const net::ConsoleNext& next) {
    if (!picked_ || !selected_train_) return;
    std::string line = "segment " + std::to_string(selected_train_) + " " + std::to_string(picked_->first);
    if (picked_->second) line += " back";
    if (hold_) line += " hold";
    if (next.rear_first) line += " rear";
    if (priority_ > 0) line += " priority " + std::to_string(priority_);
    net.send_command(line);
    picked_.reset();
    next_asked_ = -10.0;
}

void ConsolePanel::signals(NetSession& net, const TrainView& trains) {
    if (!types_read_) {
        // The types of the route's heads, as the server's window lists them.
        types_read_ = true;
        if (const auto* system = trains.signals()) {
            std::set<std::string> found;
            for (const auto& head : system->heads()) {
                if (head.type) found.insert(head.type->name);
            }
            types_.assign(found.begin(), found.end());
        }
    }
    ImGui::TextWrapped("%s", tr("A signal's own actions: right-click it on the map. Here: every signal of a type, and the defaults."));
    const char* type = type_ >= 0 && type_ < static_cast<int>(types_.size()) ? types_[static_cast<std::size_t>(type_)].c_str() : tr("a type...");
    if (ImGui::BeginCombo(tr("Signal type"), type)) {
        for (std::size_t i = 0; i < types_.size(); ++i) {
            if (ImGui::Selectable(types_[i].c_str(), type_ == static_cast<int>(i))) type_ = static_cast<int>(i);
        }
        ImGui::EndCombo();
    }
    ImGui::BeginDisabled(type_ < 0 || type_ >= static_cast<int>(types_.size()));
    const auto picked = type_ >= 0 && type_ < static_cast<int>(types_.size()) ? in_quotes(types_[static_cast<std::size_t>(type_)]) : std::string();
    const auto by_type = [&](const char* label, const char* mode) {
        if (ImGui::Button(label)) net.send_command("signal type " + picked + " " + mode);
    };
    by_type(tr("Manual"), "manual");
    ImGui::SameLine();
    by_type(tr("Open"), "open");
    ImGui::SameLine();
    by_type(tr("Automatic block"), "auto");
    if (ImGui::Button(tr("Show as a train signal"))) net.send_command("signal type " + picked + " normal on");
    ImGui::SameLine();
    if (ImGui::Button(tr("Do not show as a train signal"))) net.send_command("signal type " + picked + " normal off");
    ImGui::EndDisabled();
    ImGui::SeparatorText(tr("Defaults"));
    if (ImGui::Button(tr("Restore the defaults"))) net.send_command("signal defaults");
    if (ImGui::Button(tr("Set as the defaults"))) net.send_command("signal set-defaults");
    if (ImGui::Button(tr("Save the route database"))) net.send_command("signal save");
    ImGui::TextWrapped("%s", tr("Saved on the server, into its own folder (the installation is not touched)."));
}

} // namespace openrts::viewer
#endif
