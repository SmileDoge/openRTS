// openRTS's dynamic weather (viewer/src/weather_cycle.hpp, docs/weather.md
// "Dynamic weather"): the same seed and time give the same weather; it
// changes without jumps; the season's climate holds (snow in winter, no
// thunderstorms there); a climate without rain never rains, one all rain
// always does; a change by hand comes over a change, holds a spell and goes
// back to the cycle.

#include "../viewer/src/weather_cycle.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

namespace {

using namespace openrts;
using viewer::WeatherMix;

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

float jump(const WeatherMix& a, const WeatherMix& b) {
    return (std::max)({std::abs(a.cloud - b.cloud), std::abs(a.precipitation - b.precipitation), std::abs(a.storm - b.storm)});
}

void test_cycle() {
    viewer::WeatherSettings settings;
    settings.dynamic = true;
    const auto summer = renderer::rts::Season::summer;
    // The same seed and time: the same weather; another seed: another day.
    bool same = true, other = false;
    for (double t = 0.0; t < 6.0 * 3600.0; t += 600.0) {
        const auto a = viewer::weather_at(t, 42, settings, summer), b = viewer::weather_at(t, 42, settings, summer);
        same = same && jump(a, b) == 0.0f;
        other = other || jump(a, viewer::weather_at(t, 43, settings, summer)) > 0.05f;
    }
    check(same, "the same seed and time, the same weather");
    check(other, "another seed, another weather");

    // A week of summer a second at a time: no jump over 1 % a second (a
    // change of 12 minutes moves at most 0.95 / 720 s x 1.5); it rains some
    // of the time, storms some of the rain, never snows.
    WeatherMix last = viewer::weather_at(0.0, 7, settings, summer);
    float worst = 0.0f;
    double raining = 0.0, storming = 0.0, total = 0.0;
    bool snowed = false;
    for (double t = 1.0; t < 7.0 * 86400.0; t += 1.0) {
        const auto now = viewer::weather_at(t, 7, settings, summer);
        worst = (std::max)(worst, jump(last, now));
        last = now;
        total += 1.0;
        if (viewer::rts_weather(now) != renderer::rts::Weather::clear) raining += 1.0;
        if (viewer::storming(now)) storming += 1.0;
        snowed = snowed || now.snow;
    }
    std::cerr << "  a summer week: the largest change in a second " << worst << ", rain " << 100.0 * raining / total << " %, storms "
              << 100.0 * storming / total << " %" << std::endl;
    check(worst < 0.01f, "it changes without jumps");
    check(raining / total > 0.1 && raining / total < 0.45, "summer: it rains some of the time");
    check(storming > 0.0 && storming < raining, "summer: some rains are thunderstorms");
    check(!snowed, "summer: no snow");

    // Winter: snow, no thunderstorms.
    bool snow = false, storm = false, rain = false;
    for (double t = 0.0; t < 7.0 * 86400.0; t += 60.0) {
        const auto now = viewer::weather_at(t, 7, settings, renderer::rts::Season::winter);
        const auto rts = viewer::rts_weather(now);
        snow = snow || rts == renderer::rts::Weather::snow;
        rain = rain || rts == renderer::rts::Weather::rain;
        storm = storm || viewer::storming(now);
    }
    check(snow && !rain && !storm, "winter: snow, no rain, no thunderstorms");

    // The player's climate: none wet never rains; all wet always does
    // (but while one spell turns into the next).
    settings.by_season = false;
    settings.climate.wet = 0.0f;
    bool wet = false;
    for (double t = 0.0; t < 3.0 * 86400.0; t += 60.0) wet = wet || viewer::weather_at(t, 9, settings, summer).precipitation > 0.0f;
    check(!wet, "a dry climate: it never rains");
    settings.climate.wet = 1.0f;
    double dry = 0.0;
    for (double t = 0.0; t < 3.0 * 86400.0; t += 60.0)
        if (viewer::weather_at(t, 9, settings, summer).precipitation < 0.08f) dry += 60.0;
    check(dry == 0.0, "an all-wet climate: it always rains (" + std::to_string(dry) + " s dry)");
}

void test_forced() {
    viewer::WeatherCycle cycle;
    cycle.settings.dynamic = true;
    cycle.settings.spell_minutes = 60.0f;
    cycle.settings.change_minutes = 10.0f;
    cycle.seed = 5;
    cycle.time = 1000.0;
    cycle.shown = {.cloud = 0.2f};
    cycle.shown_valid = true;
    const WeatherMix storm{.cloud = 0.97f, .precipitation = 0.9f, .storm = 0.9f};
    cycle.force(storm);
    const auto season = renderer::rts::Season::summer;
    check(std::abs(cycle.now(season).cloud - 0.2f) < 1e-4f, "forced: from what was shown");
    cycle.time += 300.0;
    const auto halfway = cycle.now(season);
    check(halfway.cloud > 0.3f && halfway.cloud < 0.9f, "forced: on the way after half a change");
    cycle.time += 300.0 + 1800.0;
    check(jump(cycle.now(season), storm) < 1e-5f, "forced: held");
    check(!cycle.next_change(), "forced: no cycle change ahead while held");
    cycle.time = 1000.0 + 600.0 + 3600.0 + 600.0 + 1.0;
    check(jump(cycle.now(season), viewer::weather_at(cycle.time, 5, cycle.settings, season)) < 1e-5f, "forced: back to the cycle");
}

// The sky closes ahead of the rain: a change from fair to rain blends the
// clouds and the rain at one pace, yet the rain never falls from a fair sky.
void test_sky_cover() {
    const WeatherMix fair{.cloud = 0.3f};
    const WeatherMix rain{.cloud = 0.9f, .precipitation = 0.6f};
    check(std::abs(viewer::sky_cover(fair) - 0.3f) < 1e-6f, "sky cover: fair as its clouds");
    float lowest_raining = 1.0f, last = 0.0f;
    bool rising = true;
    for (int i = 0; i <= 1000; ++i) {
        const auto mix = viewer::blend(fair, rain, static_cast<float>(i) / 1000.0f);
        const float cover = viewer::sky_cover(mix);
        if (cover < last - 1e-6f) rising = false;
        last = cover;
        if (viewer::rts_weather(mix) != renderer::rts::Weather::clear) lowest_raining = (std::min)(lowest_raining, cover);
    }
    check(rising, "sky cover: closes without going back");
    check(lowest_raining > 0.7f, "sky cover: the rain begins under a grey sky (" + std::to_string(lowest_raining) + ")");
    check(viewer::sky_cover({.cloud = 0.9f, .precipitation = 0.9f, .storm = 0.9f}) > 0.96f, "sky cover: a thunderstorm's sky");
}

} // namespace

int main() {
    test_cycle();
    test_forced();
    test_sky_cover();
    if (failures == 0) std::cout << "weather cycle tests passed\n";
    return failures == 0 ? 0 : 1;
}
