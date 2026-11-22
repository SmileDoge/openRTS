#include <openrts/ui/fonts.hpp>

#include <filesystem>
#include <string>
#include <vector>

#include <imgui.h>

namespace openrts::ui {
namespace {

std::filesystem::path& font_folder() {
    static std::filesystem::path folder;
    return folder;
}

// An optional face of the shell. Missing file: null, and the drawing code
// falls back to a system face of a close size.
ImFont* add(const std::filesystem::path& file, const float size, const ImWchar* ranges) {
    std::error_code error;
    if (file.empty() || !std::filesystem::is_regular_file(file, error)) return nullptr;
    return ImGui::GetIO().Fonts->AddFontFromFileTTF(file.string().c_str(), size, nullptr, ranges);
}

} // namespace

void set_font_folder(std::filesystem::path folder) { font_folder() = std::move(folder); }

ImFont*& brand_font() {
    static ImFont* font = nullptr;
    return font;
}
ImFont*& heading_font() {
    static ImFont* font = nullptr;
    return font;
}
ImFont*& clock_font() {
    static ImFont* font = nullptr;
    return font;
}
ImFont*& button_font() {
    static ImFont* font = nullptr;
    return font;
}
ImFont*& body_font() {
    static ImFont* font = nullptr;
    return font;
}
ImFont*& small_font() {
    static ImFont* font = nullptr;
    return font;
}

// A system monospace font with Cyrillic (content names are often Russian);
// ImGui's built-in font, Latin only, when none is found.
void load_fonts() {
    auto& io = ImGui::GetIO();
    static const ImWchar ranges[] = {
        0x0020, 0x00FF, // Latin + Latin-1
        0x0400, 0x052F, // Cyrillic + supplement
        0x2010, 0x205E, // punctuation (dashes, quotes, ellipsis)
        0x2116, 0x2116, // numero sign
        0x2190, 0x21FF, // arrows (the menu's ↗)
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

    // The shell's own faces (game/assets/menu, copied beside the program).
    const auto folder = font_folder();
    brand_font() = add(folder / "Montserrat-Bold.ttf", 33.0f, ranges);
    heading_font() = add(folder / "Montserrat-SemiBold.ttf", 32.0f, ranges);
    clock_font() = add(folder / "Montserrat-Bold.ttf", 30.0f, ranges);
    button_font() = add(folder / "Montserrat-Medium.ttf", 15.0f, ranges);
    body_font() = add(folder / "Onest-Regular.ttf", 15.0f, ranges);
    small_font() = add(folder / "Onest-Regular.ttf", 13.0f, ranges);
    if (!brand_font()) brand_font() = title_font();
    if (!heading_font()) heading_font() = large_font();
    if (!clock_font()) clock_font() = title_font();
    if (!button_font()) button_font() = large_font();
    if (!body_font()) body_font() = small_font() ? small_font() : large_font();
    if (!small_font()) small_font() = body_font();
}

} // namespace openrts::ui
