#pragma once

struct ImFont;

// The interface's fonts (openrts_ui): a monospace face with Cyrillic for the
// interface, Arial for map labels, Segoe UI for the start menu's title.
namespace openrts::ui {

// Into the current ImGui context's atlas: the interface font first (it
// becomes the default), then the others. ImGui's built-in font, Latin only,
// when no file is found.
void load_fonts();

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
