#pragma once

#include <imgui.h>

#include <openrts/ui/text.hpp>

#include "viewer_state.hpp"

namespace openrts::viewer {

// The checkboxes of openRTS's own sounds (SoundExtras), for the panel's
// General tab and the launcher's settings. They take effect at once.
inline void draw_sound_extras() {
    using ui::tr;
    SoundExtras extras = sound_extras();
    bool changed = false;
    ImGui::TextDisabled("%s", tr("openRTS's own sounds (not in RTS):"));
    changed |= ImGui::Checkbox(tr("Rain and snow on the body, heard inside the train"), &extras.rain_inside);
    changed |= ImGui::Checkbox(tr("Doppler effect: a passing train's sound higher coming, lower going"), &extras.doppler);
    changed |= ImGui::Checkbox(tr("Echo in tunnels, cuttings and under platform roofs"), &extras.echo);
    changed |= ImGui::Checkbox(tr("Brake shoes squealing as the train slows to a stop"), &extras.brake_squeal);
    if (changed) set_sound_extras(extras);
}

} // namespace openrts::viewer
