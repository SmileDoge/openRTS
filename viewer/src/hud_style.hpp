#pragma once

#ifdef OPENRTS_HAS_IMGUI

#include <cstddef>
#include <string>
#include <string_view>

#include <imgui.h>

// The F4 drive panel's look (the design's 382x511 plate): the translucent
// grey gradient with its light rounded stroke, the letter-spaced Montserrat
// labels and the dim separators. Shared by the in-game menus and the track
// profile strip so they read as one family.
namespace openrts::viewer::hud {

constexpr ImU32 text_colour = IM_COL32(214, 214, 214, 255);
constexpr ImU32 label_colour = IM_COL32(198, 198, 198, 255);
constexpr ImU32 dim_colour = IM_COL32(178, 178, 178, 255);
constexpr ImU32 green_colour = IM_COL32(122, 214, 10, 255);
constexpr ImU32 red_colour = IM_COL32(245, 48, 40, 255);
constexpr ImU32 dark_red_colour = IM_COL32(97, 0, 0, 255);
// The mockup's #8CFF00 at 0.8, and a dim green that still reads on the plate.
constexpr ImU32 tape_on_colour = IM_COL32(140, 255, 0, 204);
constexpr ImU32 tape_off_colour = IM_COL32(86, 104, 88, 255);
constexpr ImU32 separator_colour = IM_COL32(200, 200, 200, 64);
constexpr float plate_radius = 24.0f;

// The baseline (the design's numbers are the glyph outlines' bottoms) to the
// top of ImGui's line box, for the face's ascent at `size`.
float line_top(ImFont* face, float size, float baseline);
std::size_t glyph_count(std::string_view value);
float text_width(ImFont* face, float size, std::string_view value, float tracking = 0.0f);
void text(ImDrawList* draw, ImFont* face, float size, ImVec2 at, ImU32 colour, std::string_view value,
          float tracking = 0.0f);
void at_baseline(ImDrawList* draw, ImFont* face, float size, float x, float baseline, ImU32 colour,
                 std::string_view value, float tracking = 0.0f);
void centred_baseline(ImDrawList* draw, ImFont* face, float size, float x, float baseline, ImU32 colour,
                      std::string_view value, float tracking = 0.0f);
void right_baseline(ImDrawList* draw, ImFont* face, float size, float x, float baseline, ImU32 colour,
                    std::string_view value, float tracking = 0.0f);
std::string elide(ImFont* face, float size, std::string_view value, float max_width);

// The design's plate: a grey 62 -> 47 vertical gradient drawn a row at a time
// so its corners are rounded, and a one-pixel light stroke. No drop shadow.
void plate(ImDrawList* draw, ImVec2 origin, ImVec2 size, float radius = plate_radius, float opacity = 1.0f);
// A one-pixel line `inset` in from both edges of the plate.
void separator(ImDrawList* draw, ImVec2 origin, float y, float width, float inset = 20.0f);
// A small F4-styled hint plate: the drive panel's face, a smaller radius and
// padding; `coloured_edge` matches its outline to the text's state colour
// (the cab's yellow focus, a door's green).
ImVec2 hint_size(std::string_view value, float padding_x = 10.0f, float padding_y = 5.0f);
ImVec2 hint(ImDrawList* draw, ImVec2 top_left, std::string_view value, ImU32 colour, float radius = 10.0f,
            float padding_x = 10.0f, float padding_y = 5.0f, float opacity = 1.0f, bool coloured_edge = false);
// ASCII and Cyrillic lower case to upper, for the design's capitalised labels.
std::string upper(std::string_view value);

// Window colours and paddings. Push before Begin, pop after End; the font is
// pushed after Begin (PushFont needs a current window) and popped before End.
void push_window_style(float padding_x = 20.0f, float padding_y = 16.0f, bool frame = false);
void pop_window_style();
void push_window_font();
void pop_window_font();
// A centred upper-case title in the drive panel's face, then the cursor's line.
void heading(std::string_view value, float size = 24.0f);

} // namespace openrts::viewer::hud

#endif
