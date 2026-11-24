#include "launcher.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#ifdef OPENRTS_HAS_IMGUI
#include <imgui.h>
#endif

#include <openrts/core/core.hpp>
#include <openrts/core/installation.hpp>
#include <openrts/core/vfs.hpp>
#include <openrts/formats/activity.hpp>
#include <openrts/formats/train_path.hpp>
#include <openrts/renderer/webgpu_context.hpp>

#include "controls_help.hpp"
#include "file_dialog.hpp"
#include "frame_limiter.hpp"
#include "brake_settings_ui.hpp"
#include "graphics_presets_ui.hpp"
#include "sound_extras_ui.hpp"
#include "graphics_settings.hpp"
#include "key_layout.hpp"
#include "rts_config.hpp"
#include "ui_fonts.hpp"
#include "ui_slider.hpp"
#include "ui_text.hpp"
#include <openrts/renderer/bc7.hpp>

#include "exit_codes.hpp"
#include "menu/ui_image.hpp"
#include "viewer_state.hpp"
#include "window_mode.hpp"
#include "window_viewer.hpp"

namespace openrts::game {
namespace {

namespace fs = std::filesystem;
using viewer::remember;
using viewer::remembered;
using viewer::tr;
using viewer::trf;

[[nodiscard]] std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

[[nodiscard]] fs::path from_utf8(const std::string& text) { return fs::path(std::u8string(text.begin(), text.end())); }

[[nodiscard]] bool is_installation(const fs::path& path) {
    std::error_code error;
    return !path.empty() && fs::is_directory(path / "ROUTES", error);
}

[[nodiscard]] std::string lower(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

[[nodiscard]] int remembered_int(const char* key, const int fallback) {
    const auto value = remembered(key);
    if (!value) return fallback;
    try {
        return std::stoi(*value);
    } catch (...) {
        return fallback;
    }
}

[[nodiscard]] float remembered_float(const char* key, const float fallback) {
    const auto value = remembered(key);
    if (!value) return fallback;
    try {
        return std::stof(*value);
    } catch (...) {
        return fallback;
    }
}

// A command-line argument for CreateProcess (the C runtime's parsing).
[[nodiscard]] std::string quoted(const std::string& argument) {
    if (!argument.empty() && argument.find_first_of(" \t\"") == std::string::npos) return argument;
    std::string out = "\"";
    std::size_t slashes = 0;
    for (const char c : argument) {
        if (c == '\\') {
            ++slashes;
            continue;
        }
        if (c == '"') out.append(slashes * 2 + 1, '\\');
        else out.append(slashes, '\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, '\\');
    out += '"';
    return out;
}

// Text without the spaces around it.
[[nodiscard]] std::string trimmed(const std::string& text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

// The characters (not the bytes) of UTF-8 text.
[[nodiscard]] std::size_t characters(const std::string& text) {
    return static_cast<std::size_t>(
        std::count_if(text.begin(), text.end(), [](const char c) { return (static_cast<unsigned char>(c) & 0xC0u) != 0x80u; }));
}

// A list kept in viewer_state.ini as one value: "a;b;c".
[[nodiscard]] std::vector<std::string> split_list(const std::string& text) {
    std::vector<std::string> list;
    std::size_t from = 0;
    while (from <= text.size()) {
        const auto end = (std::min)(text.find(';', from), text.size());
        if (auto item = trimmed(text.substr(from, end - from)); !item.empty()) list.push_back(std::move(item));
        from = end + 1;
    }
    return list;
}

[[nodiscard]] std::string join_list(const std::vector<std::string>& list) {
    std::string text;
    for (const auto& item : list) text += (text.empty() ? "" : ";") + item;
    return text;
}

template <std::size_t N>
void set_text(std::array<char, N>& buffer, const std::string& text) {
    buffer.fill(0);
    std::copy_n(text.begin(), (std::min)(text.size(), N - 1), buffer.begin());
}

constexpr const char* season_keys[] = {"spring", "summer", "autumn", "winter"};
constexpr const char* season_names[] = {"Spring", "Summer", "Autumn", "Winter"};
// RTS's three, then openRTS's thunderstorm (rain with lightning).
constexpr const char* weather_keys[] = {"clear", "snow", "rain", "storm"};
constexpr const char* weather_names[] = {"Clear", "Snow", "Rain", "Thunderstorm"};
constexpr const char* start_names[] = {"Running", "Switched on", "Cold"};
// Where the player starts (--spawn); the last one: no train, a flight over the route.
constexpr const char* spawn_names[] = {"In the cab", "Beside the car", "Above the train", "No train (fly over the route)"};
constexpr const char* spawn_keys[] = {"cab", "car", "above", "none"};
constexpr int beside_the_car = 1;
constexpr int no_train = 3;

// The menu's tabs, remembered by their keys.
// info: the version, the project's links and the keys (until 2026-11-14 a
// Controls tab of the keys alone, remembered as "controls").
enum class Tab { single, network, settings, graphics, info };
constexpr const char* tab_keys[] = {"single", "network", "settings", "graphics", "info"};
constexpr int tab_count = static_cast<int>(std::size(tab_keys));

// How far the running game got in joining a server: its "status:" lines in
// openrts.log (viewer/src/main.cpp, report_status).
enum class NetStep { none, connecting, waiting, loading, playing, failed };

// Where the player is when the dispatcher assigns his train: in the cab
// (--spawn cab), or as before (the camera follows the train from above).
constexpr const char* net_spawn_names[] = {"In the cab", "Above the train"};
// Who joins: a driver, or the dispatcher's console (an openRTS server).
constexpr const char* net_role_names[] = {"Driver", "Dispatcher (console)"};
constexpr const char* net_role_keys[] = {"driver", "dispatcher"};
constexpr int net_role_dispatcher = 1;
// The dispatcher's console in the game waits until the server is done, as
// rts_server.exe with its own console (the user's call, 2026-11-09): the
// choice is hidden, the viewer keeps --mp-console.
constexpr bool console_role_shown = false;
constexpr const char* net_spawn_keys[] = {"cab", "above"};
constexpr int net_spawn_cab = 0;

constexpr std::size_t nick_limit = 15;  // RTS takes a nick of 1 to 15 characters (docs/net.md)
constexpr std::size_t servers_kept = 8; // the servers joined before

#ifdef OPENRTS_HAS_IMGUI
// The menu's look, pushed around its window only: rounded frames, more air,
// a calm dark background, blue for what is chosen and green for Start.
class MenuStyle {
public:
    MenuStyle() {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {22.0f, 16.0f});
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {9.0f, 5.0f});
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {9.0f, 7.0f});
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {6.0f, 4.0f});
        ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 5.0f);
        colour(ImGuiCol_WindowBg, {0.085f, 0.095f, 0.115f, 1.0f});
        colour(ImGuiCol_ChildBg, {0.075f, 0.085f, 0.105f, 0.94f});
        colour(ImGuiCol_PopupBg, {0.09f, 0.10f, 0.12f, 0.98f});
        colour(ImGuiCol_Border, {0.17f, 0.18f, 0.21f, 1.0f});
        colour(ImGuiCol_FrameBg, {0.11f, 0.12f, 0.145f, 1.0f});
        colour(ImGuiCol_FrameBgHovered, {0.15f, 0.16f, 0.19f, 1.0f});
        colour(ImGuiCol_FrameBgActive, {0.19f, 0.21f, 0.25f, 1.0f});
        colour(ImGuiCol_Header, {0.95f, 0.95f, 0.96f, 0.10f});
        colour(ImGuiCol_HeaderHovered, {0.95f, 0.95f, 0.96f, 0.15f});
        colour(ImGuiCol_HeaderActive, {0.95f, 0.95f, 0.96f, 0.20f});
        colour(ImGuiCol_Button, {0.14f, 0.15f, 0.18f, 1.0f});
        colour(ImGuiCol_ButtonHovered, {0.20f, 0.22f, 0.27f, 1.0f});
        colour(ImGuiCol_ButtonActive, {0.25f, 0.27f, 0.32f, 1.0f});
        colour(ImGuiCol_CheckMark, {0.95f, 0.96f, 0.98f, 1.0f});
        colour(ImGuiCol_SliderGrab, {0.80f, 0.82f, 0.86f, 1.0f});
        colour(ImGuiCol_SliderGrabActive, {1.0f, 1.0f, 1.0f, 1.0f});
        colour(ImGuiCol_TableHeaderBg, {0.10f, 0.11f, 0.13f, 1.0f});
        colour(ImGuiCol_Separator, {0.18f, 0.19f, 0.22f, 1.0f});
        colour(ImGuiCol_Text, {0.90f, 0.92f, 0.95f, 1.0f});
        colour(ImGuiCol_TextDisabled, {0.55f, 0.57f, 0.62f, 1.0f});
        colour(ImGuiCol_Tab, {0.09f, 0.10f, 0.12f, 1.0f});
        colour(ImGuiCol_TabHovered, {0.20f, 0.22f, 0.26f, 0.90f});
        colour(ImGuiCol_TabSelected, {0.16f, 0.17f, 0.20f, 1.0f});
        colour(ImGuiCol_TabSelectedOverline, {0.95f, 0.96f, 0.98f, 1.0f});
        colour(ImGuiCol_TabDimmed, {0.09f, 0.10f, 0.12f, 1.0f});
        colour(ImGuiCol_TabDimmedSelected, {0.16f, 0.17f, 0.20f, 1.0f});
        colour(ImGuiCol_TabDimmedSelectedOverline, {0.95f, 0.96f, 0.98f, 1.0f});
    }
    ~MenuStyle() {
        ImGui::PopStyleColor(colours_);
        ImGui::PopStyleVar(9);
    }
    MenuStyle(const MenuStyle&) = delete;
    MenuStyle& operator=(const MenuStyle&) = delete;

private:
    void colour(const ImGuiCol which, const ImVec4& value) {
        ImGui::PushStyleColor(which, value);
        ++colours_;
    }
    int colours_ = 0;
};

// A row of a form: the label on the left, the control filling the rest.
void form_row(const char* label) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
}

[[nodiscard]] bool begin_form(const char* id, const float label_width) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit)) return false;
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, label_width);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

// What a choice means, on the screen under it (dim, wrapped), not only in a
// tooltip.
void hint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// Something to fix before going on (amber, wrapped).
void warning(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.95f, 0.80f, 0.45f, 1.0f});
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

constexpr auto panel_flags = ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding;

// --- The menu's own look (the mockup): dark cards on a dimmed photograph,
// Montserrat for names and numbers, Onest for the smaller lines. ---
constexpr ImU32 rgb(const int r, const int g, const int b, const int a = 255) {
    return IM_COL32(r, g, b, a);
}
constexpr ImU32 card_fill = rgb(23, 24, 28);
constexpr ImU32 card_fill_hover = rgb(30, 32, 38);
constexpr ImU32 card_line = rgb(255, 255, 255, 22);
constexpr ImU32 card_line_hover = rgb(255, 255, 255, 60);
constexpr ImU32 white = rgb(245, 247, 250);
constexpr ImU32 grey = rgb(158, 164, 176);
constexpr ImU32 dim = rgb(116, 122, 134);
constexpr ImU32 accent = rgb(82, 132, 255);

struct Rect {
    ImVec2 min, max;
    [[nodiscard]] float width() const { return max.x - min.x; }
    [[nodiscard]] float height() const { return max.y - min.y; }
    [[nodiscard]] ImVec2 centre() const { return {(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f}; }
};

[[nodiscard]] float menu_scale() {
    const auto size = ImGui::GetIO().DisplaySize;
    return (std::min)(size.x / 1920.0f, size.y / 1080.0f);
}

void label(ImDrawList* draw, ImFont* font, const float size, const ImVec2 at, const ImU32 colour, const std::string& text) {
    if (font && size > 0.0f) draw->AddText(font, size, at, colour, text.c_str());
}

[[nodiscard]] ImVec2 label_size(ImFont* font, const float size, const std::string& text) {
    if (!font) return {0.0f, 0.0f};
    return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
}

void label_right(ImDrawList* draw, ImFont* font, const float size, const float right, const float y, const ImU32 colour,
                 const std::string& text) {
    draw->AddText(font, size, {right - label_size(font, size, text).x, y}, colour, text.c_str());
}

// The text's middle on `centre`: the mockup's circles and buttons.
void label_centered(ImDrawList* draw, ImFont* font, const float size, const ImVec2 centre, const ImU32 colour,
                    const std::string& text) {
    const auto measured = label_size(font, size, text);
    draw->AddText(font, size, {centre.x - measured.x * 0.5f, centre.y - measured.y * 0.5f}, colour, text.c_str());
}


ImU32 mix_colour(const ImU32 from, const ImU32 to, const float t) {
    const auto a = ImGui::ColorConvertU32ToFloat4(from);
    const auto b = ImGui::ColorConvertU32ToFloat4(to);
    return ImGui::ColorConvertFloat4ToU32(
        {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
}

// The mockup's arrow, traced from its 13x13 SVG: one filled shape, not a
// line with a separate open head.
void arrow_up_right(ImDrawList* draw, const ImVec2 centre, const float size, const ImU32 colour, const float thickness) {
    (void)thickness;
    constexpr float box = 13.0f;
    constexpr ImVec2 outline[] = {
        {1.248f, 12.0478f}, {0.0f, 10.7998f}, {8.856f, 1.9438f}, {1.32f, 2.7598f}, {1.176f, 0.983804f},
        {12.048f, -0.000196457f}, {11.064f, 10.8958f}, {9.288f, 10.7278f}, {10.104f, 3.1918f},
    };
    ImVec2 points[9];
    const auto scale = size / box;
    const auto origin = ImVec2{centre.x - 6.024f * scale, centre.y - 6.024f * scale};
    for (int i = 0; i < 9; ++i) {
        points[i] = {origin.x + outline[i].x * scale, origin.y + outline[i].y * scale};
    }
    draw->AddConcavePolyFilled(points, 9, colour);
}

// A picture filling `area`, cropped by its middle (the mockup's photos).
void picture(ImDrawList* draw, WGPUTextureView view, const std::pair<int, int>& pixels, const Rect area, const float rounding,
             const ImU32 tint = IM_COL32_WHITE) {
    if (!view || pixels.first <= 0 || pixels.second <= 0) {
        draw->AddRectFilled(area.min, area.max, rgb(42, 45, 52), rounding);
        return;
    }
    const auto source = static_cast<float>(pixels.first) / static_cast<float>(pixels.second);
    const auto target = area.width() / (std::max)(area.height(), 1.0f);
    ImVec2 uv0{0.0f, 0.0f}, uv1{1.0f, 1.0f};
    if (source > target) {
        const auto share = target / source;
        uv0.x = (1.0f - share) * 0.5f;
        uv1.x = 1.0f - uv0.x;
    } else {
        const auto share = source / target;
        uv0.y = (1.0f - share) * 0.5f;
        uv1.y = 1.0f - uv0.y;
    }
    draw->AddImageRounded(reinterpret_cast<ImTextureID>(view), area.min, area.max, uv0, uv1, tint, rounding);
}

void open_link(const char* url) {
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
#else
    (void)url;
#endif
}

void soon_badge(ImDrawList* draw, const ImVec2 at, const float scale, const float alpha) {
    const auto size = ImVec2{72.0f * scale, 28.0f * scale};
    const Rect box{at, {at.x + size.x, at.y + size.y}};
    draw->AddRectFilled(box.min, box.max, rgb(12, 13, 16, static_cast<int>(215.0f * alpha)), 14.0f * scale);
    draw->AddRect(box.min, box.max, rgb(255, 255, 255, static_cast<int>(70.0f * alpha)), 14.0f * scale);
    const auto text = std::string("Скоро");
    const auto measured = label_size(ui::small_font(), 13.0f * scale, text);
    label(draw, ui::small_font(), 13.0f * scale, {box.centre().x - measured.x * 0.5f, box.centre().y - measured.y * 0.5f},
          rgb(235, 238, 244, static_cast<int>(255.0f * alpha)), text);
}

// A card of the mockup: the picture, its number, name and line, and the round
// arrow. Returns true when clicked (not for a card that is only "soon").
[[nodiscard]] bool draw_card(ImDrawList* draw, const char* id, const Rect area, const char* number, const char* name, const char* description,
                             WGPUTextureView view, const std::pair<int, int>& pixels, const bool primary, const bool soon, const bool clickable,
                             float& hover, const float scale) {
    (void)primary;
    hover += ((ImGui::IsMouseHoveringRect(area.min, area.max) ? 1.0f : 0.0f) - hover) * (std::min)(1.0f, ImGui::GetIO().DeltaTime * 12.0f);
    ImGui::SetCursorScreenPos(area.min);
    ImGui::PushID(id);
    const bool pressed = ImGui::InvisibleButton("##card", {area.width(), area.height()}) && clickable;
    ImGui::PopID();

    const auto lift = 3.0f * scale * hover;
    const Rect box{{area.min.x, area.min.y - lift}, {area.max.x, area.max.y - lift}};
    // The card stays quiet: only the arrow lights and the card rises.
    draw->AddRectFilled(box.min, box.max, card_fill, 24.0f * scale);
    draw->AddRect(box.min, box.max, card_line, 24.0f * scale, 0, 1.0f);

    const auto pad = 25.0f * scale;
    const auto text_height = (area.height() > 400.0f * scale ? 110.0f : 105.0f) * scale;
    const Rect photo{{box.min.x + pad, box.min.y + pad}, {box.max.x - pad, box.max.y - text_height}};
    if (view) {
        picture(draw, view, pixels, photo, 13.0f * scale, rgb(255, 255, 255, 255));
        // The picture's own thin frame, as in the mockup.
        draw->AddRect(photo.min, photo.max, rgb(255, 255, 255, 32), 13.0f * scale, 0, 1.0f * scale);
    } else {
        draw->AddRectFilled(photo.min, photo.max, rgb(38, 41, 48), 13.0f * scale);
        draw->AddRect(photo.min, photo.max, rgb(255, 255, 255, 32), 13.0f * scale, 0, 1.0f * scale);
    }

    // The mockup puts the number on the same line as the name, to its left.
    label(draw, ui::small_font(), 14.0f * scale, {box.min.x + 29.0f * scale, box.max.y - 87.0f * scale}, dim, number);
    label(draw, ui::heading_font(), 30.0f * scale, {box.min.x + 53.0f * scale, box.max.y - 94.0f * scale}, white, name);
    label(draw, ui::body_font(), 14.0f * scale, {box.min.x + 30.0f * scale, box.max.y - 50.0f * scale}, grey, description);

    const auto radius = 22.0f * scale;
    const auto centre = ImVec2{box.max.x - pad - radius, box.max.y - 57.0f * scale};
    const auto lit = hover;
    draw->AddCircleFilled(centre, radius, mix_colour(rgb(45, 48, 55), rgb(247, 248, 250), lit), 48);
    arrow_up_right(draw, centre, 13.0f * scale, mix_colour(rgb(205, 210, 219), rgb(20, 21, 24), lit), 1.6f * scale);

    if (soon) {
        soon_badge(draw, {box.max.x - pad - 72.0f * scale, box.min.y + pad + 12.0f * scale}, scale, 0.9f);
    }
    return pressed;
}

// A small outlined button of the header ("Войти").
[[nodiscard]] bool header_button(ImDrawList* draw, const char* id, const Rect area, const char* text, const float scale, float& hover) {
    hover += ((ImGui::IsMouseHoveringRect(area.min, area.max) ? 1.0f : 0.0f) - hover) * (std::min)(1.0f, ImGui::GetIO().DeltaTime * 14.0f);
    ImGui::SetCursorScreenPos(area.min);
    ImGui::PushID(id);
    const bool pressed = ImGui::InvisibleButton("##button", {area.width(), area.height()});
    ImGui::PopID();
    draw->AddRectFilled(area.min, area.max, rgb(255, 255, 255, static_cast<int>(8.0f + 10.0f * hover)), 10.0f * scale);
    draw->AddRect(area.min, area.max, rgb(255, 255, 255, static_cast<int>(48.0f + 45.0f * hover)), 10.0f * scale);
    const auto measured = label_size(ui::button_font(), 14.0f * scale, text);
    label(draw, ui::button_font(), 14.0f * scale,
          {area.centre().x - measured.x * 0.5f, area.centre().y - measured.y * 0.5f}, white, text);
    return pressed;
}

// The header of an inner page: back, its name and line, the same clock and
// account as the home page.
void page_heading(ImDrawList* draw, const Rect content, const char* name, const char* line, const float scale, const std::string& clock,
                  const std::string& date, const bool guest, float& account_hover, bool& back, bool& account_clicked) {
    const auto back_box = Rect{{content.min.x, content.min.y}, {content.min.x + 42.0f * scale, content.min.y + 42.0f * scale}};
    ImGui::SetCursorScreenPos(back_box.min);
    back = ImGui::InvisibleButton("##back", {back_box.width(), back_box.height()});
    draw->AddRectFilled(back_box.min, back_box.max, rgb(255, 255, 255, ImGui::IsItemHovered() ? 20 : 8), 12.0f * scale);
    draw->AddRect(back_box.min, back_box.max, rgb(255, 255, 255, 40), 12.0f * scale);
    label_centered(draw, ui::button_font(), 22.0f * scale, back_box.centre(), white, "←");

    label(draw, ui::brand_font(), 27.0f * scale, {content.min.x + 60.0f * scale, content.min.y + 3.0f * scale}, white, name);
    label(draw, ui::small_font(), 13.0f * scale, {content.min.x + 60.0f * scale, content.min.y + 43.0f * scale}, grey, line);

    label_right(draw, ui::clock_font(), 27.0f * scale, content.max.x - 330.0f * scale, content.min.y + 2.0f * scale, white, clock);
    label_right(draw, ui::small_font(), 12.0f * scale, content.max.x - 330.0f * scale, content.min.y + 40.0f * scale, grey, date);
    const ImVec2 avatar_centre{content.max.x - 274.0f * scale, content.min.y + 26.0f * scale};
    draw->AddLine({content.max.x - 304.0f * scale, avatar_centre.y - 26.0f * scale},
                  {content.max.x - 304.0f * scale, avatar_centre.y + 26.0f * scale}, rgb(255, 255, 255, 40), 1.0f);
    draw->AddCircleFilled(avatar_centre, 22.0f * scale, rgb(42, 46, 55), 40);
    label_centered(draw, ui::heading_font(), 18.0f * scale, avatar_centre, white, guest ? "Г" : "D");
    const auto name_size = label_size(ui::button_font(), 13.0f * scale, guest ? "Гость" : "Игрок");
    const auto level_size = label_size(ui::small_font(), 11.0f * scale, guest ? "Уровень 0" : "Уровень 1");
    const auto name_block = name_size.y + 3.0f * scale + level_size.y;
    const auto name_top = avatar_centre.y - name_block * 0.5f;
    label(draw, ui::button_font(), 13.0f * scale, {content.max.x - 240.0f * scale, name_top}, white,
          guest ? "Гость" : "Игрок");
    label(draw, ui::small_font(), 11.0f * scale, {content.max.x - 240.0f * scale, name_top + name_size.y + 3.0f * scale}, dim,
          guest ? "Уровень 0" : "Уровень 1");
    const auto button = Rect{{content.max.x - 132.0f * scale, avatar_centre.y - 22.0f * scale},
                             {content.max.x, avatar_centre.y + 22.0f * scale}};
    (void)back_box;
    account_clicked = header_button(draw, "##account", button, guest ? "Войти" : "Выход", scale, account_hover);
}

#endif


} // namespace

struct Launcher::State {
    // The new shell's pages: the mockup's home, the solo setup (the old Single
    // player tab), settings, the account place, and a "soon" card's page.
    enum class Page { home, solo, settings, account, soon };
    Page page = Page::home;
    bool embedded_settings = false;
    bool embedded_back = false;
    int settings_section = 0; // 0 game, 1 graphics, 2 controls
    int soon_kind = 0;        // 0 multiplayer, 1 depot, 2 editor
    std::optional<SessionRequest> pending_session;
    std::string session_note;
    float card_hover[8]{};
    float account_hover = 0.0f;
    float start_hover = 0.0f;
    std::unique_ptr<menu::ImageCache> images;
    std::string image_dir;

    fs::path exe_dir;
    fs::path root;
    std::array<char, 1024> root_text{};
    std::shared_ptr<core::Vfs> vfs;
    std::vector<std::string> routes;
    std::vector<std::string> consists;
    std::vector<std::string> consist_search; // viewer::search_form(consists[i])
    std::vector<formats::TrainPath> paths;
    int route = -1, consist = -1, path = -1; // -1 path: the route's first
    std::array<char, 128> filter{};
    int minutes = 12 * 60;
    int season = 1, weather = 0, start = 0; // start: running, on, cold
    int spawn = 0;                          // spawn_names
    std::vector<std::string> situations;    // saved for the chosen route (the game's Train window)
    int situation = -1;                     // -1: none
    // Saved games of the chosen route (the game's panel, F2), the newest
    // first: one chosen starts the game from it, as RTS's Load did.
    std::vector<std::string> saves;
    int save = -1;                          // -1: a new game
    // The chosen route's RTS scenarios (RTS_ACT/*.act); one chosen sets the
    // trains, the time, season and weather and the start (-1: none).
    std::vector<formats::ActivityFile> activities;
    int activity = -1;
    bool sound = true;
    float volume = 0.5f;
    bool vsync = true;
    // The brakes, the game's Train window's too (brake_settings_ui.hpp).
    viewer::BrakeSettings brakes;
    // The graphics settings, the game's panel's too (graphics_settings.hpp):
    // each change written at once, only what changed.
    viewer::GraphicsSettings graphics;
    viewer::OwnPresetsUi own_presets; // the presets of one's own, to share
    std::optional<float> fov; // "fov", else RTS's config.ini
    int fps_limit = 0; // the game's FPS limit, shared with its panel ("fps_limit"): 0 none, 30..500
    viewer::WindowMode window_mode = viewer::WindowMode::windowed; // the game's window ("window_mode"), shared with its panel
    std::string status;
    bool registry_missing = false; // "From the registry" found nothing
    Tab tab = Tab::single;
    bool select_tab = true; // the tab bar is told to show `tab` on the next frame
    // Network play: the server ("host[:port]", port 800 when none is given),
    // the nick, the servers joined before (newest first).
    std::array<char, 256> address{};
    std::array<char, 64> nick{};
    std::vector<std::string> servers;
    int net_spawn = 1; // net_spawn_names
    int net_role = 0;  // net_role_names
    // An own server: only its settings' place, the server is not there yet
    // (docs/roadmap.md, stage 3).
    int host_port = 800;
    int host_players = 0; // 0: RTS and openRTS (Legacy), 1: openRTS only
    // The running game; for a network game, how far it got in joining.
    bool network_game = false;
    bool cancelled = false;
    NetStep net_step = NetStep::none;
    std::string net_reason;             // the failed status's word
    std::string net_detail;             // the route while waiting; the failure's detail
    std::string net_address, net_nick;  // what the game was started with
    std::uint64_t log_read = 0;         // bytes of openrts.log read
    std::string log_line;               // its unfinished last line
    std::chrono::steady_clock::time_point next_log_check{};
    // The route's situations are read again now and then: one saved in the
    // game (its Train window) shows up without choosing the route again.
    std::chrono::steady_clock::time_point next_situation_scan{};
#ifdef _WIN32
    HANDLE process = nullptr;
#endif

    // openrts-ace (tools/ace): beside the game in a release's tools\, in a
    // build's tools\ace\Release\; empty when neither is there.
    std::string ace_status;
    [[nodiscard]] fs::path ace_tool() const {
        std::error_code error;
        for (const auto& candidate : {exe_dir / "tools" / "openrts-ace.exe",
                                      exe_dir.parent_path().parent_path() / "tools" / "ace" / "Release" / "openrts-ace.exe"}) {
            if (fs::exists(candidate, error)) return candidate;
        }
        return {};
    }
    // Runs it with `arguments`, no window, waiting up to 10 s: its exit code
    // (-1: did not start or finish).
    [[nodiscard]] int run_ace_tool(const std::string& arguments) const {
#ifdef _WIN32
        const auto tool = ace_tool();
        if (tool.empty()) return -1;
        std::wstring line = L"\"" + tool.wstring() + L"\" " + from_utf8(arguments).wstring();
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info)) return -1;
        DWORD code = static_cast<DWORD>(-1);
        if (WaitForSingleObject(info.hProcess, 10000) == WAIT_OBJECT_0) GetExitCodeProcess(info.hProcess, &code);
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
        return static_cast<int>(code);
#else
        (void)arguments;
        return -1;
#endif
    }
    void draw_ace_settings();

    [[nodiscard]] bool running() const {
#ifdef _WIN32
        return process != nullptr;
#else
        return false;
#endif
    }

    // A network game still joining: it can be cancelled.
    [[nodiscard]] bool joining() const noexcept {
        return network_game && (net_step == NetStep::connecting || net_step == NetStep::waiting || net_step == NetStep::loading);
    }

    void set_root(const fs::path& folder) {
        root = folder;
        set_text(root_text, utf8(root));
    }

    void load_settings() {
        viewer::apply_settings_defaults();
        fs::path folder;
        if (const auto value = remembered("root"); value && is_installation(from_utf8(*value))) folder = from_utf8(*value);
        if (folder.empty()) {
            // The openRTS folder sits in the RTS installation; or a build folder below it.
            std::error_code error;
            std::vector<fs::path> starts{fs::current_path(error), exe_dir};
            for (auto from : starts) {
                for (int up = 0; up < 6 && !from.empty() && folder.empty(); ++up) {
                    if (is_installation(from)) folder = from;
                    from = from.parent_path();
                }
            }
        }
        // Else the one Windows knows of (RTS's own key, MDD Launcher's too).
        if (folder.empty()) {
            if (const auto registered = core::registered_installation()) folder = *registered;
        }
        set_root(folder);
        minutes = std::clamp(remembered_int("launcher_time", 12 * 60), 0, 24 * 60 - 1);
        season = std::clamp(remembered_int("launcher_season", 1), 0, 3);
        weather = std::clamp(remembered_int("launcher_weather", 0), 0, 3);
        start = std::clamp(remembered_int("launcher_start", 0), 0, 2);
        sound = remembered_int("launcher_sound", 1) != 0;
        volume = std::clamp(remembered_float("volume", 0.5f), 0.0f, 1.0f);
        viewer::set_mouse_sensitivity(remembered_float("mouse_sensitivity", 1.0f));
        viewer::set_head_motion(remembered_float("head_motion", viewer::default_head_motion));
        viewer::set_background_sound(viewer::parse_background_sound(remembered("background_sound").value_or("").c_str()));
        viewer::set_rail_joint_layout(viewer::parse_rail_joint_layout(remembered("rail_joints").value_or("openrts").c_str()));
        viewer::set_body_motion_model(viewer::parse_body_motion_model(remembered("body_motion").value_or("openrts").c_str()));
        vsync = remembered_int("launcher_vsync", 1) != 0;
        brakes = viewer::load_brake_settings();
        load_graphics();
        spawn = std::clamp(remembered_int("launcher_spawn", 0), 0, no_train);
        fps_limit = viewer::clamp_fps_limit(remembered_int("fps_limit", 0));
        window_mode = viewer::parse_window_mode(remembered("window_mode").value_or("")).value_or(viewer::WindowMode::windowed);
        if (const auto value = remembered("launcher_tab")) {
            for (int i = 0; i < tab_count; ++i)
                if (*value == tab_keys[i]) tab = static_cast<Tab>(i);
            if (*value == "controls") tab = Tab::info;
        }
        set_text(address, remembered("mp_address").value_or("127.0.0.1"));
        set_text(nick, remembered("mp_nick").value_or(""));
        servers = split_list(remembered("mp_servers").value_or(""));
        if (const auto value = remembered("mp_spawn")) {
            for (int i = 0; i < 2; ++i)
                if (*value == net_spawn_keys[i]) net_spawn = i;
        }
        if (const auto value = remembered("mp_role"); value && console_role_shown) {
            for (int i = 0; i < 2; ++i)
                if (*value == net_role_keys[i]) net_role = i;
        }
    }

    // The graphics settings as kept (at the start, and after the game, whose
    // panel may have changed them).
    void load_graphics() {
        graphics = viewer::load_graphics_settings();
        fov.reset();
        if (const auto value = remembered("fov")) {
            char* end = nullptr;
            const float read = std::strtof(value->c_str(), &end);
            if (end != value->c_str()) fov = std::clamp(read, 15.0f, 100.0f);
        }
    }

    // The field of view the game takes: the remembered one, else RTS's
    // (its config.ini read once a folder), else the game's own.
    fs::path fov_root;
    float rts_fov = 48.7f;
    [[nodiscard]] float game_fov() {
        if (fov) return *fov;
        if (fov_root != root) {
            fov_root = root;
            rts_fov = 48.7f;
            if (!root.empty())
                if (const auto rts = viewer::rts_config_value(viewer::read_rts_config(root), "fov")) rts_fov = std::clamp(*rts, 15.0f, 100.0f);
        }
        return rts_fov;
    }

    void save_settings() const {
        remember("root", utf8(root));
        if (route >= 0) remember("launcher_route", routes[static_cast<std::size_t>(route)]);
        if (consist >= 0) remember("launcher_consist", consists[static_cast<std::size_t>(consist)]);
        remember("launcher_path", path >= 0 ? paths[static_cast<std::size_t>(path)].file : std::string());
        remember("launcher_time", std::to_string(minutes));
        remember("launcher_season", std::to_string(season));
        remember("launcher_weather", std::to_string(weather));
        remember("launcher_start", std::to_string(start));
        remember("launcher_sound", sound ? "1" : "0");
        remember("volume", std::to_string(volume));
        remember("launcher_vsync", vsync ? "1" : "0");
        remember("launcher_spawn", std::to_string(spawn));
        remember("launcher_situation", situation >= 0 ? situations[static_cast<std::size_t>(situation)] : std::string());
        remember("launcher_tab", tab_keys[static_cast<int>(tab)]);
        remember("mp_address", address_text());
        remember("mp_nick", nick_text());
        remember("mp_servers", join_list(servers));
        remember("mp_spawn", net_spawn_keys[net_spawn]);
        remember("mp_role", net_role_keys[net_role]);
    }

    void scan() {
        routes.clear();
        consists.clear();
        paths.clear();
        route = consist = path = -1;
        vfs.reset();
        status.clear();
        if (!is_installation(root)) {
            status = tr("There is no ROUTES folder here: give the RTS folder.");
            return;
        }
        vfs = std::make_shared<core::Vfs>();
        core::mount_installation(*vfs, root, core::load_installation_config(root));
        vfs->build_index();
        if (const auto folder = core::VirtualPath::parse("ROUTES")) {
            for (const auto& entry : vfs->list(*folder)) {
                if (!entry.directory) continue;
                bool track = false;
                for (const auto& file : vfs->list(entry.path))
                    if (!file.directory && lower(std::string(file.path.filename())).ends_with(".trk")) track = true;
                if (track) routes.emplace_back(entry.path.filename());
            }
        }
        std::set<std::string> seen;
        std::vector<core::VirtualPath> stack;
        for (const auto* name : {"TRAINS/CONSISTS_RTS", "TRAINS/CONSISTS"})
            if (const auto folder = core::VirtualPath::parse(name)) stack.push_back(*folder);
        while (!stack.empty()) {
            const auto folder = stack.back();
            stack.pop_back();
            for (const auto& entry : vfs->list(folder)) {
                if (entry.directory) {
                    stack.push_back(entry.path);
                    continue;
                }
                std::string name(entry.path.filename());
                if (lower(name).ends_with(".con")) {
                    name.resize(name.size() - 4);
                    if (seen.insert(core::fold_case(name)).second) consists.push_back(name);
                }
            }
        }
        const auto by_name = [](const std::string& a, const std::string& b) { return core::fold_case(a) < core::fold_case(b); };
        std::sort(routes.begin(), routes.end(), by_name);
        std::sort(consists.begin(), consists.end(), by_name);
        consist_search.clear();
        for (const auto& name : consists) consist_search.push_back(viewer::search_form(name));
        const auto find = [](const std::vector<std::string>& list, const std::optional<std::string>& name) {
            if (!name) return -1;
            for (std::size_t i = 0; i < list.size(); ++i)
                if (core::fold_case(list[i]) == core::fold_case(*name)) return static_cast<int>(i);
            return -1;
        };
        route = find(routes, remembered("launcher_route"));
        consist = find(consists, remembered("launcher_consist"));
        load_paths();
        if (const auto wanted = remembered("launcher_path"); wanted && !wanted->empty()) {
            for (std::size_t i = 0; i < paths.size(); ++i)
                if (lower(paths[i].file) == lower(*wanted)) path = static_cast<int>(i);
        }
        if (routes.empty()) status = tr("No routes in this folder.");
    }

    [[nodiscard]] fs::path situation_folder() const {
        return route < 0 ? fs::path{} : viewer::user_data_directory() / "situations" / routes[static_cast<std::size_t>(route)];
    }

    // The chosen route's saved situations, by name.
    [[nodiscard]] std::vector<std::string> scan_situations() const {
        std::vector<std::string> found;
        if (route < 0) return found;
        std::error_code error;
        for (const auto& entry : fs::directory_iterator(situation_folder(), error)) {
            if (entry.path().extension() == ".situation") found.push_back(utf8(entry.path().stem()));
        }
        std::sort(found.begin(), found.end());
        return found;
    }

    [[nodiscard]] fs::path saves_folder() const {
        return route < 0 ? fs::path{} : viewer::user_data_directory() / "saves" / routes[static_cast<std::size_t>(route)];
    }
    // The chosen route's saved games by name, the newest first.
    [[nodiscard]] std::vector<std::string> scan_saves() const {
        std::vector<std::pair<fs::file_time_type, std::string>> found;
        std::error_code error;
        if (route < 0) return {};
        for (const auto& entry : fs::directory_iterator(saves_folder(), error))
            if (entry.path().extension() == ".save") found.emplace_back(entry.last_write_time(error), utf8(entry.path().stem()));
        std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::vector<std::string> names;
        for (auto& [_, name] : found) names.push_back(std::move(name));
        return names;
    }
    void refresh_saves() {
        auto found = scan_saves();
        if (found == saves) return;
        const auto chosen = save >= 0 && save < static_cast<int>(saves.size()) ? saves[static_cast<std::size_t>(save)] : std::string();
        saves = std::move(found);
        save = -1;
        for (std::size_t i = 0; i < saves.size() && !chosen.empty(); ++i)
            if (saves[i] == chosen) save = static_cast<int>(i);
    }

    // The route's saved situations, the remembered one chosen again.
    void load_situations() {
        situations = scan_situations();
        situation = -1;
        if (const auto wanted = remembered("launcher_situation"); wanted && !wanted->empty()) {
            for (std::size_t i = 0; i < situations.size(); ++i)
                if (situations[i] == *wanted) situation = static_cast<int>(i);
        }
    }

    // The situations read again (saved or deleted meanwhile), the chosen
    // one kept while it is there.
    void refresh_situations() {
        auto found = scan_situations();
        if (found == situations) return;
        const auto chosen = situation >= 0 ? situations[static_cast<std::size_t>(situation)] : std::string();
        situations = std::move(found);
        situation = -1;
        for (std::size_t i = 0; i < situations.size() && !chosen.empty(); ++i)
            if (situations[i] == chosen) situation = static_cast<int>(i);
    }

    // A consist is wanted: not when the trains come from a scenario or a
    // situation, or there is no train.
    [[nodiscard]] bool consist_used() const noexcept { return spawn != no_train && situation < 0 && activity < 0 && save < 0; }
    [[nodiscard]] bool ready() const noexcept { return route >= 0 && (save >= 0 || !consist_used() || consist >= 0); }
    [[nodiscard]] const formats::ActivityFile* chosen_activity() const noexcept {
        return activity >= 0 && activity < static_cast<int>(activities.size()) ? &activities[static_cast<std::size_t>(activity)] : nullptr;
    }

    // The shell's request from the solo choices. A saved game or a scenario
    // sets its own clock and trains; the request then only names the files.
    [[nodiscard]] SessionRequest make_session() const {
        SessionRequest request;
        request.root = root;
        if (route >= 0) request.route = routes[static_cast<std::size_t>(route)];
        if (save >= 0 && save < static_cast<int>(saves.size())) {
            request.save = saves_folder() / (saves[static_cast<std::size_t>(save)] + ".save");
        } else if (const auto* chosen = chosen_activity()) {
            request.activity = chosen->file;
            request.minutes = std::clamp(chosen->activity.start_time / 60, 0, 24 * 60 - 1);
            request.season = std::clamp(chosen->activity.season, 0, 3);
            request.weather = std::clamp(chosen->activity.weather, 0, 3);
        } else if (situation >= 0 && situation < static_cast<int>(situations.size())) {
            request.situation = situation_folder() / (situations[static_cast<std::size_t>(situation)] + ".situation");
        } else if (consist_used() && consist >= 0) {
            request.consist = consists[static_cast<std::size_t>(consist)];
            if (path >= 0 && path < static_cast<int>(paths.size())) request.path = paths[static_cast<std::size_t>(path)].file;
        }
        request.start_state = start == 1 ? 1 : start == 2 ? 0 : -1;
        request.start_view = spawn == 0 ? 1 : spawn == 1 ? 2 : spawn == 2 ? 3 : 0;
        if (request.save.empty() && request.activity.empty()) {
            request.minutes = minutes;
            request.season = season;
            request.weather = weather;
        }
        request.realistic_air = true;
        request.brakes = brakes;
        request.sound = sound;
        request.volume = volume;
        request.title = request.route;
        if (!request.consist.empty()) request.title += " — " + request.consist;
        return request;
    }

    void begin_session() {
        if (running() || !ready()) return;
        pending_session = make_session();
        save_settings();
        status.clear();
        session_note.clear();
    }

    [[nodiscard]] std::string address_text() const { return trimmed(address.data()); }
    [[nodiscard]] std::string nick_text() const { return trimmed(nick.data()); }
    [[nodiscard]] bool nick_fits() const {
        const auto length = characters(nick_text());
        return length >= 1 && length <= nick_limit;
    }
    // A server can be joined: the RTS folder (the server's route is opened
    // from it), an address and a nick.
    [[nodiscard]] bool network_ready() const { return vfs != nullptr && !address_text().empty() && nick_fits(); }
    [[nodiscard]] bool ready_for(const Tab which) const { return which == Tab::network ? network_ready() : ready(); }

    void load_paths() {
        load_situations();
        saves = scan_saves();
        save = -1;
        paths.clear();
        path = -1;
        activities.clear();
        activity = -1;
        if (!vfs || route < 0) return;
        if (const auto folder = core::VirtualPath::parse("ROUTES/" + routes[static_cast<std::size_t>(route)])) {
            activities = formats::load_activities(*vfs, *folder);
            std::erase_if(activities, [](const formats::ActivityFile& file) { return !file.activity.ok(); });
            paths = formats::load_train_paths(*vfs, *folder);
            std::erase_if(paths, [](const formats::TrainPath& p) { return !p.ok(); });
            std::sort(paths.begin(), paths.end(), [](const formats::TrainPath& a, const formats::TrainPath& b) {
                return lower(a.name.empty() ? a.file : a.name) < lower(b.name.empty() ? b.file : b.name);
            });
        }
    }

    [[nodiscard]] fs::path game_exe() const {
        const std::vector<fs::path> candidates{
            exe_dir / "openrts_viewer.exe",
            exe_dir.parent_path().parent_path() / "viewer" / exe_dir.filename() / "openrts_viewer.exe",
        };
        std::error_code error;
        for (const auto& candidate : candidates)
            if (fs::is_regular_file(candidate, error)) return candidate;
        return {};
    }

    // The game's command line for a tab's choice (empty: not ready, or no game).
    [[nodiscard]] std::string command_line(const Tab which) const {
        if (!ready_for(which)) return {};
        const auto exe = game_exe();
        if (exe.empty()) return {};
        std::vector<std::string> args{utf8(exe), "--window"};
        if (which == Tab::network) {
            // The route, its season and weather, the time and the train are the server's.
            args.insert(args.end(), {"--mp-connect", address_text(), "--mp-nick", nick_text(), "--root", utf8(root)});
            if (net_role == net_role_dispatcher) {
                args.push_back("--mp-console");
            } else if (net_spawn == net_spawn_cab) {
                args.insert(args.end(), {"--spawn", "cab"});
            }
        } else {
            args.insert(args.end(), {"--route", routes[static_cast<std::size_t>(route)], "--root", utf8(root)});
            if (save >= 0 && save < static_cast<int>(saves.size())) {
                // A saved game: its scenario, trains, clock and weather are in
                // the file (the game reads them from it).
                args.insert(args.end(), {"--load", utf8(saves_folder() / (saves[static_cast<std::size_t>(save)] + ".save"))});
            } else if (const auto* chosen = chosen_activity()) {
                // The scenario sets the trains, the start, the time, season
                // and weather (the game reads them from it).
                args.insert(args.end(), {"--activity", chosen->file});
            } else if (situation >= 0) {
                args.insert(args.end(), {"--situation", utf8(situation_folder() / (situations[static_cast<std::size_t>(situation)] + ".situation"))});
            } else if (consist_used()) {
                args.insert(args.end(), {"--consist", consists[static_cast<std::size_t>(consist)]});
                if (path >= 0) {
                    args.push_back("--path");
                    args.push_back(paths[static_cast<std::size_t>(path)].file);
                }
            }
            if (!chosen_activity() && save < 0) {
                if (spawn != no_train) args.insert(args.end(), {"--spawn", spawn_keys[spawn]});
                char time[16];
                std::snprintf(time, sizeof time, "%02d:%02d", minutes / 60, minutes % 60);
                args.insert(args.end(), {"--time", time, "--season", season_keys[season], "--weather", weather_keys[weather]});
                if (start == 1) args.insert(args.end(), {"--start", "on"});
                if (start == 2) args.insert(args.end(), {"--start", "cold"});
            }
        }
        if (sound) {
            args.insert(args.end(), {"--volume", std::to_string(volume)});
        } else {
            args.push_back("--no-sound");
        }
        if (graphics.msaa != 4) args.insert(args.end(), {"--msaa", std::to_string(graphics.msaa)});
        if (!vsync) args.push_back("--no-vsync");
        if (graphics.experimental) args.push_back("--experimental");
        if (graphics.hdr) args.push_back("--hdr");
        std::string command;
        for (const auto& a : args) command += (command.empty() ? "" : " ") + quoted(a);
        return command;
    }

    // What the running network game is doing, or why it could not join.
    [[nodiscard]] std::string net_message() const {
        switch (net_step) {
        case NetStep::none: return {};
        case NetStep::connecting: return trf("Connecting to %s...", net_address.c_str());
        case NetStep::waiting:
            return trf("On the server, route %s. Waiting for a train: the dispatcher assigns one to %s on the console.", net_detail.c_str(),
                       net_nick.c_str());
        case NetStep::loading: return tr("A train is assigned: loading the world and the route...");
        case NetStep::playing: return trf("In the game on %s. The menu comes back when it is closed.", net_address.c_str());
        case NetStep::failed: break;
        }
        if (net_reason == "unreachable") return trf("The server %s does not answer: check the address and the port.", net_address.c_str());
        if (net_reason == "refused") {
            if (net_detail == "nick") return trf("The nick %s is in use on the server: choose another.", net_nick.c_str());
            if (net_detail == "full") return tr("The server takes no new players.");
            if (net_detail == "version") return tr("The server speaks another protocol version (RTS's is 101).");
            return trf("The server refused the login (code %s).", net_detail.c_str());
        }
        if (net_reason == "closed") return tr("The server closed the connection.");
        if (net_reason == "no-world") return tr("The server did not send the world.");
        if (net_reason == "timeout") return tr("No train was assigned in 30 minutes.");
        if (net_reason == "no-route") return trf("The server plays route %s, and it is not in the RTS folder.", net_detail.c_str());
        return trf("The game could not join the server: see openrts.log in %s.", utf8(viewer::user_data_directory()).c_str());
    }

    // A "status: <word> [detail]" line of the game (viewer/src/main.cpp).
    void take_status(const std::string& line) {
        constexpr std::string_view prefix = "status: ";
        if (!line.starts_with(prefix)) return;
        const auto rest = line.substr(prefix.size());
        const auto space = rest.find(' ');
        const auto word = rest.substr(0, space);
        const auto detail = space == std::string::npos ? std::string() : rest.substr(space + 1);
        if (word == "connecting") {
            net_step = NetStep::connecting;
        } else if (word == "waiting") {
            net_step = NetStep::waiting;
            net_detail = detail;
        } else if (word == "loading") {
            net_step = NetStep::loading;
        } else if (word == "playing") {
            net_step = NetStep::playing;
        } else {
            net_step = NetStep::failed;
            net_reason = word;
            net_detail = detail;
        }
        status = net_message();
    }

    // The game's new lines in openrts.log, for its status lines.
    void read_log() {
#ifdef _WIN32
        const auto log_path = viewer::user_data_directory() / L"openrts.log";
        const HANDLE file = CreateFileW(log_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER at{};
        at.QuadPart = static_cast<LONGLONG>(log_read);
        if (SetFilePointerEx(file, at, nullptr, FILE_BEGIN)) {
            char buffer[16384];
            DWORD got = 0;
            while (ReadFile(file, buffer, sizeof buffer, &got, nullptr) && got > 0) {
                log_read += got;
                log_line.append(buffer, got);
                for (auto end = log_line.find('\n'); end != std::string::npos; end = log_line.find('\n')) {
                    auto line = log_line.substr(0, end);
                    log_line.erase(0, end + 1);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    take_status(line);
                }
            }
        }
        CloseHandle(file);
#endif
    }

    void launch(const Tab which) {
        if (running() || !ready_for(which)) return;
        const auto command = command_line(which);
        if (command.empty()) {
            status = tr("openrts_viewer.exe is not found next to the menu.");
            return;
        }
        network_game = which == Tab::network;
        cancelled = false;
        net_step = network_game ? NetStep::connecting : NetStep::none;
        net_reason.clear();
        net_detail.clear();
        net_address = address_text();
        net_nick = nick_text();
        log_read = 0;
        log_line.clear();
        if (network_game) {
            std::erase(servers, net_address);
            servers.insert(servers.begin(), net_address);
            if (servers.size() > servers_kept) servers.resize(servers_kept);
        }
        save_settings();
#ifdef _WIN32
        const int length = MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, nullptr, 0);
        std::wstring wide(static_cast<std::size_t>((std::max)(length, 1)), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, wide.data(), length);
        // The game's console output goes to openrts.log beside the settings
        // (no console window); a network game's steps are read back from it.
        SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        const auto log_path = viewer::user_data_directory() / L"openrts.log";
        // The last two runs' logs kept beside it (openrts.1.log, then
        // openrts.2.log): a player who starts the game again has not lost
        // what happened (testers 2026-11-21: a helper's log, needed for a
        // network bug, was gone once he logged in again).
        {
            const auto older = viewer::user_data_directory() / L"openrts.2.log";
            const auto old = viewer::user_data_directory() / L"openrts.1.log";
            MoveFileExW(old.c_str(), older.c_str(), MOVEFILE_REPLACE_EXISTING);
            MoveFileExW(log_path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        const HANDLE log = CreateFileW(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        STARTUPINFOW startup{};
        startup.cb = sizeof startup;
        if (log != INVALID_HANDLE_VALUE) {
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdOutput = log;
            startup.hStdError = log;
            startup.hStdInput = nullptr;
        }
        PROCESS_INFORMATION info{};
        const BOOL ok = CreateProcessW(nullptr, wide.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                       &startup, &info);
        if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
        if (!ok) {
            net_step = NetStep::none;
            status = viewer::trf("The game did not start (error %lu).", static_cast<unsigned long>(GetLastError()));
            return;
        }
        CloseHandle(info.hThread);
        process = info.hProcess;
        status = network_game ? net_message() : std::string(tr("The game is running. The menu comes back when it is closed."));
#else
        status = tr("Starting the game is only done on Windows yet.");
#endif
    }

    // Stops a network game that is still joining (the server sees the
    // connection close).
    void cancel() {
#ifdef _WIN32
        if (!process || !joining()) return;
        TerminateProcess(process, 1);
        cancelled = true;
#endif
    }

    void poll() {
        if (const auto now = std::chrono::steady_clock::now(); now >= next_situation_scan) {
            next_situation_scan = now + std::chrono::milliseconds(1500);
            refresh_situations();
            refresh_saves();
        }
#ifdef _WIN32
        if (!process) return;
        const bool ended = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
        const auto now = std::chrono::steady_clock::now();
        if (joining() && !cancelled && (ended || now >= next_log_check)) {
            next_log_check = now + std::chrono::milliseconds(250);
            read_log();
        }
        if (!ended) return;
        DWORD code = 0;
        GetExitCodeProcess(process, &code);
        CloseHandle(process);
        process = nullptr;
        // Situations saved in the game are there at once.
        refresh_situations();
        // The game's panel may have changed it.
        fps_limit = viewer::clamp_fps_limit(remembered_int("fps_limit", 0));
        window_mode = viewer::parse_window_mode(remembered("window_mode").value_or("")).value_or(viewer::WindowMode::windowed);
        load_graphics();
        if (cancelled) {
            net_step = NetStep::none;
            status = tr("The connection was cancelled.");
        } else if (!network_game || net_step != NetStep::failed) {
            // A failed join keeps its status: it says why.
            net_step = NetStep::none;
            status = code == static_cast<DWORD>(viewer::exit_code::server_gone) ? std::string(tr("The server stopped: the network game is over."))
                   : code == 0 ? std::string(tr("The game is over."))
                               : viewer::trf("The game ended with code %lu: see openrts.log in %s.", static_cast<unsigned long>(code),
                                             utf8(viewer::user_data_directory()).c_str());
        }
#endif
    }

#ifdef OPENRTS_HAS_IMGUI
    void draw_home();
    void draw_solo();
    void draw_settings_page();
    void draw_account();
    void draw_soon();
    void draw_footer(ImDrawList* draw, float scale, ImVec2 display);
    void draw_tabs();
    void draw_single(float height);
    void draw_network(float height);
    void draw_settings(float height);
    void draw_graphics(float height);
    void draw_info(float height);
    void draw_bar(float button_width, float button_height, float status_height);
#endif
};

#ifdef OPENRTS_HAS_IMGUI
// The clock and the date of the header (the mockup's right side).
[[nodiscard]] std::pair<std::string, std::string> header_clock() {
    const auto now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char time[8]{};
    std::snprintf(time, sizeof time, "%02d:%02d", local.tm_hour, local.tm_min);
    constexpr const char* months[] = {"января", "февраля", "марта", "апреля", "мая", "июня",
                                      "июля", "августа", "сентября", "октября", "ноября", "декабря"};
    char date[64]{};
    std::snprintf(date, sizeof date, "%d %s %d", local.tm_mday, months[std::clamp(local.tm_mon, 0, 11)], local.tm_year + 1900);
    return {time, date};
}

void Launcher::State::draw_footer(ImDrawList* draw, const float scale, const ImVec2 display) {
    const auto y = display.y - 38.0f * scale;
    const auto left = std::string("DARK TEAM  /  Build: ") + std::string(core::version());
    label(draw, ui::small_font(), 12.0f * scale, {72.0f * scale, y}, dim, left);

    struct Link {
        const char* text;
        const char* url;
    };
    constexpr Link links[] = {{"Discord ↗", "https://discord.gg/j8pkDAB4be"},
                              {"Telegram ↗", "https://t.me/darkteamdev"},
                              {"ВКонтакте ↗", "https://vk.com/darkteamdev"},
                              {"dark-team.ru ↗", "https://dark-team.ru"}};
    float right = display.x - 72.0f * scale;
    for (int i = 3; i >= 0; --i) {
        const auto text = std::string(links[i].text);
        const auto measured = label_size(ui::small_font(), 12.0f * scale, text);
        const auto area = Rect{{right - measured.x, y - 2.0f * scale}, {right, y + measured.y + 2.0f * scale}};
        ImGui::SetCursorScreenPos(area.min);
        ImGui::PushID(i + 100);
        const bool pressed = ImGui::InvisibleButton("##link", {area.width(), area.height()});
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        label(draw, ui::small_font(), 12.0f * scale, area.min, hovered ? white : grey, text);
        if (pressed) open_link(links[i].url);
        right -= measured.x + 22.0f * scale;
    }
}

void Launcher::State::draw_home() {
    auto* draw = ImGui::GetWindowDrawList();
    const auto display = ImGui::GetIO().DisplaySize;
    const auto scale = menu_scale();
    const auto margin = 72.0f * scale;

    auto* cache = images.get();
    draw->AddRectFilledMultiColor({0, 0}, display, rgb(14, 16, 19, 255), rgb(15, 17, 20, 255), rgb(21, 23, 28, 255),
                                  rgb(19, 21, 25, 255));

    const auto [clock, date] = header_clock();
    label(draw, ui::brand_font(), 34.0f * scale, {margin, 48.0f * scale}, white, "DARK TEAM RAILWAY ENGINE");
    label(draw, ui::body_font(), 14.0f * scale, {margin + 1.0f * scale, 92.0f * scale}, grey, "Инновации в движении");

    const auto top_right = display.x - margin;
    const ImVec2 avatar_centre{top_right - 274.0f * scale, 82.0f * scale};
    const auto clock_size = label_size(ui::clock_font(), 29.0f * scale, clock);
    const auto date_size = label_size(ui::small_font(), 12.0f * scale, date);
    const auto clock_block = clock_size.y + 6.0f * scale + date_size.y;
    const auto clock_top = avatar_centre.y - clock_block * 0.5f;
    label_right(draw, ui::clock_font(), 29.0f * scale, top_right - 330.0f * scale, clock_top, white, clock);
    label_right(draw, ui::small_font(), 12.0f * scale, top_right - 330.0f * scale, clock_top + clock_size.y + 6.0f * scale, grey, date);
    draw->AddLine({top_right - 304.0f * scale, avatar_centre.y - 26.0f * scale},
                  {top_right - 304.0f * scale, avatar_centre.y + 26.0f * scale}, rgb(255, 255, 255, 38), 1.0f);
    draw->AddCircleFilled(avatar_centre, 22.0f * scale, rgb(42, 46, 55), 44);
    label_centered(draw, ui::heading_font(), 18.0f * scale, avatar_centre, white, "Г");

    const auto name_size = label_size(ui::button_font(), 14.0f * scale, "Гость");
    const auto level_size = label_size(ui::small_font(), 11.0f * scale, "Уровень 0");
    const auto name_block = name_size.y + 3.0f * scale + level_size.y;
    const auto name_top = avatar_centre.y - name_block * 0.5f;
    label(draw, ui::button_font(), 14.0f * scale, {top_right - 240.0f * scale, name_top}, white, "Гость");
    label(draw, ui::small_font(), 11.0f * scale, {top_right - 240.0f * scale, name_top + name_size.y + 3.0f * scale}, dim,
          "Уровень 0");
    const auto account = Rect{{top_right - 132.0f * scale, avatar_centre.y - 22.0f * scale},
                              {top_right, avatar_centre.y + 22.0f * scale}};
    if (header_button(draw, "##account", account, "Войти", scale, account_hover)) page = Page::account;

    const auto content = Rect{{margin, 156.0f * scale}, {display.x - margin, display.y - 88.0f * scale}};
    const auto gap = 24.0f * scale;
    const auto row1 = content.height() * (452.0f / 836.0f);
    const auto first = content.width() * (1072.0f / 1776.0f);
    // The second row's three cards share what the gaps leave: 616, 616 and 496 px.
    const auto row2_width = content.width() - 2.0f * gap;
    const auto third = row2_width * (616.0f / 1728.0f);
    const auto fourth = third;
    const auto fifth = row2_width - third - fourth;

    const auto load = [&](const char* name) {
        if (!cache) return WGPUTextureView{};
        return cache->load(fs::path(image_dir) / name);
    };
    const auto pixels_of = [&](const char* name) {
        return cache ? cache->size(fs::path(image_dir) / name) : std::pair{0, 0};
    };

    if (draw_card(draw, "card1", {content.min, {content.min.x + first, content.min.y + row1}}, "01", "Машинист",
                  "Займите место за пультом. Ваш первый рейс начинается здесь", load("cab.png"), pixels_of("cab.png"), true, false,
                  true, card_hover[0], scale)) {
        page = Page::solo;
    }
    if (draw_card(draw, "card2", {{content.min.x + first + gap, content.min.y}, {content.max.x, content.min.y + row1}}, "02", "Редактор",
                  "Создавайте составы и собственные сценарии", load("field.png"), pixels_of("field.png"), false, false, false,
                  card_hover[1], scale)) {
        soon_kind = 2;
        page = Page::soon;
    }
    const auto row2_top = content.min.y + row1 + gap;
    (void)draw_card(draw, "card3", {{content.min.x, row2_top}, {content.min.x + third, content.max.y}}, "03", "Мультиплеер",
                    "Общий маршрут. Настоящее РП", load("station.png"), pixels_of("station.png"), false, true, false, card_hover[2], scale);
    (void)draw_card(draw, "card4", {{content.min.x + third + gap, row2_top}, {content.min.x + third + gap + fourth, content.max.y}}, "04",
                    "Депо", "Обучение управлению", load("emu.png"), pixels_of("emu.png"), false, true, false, card_hover[3], scale);

    // Settings: no picture, the mockup's list of rows.
    const Rect settings_box{{content.max.x - fifth, row2_top}, {content.max.x, content.max.y}};
    draw->AddRectFilled(settings_box.min, settings_box.max, card_fill, 24.0f * scale);
    draw->AddRect(settings_box.min, settings_box.max, card_line, 24.0f * scale);
    label(draw, ui::small_font(), 14.0f * scale, {settings_box.min.x + 28.0f * scale, settings_box.min.y + 35.0f * scale}, dim, "05");
    label(draw, ui::heading_font(), 28.0f * scale, {settings_box.min.x + 55.0f * scale, settings_box.min.y + 27.0f * scale}, white,
          "Настройки");
    constexpr const char* rows[] = {"Графика", "Звук", "Интерфейс", "Улучшения", "Управление"};
    const auto row_top = settings_box.min.y + 92.0f * scale;
    const Rect settings_head{{settings_box.min.x + 20.0f * scale, settings_box.min.y + 16.0f * scale},
                             {settings_box.max.x - 20.0f * scale, row_top - 4.0f * scale}};
    ImGui::SetCursorScreenPos(settings_head.min);
    const bool settings_head_pressed = ImGui::InvisibleButton("##settings_head", {settings_head.width(), settings_head.height()});
    if (settings_head_pressed) {
        page = Page::settings;
        settings_section = 0;
    }
    const auto row_height = (settings_box.height() - 116.0f * scale) / 5.0f;
    for (int i = 0; i < 5; ++i) {
        const Rect row{{settings_box.min.x + 20.0f * scale, row_top + row_height * static_cast<float>(i)},
                       {settings_box.max.x - 20.0f * scale, row_top + row_height * static_cast<float>(i) + row_height}};
        ImGui::SetCursorScreenPos(row.min);
        ImGui::PushID(i + 20);
        const bool pressed = ImGui::InvisibleButton("##row", {row.width(), row.height()});
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const auto row_text = label_size(ui::body_font(), 15.0f * scale, rows[i]);
        label(draw, ui::body_font(), 15.0f * scale, {row.min.x + 8.0f * scale, row.centre().y - row_text.y * 0.5f},
              hovered ? white : grey, rows[i]);
        arrow_up_right(draw, {row.max.x - 15.0f * scale, row.centre().y}, 12.0f * scale,
                       hovered ? white : dim, 1.5f * scale);
        if (pressed) {
            page = Page::settings;
            settings_section = i == 0 ? 1 : i == 1 ? 3 : i == 2 ? 0 : i == 4 ? 2 : 4;
        }
    }

    draw_footer(draw, scale, display);
}

void Launcher::State::draw_solo() {
    auto* draw = ImGui::GetWindowDrawList();
    const auto display = ImGui::GetIO().DisplaySize;
    const auto scale = menu_scale();
    const auto margin = 72.0f * scale;
    const auto background = fs::path(image_dir) / "station.png";
    auto* cache = images.get();
    picture(draw, cache ? cache->load(background) : nullptr, cache ? cache->size(background) : std::pair{0, 0}, {{0, 0}, display}, 0.0f);
    draw->AddRectFilled({0, 0}, display, rgb(10, 12, 16, 238));

    const auto [clock, date] = header_clock();
    const Rect header{{margin, 26.0f * scale}, {display.x - margin, 94.0f * scale}};
    bool back = false, account_clicked = false;
    page_heading(draw, header, "Машинист", "Маршрут, локомотив, состав и условия поездки", scale, clock, date, true, account_hover, back,
                 account_clicked);
    if (back) page = Page::home;
    if (account_clicked) page = Page::account;

    const auto top = 128.0f * scale;
    const auto bottom = display.y - 116.0f * scale;
    ImGui::SetCursorScreenPos({margin, top});
    ImGui::BeginChild("##solo_setup", {display.x - margin * 2.0f, bottom - top}, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    draw_single(bottom - top - 4.0f * scale);
    ImGui::EndChild();

    const auto button = Rect{{display.x - margin - 250.0f * scale, bottom + 14.0f * scale}, {display.x - margin, bottom + 58.0f * scale}};
    start_hover += ((ImGui::IsMouseHoveringRect(button.min, button.max) && ready() ? 1.0f : 0.0f) - start_hover) *
                   (std::min)(1.0f, ImGui::GetIO().DeltaTime * 12.0f);
    ImGui::SetCursorScreenPos(button.min);
    ImGui::BeginDisabled(!ready());
    const bool start = ImGui::InvisibleButton("##start", {button.width(), button.height()});
    ImGui::EndDisabled();
    draw->AddRectFilled(button.min, button.max, ready() ? rgb(247, 248, 250) : rgb(45, 48, 55), 12.0f * scale);
    const auto start_text = std::string("Начать поездку  →");
    const auto measured = label_size(ui::button_font(), 15.0f * scale, start_text);
    label(draw, ui::button_font(), 15.0f * scale, {button.centre().x - measured.x * 0.5f, button.centre().y - measured.y * 0.5f},
          ready() ? rgb(19, 21, 25) : dim, start_text);
    if (start) begin_session();

    if (!ready()) {
        label(draw, ui::small_font(), 12.0f * scale, {margin, button.centre().y - 8.0f * scale}, grey,
              "Выберите маршрут и состав, чтобы начать");
    } else if (!status.empty()) {
        label(draw, ui::small_font(), 12.0f * scale, {margin, button.centre().y - 8.0f * scale}, grey, status);
    }
}

void Launcher::State::draw_settings_page() {
    MenuStyle style;
    auto* draw = ImGui::GetWindowDrawList();
    const auto display = ImGui::GetIO().DisplaySize;
    const auto scale = menu_scale();
    const auto margin = 72.0f * scale;
    const auto background = fs::path(image_dir) / "station.png";
    auto* cache = images.get();
    picture(draw, cache ? cache->load(background) : nullptr, cache ? cache->size(background) : std::pair{0, 0}, {{0, 0}, display}, 0.0f);
    draw->AddRectFilled({0, 0}, display, rgb(10, 12, 16, 242));

    const auto [clock, date] = header_clock();
    const Rect header{{margin, 26.0f * scale}, {display.x - margin, 94.0f * scale}};
    bool back = false, account_clicked = false;
    page_heading(draw, header, "Настройки", "Игра, графика и управление", scale, clock, date, true, account_hover, back, account_clicked);
    if (back) {
        if (embedded_settings) embedded_back = true;
        else page = Page::home;
    }
    if (account_clicked && !embedded_settings) page = Page::account;

    const auto top = 128.0f * scale;
    const auto bottom = display.y - 80.0f * scale;
    const auto side_width = 260.0f * scale;
    const Rect side{{margin, top}, {margin + side_width, bottom}};
    draw->AddRectFilled(side.min, side.max, rgb(20, 22, 27, 230), 18.0f * scale);
    draw->AddRect(side.min, side.max, card_line, 18.0f * scale);
    constexpr const char* sections[] = {"Игра и звук", "Графика", "Управление", "Звук", "Улучшения"};
    for (int i = 0; i < 5; ++i) {
        const Rect row{{side.min.x + 10.0f * scale, side.min.y + 12.0f * scale + static_cast<float>(i) * 46.0f * scale},
                       {side.max.x - 10.0f * scale, side.min.y + 50.0f * scale + static_cast<float>(i) * 46.0f * scale}};
        ImGui::SetCursorScreenPos(row.min);
        ImGui::PushID(i + 400);
        const bool pressed = ImGui::InvisibleButton("##section", {row.width(), row.height()});
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        if (settings_section == i) draw->AddRectFilled(row.min, row.max, rgb(255, 255, 255, 14), 10.0f * scale);
        const auto section_size = label_size(ui::button_font(), 14.0f * scale, sections[i]);
        label(draw, ui::button_font(), 14.0f * scale, {row.min.x + 14.0f * scale, row.centre().y - section_size.y * 0.5f},
              settings_section == i || hovered ? white : grey, sections[i]);
        if (pressed) settings_section = i;
    }

    const auto content_left = side.max.x + 24.0f * scale;
    ImGui::SetCursorScreenPos({content_left, top});
    ImGui::PushFont(ui::body_font());
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.075f, 0.085f, 0.105f, 0.94f});
    ImGui::BeginChild("##settings_content", {display.x - margin - content_left, bottom - top}, ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    switch (settings_section) {
    case 1: draw_graphics(bottom - top - 24.0f * scale); break;
    case 2: draw_info(bottom - top - 24.0f * scale); break;
    case 3: draw_settings(bottom - top - 24.0f * scale); break;
    case 4: draw_ace_settings(); break;
    default: draw_settings(bottom - top - 24.0f * scale); break;
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void Launcher::State::draw_account() {
    auto* draw = ImGui::GetWindowDrawList();
    const auto display = ImGui::GetIO().DisplaySize;
    const auto scale = menu_scale();
    const auto margin = 72.0f * scale;
    const auto background = fs::path(image_dir) / "station.png";
    auto* cache = images.get();
    picture(draw, cache ? cache->load(background) : nullptr, cache ? cache->size(background) : std::pair{0, 0}, {{0, 0}, display}, 0.0f);
    draw->AddRectFilled({0, 0}, display, rgb(10, 12, 16, 242));

    const auto [clock, date] = header_clock();
    const Rect header{{margin, 26.0f * scale}, {display.x - margin, 94.0f * scale}};
    bool back = false, account_clicked = false;
    page_heading(draw, header, "Профиль", "Личный кабинет", scale, clock, date, true, account_hover, back, account_clicked);
    if (back) page = Page::home;

    const auto width = 560.0f * scale;
    const auto height = 300.0f * scale;
    const Rect box{{(display.x - width) * 0.5f, (display.y - height) * 0.5f}, {(display.x + width) * 0.5f, (display.y + height) * 0.5f}};
    draw->AddRectFilled(box.min, box.max, card_fill, 24.0f * scale);
    draw->AddRect(box.min, box.max, card_line, 24.0f * scale);
    draw->AddCircleFilled({box.centre().x, box.min.y + 76.0f * scale}, 36.0f * scale, rgb(42, 46, 55), 48);
    label_centered(draw, ui::heading_font(), 30.0f * scale, {box.centre().x, box.min.y + 76.0f * scale}, white, "Г");
    const auto title = std::string("Вы вошли как гость");
    const auto title_size = label_size(ui::heading_font(), 24.0f * scale, title);
    label(draw, ui::heading_font(), 24.0f * scale, {box.centre().x - title_size.x * 0.5f, box.min.y + 128.0f * scale}, white, title);
    const auto text = std::string("Вход в аккаунт появится в следующих обновлениях. Одиночная игра доступна без аккаунта.");
    const auto text_size = label_size(ui::body_font(), 14.0f * scale, text);
    label(draw, ui::body_font(), 14.0f * scale, {box.centre().x - text_size.x * 0.5f, box.min.y + 166.0f * scale}, grey, text);
    soon_badge(draw, {box.centre().x - 36.0f * scale, box.min.y + 212.0f * scale}, scale, 1.0f);
}

void Launcher::State::draw_soon() {
    auto* draw = ImGui::GetWindowDrawList();
    const auto display = ImGui::GetIO().DisplaySize;
    const auto scale = menu_scale();
    const auto margin = 72.0f * scale;
    const auto background = fs::path(image_dir) / "station.png";
    auto* cache = images.get();
    picture(draw, cache ? cache->load(background) : nullptr, cache ? cache->size(background) : std::pair{0, 0}, {{0, 0}, display}, 0.0f);
    draw->AddRectFilled({0, 0}, display, rgb(10, 12, 16, 242));

    const char* names[] = {"Мультиплеер", "Депо", "Редактор"};
    const char* lines[] = {"Совместные поездки на серверах dark-team.ru", "Подвижной состав и раскраски", "Создание и правка маршрутов"};
    const auto [clock, date] = header_clock();
    const Rect header{{margin, 26.0f * scale}, {display.x - margin, 94.0f * scale}};
    bool back = false, account_clicked = false;
    page_heading(draw, header, names[std::clamp(soon_kind, 0, 2)], lines[std::clamp(soon_kind, 0, 2)], scale, clock, date, true,
                 account_hover, back, account_clicked);
    if (back) page = Page::home;

    const auto width = 620.0f * scale;
    const auto height = 220.0f * scale;
    const Rect box{{(display.x - width) * 0.5f, (display.y - height) * 0.5f}, {(display.x + width) * 0.5f, (display.y + height) * 0.5f}};
    draw->AddRectFilled(box.min, box.max, card_fill, 24.0f * scale);
    draw->AddRect(box.min, box.max, card_line, 24.0f * scale);
    soon_badge(draw, {box.centre().x - 41.4f * scale, box.min.y + 42.0f * scale}, scale * 1.15f, 1.0f);
    const auto text = std::string("Раздел появится в одном из следующих обновлений.");
    const auto measured = label_size(ui::body_font(), 15.0f * scale, text);
    label(draw, ui::body_font(), 15.0f * scale, {box.centre().x - measured.x * 0.5f, box.min.y + 110.0f * scale}, grey, text);
    const auto sub = std::string("Следите за новостями в Discord и Telegram.");
    const auto sub_size = label_size(ui::small_font(), 13.0f * scale, sub);
    label(draw, ui::small_font(), 13.0f * scale, {box.centre().x - sub_size.x * 0.5f, box.min.y + 142.0f * scale}, dim, sub);
}

// The tabs. Their items close at once; the chosen tab is drawn below them.
void Launcher::State::draw_tabs() {    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {20.0f, 8.0f});
    auto* large = viewer::large_font();
    if (large) ImGui::PushFont(large);
    // A tab chosen in code shows on the next frame; till then the bar's
    // own choice does not count.
    const bool forcing = select_tab;
    select_tab = false;
    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_NoTooltip)) {
        const std::string labels[] = {std::string(tr("Single player")) + "###single", std::string(tr("Network game")) + "###network",
                                      std::string(tr("Settings")) + "###settings", std::string(tr("Graphics")) + "###graphics",
                                      std::string(tr("Information")) + "###info"};
        for (int i = 0; i < tab_count; ++i) {
            const auto which = static_cast<Tab>(i);
            const ImGuiTabItemFlags flags = forcing && tab == which ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (!ImGui::BeginTabItem(labels[i].c_str(), nullptr, flags)) continue;
            ImGui::EndTabItem();
            if (!forcing && tab != which) {
                tab = which;
                remember("launcher_tab", tab_keys[i]);
            }
        }
        ImGui::EndTabBar();
    }
    if (large) ImGui::PopFont();
    ImGui::PopStyleVar();
}

// Single player: route, consist (with its search) and path on the left, the
// trip on the right.
void Launcher::State::draw_single(const float height) {
    MenuStyle style;
    if (!vfs) {
        if (ImGui::BeginChild("##single_empty", {0.0f, height}, panel_flags)) {
            ImGui::TextWrapped("%s", tr("The routes and consists are read from the RTS folder: give it in Settings."));
            ImGui::Spacing();
            if (ImGui::Button(tr("Go to Settings"))) {
                tab = Tab::settings;
                select_tab = true;
            }
        }
        ImGui::EndChild();
        return;
    }
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    ImGui::PushFont(ui::body_font());
    const float options_width = std::clamp(ImGui::GetIO().DisplaySize.x * 0.36f, 430.0f, 540.0f);
    const float lists_width = ImGui::GetContentRegionAvail().x - options_width - spacing;

    if (ImGui::BeginChild("##lists", {lists_width, height})) {
        constexpr auto table_flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame;
        if (ImGui::BeginTable("##choice", 3, table_flags, ImGui::GetContentRegionAvail())) {
            const auto route_header = std::string(tr("Route")) + "  " + std::to_string(routes.size()) + "###route";
            const auto consist_header = std::string(tr("Consist")) + "  " + std::to_string(consists.size()) + "###consist";
            const auto path_header = std::string(tr("Train path")) + "  " + std::to_string(paths.size()) + "###path";
            ImGui::TableSetupColumn(route_header.c_str());
            ImGui::TableSetupColumn(consist_header.c_str());
            ImGui::TableSetupColumn(path_header.c_str());
            ImGui::TableHeadersRow();
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            if (ImGui::BeginListBox("##routes", {-FLT_MIN, -FLT_MIN})) {
                for (std::size_t i = 0; i < routes.size(); ++i) {
                    if (ImGui::Selectable(routes[i].c_str(), route == static_cast<int>(i)) && route != static_cast<int>(i)) {
                        route = static_cast<int>(i);
                        load_paths();
                    }
                }
                ImGui::EndListBox();
            }

            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!consist_used());
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##filter", tr("search"), filter.data(), filter.size());
            const auto wanted = viewer::search_form(filter.data());
            if (ImGui::BeginListBox("##consists", {-FLT_MIN, -FLT_MIN})) {
                for (std::size_t i = 0; i < consists.size(); ++i) {
                    if (!wanted.empty() && i < consist_search.size() && consist_search[i].find(wanted) == std::string::npos) continue;
                    if (ImGui::Selectable(consists[i].c_str(), consist == static_cast<int>(i))) consist = static_cast<int>(i);
                }
                ImGui::EndListBox();
            }

            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!consist_used());
            if (ImGui::BeginListBox("##paths", {-FLT_MIN, -FLT_MIN})) {
                if (ImGui::Selectable(tr("(the route's first path)"), path < 0)) path = -1;
                for (std::size_t i = 0; i < paths.size(); ++i) {
                    const auto& p = paths[i];
                    const auto label = (p.name.empty() ? p.file : p.name) + "##" + p.file;
                    if (ImGui::Selectable(label.c_str(), path == static_cast<int>(i))) path = static_cast<int>(i);
                    if (ImGui::IsItemHovered() && !p.start.empty()) ImGui::SetTooltip("%s -> %s (%s)", p.start.c_str(), p.end.c_str(), p.file.c_str());
                }
                ImGui::EndListBox();
            }
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    if (ImGui::BeginChild("##trip_panel", {options_width, height}, panel_flags)) {
        const float label_width = std::clamp(options_width * 0.48f, 190.0f, 240.0f);
        ImGui::SeparatorText(tr("Trip"));
        const auto* scenario = chosen_activity();
        if (begin_form("##saved_game_form", label_width)) {
            // A saved game (as RTS's Load): it sets everything below.
            form_row(tr("Saved game"));
            const auto fresh = std::string(tr("(a new game)"));
            const auto& shown = save >= 0 && save < static_cast<int>(saves.size()) ? saves[static_cast<std::size_t>(save)] : fresh;
            ImGui::BeginDisabled(saves.empty());
            if (ImGui::BeginCombo("##saved_game", shown.c_str())) {
                if (ImGui::Selectable(fresh.c_str(), save < 0)) save = -1;
                for (int i = 0; i < static_cast<int>(saves.size()); ++i)
                    if (ImGui::Selectable(saves[static_cast<std::size_t>(i)].c_str(), save == i)) save = i;
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            ImGui::EndTable();
        }
        if (save >= 0) hint(tr("The saved game sets the scenario, the trains, the time and the weather: the choices below wait."));
        ImGui::BeginDisabled(save >= 0);
        if (begin_form("##scenario_form", label_width)) {
            // The route's RTS scenarios (RTS_ACT): one sets everything below.
            form_row(tr("Scenario"));
            const auto none = std::string(tr("none"));
            const auto shown = scenario ? (scenario->activity.name.empty() ? scenario->file : scenario->activity.name) : none;
            ImGui::BeginDisabled(activities.empty());
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##activity", shown.c_str())) {
                if (ImGui::Selectable(none.c_str(), activity < 0)) activity = -1;
                for (int i = 0; i < static_cast<int>(activities.size()); ++i) {
                    const auto& file = activities[static_cast<std::size_t>(i)];
                    const auto label = (file.activity.name.empty() ? file.file : file.activity.name) + "##" + file.file;
                    if (ImGui::Selectable(label.c_str(), activity == i)) activity = i;
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            ImGui::EndTable();
        }
        if (scenario) {
            const auto& a = scenario->activity;
            const auto clock = [](const std::int32_t seconds) {
                char text[16];
                std::snprintf(text, sizeof text, "%02d:%02d", seconds / 3600 % 24, seconds / 60 % 60);
                return std::string(text);
            };
            if (!a.starts_in.empty() || !a.ends_in.empty()) hint(trf("From %s to %s", a.starts_in.c_str(), a.ends_in.c_str()).c_str());
            hint(trf("Starts at %s, lasts %s; %s, %s", clock(a.start_time).c_str(), clock(a.duration).c_str(),
                     tr(season_names[std::clamp(a.season, 0, 3)]), tr(weather_names[std::clamp(a.weather, 0, 2)]))
                     .c_str());
            if (!a.description.empty()) hint(a.description.c_str());
            std::size_t traffic = 0;
            for (const auto& service : a.services)
                if (service.type == formats::ActivityServiceType::traffic) traffic += service.trains.size();
            hint(trf("The scenario's events run: its messages (F11), its reports (F8), signals, sounds; its %zu traffic trains come "
                     "out at their times and run as RTS's traffic (block points, the end of their paths).",
                     traffic)
                     .c_str());
        } else if (activities.empty() && route >= 0) {
            hint(tr("The route has no scenarios (RTS_ACT)."));
        }
        ImGui::Spacing();
        ImGui::BeginDisabled(scenario != nullptr);
        if (begin_form("##trip", label_width)) {
            char time[16];
            std::snprintf(time, sizeof time, "%02d:%02d", minutes / 60, minutes % 60);
            form_row(tr("Time"));
            ImGui::SliderInt("##time", &minutes, 0, 24 * 60 - 1, time);
            const char* seasons[4];
            for (int i = 0; i < 4; ++i) seasons[i] = tr(season_names[i]);
            form_row(tr("Season"));
            ImGui::Combo("##season", &season, seasons, 4);
            const char* weathers[4];
            for (int i = 0; i < 4; ++i) weathers[i] = tr(weather_names[i]);
            form_row(tr("Weather"));
            ImGui::Combo("##weather", &weather, weathers, 4);
            const char* starts[3];
            for (int i = 0; i < 3; ++i) starts[i] = tr(start_names[i]);
            form_row(tr("Train start"));
            ImGui::Combo("##start", &start, starts, 3);
            const char* spawns[4];
            for (int i = 0; i < 4; ++i) spawns[i] = tr(spawn_names[i]);
            form_row(tr("Start at"));
            ImGui::Combo("##spawn", &spawn, spawns, 4);
            form_row(tr("Situation"));
            const auto none = std::string(tr("none"));
            const auto& shown = situation >= 0 ? situations[static_cast<std::size_t>(situation)] : none;
            ImGui::BeginDisabled(situations.empty());
            if (ImGui::BeginCombo("##situation", shown.c_str())) {
                if (ImGui::Selectable(none.c_str(), situation < 0)) situation = -1;
                for (int i = 0; i < static_cast<int>(situations.size()); ++i) {
                    if (ImGui::Selectable(situations[static_cast<std::size_t>(i)].c_str(), situation == i)) situation = i;
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            ImGui::EndTable();
        }
        ImGui::EndDisabled();
        ImGui::Spacing();
        if (!scenario) {
            // A scenario sets all of it.
            if (spawn == beside_the_car) hint(tr("On foot next to the cab car, as RTS starts an activity."));
            if (spawn == no_train) hint(tr("A flight over the route; the first consist put on the track in Track control (Ctrl+Alt+F7) is yours."));
            if (situation >= 0) {
                hint(tr("The situation's trains replace the consist chosen here."));
            } else if (situations.empty()) {
                hint(tr("Situations are saved in the game: Train window, Situations."));
            }
        }
        ImGui::EndDisabled(); // the choices a saved game sets
    }
    ImGui::EndChild();
    ImGui::PopFont();
}

// Network play: joining a server on the left; an own server on the right,
// its place kept for when there is one.
void Launcher::State::draw_network(const float height) {
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float host_width = std::clamp(ImGui::GetIO().DisplaySize.x * 0.36f, 430.0f, 540.0f);
    const float join_width = ImGui::GetContentRegionAvail().x - host_width - spacing;

    if (ImGui::BeginChild("##join", {join_width, height}, panel_flags)) {
        const float label_width = std::clamp(join_width * 0.30f, 170.0f, 240.0f);
        ImGui::SeparatorText(tr("Join a server"));
        if (begin_form("##join_form", label_width)) {
            form_row(tr("Server address"));
            ImGui::InputTextWithHint("##address", "127.0.0.1:800", address.data(), address.size());
            form_row(tr("Nick"));
            ImGui::InputText("##nick", nick.data(), nick.size());
            if (console_role_shown) {
                const char* roles[2];
                for (int i = 0; i < 2; ++i) roles[i] = tr(net_role_names[i]);
                form_row(tr("Join as"));
                ImGui::Combo("##net_role", &net_role, roles, 2);
            }
            if (net_role != net_role_dispatcher) {
                const char* spawns[2];
                for (int i = 0; i < 2; ++i) spawns[i] = tr(net_spawn_names[i]);
                form_row(tr("Start at"));
                ImGui::Combo("##net_spawn", &net_spawn, spawns, 2);
            }
            ImGui::EndTable();
        }
        if (net_role == net_role_dispatcher) {
            hint(tr("The dispatcher's console of an openRTS server: the route's map and the console's window (trains, players, routes, signals, time). "
                    "The server lets in a console from its own computer with any nick, from elsewhere the nicks given to it with --dispatcher."));
        } else {
            hint(net_spawn == net_spawn_cab ? tr("When the dispatcher assigns your train, you sit at its desk.")
                                            : tr("When the dispatcher assigns your train, the camera follows it from above."));
        }
        hint(tr("The port is 800 when none is given. The nick is how the other players and the dispatcher see you."));
        const auto length = characters(nick_text());
        if (length == 0) {
            warning(tr("Give a nick: 1 to 15 characters."));
        } else if (length > nick_limit) {
            warning(trf("The nick has %zu characters: RTS takes 15 at most.", length).c_str());
        }
        if (!vfs) warning(tr("Give the RTS folder in Settings: the server's route is opened from it."));
        ImGui::Spacing();
        ImGui::SeparatorText(tr("Servers joined before"));
        if (servers.empty()) {
            ImGui::TextDisabled("%s", tr("none yet"));
        } else {
            const auto rows = static_cast<float>((std::min)(servers.size(), servers_kept));
            const float box = rows * ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().FramePadding.y * 2.0f;
            if (ImGui::BeginListBox("##servers", {-FLT_MIN, box})) {
                const auto current = address_text();
                for (const auto& server : servers) {
                    if (ImGui::Selectable(server.c_str(), server == current)) set_text(address, server);
                }
                ImGui::EndListBox();
            }
        }
        ImGui::Spacing();
        ImGui::SeparatorText(tr("In the game"));
        hint(tr("The server gives the route, the season, the weather and the time. Your train is assigned by the dispatcher on the console; until then the game waits, and this menu says so."));
        hint(tr("An RTS server (rts_server.exe) takes openRTS and RTS (rts1.exe) players together."));
    }
    ImGui::EndChild();
    ImGui::SameLine();

    if (ImGui::BeginChild("##host", {host_width, height}, panel_flags)) {
        const float label_width = std::clamp(host_width * 0.40f, 150.0f, 210.0f);
        ImGui::SeparatorText(tr("Own server"));
        hint(tr("Not there yet: an openRTS server is the next stage. For now start an RTS server (rts_server.exe) and join it."));
        ImGui::Spacing();
        ImGui::BeginDisabled(true);
        if (begin_form("##host_form", label_width)) {
            form_row(tr("Port"));
            ImGui::InputInt("##host_port", &host_port, 0, 0);
            form_row(tr("Players"));
            const char* kinds[] = {tr("RTS and openRTS (Legacy)"), tr("openRTS only")};
            ImGui::Combo("##host_players", &host_players, kinds, 2);
            ImGui::EndTable();
        }
        hint(host_players == 0 ? tr("Legacy: rts1.exe joins too; the simulation follows RTS for everyone.")
                               : tr("openRTS only: its own protocol, and the server's simulation settings."));
        ImGui::Spacing();
        ImGui::Button(tr("Start the server"), {-FLT_MIN, 0.0f});
        ImGui::EndDisabled();
    }
    ImGui::EndChild();
}

// Settings: the game (RTS folder, language, keys) and the sound on the left,
// the graphics on the right.
void Launcher::State::draw_settings(const float height) {
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float half = (ImGui::GetContentRegionAvail().x - spacing) * 0.5f;
    const float label_width = std::clamp(half * 0.38f, 170.0f, 260.0f);

    if (ImGui::BeginChild("##settings_game", {half, height}, panel_flags)) {
        ImGui::SeparatorText(tr("Game"));
        if (begin_form("##game_form", label_width)) {
            form_row(tr("RTS folder"));
            const char* read = tr("Read");
            const char* browse = tr("Browse...");
            const char* registry = tr("From the registry");
            const float pad = ImGui::GetStyle().FramePadding.x * 2.0f + 24.0f;
            const float read_width = ImGui::CalcTextSize(read).x + pad;
            const float browse_width = ImGui::CalcTextSize(browse).x + pad;
            const float registry_width = ImGui::CalcTextSize(registry).x + pad;
            ImGui::SetNextItemWidth(-(read_width + browse_width + registry_width + spacing * 3.0f));
            const bool entered = ImGui::InputText("##root", root_text.data(), root_text.size(), ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            bool chosen = false;
            if (ImGui::Button(browse, {browse_width, 0.0f})) {
                // The system's folder picker, from the folder given (or the one above it).
                fs::path from = from_utf8(std::string(root_text.data()));
                std::error_code error;
                if (!from.empty() && !fs::is_directory(from, error)) from = from.parent_path();
                if (const auto folder = viewer::open_folder_dialog(from, from_utf8(tr("Choose the RTS folder (the one with ROUTES and TRAINS)")).wstring())) {
                    set_root(*folder);
                    chosen = true;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button(registry, {registry_width, 0.0f})) {
                // RTS's own key (MSTS's before it): where the installers put it.
                const auto folder = core::registered_installation();
                registry_missing = !folder;
                if (folder) {
                    set_root(*folder);
                    chosen = true;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button(read, {read_width, 0.0f}) || entered || chosen) {
                set_root(from_utf8(std::string(root_text.data())));
                scan();
                if (is_installation(root)) {
                    remember("root", utf8(root));
                    registry_missing = false;
                }
            }
            int language = viewer::ui_language() == viewer::UiLanguage::russian ? 0 : 1;
            const char* languages[] = {"Русский", "English"};
            form_row("Язык / Language");
            if (ImGui::Combo("##language", &language, languages, 2)) {
                viewer::set_ui_language(language == 0 ? viewer::UiLanguage::russian : viewer::UiLanguage::english);
                remember("language", language == 0 ? "ru" : "en");
            }
            int layout = viewer::key_layout() == viewer::KeyLayout::rts ? 0 : 1;
            const char* layouts[] = {tr("RTS (Num0 / Num1)"), tr("openRTS (no numeric keypad)")};
            form_row(tr("Keys"));
            if (ImGui::Combo("##keys", &layout, layouts, 2)) {
                viewer::set_key_layout(layout == 0 ? viewer::KeyLayout::rts : viewer::KeyLayout::openrts);
                remember("key_layout", layout == 0 ? "rts" : "openrts");
            }
            form_row(tr("Key hints at the bottom of the screen"));
            bool hints = viewer::key_hints();
            if (ImGui::Checkbox("##key_hints", &hints)) {
                viewer::set_key_hints(hints);
                remember("key_hints", hints ? "1" : "0");
            }
            form_row(tr("Mouse sensitivity"));
            float sensitivity = viewer::mouse_sensitivity();
            ImGui::SetNextItemWidth(220.0f);
            if (viewer::slider_with_input("##mouse_sensitivity", &sensitivity, viewer::min_mouse_sensitivity, viewer::max_mouse_sensitivity, "%.2f", "%.2f")) {
                viewer::set_mouse_sensitivity(sensitivity);
                remember("mouse_sensitivity", std::to_string(viewer::mouse_sensitivity()));
            }
            form_row(tr("Head motion in the cab"));
            float head = viewer::head_motion();
            ImGui::SetNextItemWidth(220.0f);
            if (viewer::slider_with_input("##head_motion", &head, 0.0f, viewer::max_head_motion, "%.2f", "%.2f")) {
                viewer::set_head_motion(head);
                remember("head_motion", std::to_string(viewer::head_motion()));
            }
            ImGui::TextDisabled("%s", tr("(the head nods with speeding up, braking and rail joints and leans in curves; 0: as RTS, the cab only sways)"));
            ImGui::EndTable();
        }
        if (vfs) {
            hint(trf("Found: %zu routes, %zu consists.", routes.size(), consists.size()).c_str());
        } else {
            warning(tr("There is no ROUTES folder here: give the RTS folder."));
        }
        if (registry_missing) warning(tr("Windows's registry names no RTS folder with ROUTES (Microsoft Games\\Train Simulator\\1.0, Path)."));
        ImGui::Spacing();
        ImGui::SeparatorText(tr("Sound"));
        if (begin_form("##sound_form", label_width)) {
            form_row(tr("Sound"));
            ImGui::Checkbox("##sound", &sound);
            form_row(tr("Sound volume"));
            ImGui::BeginDisabled(!sound);
            viewer::slider_with_input("##volume", &volume, 0.0f, 1.0f, "%.2f", "%.2f");
            form_row(tr("Sound in the background (minimised or behind another window)"));
            int quiet = static_cast<int>(viewer::background_sound());
            const char* choices[] = {tr("plays as in front"), tr("quieter"), tr("silent")};
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::Combo("##background_sound", &quiet, choices, 3)) {
                viewer::set_background_sound(static_cast<viewer::BackgroundSound>(quiet));
                remember("background_sound", viewer::background_sound_key(viewer::background_sound()));
            }
            form_row(tr("Own sounds"));
            ImGui::BeginGroup();
            viewer::draw_sound_extras();
            ImGui::EndGroup();
            ImGui::EndDisabled();
            form_row(tr("Rail joints"));
            int joints = viewer::rail_joint_layout() == simulation::RailJointLayout::rts ? 1 : 0;
            const char* joint_layouts[] = {"DTRE", "RTS"};
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::Combo("##rail_joints", &joints, joint_layouts, 2)) {
                viewer::set_rail_joint_layout(joints == 1 ? simulation::RailJointLayout::rts : simulation::RailJointLayout::openrts);
                remember("rail_joints", joints == 1 ? "rts" : "openrts");
            }
            ImGui::TextDisabled("%s", joints == 1 ? tr("(as RTS: a joint every 50 m; on stretches over 2 km four joints 12.5 m apart every 600 m)")
                                                  : tr("(25 m rails; on stretches of 1 km or more between switches welded strings of about 800 m with 3 or 4 joints 12.5 m apart between them)"));
            form_row(tr("Body motion"));
            int bodies = viewer::body_motion_model() == simulation::BodyMotionModel::rts ? 1 : 0;
            const char* body_models[] = {"DTRE", "RTS"};
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::Combo("##body_motion", &bodies, body_models, 2)) {
                viewer::set_body_motion_model(bodies == 1 ? simulation::BodyMotionModel::rts : simulation::BodyMotionModel::openrts);
                remember("body_motion", bodies == 1 ? "rts" : "openrts");
            }
            ImGui::TextDisabled("%s", bodies == 1 ? tr("(as RTS: one swing that grows with the speed, sideways, up and down)")
                                                  : tr("(each body on its own springs over its bogies: the track's errors under each bogie, rail joints and switches, hunting, curves; bounce, pitch, sway, yaw and roll)"));
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    if (ImGui::BeginChild("##settings_files", {0.0f, height}, panel_flags)) {
#ifdef _WIN32
        // The settings and the game's log (openrts.log, for a bug report).
        ImGui::SeparatorText(tr("Settings and log"));
        if (ImGui::Button(tr("Open the settings and log folder"), {-FLT_MIN, 0.0f}))
            ShellExecuteW(nullptr, L"open", viewer::user_data_directory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        hint(utf8(viewer::user_data_directory()).c_str());
        ImGui::Spacing();
#endif
        draw_ace_settings();
        ImGui::Spacing();
        // For developers (viewer_state.hpp): F12 in the game too.
        ImGui::SeparatorText(tr("For developers"));
        if (begin_form("##developer_form", label_width)) {
            form_row(tr("Settings for developers (F12)"));
            bool developer = viewer::developer_mode();
            if (ImGui::Checkbox("##developer_mode", &developer)) viewer::set_developer_mode(developer);
            ImGui::EndTable();
        }
        hint(tr("Debug settings and readouts, the Performance tab and the inspector."));
    }
    ImGui::EndChild();
}

// ACE textures: openrts-ace, and opening .ace with it.
void Launcher::State::draw_ace_settings() {
    ImGui::SeparatorText(tr("ACE textures"));
    hint(tr("openrts-ace turns ACE textures into PNG and back (DXT1, RGB, RGB + mask, RGBA). Registered, a double click on an .ace shows it as PNG, and PNG files and folders get \"to ACE (openRTS)\" in their menu (this Windows user only)."));
    if (ace_tool().empty()) {
        warning(tr("openrts-ace is not found beside the game."));
        return;
    }
    if (ImGui::Button(tr("Open .ace with openRTS"))) {
        const int code = run_ace_tool("--register --no-wait");
        ace_status = code == 0 ? tr("Done: .ace opens with openrts-ace. If Windows still opens it with another program: Open with -> openrts-ace -> Always.")
                               : trf("openrts-ace did not finish (code %d).", code);
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Remove##ace"))) {
        const int code = run_ace_tool("--unregister --no-wait");
        ace_status = code == 0 ? tr("The association is removed; .ace opens as before.") : trf("openrts-ace did not finish (code %d).", code);
    }
    if (!ace_status.empty()) hint(ace_status.c_str());
}

namespace {

// What a setting costs the processor and the video card, beside it: from
// openRTS's own frames and players' reports, roughly; coloured from grey
// (nothing) through green, yellow, orange and red to violet.
enum class Load { unknown, none, negligible, medium, above_medium, high, extreme };

[[nodiscard]] const char* load_name(const Load load) {
    switch (load) {
    case Load::unknown: return "-";
    case Load::none: return "no effect";
    case Load::negligible: return "almost none";
    case Load::medium: return "medium";
    case Load::above_medium: return "above medium";
    case Load::high: return "high";
    case Load::extreme: return "very high";
    }
    return "";
}

[[nodiscard]] ImVec4 load_colour(const Load load) {
    switch (load) {
    case Load::negligible: return {0.45f, 0.80f, 0.50f, 1.0f};
    case Load::medium: return {0.92f, 0.84f, 0.36f, 1.0f};
    case Load::above_medium: return {1.00f, 0.64f, 0.28f, 1.0f};
    case Load::high: return {1.00f, 0.40f, 0.32f, 1.0f};
    case Load::extreme: return {0.90f, 0.40f, 0.95f, 1.0f};
    default: return {0.55f, 0.58f, 0.64f, 1.0f};
    }
}

constexpr float load_width = 128.0f;

// A group's table: the setting, its control, CPU, GPU.
[[nodiscard]] bool begin_group(const char* id, const float label_width) {
    if (!ImGui::BeginTable(id, 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) return false;
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, label_width);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("CPU", ImGuiTableColumnFlags_WidthFixed, load_width);
    ImGui::TableSetupColumn("GPU", ImGuiTableColumnFlags_WidthFixed, load_width);
    ImGui::TableHeadersRow();
    return true;
}

// A row: the label, its loads, then the control's cell, where the caller
// draws the control (the whole width).
void group_row(const char* label, const Load cpu, const Load gpu) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextWrapped("%s", label);
    for (int i = 0; i < 2; ++i) {
        const Load load = i == 0 ? cpu : gpu;
        ImGui::TableSetColumnIndex(2 + i);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(load_colour(load), "%s", tr(load_name(load)));
    }
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

// What a row's setting does, under its control (dim, wrapped).
void row_note(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

[[nodiscard]] Load by_steps(const float value, const float first, const float second, const Load low, const Load mid, const Load top) {
    return value <= first ? low : value <= second ? mid : top;
}

} // namespace

// Graphics: a preset, then every setting of the game's panel in groups, each
// with its load on the processor (CPU) and the video card (GPU).
void Launcher::State::draw_graphics(const float height) {
    const auto before = graphics;
    auto& g = graphics;
    auto& e = g.effects;
    if (ImGui::BeginChild("##graphics", {0.0f, height}, panel_flags)) {
        const float label_width = std::clamp(ImGui::GetContentRegionAvail().x * 0.30f, 220.0f, 360.0f);

        ImGui::SeparatorText(tr("Preset"));
        const auto preset = viewer::matching_graphics_preset(g);
        for (int i = 0; i < viewer::graphics_preset_count; ++i) {
            const auto which = static_cast<viewer::GraphicsPreset>(i);
            const bool chosen = preset && *preset == which;
            if (chosen) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.16f, 0.52f, 0.30f, 1.0f});
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.20f, 0.62f, 0.36f, 1.0f});
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.14f, 0.45f, 0.26f, 1.0f});
            }
            if (ImGui::Button(tr(viewer::graphics_preset_name(which)), {140.0f, 0.0f})) viewer::apply_graphics_preset(g, which);
            if (chosen) ImGui::PopStyleColor(3);
            ImGui::SameLine();
        }
        ImGui::AlignTextToFramePadding();
        if (preset) {
            ImGui::TextUnformatted("");
        } else {
            ImGui::TextColored(ImVec4{0.95f, 0.80f, 0.45f, 1.0f}, "%s", tr("Custom (changed by hand)"));
        }
        hint(preset ? tr(viewer::graphics_preset_note(*preset))
                    : tr("A preset sets the costly settings together: the shadows, the ambient occlusion, the lamp and cloud shadows. Anti-aliasing and the look (HDR, exposure, the lamps' strength, the sky and the water) stay as they are."));
        ImGui::SeparatorText(tr("Your presets (to share with other players)"));
        viewer::draw_own_graphics_presets(own_presets, g, 260.0f);
        ImGui::Spacing();
        // The load's scale, in its colours.
        ImGui::TextDisabled("%s", tr("The load on the processor (CPU) and the video card (GPU):"));
        for (const Load load : {Load::none, Load::negligible, Load::medium, Load::above_medium, Load::high, Load::extreme}) {
            ImGui::SameLine();
            ImGui::TextColored(load_colour(load), "%s", tr(load_name(load)));
        }

        ImGui::SeparatorText(tr("Screen"));
        if (begin_group("##screen", label_width)) {
            int backend = 0;
            if (const auto key = remembered("backend"))
                backend = static_cast<int>(renderer::parse_graphics_backend(*key).value_or(renderer::GraphicsBackend::automatic));
            const char* backends[] = {tr("Automatic"), "Vulkan", "DirectX 12", "OpenGL"};
            group_row(tr("Graphics API"), Load::unknown, Load::unknown);
            if (ImGui::Combo("##backend", &backend, backends, 4))
                remember("backend", renderer::graphics_backend_key(static_cast<renderer::GraphicsBackend>(backend)));
            row_note(tr("Automatic: Vulkan, else DirectX 12. Try DirectX 12 if the game does not start or the screen stays black; OpenGL is for old cards, the experimental renderer may not work there."));
            {
                int mode = static_cast<int>(window_mode);
                const char* modes[] = {viewer::window_mode_name(viewer::WindowMode::windowed),
                                       viewer::window_mode_name(viewer::WindowMode::borderless),
                                       viewer::window_mode_name(viewer::WindowMode::fullscreen)};
                group_row(tr("Window mode"), Load::none, Load::none);
                if (ImGui::Combo("##window_mode", &mode, modes, viewer::window_mode_count)) {
                    window_mode = static_cast<viewer::WindowMode>(mode);
                    remember("window_mode", viewer::window_mode_key(window_mode));
                }
            }
            group_row(tr("VSync"), Load::none, Load::none);
            ImGui::Checkbox("##vsync", &vsync);
            {
                // One slider: 30..500, its right end "no limit".
                int shown = fps_limit == 0 ? viewer::max_fps_limit + 1 : fps_limit;
                const std::string format = shown > viewer::max_fps_limit ? std::string(tr("no limit")) : std::string(tr("%d frames/s"));
                group_row(tr("FPS limit"), Load::unknown, Load::unknown);
                if (ImGui::SliderInt("##fps_limit", &shown, viewer::min_fps_limit, viewer::max_fps_limit + 1, format.c_str(),
                                     ImGuiSliderFlags_AlwaysClamp)) {
                    fps_limit = shown > viewer::max_fps_limit ? 0 : shown;
                    remember("fps_limit", std::to_string(fps_limit));
                }
                row_note(tr("In the background (another window in front, or minimised): at most 30 frames/s."));
            }
            {
                constexpr std::uint32_t counts[] = {1, 2, 4, 8};
                const char* names[] = {tr("no anti-aliasing"), "2x", "4x", "8x"};
                int index = 2;
                for (int i = 0; i < 4; ++i) {
                    if (counts[i] == g.msaa) index = i;
                }
                group_row(tr("Anti-aliasing (MSAA)"), Load::none, g.msaa >= 8 ? Load::high : Load::above_medium);
                if (ImGui::Combo("##msaa", &index, names, 4)) g.msaa = counts[index];
                if (g.msaa <= 1)
                    row_note(tr("Without it RTS's content loses its alpha to coverage: glass and see-through overlays hide what is drawn behind them, thin wires break up."));
                else if (g.msaa != 4)
                    row_note(tr("2x and 8x where the video card takes them, else 4x. RTS draws with 4x."));
            }
            {
                float shown = game_fov();
                group_row(tr("Field of view"), Load::negligible, Load::negligible);
                if (viewer::slider_with_input("##fov", &shown, 15.0f, 100.0f, tr("%.1f degrees"), "%.1f")) {
                    fov = shown;
                    remember("fov", std::to_string(shown));
                }
                if (!fov) row_note(tr("RTS's, from its config.ini, until changed here or in the game (Num+ / Num-)."));
            }
            ImGui::EndTable();
        }
        hint(tr("Window mode: full screen takes the monitor in its own mode; a borderless window covers it and Alt+Tab is quicker. In the game Alt+Enter switches between the window and full screen."));

        ImGui::SeparatorText(tr("Renderer"));
        if (begin_group("##renderer", label_width)) {
            group_row(tr("Experimental renderer"), Load::negligible, Load::medium);
            ImGui::Checkbox("##experimental", &g.experimental);
            row_note(tr("openRTS's own lighting, with the shadows, the ambient occlusion and the lamps below. Off: as RTS draws."));
            group_row("HDR", Load::none, Load::negligible);
            ImGui::Checkbox("##hdr", &g.hdr);
            ImGui::SameLine();
            ImGui::TextDisabled("%s", tr("tone curve with the experimental renderer"));
            if (g.hdr) {
                const char* curves[] = {tr("none (as RTS)"), "Khronos PBR Neutral", "AgX (punchy)", "ACES", tr("auto: Khronos by day, AgX by night"),
                                        tr("auto: ACES by day, AgX by night")};
                group_row(tr("Tone curve"), Load::none, Load::none);
                ImGui::Combo("##tone", &g.tone_curve, curves, 6);
                group_row(tr("Exposure"), Load::none, Load::none);
                viewer::slider_with_input("##exposure", &g.exposure, 0.25f, 4.0f, "%.2f", "%.2f", ImGuiSliderFlags_Logarithmic);
                group_row(tr("Auto exposure"), Load::none, Load::negligible);
                ImGui::Checkbox("##auto_exposure", &g.auto_exposure.on);
                ImGui::SameLine();
                ImGui::TextDisabled("%s", tr("the picture gets used to the light, as an eye"));
                if (g.auto_exposure.on) {
                    group_row(tr("Auto exposure strength"), Load::none, Load::none);
                    viewer::slider_with_input("##auto_exposure_strength", &g.auto_exposure.strength, 0.0f, 1.0f, "%.2f", "%.2f");
                }
                if (g.tone_curve == 4 || g.tone_curve == 5) {
                    group_row(tr("Night exposure"), Load::none, Load::none);
                    viewer::slider_with_input("##night_exposure", &g.night_exposure, 0.5f, 6.0f, "%.2f", "%.2f", ImGuiSliderFlags_Logarithmic);
                }
                group_row(tr("Saturation"), Load::none, Load::none);
                viewer::slider_with_input("##saturation", &g.saturation, 0.5f, 1.8f, "%.2f", "%.2f");
            }
            ImGui::EndTable();
        }

        // The experimental renderer's own.
        if (!g.experimental) warning(tr("The shadows, the ambient occlusion and the lamps below work with the experimental renderer only."));
        ImGui::BeginDisabled(!g.experimental);
        ImGui::SeparatorText(tr("Shadows"));
        if (begin_group("##shadows", label_width)) {
            group_row(tr("Sun shadows"), Load::above_medium, Load::high);
            ImGui::Checkbox("##shadows", &e.shadows);
            ImGui::BeginDisabled(!e.shadows);
            group_row(tr("Shadow distance"), by_steps(e.shadow_distance, 250.0f, 500.0f, Load::medium, Load::above_medium, Load::high),
                      by_steps(e.shadow_distance, 250.0f, 500.0f, Load::medium, Load::above_medium, Load::high));
            viewer::slider_with_input("##shadow_distance", &e.shadow_distance, 100.0f, 1000.0f, tr("%.0f m"), "%.0f");
            {
                const char* sizes[] = {"1024", "2048", "4096"};
                int quality = e.shadow_map_size <= 1024 ? 0 : e.shadow_map_size >= 4096 ? 2 : 1;
                const Load loads[] = {Load::medium, Load::above_medium, Load::high};
                group_row(tr("Shadow quality"), Load::none, loads[quality]);
                if (ImGui::Combo("##shadow_quality", &quality, sizes, 3)) e.shadow_map_size = 1024u << quality;
                row_note(tr("Texels a side of each cascade's map: sharper shadows, more video memory (4096: 64 MB a cascade)."));
            }
            {
                int cascades = static_cast<int>(e.shadow_cascades);
                const Load load = cascades <= 2 ? Load::medium : cascades == 3 ? Load::above_medium : Load::high;
                group_row(tr("Shadow cascades"), load, load);
                if (ImGui::SliderInt("##shadow_cascades", &cascades, 2, 4)) e.shadow_cascades = static_cast<std::uint32_t>(cascades);
                row_note(tr("Pieces the shadow distance is split into: more keeps the middle distance sharp, each one more pass."));
            }
            group_row(tr("Shadow softness"), Load::none, Load::negligible);
            viewer::slider_with_input("##shadow_softness", &e.shadow_softness, 0.5f, 4.0f, "%.1f", "%.1f");
            group_row(tr("Shadow cache (what stays still)"), Load::negligible, Load::negligible);
            ImGui::Checkbox("##shadow_cache", &e.cache_far_cascades);
            row_note(tr("The terrain, trees and buildings are drawn into the sun's shadows once and kept, only the strips the view moves into again; trains, cars and moving parts every frame. Video memory: a map a cascade more (64 MB at 4096)."));
            group_row(tr("Sharp shadows in the cab"), Load::medium, Load::medium);
            ImGui::Checkbox("##shadow_cab", &e.cab_shadows);
            row_note(tr("A shadow map of its own around the cab: sharp edges on the desk, one more pass."));
            ImGui::EndDisabled();
            ImGui::EndTable();
        }

        ImGui::SeparatorText(tr("Ambient occlusion"));
        if (begin_group("##occlusion", label_width)) {
            group_row(tr("Ambient occlusion, GTAO"), Load::medium, e.occlusion_low ? Load::high : Load::extreme);
            ImGui::Checkbox("##ao", &e.ambient_occlusion);
            row_note(tr("Darkens the sky light in corners and creases, from the depth of the scene."));
            ImGui::BeginDisabled(!e.ambient_occlusion);
            group_row(tr("Occlusion quality"), Load::none, e.occlusion_low ? Load::high : Load::extreme);
            if (ImGui::RadioButton(tr("low (half resolution, several times faster)"), e.occlusion_low)) e.occlusion_low = true;
            ImGui::SameLine();
            if (ImGui::RadioButton(tr("high##occlusion"), !e.occlusion_low)) e.occlusion_low = false;
            ImGui::EndDisabled();
            ImGui::EndTable();
        }

        ImGui::SeparatorText("Bloom");
        if (begin_group("##bloom", label_width)) {
            group_row(tr("Bloom: light spreads round lamps, signals and the sun (HDR)"), Load::negligible, Load::negligible);
            ImGui::Checkbox("##bloom", &e.bloom);
            row_note(tr("The lights' glow and what is brighter than white, spread round them as an eye and a lens scatter it."));
            ImGui::BeginDisabled(!e.bloom);
            group_row(tr("Bloom strength"), Load::none, Load::none);
            viewer::slider_with_input("##bloom_strength", &e.bloom_strength, 0.0f, 3.0f, "%.2f", "%.2f");
            group_row(tr("At night from the textures' brightest too (lamps' bulbs, lit windows, signs)"), Load::none, Load::none);
            ImGui::Checkbox("##bloom_textures", &e.bloom_textures);
            row_note(tr("Only what a texture paints near white: a dark wall, or a lamp's light painted onto a platform, does not glow. Off: only the signals, the train's lights and the sun."));
            ImGui::EndDisabled();
            group_row(tr("Light shafts: the sun's beams in the air through trees, windows and clouds (HDR, not as in RTS)"), Load::negligible,
                      Load::medium);
            ImGui::Checkbox("##light_shafts", &e.light_shafts);
            row_note(tr("Each pixel's view ray marched through the sun shadows and the clouds' at half the screen's size: the shadows stand in the air as beams. They need the sun shadows."));
            ImGui::BeginDisabled(!e.light_shafts);
            group_row(tr("Light shafts' strength"), Load::none, Load::none);
            viewer::slider_with_input("##shafts_strength", &e.light_shafts_strength, 0.0f, 4.0f, "%.2f", "%.2f");
            ImGui::EndDisabled();
            ImGui::EndTable();
        }

        ImGui::SeparatorText(tr("Lamps"));
        if (begin_group("##lamps", label_width)) {
            group_row(tr("Every lamp, clustered"), Load::negligible, Load::medium);
            ImGui::Checkbox("##clusters", &e.clustered_lights);
            row_note(tr("Every lamp and headlight near the camera lights the scene, not RTS's nine a ring."));
            {
                int count = static_cast<int>(e.lamp_shadows);
                const Load load = count == 0 ? Load::none : count <= 2 ? Load::medium : count <= 4 ? Load::above_medium : Load::high;
                group_row(tr("Lamp shadows"), load, load);
                if (ImGui::SliderInt("##lamp_shadows", &count, 0, static_cast<int>(renderer::SceneEffects::max_lamp_shadows)))
                    e.lamp_shadows = static_cast<std::uint32_t>(count);
                row_note(tr("Lamps that throw shadows: those lighting the cab (or the view) most, by power, direction and reach, so a floodlight does not shine through the roof. Each is one more pass."));
            }
            {
                const Load load = g.light_range < 1.0f ? Load::negligible : g.light_range <= 1000.0f ? Load::medium : Load::above_medium;
                group_row(tr("Light range"), load, load);
                viewer::slider_with_input("##light_range", &g.light_range, 0.0f, 3000.0f, g.light_range < 1.0f ? tr("as RTS") : tr("%.0f m"), "%.0f");
                row_note(tr("How far the scenery's lamps and floodlights still light the scene; at 0 as RTS's MaxLightDistance."));
            }
            group_row(tr("Headlight reach"), Load::none, Load::negligible);
            viewer::slider_with_input("##headlight_reach", &g.headlight_reach, 1.0f, 5.0f, "x%.1f", "%.1f");
            row_note(tr("How far the trains' headlights and searchlights reach, as a factor of their .eng; 1 as RTS."));
            group_row(tr("Physical light falloff"), Load::none, Load::none);
            ImGui::Checkbox("##light_falloff", &g.physical_light);
            row_note(tr("Headlights and lamps fall off as 1/distance squared with a soft cone edge; off: the RTS light shapes."));
            group_row(tr("Lamps in daylight"), Load::none, Load::none);
            viewer::slider_with_input("##lamps_daylight", &e.lamps_in_daylight, 0.0f, 1.0f, "%.2f", "%.2f");
            row_note(tr("How strong lamps are in full daylight against the night: an eye used to the sun hardly sees a cab lamp. 1: as at night."));
            group_row(tr("Night sky light"), Load::none, Load::none);
            viewer::slider_with_input("##night_ambient", &g.night_ambient, 0.5f, 8.0f, "%.2f", "%.2f", ImGuiSliderFlags_Logarithmic);
            row_note(tr("The environment's ambient light at night, as a factor: the unlit surroundings brighter, the lamps as they are. No effect by day."));
            group_row(tr("Ground bounce"), Load::none, Load::negligible);
            viewer::slider_with_input("##ground_bounce", &e.ground_bounce, 0.0f, 0.5f, "%.2f", "%.2f");
            row_note(tr("Light the sunlit ground throws back onto bogies and the underframe, as the ground's reflectance (ballast and grass about 0.2). 0: none."));
            group_row(tr("Light in the cab by day"), Load::none, Load::negligible);
            viewer::slider_with_input("##indoor_light", &e.indoor_light, 0.0f, 1.0f, "%.2f", "%.2f");
            row_note(tr("The sun through the windows and the sky's light thrown about the cab or salon you are in, which the shadows alone leave dark. 0: none."));
            group_row(tr("Light FullBright as ordinary surfaces"), Load::none, Load::none);
            ImGui::Checkbox("##fullbright", &g.light_fullbright);
            row_note(tr("MSTS FullBright surfaces (platforms, signs) take sun, sky and lamps; their darkened night textures are left out."));
            group_row(tr("A lamp throws no shadow of its own model"), Load::negligible, Load::none);
            ImGui::Checkbox("##lamp_own_model", &g.lamp_own_model);
            ImGui::EndTable();
        }
        ImGui::EndDisabled();

        ImGui::SeparatorText(tr("Sky and water"));
        if (begin_group("##sky", label_width)) {
            group_row(tr("openRTS sky: atmosphere, volumetric clouds (not as in RTS)"), Load::negligible, Load::above_medium);
            ImGui::Checkbox("##native_sky", &g.native_sky);
            ImGui::BeginDisabled(!g.native_sky);
            group_row(tr("Cloud cover by the weather (clear 42 %, snow 85 %, rain 92 %, thunderstorm 97 %)"), Load::none, Load::none);
            ImGui::Checkbox("##cloud_cover_weather", &g.cloud_cover_weather);
            ImGui::BeginDisabled(g.cloud_cover_weather);
            group_row(tr("Cloud cover"), Load::none, Load::negligible);
            viewer::slider_with_input("##cloud_cover", &g.cloud_cover, 0.0f, 100.0f, "%.0f %%", "%.0f");
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!g.experimental);
            group_row(tr("Cloud shadows on the ground (experimental renderer)"), Load::none, Load::medium);
            ImGui::Checkbox("##cloud_shadows", &g.cloud_shadows);
            ImGui::BeginDisabled(!g.cloud_shadows);
            group_row(tr("Cloud shadow darkness (the sun a thick cloud takes)"), Load::none, Load::none);
            viewer::slider_with_input("##cloud_shadow_darkness", &g.cloud_shadow_darkness, 0.0f, 100.0f, "%.0f %%", "%.0f");
            group_row(tr("Cloud shadow sharpness (1: soft, as the clouds)"), Load::none, Load::none);
            viewer::slider_with_input("##cloud_shadow_sharpness", &g.cloud_shadow_sharpness, 1.0f, 10.0f, "%.1f", "%.1f");
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!g.experimental);
            group_row(tr("Aerial perspective"), Load::none, Load::negligible);
            ImGui::Checkbox("##aerial", &e.aerial_perspective);
            row_note(tr("Distance haze thinning with height and brighter towards the sun, instead of RTS's linear fog."));
            ImGui::EndDisabled();
            group_row(tr("openRTS water: waves, sky reflection, soft shores (not as in RTS)"), Load::none, Load::medium);
            ImGui::Checkbox("##native_water", &g.native_water);
            ImGui::EndTable();
        }

        ImGui::SeparatorText(tr("World"));
        if (begin_group("##world", label_width)) {
            group_row(tr("Keep trees off tracks and roads (not as in RTS)"), Load::negligible, Load::none);
            ImGui::Checkbox("##forest_clearance", &g.forest_clearance);
            ImGui::BeginDisabled(!g.forest_clearance);
            group_row(tr("Trees from the track's or road's edge"), Load::none, Load::none);
            viewer::slider_with_input("##forest_clearance_distance", &g.forest_clearance_distance, 0.5f, 20.0f, tr("%.1f m"), "%.1f");
            ImGui::EndDisabled();
            group_row(tr("Signal lights at the lens's size, far off a dot (not as in RTS)"), Load::none, Load::none);
            ImGui::Checkbox("##signal_lights", &g.signal_lights_real_size);
            group_row(tr("Night textures of the models (as in RTS)"), Load::none, Load::none);
            ImGui::Checkbox("##night_textures", &g.night_textures);
            row_note(tr("(TEXTURES\\NIGHT: lit windows and lamps painted in; off: the day's textures at night, lit by the lamps)"));
            group_row(tr("Sharp cab screens: drawn at a higher resolution (not as in RTS)"), Load::negligible, Load::negligible);
            ImGui::Checkbox("##screen_supersampling", &g.screen_supersampling);
            group_row(tr("Smooth the compression blocks of the trains' textures (BC7, not as in RTS)"), Load::medium, Load::negligible);
            ImGui::Checkbox("##smooth_train_textures", &g.smooth_train_textures);
            row_note(tr("DXT1's 4x4 blocks in the shade of cars and cabs smoothed; the first load of a train a few seconds longer, then from the cache; twice the video memory for the trains."));
            group_row(tr("Uncompressed textures and PBR maps (BC7, normals BC5)"), Load::none, Load::none);
            {
                const char* modes[] = {tr("Don't compress (as in RTS)"), tr("Compress, not kept on disk"), tr("Compress and keep on disk")};
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::Combo("##compress_textures", &g.compress_textures, modes, 3);
            }
            row_note(tr("Compressed: a quarter of their video memory, barely seen; for cards with 8 GB and less. Kept on disk: the first load of a route or train longer, then read from the cache. Not kept: every load longer, nothing written."));
            {
                // The disk cache (renderer/bc7.hpp): counted every 2 s while shown.
                static renderer::TextureCacheSize cache_size;
                static double counted_at = -100.0;
                const auto folder = viewer::user_data_directory() / "texture_cache" / "bc7";
                if (ImGui::GetTime() - counted_at > 2.0) {
                    cache_size = renderer::texture_cache_size(folder);
                    counted_at = ImGui::GetTime();
                }
                group_row(tr("The compressed textures' cache on disk"), Load::none, Load::none);
                ImGui::Text(tr("%.0f MB, %llu files"), static_cast<double>(cache_size.bytes) / 1048576.0,
                            static_cast<unsigned long long>(cache_size.files));
                ImGui::SameLine();
                if (ImGui::Button(tr("Clear the cache"))) {
                    renderer::clear_texture_cache(folder);
                    counted_at = -100.0;
                }
                row_note(tr("The trains' smoothed textures and the compressed ones; cleared, they are made again on the next load."));
            }
            group_row(tr("Relief by the materials' height maps (parallax, not as in RTS)"), Load::negligible, Load::none);
            ImGui::Checkbox("##parallax", &g.effects.parallax);
            ImGui::BeginDisabled(!g.effects.parallax);
            group_row(tr("Relief out to"), Load::none, Load::none);
            viewer::slider_with_input("##parallax_distance", &g.effects.parallax_distance, 5.0f, 100.0f, tr("%.0f m"), "%.0f");
            ImGui::EndDisabled();
            row_note(tr("Only materials with a height map: the image shifts where the eye's ray meets the relief (rivets, ribs, panel seams); the outline stays flat."));
            group_row(tr("Depth for ReShade"), Load::none, Load::negligible);
            ImGui::Checkbox("##reshade_depth", &g.reshade_depth);
            row_note(tr("Keeps the scene depth to the end of the frame and, with anti-aliasing, adds a copy ReShade can take. In ReShade: RESHADE_DEPTH_INPUT_IS_REVERSED 1, RESHADE_DEPTH_MULTIPLIER 20 (see INSTALL.md)."));
            ImGui::EndTable();
        }
        hint(tr("The game reads these at its start; they are in the game's panel as well, and what is changed there shows here."));
    }
    ImGui::EndChild();
    if (g != before) viewer::save_graphics_settings(g, before);
}

namespace {
}

// The version, then the simulator's keys and mouse (the game's panel has the
// same in its Controls tab).
void Launcher::State::draw_info(const float height) {
    if (ImGui::BeginChild("##info", {0.0f, height}, panel_flags)) {
        ImGui::TextUnformatted(core::version_text().c_str());
        ImGui::SeparatorText(tr("Controls"));
        viewer::draw_controls_help();
    }
    ImGui::EndChild();
}

// The bottom bar of a game tab: what was chosen, and the button that starts
// it (Cancel while a network game is joining).
void Launcher::State::draw_bar(const float button_width, const float button_height, const float status_height) {
    const float line = ImGui::GetTextLineHeightWithSpacing();
    const float bar_height = ImGui::GetContentRegionAvail().y - status_height;
    const float bar_top = ImGui::GetCursorPosY() + (std::max)(0.0f, (bar_height - button_height) * 0.5f);
    ImGui::SetCursorPosY(bar_top + (std::max)(0.0f, (button_height - 2.0f * line) * 0.5f));
    ImGui::BeginGroup();
    if (tab == Tab::network) {
        const auto server = address_text();
        const auto who = nick_text();
        ImGui::Text("%s  ·  %s", server.empty() ? tr("no server address") : server.c_str(), who.empty() ? tr("no nick") : who.c_str());
        ImGui::TextDisabled("%s", tr("The route, the time and the train come from the server."));
    } else if (!ready()) {
        ImGui::TextDisabled("%s", tr("Choose a route and a consist."));
    } else if (!consist_used()) {
        const auto what = situation >= 0 ? viewer::trf("situation %s", situations[static_cast<std::size_t>(situation)].c_str())
                                         : std::string(tr("no train"));
        ImGui::Text("%s  ·  %s", routes[static_cast<std::size_t>(route)].c_str(), what.c_str());
        ImGui::TextDisabled("%02d:%02d  ·  %s  ·  %s", minutes / 60, minutes % 60, tr(season_names[season]), tr(weather_names[weather]));
    } else {
        ImGui::Text("%s  ·  %s", routes[static_cast<std::size_t>(route)].c_str(), consists[static_cast<std::size_t>(consist)].c_str());
        const auto& shown = path < 0 || path >= static_cast<int>(paths.size())
                                ? std::string(tr("(the route's first path)"))
                                : (paths[static_cast<std::size_t>(path)].name.empty() ? paths[static_cast<std::size_t>(path)].file
                                                                                      : paths[static_cast<std::size_t>(path)].name);
        ImGui::TextDisabled("%s  ·  %02d:%02d  ·  %s  ·  %s", shown.c_str(), minutes / 60, minutes % 60, tr(season_names[season]),
                            tr(weather_names[weather]));
    }
    ImGui::EndGroup();

    ImGui::SetCursorPos({ImGui::GetWindowContentRegionMax().x - button_width, bar_top});
    auto* large = viewer::large_font();
    if (large) ImGui::PushFont(large);
    if (running() && joining()) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.55f, 0.22f, 0.20f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.66f, 0.27f, 0.24f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.48f, 0.18f, 0.16f, 1.0f});
        if (ImGui::Button(tr("Cancel"), {button_width, button_height})) cancel();
        ImGui::PopStyleColor(3);
    } else {
        const bool busy = running();
        ImGui::BeginDisabled(busy || !ready_for(tab));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.16f, 0.52f, 0.30f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.20f, 0.62f, 0.36f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.14f, 0.45f, 0.26f, 1.0f});
        const char* label = busy ? tr("The game is running") : tab == Tab::network ? tr("Connect") : tr("Start");
        if (ImGui::Button(label, {button_width, button_height})) launch(tab);
        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();
    }
    if (large) ImGui::PopFont();
}
#endif


Launcher::Launcher(std::filesystem::path exe_dir) : state_(std::make_unique<State>()) {
    state_->exe_dir = std::move(exe_dir);
    state_->image_dir = utf8(state_->exe_dir / "menu");
    state_->load_settings();
    state_->scan();
    // No RTS folder yet: the settings first, where it is given.
    if (!state_->vfs) {
        state_->page = State::Page::settings;
        state_->settings_section = 0;
    }
}

std::string Launcher::attach(const viewer::SceneGpu& gpu, viewer::ViewerCamera&) {
#ifdef OPENRTS_HAS_IMGUI
    if (!state_->images) state_->images = std::make_unique<menu::ImageCache>();
    state_->images->set_device(gpu.device);
#endif
    return {};
}

void Launcher::detach() {
    if (state_->images) state_->images->clear();
}

std::optional<SessionRequest> Launcher::take_session_request() {
    auto request = std::move(state_->pending_session);
    state_->pending_session.reset();
    return request;
}

void Launcher::session_finished(const std::string& note) {
    state_->page = State::Page::home;
    state_->session_note = note;
    state_->status = note;
    state_->scan();
    state_->load_graphics();
    state_->fps_limit = viewer::clamp_fps_limit(remembered_int("fps_limit", 0));
    state_->window_mode = viewer::parse_window_mode(remembered("window_mode").value_or("")).value_or(viewer::WindowMode::windowed);
}

void Launcher::session_failed(const std::string& error) {
    state_->page = State::Page::solo;
    state_->status = error;
}

void Launcher::show_home() {
    state_->page = State::Page::home;
}

bool Launcher::draw_embedded_settings() {
    state_->embedded_settings = true;
    state_->embedded_back = false;
    state_->draw_settings_page();
    state_->embedded_settings = false;
    const bool back = state_->embedded_back;
    state_->embedded_back = false;
    return back;
}

std::string Launcher::command() const { return state_->command_line(Tab::single); }

std::string Launcher::network_command() const { return state_->command_line(Tab::network); }

void Launcher::show_tab(const std::string_view key) {
    for (int i = 0; i < tab_count; ++i) {
        if (key != tab_keys[i]) continue;
        state_->tab = static_cast<Tab>(i);
        state_->select_tab = true;
    }
    if (key == "single") state_->page = State::Page::solo;
    else if (key == "settings") state_->page = State::Page::settings;
    else if (key == "graphics") {
        state_->page = State::Page::settings;
        state_->settings_section = 1;
    } else if (key == "network") {
        state_->page = State::Page::soon;
        state_->soon_kind = 0;
    } else if (key == "info") {
        state_->page = State::Page::settings;
        state_->settings_section = 2;
    } else if (key == "controls") {
        state_->page = State::Page::settings;
        state_->settings_section = 2;
    } else if (key == "sound") {
        state_->page = State::Page::settings;
        state_->settings_section = 3;
    }
}

void Launcher::apply_window_options(viewer::WindowOptions& options) const {
    const auto& s = *state_;
    options.msaa = viewer::valid_msaa_samples(static_cast<int>(s.graphics.msaa));
    options.vsync = s.vsync;
    options.shading = s.graphics.experimental ? renderer::ShadingPath::experimental : renderer::ShadingPath::rts_compatible;
    options.hdr = s.graphics.hdr;
    options.pbr = s.graphics.pbr;
    options.fps_limit = s.fps_limit;
    options.window_mode = s.window_mode;
    options.sound_volume = s.volume;
}

std::string Launcher::summary() const {
    const auto& s = *state_;
    return "root " + utf8(s.root) + ": " + std::to_string(s.routes.size()) + " routes, " + std::to_string(s.consists.size()) +
           " consists, " + std::to_string(s.paths.size()) + " paths of the chosen route; tab " + tab_keys[static_cast<int>(s.tab)] +
           "; server " + s.address_text() + " as " + s.nick_text() + (s.status.empty() ? "" : " (" + s.status + ")");
}

Launcher::~Launcher() {
#ifdef _WIN32
    if (state_->process) CloseHandle(state_->process);
#endif
}

std::optional<viewer::ScenePresence> Launcher::presence() const {
    if (state_->running()) return std::nullopt;
    return viewer::ScenePresence{.details = tr("In the menu")};
}

void Launcher::update(renderer::ModelRenderer&, viewer::ViewerCamera&, const viewer::SceneClock&, const viewer::SceneInput&) {
    state_->poll();
}

viewer::SceneEnvironment Launcher::environment() const {
    viewer::SceneEnvironment environment;
    environment.clear_colour = {0.10f, 0.11f, 0.13f, 1.0f};
    return environment;
}

void Launcher::draw_overlay(const viewer::ViewerCamera&) const {
#ifdef OPENRTS_HAS_IMGUI
    auto& s = *state_;
    const auto& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize(io.DisplaySize);
    constexpr auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                           ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4{0.0f, 0.0f, 0.0f, 0.0f});
    ImGui::Begin("##launcher", nullptr, flags);

    switch (s.page) {
    case State::Page::home: s.draw_home(); break;
    case State::Page::solo: s.draw_solo(); break;
    case State::Page::settings: s.draw_settings_page(); break;
    case State::Page::account: s.draw_account(); break;
    case State::Page::soon: s.draw_soon(); break;
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
#endif
}

} // namespace openrts::game
