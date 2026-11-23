#pragma once

// openRTS's dynamic weather (docs/weather.md, "Dynamic weather"; testers'
// proposal 2026-11-17): the clouds' cover, the rain or snow and the
// thunderstorms change by themselves over the game's time, from a climate
// (the season's or the player's own), and can be changed by hand in the
// game. RTS has a weather fixed for the whole game.
//
// The weather is a function of the game's time and a seed, as the lightning
// is (lightning.hpp): whoever has the same seed and clock has the same
// weather, so a server can later give it to every player with no package
// but the seed. Single player for now.

#include <cstdint>
#include <optional>

#include <openrts/renderer/rts_season.hpp>

namespace openrts::viewer {

// The weather at a moment.
struct WeatherMix {
    float cloud = 0.42f;       // the clouds' cover, 0..1
    float precipitation = 0.0f; // 0 none .. 1 heavy (rain, or snow in winter)
    float storm = 0.0f;        // 0 .. 1 a thunderstorm's strength (lightning, darker clouds)
    bool snow = false;         // what falls: snow (winter) or rain
};

// How a place's weather goes.
struct WeatherClimate {
    float cloudiness = 0.5f; // the clouds' usual cover when nothing falls, 0..1
    float wet = 0.3f;        // the share of spells with rain or snow, 0..1
    float storms = 0.2f;     // the share of rains that are thunderstorms, 0..1 (none in winter)
};

// The season's climate (by_season): winter overcast with snow, spring and
// autumn rainy, summer clearer with thunderstorms.
[[nodiscard]] WeatherClimate season_climate(renderer::rts::Season season) noexcept;

struct WeatherSettings {
    bool dynamic = false;  // off: the weather chosen stays (as RTS)
    bool by_season = true; // the season's climate, else `climate`
    WeatherClimate climate;
    float spell_minutes = 45.0f;  // a weather lasts about this long, game minutes (10..240)
    float change_minutes = 12.0f; // it turns into the next over this long (1..60, at most half a spell)
};

// The weather `seed` gives at game time `time` (s since the cycle began):
// spells of about `spell_minutes` (each starting up to half a spell late),
// each turning into the next over `change_minutes`.
[[nodiscard]] WeatherMix weather_at(double time, std::uint64_t seed, const WeatherSettings& settings, renderer::rts::Season season);

// What the scene makes of a mix: RTS's weather (its environment file,
// textures, falling drops, sound, the rails) once 8 % of the precipitation
// falls, and a thunderstorm once its strength is 0.3 with rain.
[[nodiscard]] renderer::rts::Weather rts_weather(const WeatherMix& mix) noexcept;
[[nodiscard]] bool storming(const WeatherMix& mix) noexcept;
// The cover the sky shows: the mix's clouds, closed ahead of what falls
// (the rain or snow comes under a grey sky, not out of a fair one while a
// change blends both at one pace): most of the way by the time it begins
// to fall, all of it (rain 90 %, snow 85 %, storm 97 %) soon after.
[[nodiscard]] float sky_cover(const WeatherMix& mix) noexcept;
// Smoothly between two mixes (t 0..1).
[[nodiscard]] WeatherMix blend(const WeatherMix& a, const WeatherMix& b, float t) noexcept;

// The game's weather: its settings, seed and clock, and a change asked by
// hand (the panel's "now"): the weather goes to `forced` over a change,
// holds it a spell, and goes back to the cycle over another change.
struct WeatherCycle {
    WeatherSettings settings;
    std::uint64_t seed = 1;
    double time = 0.0; // game seconds the cycle has run
    std::optional<WeatherMix> forced;
    WeatherMix forced_from;
    double forced_at = 0.0;
    // The weather shown (the scene eases it towards `now`, so a setting
    // changed by hand does not jump), for the panel.
    WeatherMix shown;
    bool shown_valid = false;

    [[nodiscard]] WeatherMix now(renderer::rts::Season season) const;
    // Ask for `target` from now on (from what is shown).
    void force(const WeatherMix& target);
    // Game seconds until the cycle's next spell begins to come (none while forced).
    [[nodiscard]] std::optional<double> next_change() const;
};

// The one the panel and the scene share.
[[nodiscard]] WeatherCycle& weather_cycle();

} // namespace openrts::viewer
