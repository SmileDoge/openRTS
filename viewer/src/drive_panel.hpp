#pragma once

#ifdef OPENRTS_HAS_IMGUI

#include <cstdint>
#include <string>

#include <openrts/formats/signal_config.hpp>

namespace openrts::viewer {

// The F4 panel, TME's DrivingHudWindow as the design in Макеты/F4 draws it:
// what the driver looks at most. Speed and the permitted speed, the
// acceleration, the brake pressures, the next signal and platform, the time
// and the frame rate. TrainView makes the values; the panel only draws them.
struct DrivePanelData {
    bool moving = true;        // ДВИЖЕНИЕ; false: СТОЯНКА (|speed| < 0.1 m/s)
    double speed_kmh = 0.0;
    double limit_kmh = 0.0;    // 0: the route sets no limit for the train
    bool overspeed = false;
    double acceleration = 0.0; // m/s², + the way the train's front points
    // The locomotive's vigilance check: `vigilance` tells one is asked for,
    // `vigilance_lit` the lamp's state this frame (it blinks).
    bool vigilance = false;
    bool vigilance_lit = false;
    bool tape_recording = false;
    // ТМ, УР, ПМ, ТЦ in kgf/cm² (atm); NaN where the train has no such value.
    float pressures[4] = {};
    bool signal = false;
    formats::SignalAspect signal_aspect = formats::SignalAspect::stop;
    std::string signal_name; // the signal unit's own name ("Ч"), may be empty
    double signal_distance = 0.0;
    bool platform = false;
    std::string platform_name;
    double platform_distance = 0.0;
    int hour = 0, minute = 0, second = 0;
    float fps = 0.0f;
};

// The design's size: 382x511 px, drawn at the top right of the screen.
inline constexpr float drive_panel_width = 382.0f;
inline constexpr float drive_panel_height = 511.0f;

// Draws the panel into the current window's draw list, whose top left corner
// is the panel's. The window is sized by a dummy of the size above and has no
// padding or background of its own.
void draw_drive_panel(const DrivePanelData& data);

} // namespace openrts::viewer

#endif
