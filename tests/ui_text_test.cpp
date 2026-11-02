// The viewer's interface translation (viewer/src/ui_text): Russian by key,
// ids kept, English and unknown text passed through, printf formats.
#include <cstring>
#include <iostream>
#include <string>

#include "../viewer/src/ui_text.hpp"

namespace {

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

} // namespace

int main() {
    using namespace openrts::viewer;
    set_ui_language(UiLanguage::russian);
    check(std::strcmp(tr("Teleport here"), "Телепортироваться сюда") == 0, "a key gives its Russian text (UTF-8)");
    check(std::strcmp(tr("Track control (Ctrl+Alt+F7)###track_control"), "Трек-контроль (Ctrl+Alt+F7)###track_control") == 0,
          "a window title keeps its ### id");
    check(std::strcmp(tr("Override %s%s##%u"), "Переопределить %s%s##%u") == 0, "a format with an id");
    const char* unknown = "no such text in the table";
    check(tr(unknown) == unknown, "unknown text comes back as it is");
    check(trf("path %s set", "kuban.pat") == "маршрут kuban.pat задан", "trf translates and formats");
    set_ui_language(UiLanguage::english);
    const char* key = "Teleport here";
    check(tr(key) == key, "English: the key itself");
    check(trf("path %s set", "a") == "path a set", "English trf");
    set_ui_language(UiLanguage::russian);
    if (failures == 0) std::cout << "ui text: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
