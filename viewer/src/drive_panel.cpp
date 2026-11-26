#include "drive_panel.hpp"

#ifdef OPENRTS_HAS_IMGUI

#include <cmath>
#include <cstdio>
#include <string>

#include <imgui.h>

#include <openrts/ui/fonts.hpp>

#include "hud_style.hpp"
#include "ui_text.hpp"

namespace openrts::viewer {

using namespace hud;

namespace {

constexpr float width = drive_panel_width;
constexpr float height = drive_panel_height;
// The design's text column (Макеты/F4, 382x511): its text starts 31.7 px
// from the plate's left edge and ends at 350.5.
constexpr float right_edge = 350.5f;
constexpr float centre_x = 191.0f;

const char* aspect_name(const formats::SignalAspect aspect) {
    using A = formats::SignalAspect;
    if (aspect == A::stop) return tr("stop signal");
    if (aspect == A::stop_and_proceed) return tr("stop, pass allowed");
    if (aspect == A::restricting) return tr("call-on signal");
    if (aspect <= A::approach_3) return tr("yellow");
    if (aspect <= A::clear_2) return tr("green");
    return tr("aspect unknown");
}

ImU32 aspect_colour(const formats::SignalAspect aspect) {
    using A = formats::SignalAspect;
    switch (aspect) {
    case A::clear_1:
    case A::clear_2:
        return IM_COL32(0, 255, 38, 255);
    case A::approach_1:
    case A::approach_2:
    case A::approach_3:
        return IM_COL32(255, 218, 40, 255);
    case A::stop:
        return IM_COL32(245, 48, 40, 255);
    case A::stop_and_proceed:
        return IM_COL32(255, 218, 40, 255);
    case A::restricting:
        return IM_COL32(245, 248, 255, 255);
    default:
        return IM_COL32(106, 114, 117, 255);
    }
}

std::string distance_text(const double metres) {
    char text[32];
    if (metres >= 1000.0) std::snprintf(text, sizeof text, tr("%.1f km"), metres / 1000.0);
    else std::snprintf(text, sizeof text, tr("%.0f m."), metres);
    return text;
}

std::string pressure_text(const float value) {
    if (std::isnan(value)) return "—";
    char text[32];
    std::snprintf(text, sizeof text, "%.1f", value);
    return text;
}

} // namespace

void draw_drive_panel(const DrivePanelData& data) {
    ImGui::Dummy(ImVec2{width, height});
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetWindowPos();
    const float x = origin.x;
    const float y = origin.y;

    auto* title = ui::hud_title_font();
    auto* label = ui::hud_label_font();
    auto* value = ui::hud_value_font();
    auto* pressure = ui::hud_pressure_font();
    auto* text_face = ui::hud_text_font();
    auto* small = ui::hud_small_font();
    auto* tiny = ui::hud_tiny_font();

    plate(draw, origin, ImVec2{width, height});

    // ДВИЖЕНИЕ / СТОЯНКА (Montserrat SemiBold 32, the design's 61.9 baseline;
    // the face itself gives the design's 167 px, so no extra tracking here).
    centred_baseline(draw, title, 32.0f, x + centre_x, y + 61.9f, text_colour,
                     tr(data.moving ? "MOVING" : "STANDING"));

    // СКОРОСТЬ and ДОП. СКОРОСТЬ (Montserrat Medium 15; the mockup's letter
    // spacing widens the short words to the design's 83/125 px).
    at_baseline(draw, label, 15.0f, x + 31.7f, y + 92.1f, label_colour, tr("SPEED"), 1.4f);
    at_baseline(draw, label, 15.0f, x + 142.2f, y + 92.1f, label_colour, tr("PERMITTED SPEED"), 1.4f);

    // The speed (Onest 55) and the limit (Onest 35), each centred on its
    // column so a wider number does not drift away from its label.
    char number[32];
    std::snprintf(number, sizeof number, "%.0f", data.speed_kmh);
    centred_baseline(draw, value, 55.0f, x + 72.0f, y + 148.4f, data.overspeed ? red_colour : green_colour, number);
    if (data.limit_kmh > 0.0) {
        std::snprintf(number, sizeof number, "%.0f", data.limit_kmh);
        centred_baseline(draw, pressure, 35.0f, x + 208.2f, y + 135.3f, text_colour, number);
    } else {
        centred_baseline(draw, pressure, 35.0f, x + 208.2f, y + 135.3f, dim_colour, "—");
    }

    // The vigilance triangle (red and blinking while the locomotive asks
    // for a check, the design's dark red otherwise) and the tape's arrow and
    // cassette (the mockup's 308.5/335.5 circles, bright green while rolling).
    draw->AddTriangleFilled(ImVec2{x + 295.95f, y + 79.75f}, ImVec2{x + 347.05f, y + 79.75f},
                            ImVec2{x + 321.5f, y + 124.0f},
                            data.vigilance && data.vigilance_lit ? red_colour : dark_red_colour);
    const ImU32 tape = data.tape_recording ? tape_on_colour : tape_off_colour;
    draw->AddCircle(ImVec2{x + 308.5f, y + 167.0f}, 9.5f, tape, 24, 2.0f);
    draw->AddCircle(ImVec2{x + 335.5f, y + 167.0f}, 9.5f, tape, 24, 2.0f);
    draw->AddLine(ImVec2{x + 308.5f, y + 157.5f}, ImVec2{x + 335.5f, y + 157.5f}, tape, 2.0f);
    draw->AddLine(ImVec2{x + 321.5f, y + 134.0f}, ImVec2{x + 321.5f, y + 152.0f}, tape, 2.0f);
    draw->AddTriangleFilled(ImVec2{x + 314.9f, y + 144.9f}, ImVec2{x + 329.1f, y + 144.9f},
                            ImVec2{x + 321.5f, y + 152.7f}, tape);

    char acceleration[80];
    std::snprintf(acceleration, sizeof acceleration, tr("Acceleration: %.1f m/s2"), data.acceleration);
    at_baseline(draw, small, 15.0f, x + 31.2f, y + 175.1f, label_colour, acceleration, 0.9f);
    separator(draw, origin, 188.5f, width);

    // ДАВЛЕНИЕ: ТМ, УР, ПМ, ТЦ (labels Montserrat Medium 15, values Onest 35).
    const char* names[4] = {tr("TM"), tr("UR"), tr("PM"), tr("TC")};
    constexpr float label_columns[4] = {83.0f, 155.0f, 227.0f, 298.0f};
    constexpr float value_columns[4] = {83.5f, 156.5f, 227.5f, 298.0f};
    for (int i = 0; i < 4; ++i) {
        centred_baseline(draw, label, 15.0f, x + label_columns[i], y + 219.6f, label_colour, names[i], 0.5f);
        centred_baseline(draw, pressure, 35.0f, x + value_columns[i], y + 255.45f, text_colour,
                         pressure_text(data.pressures[i]));
    }
    centred_baseline(draw, tiny, 10.0f, x + centre_x, y + 273.1f, dim_colour, tr("PRESSURE | KGF/CM2"), 1.25f);
    separator(draw, origin, 285.5f, width);

    // СЛЕДУЮЩИЙ СИГНАЛ (Montserrat Medium 15; the design's wide letter
    // spacing). The circle's centre is the cap height's centre of the name,
    // so it sits on the text's axis.
    centred_baseline(draw, label, 15.0f, x + centre_x, y + 316.3f, text_colour, tr("NEXT SIGNAL"), 1.7f);
    if (data.signal) {
        const ImVec2 centre{x + 39.5f, y + 337.5f};
        if (data.signal_aspect == formats::SignalAspect::stop_and_proceed) {
            // Red-yellow, not red-white: the passable signal's own pair.
            draw->AddCircleFilled(centre, 8.5f, IM_COL32(255, 218, 40, 255), 24);
            draw->PathClear();
            draw->PathArcTo(centre, 8.5f, 1.57079632679f, 4.71238898038f, 24);
            draw->PathFillConvex(IM_COL32(245, 48, 40, 255));
        } else {
            draw->AddCircleFilled(centre, 8.5f, aspect_colour(data.signal_aspect), 24);
        }
        const std::string distance = distance_text(data.signal_distance);
        const float distance_width = text_width(text_face, 15.0f, distance);
        const std::string name = data.signal_name.empty()
                                     ? std::string(aspect_name(data.signal_aspect))
                                     : trf("signal «%s»", data.signal_name.c_str());
        at_baseline(draw, text_face, 15.0f, x + 53.7f, y + 342.9f, text_colour,
                    elide(text_face, 15.0f, name, right_edge - 53.7f - distance_width - 12.0f), 0.5f);
        right_baseline(draw, text_face, 15.0f, x + 349.6f, y + 342.9f, dim_colour, distance, 0.5f);
    } else {
        at_baseline(draw, text_face, 15.0f, x + 53.7f, y + 342.9f, dim_colour, tr("none ahead"), 1.0f);
    }
    separator(draw, origin, 356.5f, width);

    // ПЛАТФОРМА (Montserrat Medium 15).
    centred_baseline(draw, label, 15.0f, x + centre_x, y + 388.5f, text_colour, tr("PLATFORM"), 6.5f);
    if (data.platform) {
        at_baseline(draw, text_face, 15.0f, x + 32.2f, y + 407.0f, text_colour,
                    elide(text_face, 15.0f, data.platform_name, right_edge - 32.2f), 0.5f);
        char distance[48];
        std::snprintf(distance, sizeof distance, tr("%.0f m"), data.platform_distance);
        at_baseline(draw, text_face, 15.0f, x + 31.4f, y + 424.0f, dim_colour, distance, 0.5f);
    } else {
        at_baseline(draw, text_face, 15.0f, x + 32.2f, y + 407.0f, dim_colour, tr("none ahead"), 1.0f);
    }
    separator(draw, origin, 434.5f, width);

    // ВРЕМЯ and FPS; F4 hides the panel.
    char time[48];
    std::snprintf(time, sizeof time, tr("TIME: %02d.%02d.%02d"), data.hour, data.minute, data.second);
    at_baseline(draw, text_face, 15.0f, x + 32.5f, y + 464.1f, text_colour, time, 1.35f);
    char fps[32];
    std::snprintf(fps, sizeof fps, "FPS: %.0f", data.fps);
    right_baseline(draw, text_face, 15.0f, x + 349.6f, y + 464.1f, dim_colour, fps, 0.7f);
    at_baseline(draw, small, 15.0f, x + 32.4f, y + 481.1f, dim_colour, tr("F4 - hide"), 0.7f);
}

} // namespace openrts::viewer

#endif
