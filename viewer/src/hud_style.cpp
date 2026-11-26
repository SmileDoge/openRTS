#include "hud_style.hpp"

#ifdef OPENRTS_HAS_IMGUI

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <iterator>

#include <openrts/ui/fonts.hpp>

namespace openrts::viewer::hud {
namespace {

struct Colour {
    ImGuiCol slot;
    ImU32 value;
};

// The drive panel's palette over ImGui's stock widget states: quiet frames,
// lime highlights and the same dim separators and table rules.
constexpr Colour window_colours[] = {
    {ImGuiCol_Text, text_colour},
    {ImGuiCol_TextDisabled, dim_colour},
    {ImGuiCol_WindowBg, IM_COL32(47, 47, 47, 235)},
    {ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0)},
    {ImGuiCol_PopupBg, IM_COL32(52, 52, 52, 245)},
    {ImGuiCol_Border, IM_COL32(255, 255, 255, 38)},
    {ImGuiCol_BorderShadow, IM_COL32(0, 0, 0, 0)},
    {ImGuiCol_TitleBg, IM_COL32(62, 62, 62, 235)},
    {ImGuiCol_TitleBgActive, IM_COL32(62, 62, 62, 245)},
    {ImGuiCol_TitleBgCollapsed, IM_COL32(47, 47, 47, 235)},
    {ImGuiCol_MenuBarBg, IM_COL32(47, 47, 47, 235)},
    {ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 0)},
    {ImGuiCol_ScrollbarGrab, IM_COL32(255, 255, 255, 56)},
    {ImGuiCol_ScrollbarGrabHovered, IM_COL32(255, 255, 255, 84)},
    {ImGuiCol_ScrollbarGrabActive, IM_COL32(255, 255, 255, 112)},
    {ImGuiCol_CheckMark, green_colour},
    {ImGuiCol_SliderGrab, green_colour},
    {ImGuiCol_SliderGrabActive, tape_on_colour},
    {ImGuiCol_FrameBg, IM_COL32(255, 255, 255, 16)},
    {ImGuiCol_FrameBgHovered, IM_COL32(255, 255, 255, 28)},
    {ImGuiCol_FrameBgActive, IM_COL32(255, 255, 255, 40)},
    {ImGuiCol_Button, IM_COL32(255, 255, 255, 18)},
    {ImGuiCol_ButtonHovered, IM_COL32(167, 241, 29, 40)},
    {ImGuiCol_ButtonActive, IM_COL32(167, 241, 29, 64)},
    {ImGuiCol_Header, IM_COL32(167, 241, 29, 34)},
    {ImGuiCol_HeaderHovered, IM_COL32(167, 241, 29, 48)},
    {ImGuiCol_HeaderActive, IM_COL32(167, 241, 29, 64)},
    {ImGuiCol_Separator, separator_colour},
    {ImGuiCol_SeparatorHovered, IM_COL32(200, 200, 200, 96)},
    {ImGuiCol_SeparatorActive, IM_COL32(200, 200, 200, 128)},
    {ImGuiCol_ResizeGrip, IM_COL32(255, 255, 255, 24)},
    {ImGuiCol_ResizeGripHovered, IM_COL32(255, 255, 255, 56)},
    {ImGuiCol_ResizeGripActive, IM_COL32(255, 255, 255, 84)},
    {ImGuiCol_TableHeaderBg, IM_COL32(255, 255, 255, 10)},
    {ImGuiCol_TableBorderStrong, IM_COL32(200, 200, 200, 64)},
    {ImGuiCol_TableBorderLight, IM_COL32(200, 200, 200, 38)},
    {ImGuiCol_TableRowBg, IM_COL32(0, 0, 0, 0)},
    {ImGuiCol_TableRowBgAlt, IM_COL32(255, 255, 255, 8)},
    {ImGuiCol_TextSelectedBg, IM_COL32(167, 241, 29, 48)},
    {ImGuiCol_DragDropTarget, IM_COL32(167, 241, 29, 160)},
    {ImGuiCol_NavHighlight, IM_COL32(167, 241, 29, 160)},
    {ImGuiCol_NavWindowingHighlight, IM_COL32(255, 255, 255, 180)},
    {ImGuiCol_NavWindowingDimBg, IM_COL32(20, 20, 20, 80)},
    {ImGuiCol_ModalWindowDimBg, IM_COL32(20, 20, 20, 120)},
};

constexpr int style_var_count = 7;

// The inset a rounded plate's edge has at `y`: its corner's circle, clamped
// to the smaller half of the plate.
float corner_inset(const float y, const float height, const float radius) {
    if (radius <= 0.0f) return 0.0f;
    if (y < radius) {
        const float d = radius - y;
        return radius - std::sqrt((std::max)(0.0f, radius * radius - d * d));
    }
    if (y > height - radius) {
        const float d = radius - (height - y);
        return radius - std::sqrt((std::max)(0.0f, radius * radius - d * d));
    }
    return 0.0f;
}

} // namespace

float line_top(ImFont* face, const float size, const float baseline) {
    if (face && face->FontSize > 0.0f) return baseline - face->Ascent * (size / face->FontSize);
    return baseline - size * 0.97f;
}

std::size_t glyph_count(const std::string_view text) {
    std::size_t count = 0;
    for (const unsigned char byte : text) {
        if ((byte & 0xC0) != 0x80) ++count;
    }
    return count;
}

float text_width(ImFont* face, const float size, const std::string_view text, const float tracking) {
    if (text.empty()) return 0.0f;
    const float width = face ? face->CalcTextSizeA(size, FLT_MAX, 0.0f, text.data(), text.data() + text.size()).x
                             : ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
    return width + tracking * static_cast<float>((std::max)(std::size_t{1}, glyph_count(text)) - 1);
}

// Text with letter spacing; ImGui spaces whole strings only, so a code point
// at a time. `y` is the top of the line box, not the cap's.
void text(ImDrawList* draw, ImFont* face, const float size, const ImVec2 at, const ImU32 colour,
          const std::string_view value, const float tracking) {
    if (value.empty()) return;
    const char* begin = value.data();
    const char* end = begin + value.size();
    if (tracking <= 0.0f) {
        draw->AddText(face, size, at, colour, begin, end);
        return;
    }
    ImVec2 where = at;
    for (const char* point = begin; point < end;) {
        const char* next = point + 1;
        while (next < end && (static_cast<unsigned char>(*next) & 0xC0) == 0x80) ++next;
        draw->AddText(face, size, where, colour, point, next);
        where.x += (face ? face->CalcTextSizeA(size, FLT_MAX, 0.0f, point, next).x : ImGui::CalcTextSize(point, next).x) + tracking;
        point = next;
    }
}

void at_baseline(ImDrawList* draw, ImFont* face, const float size, const float x, const float baseline,
                 const ImU32 colour, const std::string_view value, const float tracking) {
    text(draw, face, size, ImVec2{x, line_top(face, size, baseline)}, colour, value, tracking);
}

void centred_baseline(ImDrawList* draw, ImFont* face, const float size, const float x, const float baseline,
                      const ImU32 colour, const std::string_view value, const float tracking) {
    text(draw, face, size, ImVec2{x - text_width(face, size, value, tracking) * 0.5f, line_top(face, size, baseline)},
         colour, value, tracking);
}

void right_baseline(ImDrawList* draw, ImFont* face, const float size, const float x, const float baseline,
                    const ImU32 colour, const std::string_view value, const float tracking) {
    text(draw, face, size, ImVec2{x - text_width(face, size, value, tracking), line_top(face, size, baseline)},
         colour, value, tracking);
}

std::string elide(ImFont* face, const float size, const std::string_view value, const float max_width) {
    if (text_width(face, size, value) <= max_width) return std::string(value);
    const float dots = text_width(face, size, "…");
    std::string out;
    float used = 0.0f;
    const char* end = value.data() + value.size();
    for (const char* point = value.data(); point < end;) {
        const char* next = point + 1;
        while (next < end && (static_cast<unsigned char>(*next) & 0xC0) == 0x80) ++next;
        const float w = face ? face->CalcTextSizeA(size, FLT_MAX, 0.0f, point, next).x : ImGui::CalcTextSize(point, next).x;
        if (used + w + dots > max_width) break;
        out.append(point, next);
        used += w;
        point = next;
    }
    out += "…";
    return out;
}

void plate(ImDrawList* draw, const ImVec2 origin, const ImVec2 size, const float radius, const float opacity) {
    if (!draw || size.x < 1.0f || size.y < 1.0f) return;
    const float alpha = std::clamp(opacity, 0.0f, 1.0f);
    if (alpha <= 0.0f) return;
    const int fill_alpha = static_cast<int>(226.0f * alpha + 0.5f);
    const int stroke_alpha = static_cast<int>(38.0f * alpha + 0.5f);
    const float r = (std::max)(0.0f, (std::min)(radius, (std::min)(size.x, size.y) * 0.5f));
    const int rows = static_cast<int>(std::ceil(size.y));
    for (int row = 0; row < rows; ++row) {
        const float y = static_cast<float>(row) + 0.5f;
        const float edge = corner_inset(y, size.y, r);
        const float left = origin.x + edge;
        const float right = origin.x + size.x - edge;
        if (right <= left) continue;
        const float t = y / size.y;
        const auto channel = static_cast<int>(62.0f + (47.0f - 62.0f) * t);
        draw->AddRectFilled(ImVec2{left, origin.y + static_cast<float>(row)},
                            ImVec2{right, origin.y + static_cast<float>(row) + 1.0f},
                            IM_COL32(channel, channel, channel, fill_alpha));
    }
    draw->AddRect(origin, ImVec2{origin.x + size.x, origin.y + size.y}, IM_COL32(255, 255, 255, stroke_alpha), r, 0, 1.0f);
}

void separator(ImDrawList* draw, const ImVec2 origin, const float y, const float width, const float inset) {
    draw->AddLine(ImVec2{origin.x + inset, origin.y + y}, ImVec2{origin.x + width - inset, origin.y + y},
                  separator_colour, 1.0f);
}

ImVec2 hint_size(const std::string_view value, const float padding_x, const float padding_y) {
    auto* face = ui::hud_text_font();
    const float size = face ? face->FontSize : ImGui::GetFontSize();
    return ImVec2{text_width(face, size, value) + padding_x * 2.0f, size + padding_y * 2.0f};
}

ImVec2 hint(ImDrawList* draw, const ImVec2 top_left, const std::string_view value, const ImU32 colour, const float radius,
            const float padding_x, const float padding_y, const float opacity, const bool coloured_edge) {
    const ImVec2 size = hint_size(value, padding_x, padding_y);
    if (value.empty()) return size;
    plate(draw, top_left, size, radius, opacity);
    if (coloured_edge) {
        const auto edge = (colour & 0x00FFFFFFu) | 0x80000000u;
        draw->AddRect(ImVec2{top_left.x - 1.0f, top_left.y - 1.0f},
                      ImVec2{top_left.x + size.x + 1.0f, top_left.y + size.y + 1.0f}, edge, radius + 1.0f, 0, 1.0f);
    }
    auto* face = ui::hud_text_font();
    const float font_size = face ? face->FontSize : ImGui::GetFontSize();
    const float ascent = face && face->FontSize > 0.0f ? face->Ascent * (font_size / face->FontSize) : font_size * 0.97f;
    at_baseline(draw, face, font_size, top_left.x + padding_x, top_left.y + padding_y + ascent, colour, value);
    return size;
}

std::string upper(const std::string_view value) {
    std::string out(value);
    for (std::size_t i = 0; i < out.size(); ++i) {
        auto byte = static_cast<unsigned char>(out[i]);
        if (byte >= 'a' && byte <= 'z') {
            out[i] = static_cast<char>(byte - 0x20);
        } else if (byte == 0xD0 && i + 1 < out.size()) {
            // а-п: the second byte 0xB0..0xBF to 0x90..0x9F.
            const auto next = static_cast<unsigned char>(out[i + 1]);
            if (next >= 0xB0 && next <= 0xBF) out[i + 1] = static_cast<char>(next - 0x20);
        } else if (byte == 0xD1 && i + 1 < out.size()) {
            const auto next = static_cast<unsigned char>(out[i + 1]);
            if (next >= 0x80 && next <= 0x8F) {
                // р-я: the lead 0xD1 to 0xD0 and the second byte +0x20.
                out[i] = static_cast<char>(0xD0);
                out[i + 1] = static_cast<char>(next + 0x20);
            } else if (next == 0x91) {
                // ё to Ё.
                out[i] = static_cast<char>(0xD0);
                out[i + 1] = static_cast<char>(0x81);
            }
        }
    }
    return out;
}

void push_window_style(const float padding_x, const float padding_y, const bool frame) {
    for (const auto& colour : window_colours) ImGui::PushStyleColor(colour.slot, colour.value);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{padding_x, padding_y});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, frame ? plate_radius : 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, frame ? 1.0f : 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowTitleAlign, ImVec2{0.5f, 0.5f});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{10.0f, 6.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{12.0f, 6.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2{8.0f, 5.0f});
}

void pop_window_style() {
    ImGui::PopStyleVar(style_var_count);
    ImGui::PopStyleColor(static_cast<int>(std::size(window_colours)));
}

void push_window_font() { ImGui::PushFont(ui::hud_text_font()); }

void pop_window_font() { ImGui::PopFont(); }

void heading(const std::string_view value, const float size) {
    if (value.empty()) return;
    auto* face = ui::hud_title_font();
    auto* draw = ImGui::GetWindowDrawList();
    const std::string shown = upper(value);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float width = text_width(face, size, shown);
    const float available = ImGui::GetContentRegionAvail().x;
    const float ascent = face && face->FontSize > 0.0f ? face->Ascent * (size / face->FontSize) : size * 0.97f;
    centred_baseline(draw, face, size, at.x + (std::max)(available, width) * 0.5f, at.y + ascent, text_colour, shown);
    ImGui::Dummy(ImVec2{width, size * 1.25f});
}

} // namespace openrts::viewer::hud

#endif
