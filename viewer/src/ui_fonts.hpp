#pragma once

struct ImFont;

namespace openrts::viewer {

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

} // namespace openrts::viewer
