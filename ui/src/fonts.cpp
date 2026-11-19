#include <openrts/ui/fonts.hpp>

#include <filesystem>
#include <string>
#include <vector>

#include <imgui.h>

namespace openrts::ui {

// A system monospace font with Cyrillic (content names are often Russian);
// ImGui's built-in font, Latin only, when none is found.
void load_fonts() {
    auto& io = ImGui::GetIO();
    static const ImWchar ranges[] = {
        0x0020, 0x00FF, // Latin + Latin-1
        0x0400, 0x052F, // Cyrillic + supplement
        0x2010, 0x205E, // punctuation (dashes, quotes, ellipsis)
        0x2116, 0x2116, // numero sign
        0,
    };
    const std::vector<std::string> candidates{
        "C:/Windows/Fonts/consola.ttf",
        "C:/Windows/Fonts/lucon.ttf",
        "C:/Windows/Fonts/cour.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    };
    bool loaded = false;
    for (const auto& file : candidates) {
        if (!std::filesystem::is_regular_file(file)) continue;
        if (io.Fonts->AddFontFromFileTTF(file.c_str(), 15.0f, nullptr, ranges)) {
            loaded = true;
            break;
        }
    }
    if (!loaded) io.Fonts->AddFontDefault();
    // Map labels (Track control station names; RTS: Arial 16, here a little
    // larger for today's screens).
    for (const char* file : {"C:/Windows/Fonts/arial.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"}) {
        if (!std::filesystem::is_regular_file(file)) continue;
        if (auto* font = io.Fonts->AddFontFromFileTTF(file, 20.0f, nullptr, ranges)) {
            map_label_font() = font;
            break;
        }
    }
    // The start menu's title and large button (Segoe UI Semibold).
    for (const char* file : {"C:/Windows/Fonts/seguisb.ttf", "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf",
                             "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"}) {
        if (!std::filesystem::is_regular_file(file)) continue;
        title_font() = io.Fonts->AddFontFromFileTTF(file, 34.0f, nullptr, ranges);
        large_font() = io.Fonts->AddFontFromFileTTF(file, 21.0f, nullptr, ranges);
        break;
    }
}

} // namespace openrts::ui
