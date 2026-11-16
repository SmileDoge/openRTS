#pragma once

// The presets of one's own (graphics_settings.hpp, OwnGraphicsPreset) in the
// start menu's Graphics page and the game's panel: choose one, save the
// settings as one, copy one's code for another player, take one from the
// clipboard, open their folder.

#include <string>
#include <vector>

#include "graphics_settings.hpp"

namespace openrts::viewer {

struct OwnPresetsUi {
    std::vector<OwnGraphicsPreset> presets;
    bool listed = false; // read from the folder (again on "Refresh" and after a change)
    int chosen = -1;     // in presets
    char name[96]{};
    std::string status;
    bool status_bad = false;
};

// Draws the block (ImGui). True when `settings` changed: a preset applied.
// `width`: the buttons' (and the name field's) width.
bool draw_own_graphics_presets(OwnPresetsUi& ui, GraphicsSettings& settings, float width);

} // namespace openrts::viewer
