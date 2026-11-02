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

} // namespace openrts::viewer
