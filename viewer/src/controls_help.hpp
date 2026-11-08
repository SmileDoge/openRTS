#pragma once

// The keys and the mouse, a section each (camera and world, a train
// without a DLL, the engineer and the cab, RTS's information keys, the
// window), as the README's "Controls" has them: the game panel's and the
// start menu's "Controls" tab. The engineer's keys follow the chosen key
// layout (key_layout.hpp). Draws into the current ImGui window.
namespace openrts::viewer {

void draw_controls_help();

} // namespace openrts::viewer
