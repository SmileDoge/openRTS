#pragma once

namespace openrts::viewer {

// On a crash the viewer writes the exception and the call stack, with
// function names and lines from its .pdb, to stderr (the log), so a crash
// a player meets can be found from the log alone.
void install_crash_report();

} // namespace openrts::viewer
