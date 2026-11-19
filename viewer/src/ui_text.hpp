#pragma once

// The interface's text lives in openrts_ui (include/openrts/ui/text.hpp);
// the viewer's code uses it under its own namespace.
#include <openrts/ui/text.hpp>

namespace openrts::viewer {

using ui::search_form;
using ui::set_ui_language;
using ui::tr;
using ui::trf;
using ui::ui_language;
using ui::UiLanguage;

} // namespace openrts::viewer
