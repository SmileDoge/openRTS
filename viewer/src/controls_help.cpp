#include "controls_help.hpp"

#include "key_layout.hpp"
#include "ui_text.hpp"

#include <string>

#ifdef OPENRTS_HAS_IMGUI
#include <imgui.h>
#endif

namespace openrts::viewer {

#ifdef OPENRTS_HAS_IMGUI
namespace {

// A section: its title, then a table of keys and what they do.
bool begin_section(const char* title, const char* id) {
    ImGui::Spacing();
    ImGui::SeparatorText(tr(title));
    constexpr auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable(id, 2, flags)) return false;
    ImGui::TableSetupColumn("##keys", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 9.5f);
    ImGui::TableSetupColumn("##action", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

// `keys` and `action` are the English texts (translated here).
void row(const char* keys, const char* action) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(tr(keys));
    ImGui::TableSetColumnIndex(1);
    ImGui::TextWrapped("%s", tr(action));
}

void note(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", tr(text));
    ImGui::PopStyleColor();
}

} // namespace
#endif

void draw_controls_help() {
#ifdef OPENRTS_HAS_IMGUI
    const char* layout = key_layout() == KeyLayout::rts ? tr("RTS (Num0 / Num1)") : tr("openRTS (no numeric keypad)");
    ImGui::TextWrapped("%s", trf("Key layout: %s (Settings, \"Keys\").", layout).c_str());

    if (begin_section("Camera and world", "##controls_camera")) {
        row("RMB + mouse", "look around");
        row("W A S D", "move the free camera");
        row("Q / E", "down / up");
        row("Shift", "faster");
        row("F", "free / orbit camera");
        row("V", "the camera follows the train");
        row("C", "put the train on the track by the camera");
        row("J", "throw the switch ahead");
        row("T", "track diagram (yellow lines) on / off");
        row("L", "switch and signal labels");
        row("R", "renderer: RTS / experimental");
        row("Pause", "pause");
        row("Alt+Enter", "full screen / window");
        ImGui::EndTable();
    }

    if (begin_section("Train without a DLL", "##controls_train")) {
        row("Home / End", "throttle");
        row("Page Up / Page Down", "reverser");
        row("] / [", "driver's brake valve (395)");
        row(". / ,", "independent brake");
        row("K", "EP brake on / off");
        row("Space (hold)", "horn");
        row("B", "whistle on / off");
        row("X (hold)", "sand");
        row("P", "pantographs down / up");
        row("Y / U", "left / right doors (standing)");
        ImGui::EndTable();
    }
    note("A locomotive with a DLL is driven from its cab with the mouse and the cab keys of its .sd, as in RTS: \"Cab keys of the locomotive DLL\" in the Train window gives them the keyboard (the camera keys are off then, the F keys work).");

    if (begin_section("Engineer and cab", "##controls_cab")) {
        row("Enter", "stand at the desk / sit / open a door / climb up");
        row("Ctrl+Enter", "step into the gangway; at a door outside: climb up");
        row("Arrows, Shift, End", "walk, run, slower");
        row(window_key_name(), "open the window");
        row(head_out_key_name(), "lean out");
        row(car_menu_key_name(), "the car menu at the coupling or bogie under the cursor (outside)");
        row("RMB + mouse", "look around");
        row("Mouse", "click: a control; wheel: one position; drag: handles and blinds");
        row("Alt", "captions of the cab's controls");
        ImGui::EndTable();
    }
    note("The 3D cab: \"3D cab view\" in the Train window.");

    if (begin_section("Information (as in RTS)", "##controls_info")) {
        row("F5", "the train's cars; again: the next page");
        row("Shift+F5", "the train as a whole");
        row("Ctrl+F5", "speed limits and the kilometre");
        row("F6", "the stations ahead");
        row("F7", "platform and siding names");
        row("Ctrl+F7", "signal names");
        row("Ctrl+Alt+F7", "the Track control window");
        row("Ctrl+F6", "save the view for a bug report");
        ImGui::EndTable();
    }
#endif
}

} // namespace openrts::viewer
