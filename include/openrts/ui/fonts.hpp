#pragma once

#include <filesystem>

struct ImFont;

// The interface's fonts (openrts_ui): a monospace face with Cyrillic for the
// interface, Arial for map labels, Segoe UI for the start menu's title.
namespace openrts::ui {

// Where the shell's own fonts are (Montserrat, Onest, OFL): the folder copied
// beside openrts_game.exe. Empty: only the system fonts.
void set_font_folder(std::filesystem::path folder);

// Into the current ImGui context's atlas: the interface font first (it
// becomes the default), then the others. ImGui's built-in font, Latin only,
// when no file is found.
void load_fonts();

// The shell's faces (menu_ui): Montserrat for the brand, headings and
// buttons, Onest for descriptions. Null when the folder has no files: the
// other faces stand in.
ImFont*& brand_font();     // DARK TEAM RAILWAY ENGINE, about 33 px
ImFont*& heading_font();   // card titles, about 32 px
ImFont*& clock_font();     // the menu's clock, about 30 px
ImFont*& button_font();    // buttons and rows, about 15 px
ImFont*& body_font();      // descriptions, about 15 px
ImFont*& small_font();     // captions, about 13 px

// The driver's summary (F4, viewer/drive_panel.cpp): Montserrat and Onest at
// the sizes the panel was drawn at. System faces of the same size stand in
// when the shell's folder is not there (openrts_viewer alone).
ImFont*& hud_title_font();    // ДВИЖЕНИЕ, 32 px
ImFont*& hud_label_font();    // СКОРОСТЬ, 15 px
ImFont*& hud_value_font();    // the speed, 55 px
ImFont*& hud_pressure_font(); // the limit and the pressures, 35 px
ImFont*& hud_section_font();  // section titles, 15 px
ImFont*& hud_text_font();     // the signal's and platform's names, 15 px
ImFont*& hud_small_font();    // the acceleration and the hint, 15 px
ImFont*& hud_tiny_font();     // the unit line, 10 px

// Arial with Cyrillic for labels on maps (Track control's station names, as
// RTS writes them in Arial 16). Null when the font file is not there: the UI
// font is used, enlarged.
inline ImFont*& map_label_font() {
    static ImFont* font = nullptr;
    return font;
}

// The start menu's title ("openRTS", about 34 px) and its large button
// (about 21 px, with Cyrillic), drawn at their own size rather than the UI
// font scaled up, which blurs. Null when no font file is there.
inline ImFont*& title_font() {
    static ImFont* font = nullptr;
    return font;
}
inline ImFont*& large_font() {
    static ImFont* font = nullptr;
    return font;
}

} // namespace openrts::ui
