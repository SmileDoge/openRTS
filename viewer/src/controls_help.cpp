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
        row("Side button / Caps Lock", "mouse lock: the view always turns with the mouse, a click works what the dot in the middle is on; again: the cursor back");
        row("Wheel held + wheel", "zoom while held; let go: the view comes back");
        row("Ctrl+wheel", "zoom (smooth; in the cab too)");
        row("W A S D", "move the free camera");
        row("Q / E", "down / up");
        row("Shift", "faster");
        row("F", "free / orbit camera");
        row("Ctrl+F", "from the cab (or on foot) to the free camera; again: back into the cab (without one, or the dispatcher: on foot where the camera is)");
        row("V", "the camera follows the train");
        row("T", "track diagram (yellow lines) on / off");
        row("L", "switch and signal labels");
        row("F10", "renderer: RTS / experimental");
        row("Pause / F1", "pause");
        row("Alt+Enter", "full screen / window");
        ImGui::EndTable();
    }

    if (begin_section("Locomotive without a DLL (openRTS only)", "##controls_train")) {
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
    note("RTS cannot drive a locomotive without a DLL; openRTS drives MSTS stock without one and Track control's test trains with these keys.");
    note("A locomotive with a DLL is driven from its cab with the mouse and the cab keys of its .sd, as in RTS: on foot (the 3D cab) the keys work at once; with the free camera \"Cab keys of the locomotive DLL\" in the Train window gives them the keyboard (the camera keys are off then, the F keys work).");

    if (begin_section("Engineer and cab", "##controls_cab")) {
        row("Enter", "in the chair: rise at the desk and sit back / sit down / open a door / climb up");
        row("Ctrl+Enter", "get up from the chair, step into the gangway; at a door outside: climb up");
        if (key_layout() == KeyLayout::openrts) {
            row("E", "on foot: as Enter (a door, a window, the chair, a switch); with nothing at hand the car menu at the coupling or bogie in front; seated: the cab's key");
            row("Shift+E", "on foot: as Ctrl+Enter (climb in, go out, go through, a ladder)");
        }
        row("Arrows, Shift, End", "walk, run, slower");
        row(walk_collision_key_name(), "on foot outside: walk through the world's objects / collide with them again");
        row(window_key_name(), "open the window");
        row(head_out_key_name(), "lean out");
        row(car_menu_key_name(), "the car menu at the coupling or bogie under the cursor (outside)");
        row(flashlight_key_name(), "the flashlight on / off");
        row(clock_key_name(), "the time of day on the screen / off (shown anyway while time runs faster)");
        row(tape_key_name(), "the tape (RTS_TAPES, read by RTS's lenta.xls): start on the locomotive driven / stop");
        if (key_layout() == KeyLayout::rts) row("Num5", "which locomotive the tape records");
        row("F12", "settings for developers and the inspector (the map's objects)");
        row("RMB + mouse", "look around");
        row("Side button / Caps Lock", "mouse lock: the view always turns with the mouse, a click works what the dot in the middle is on; again: the cursor back");
        row("Wheel held + wheel", "zoom while held; let go: the view comes back");
        row("Mouse", "click: a control; wheel: a handle one position (not buttons and switches); drag: handles and blinds");
        row("F3", "view from the first car, again from the last, again back; the wheel nearer, Ctrl+wheel zoom");
        row("Wheel at the chair", "off the controls: the head nearer to what the cursor points at, or back");
        row("Ctrl+wheel", "zoom (smooth; in the cab too)");
        row("Num+ / Num-", "field of view narrower / wider, as RTS (15..90 degrees)");
        row("Num* / Ctrl+0", "field of view back as it started, the zoom off");
        row("Alt", "captions of the cab's controls");
        ImGui::EndTable();
    }
    note("The 3D cab: \"3D cab view\" in the Train window.");

    if (begin_section("Information (as in RTS)", "##controls_info")) {
        row("Esc", "the openRTS panel and the DLL cab window: show / hide");
        row("F5", "the train's cars; again: the next page");
        row("Shift+F5", "the train as a whole");
        row("Ctrl+F5", "speed limits and the kilometre");
        row("F4", "a small card: the time, the speed, the limit now and next, the signal ahead (openRTS)");
        row("Ctrl+F4", "the track's profile at the top: the grade, the kilometres, the limits, the signals and the stations; the wheel over it changes the span, a drag shifts it, a double click puts it back (openRTS)");
        row("F6", "the stations ahead and the sidings (yellow); in a scenario with a timetable, the timetable");
        row(board_key_name(), "a scenario's stop: start the boarding of the passengers");
        row("F7", "platform and siding names");
        row("Ctrl+F7", "signal names");
        row("Ctrl+Alt+F7", "the Track control window");
        row("F11", "the scenario's window: its messages");
        row("F8", "the scenario's menu: reports");
        row("Ctrl+F9", "the speed recorder tape: the tapes drawn as on a 3SL-2M's paper (openRTS)");
        row("Ctrl+F6", "save the view for a bug report");
        ImGui::EndTable();
    }
#endif
}

} // namespace openrts::viewer
