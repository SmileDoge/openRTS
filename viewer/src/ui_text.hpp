#pragma once

#include <string>
#include <string_view>

namespace openrts::viewer {

// The language of the viewer's interface. Russian by default (the content is
// mostly from the former USSR railways); English selectable, remembered in
// viewer_state.ini ("language").
enum class UiLanguage : unsigned char { russian, english };

void set_ui_language(UiLanguage language) noexcept;
[[nodiscard]] UiLanguage ui_language() noexcept;

// The text in the interface language. The key is the English text as it is
// written in the code; a "label##id" key keeps its id. Unknown text comes back
// as it is (also when it is not a literal).
[[nodiscard]] const char* tr(const char* english);

// tr() of a printf format, formatted.
[[nodiscard]] std::string trf(const char* english, ...);

// A name as a search compares it: its case folded, Cyrillic too, and the
// Cyrillic letters as the Latin ones typed for them, so "2es6" and "2эс6"
// both find "2ЭС6-114", "vl80" "ВЛ80с", "chs4" "ЧС4т". A name matches a
// search when search_form(name) contains search_form(typed).
[[nodiscard]] std::string search_form(std::string_view text);

} // namespace openrts::viewer
