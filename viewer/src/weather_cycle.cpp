#include "weather_cycle.hpp"

#include <algorithm>
#include <cmath>

namespace openrts::viewer {

namespace {

// splitmix64: the same on every compiler (lightning.hpp does the same).
std::uint64_t mix64(std::uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// Uniform 0..1 numbers of a spell.
struct Draws {
    std::uint64_t state;
    float next() noexcept {
        state = mix64(state);
        return static_cast<float>(state >> 40) / static_cast<float>(1ull << 24);
    }
};

float smooth(const float t) noexcept {
    const float x = std::clamp(t, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

double spell_seconds(const WeatherSettings& s) { return std::clamp(static_cast<double>(s.spell_minutes), 10.0, 240.0) * 60.0; }

double change_seconds(const WeatherSettings& s) {
    return (std::min)(std::clamp(static_cast<double>(s.change_minutes), 1.0, 60.0) * 60.0, 0.5 * spell_seconds(s));
}

// Spell k's weather and when (s) it begins to come.
WeatherMix spell(const std::int64_t k, const std::uint64_t seed, const WeatherSettings& s, const renderer::rts::Season season) {
    Draws d{mix64(seed ^ mix64(static_cast<std::uint64_t>(k) * 0x2545F4914F6CDD1Dull))};
    const auto climate = s.by_season ? season_climate(season) : s.climate;
    WeatherMix mix;
    mix.snow = season == renderer::rts::Season::winter;
    const float wet = d.next(), amount = d.next(), cover = d.next(), storm = d.next(), strength = d.next();
    if (wet < climate.wet) {
        mix.precipitation = 0.25f + 0.75f * amount;
        mix.cloud = 0.85f + 0.12f * cover;
        if (!mix.snow && storm < climate.storms) {
            mix.storm = 0.6f + 0.4f * strength;
            mix.precipitation = (std::max)(mix.precipitation, 0.7f);
            mix.cloud = 0.97f;
        }
    } else {
        mix.cloud = std::clamp(climate.cloudiness + 0.6f * (cover - 0.5f), 0.05f, 0.85f);
    }
    return mix;
}

double spell_start(const std::int64_t k, const std::uint64_t seed, const WeatherSettings& s) {
    Draws d{mix64(seed ^ 0xA5A5A5A5u ^ mix64(static_cast<std::uint64_t>(k)))};
    return (static_cast<double>(k) + 0.5 * d.next()) * spell_seconds(s);
}

} // namespace

WeatherClimate season_climate(const renderer::rts::Season season) noexcept {
    switch (season) {
    case renderer::rts::Season::winter: return {.cloudiness = 0.7f, .wet = 0.35f, .storms = 0.0f};
    case renderer::rts::Season::spring: return {.cloudiness = 0.55f, .wet = 0.3f, .storms = 0.1f};
    case renderer::rts::Season::autumn: return {.cloudiness = 0.7f, .wet = 0.4f, .storms = 0.05f};
    default: return {.cloudiness = 0.4f, .wet = 0.25f, .storms = 0.35f};
    }
}

WeatherMix blend(const WeatherMix& a, const WeatherMix& b, const float t) noexcept {
    const float s = std::clamp(t, 0.0f, 1.0f);
    WeatherMix m;
    m.cloud = a.cloud + (b.cloud - a.cloud) * s;
    m.precipitation = a.precipitation + (b.precipitation - a.precipitation) * s;
    m.storm = a.storm + (b.storm - a.storm) * s;
    // What falls: the coming one's once its own share is the larger.
    m.snow = (a.precipitation * (1.0f - s) > b.precipitation * s) ? a.snow : b.snow;
    return m;
}

WeatherMix weather_at(const double time, const std::uint64_t seed, const WeatherSettings& s, const renderer::rts::Season season) {
    const double spell_length = spell_seconds(s);
    const double change = change_seconds(s);
    const double t = (std::max)(time, 0.0);
    auto k = static_cast<std::int64_t>(std::floor(t / spell_length));
    // Before spell k has begun to come, spell k - 1 holds (its change over:
    // it began at least half a spell before).
    if (t < spell_start(k, seed, s)) --k;
    const double start = spell_start(k, seed, s);
    const auto now = spell(k, seed, s, season);
    if (t >= start + change) return now;
    return blend(spell(k - 1, seed, s, season), now, smooth(static_cast<float>((t - start) / change)));
}

renderer::rts::Weather rts_weather(const WeatherMix& mix) noexcept {
    if (mix.precipitation < 0.08f) return renderer::rts::Weather::clear;
    return mix.snow ? renderer::rts::Weather::snow : renderer::rts::Weather::rain;
}

bool storming(const WeatherMix& mix) noexcept { return !mix.snow && mix.storm >= 0.3f && mix.precipitation >= 0.08f; }

WeatherMix WeatherCycle::now(const renderer::rts::Season season) const {
    const auto cycle = weather_at(time, seed, settings, season);
    if (!forced) return cycle;
    const double change = change_seconds(settings);
    const double held = forced_at + change + spell_seconds(settings);
    const double since = time - forced_at;
    if (since < change) return blend(forced_from, *forced, smooth(static_cast<float>(since / change)));
    if (time < held) return *forced;
    return blend(*forced, cycle, smooth(static_cast<float>((time - held) / change)));
}

void WeatherCycle::force(const WeatherMix& target) {
    forced_from = shown_valid ? shown : weather_at(time, seed, settings, renderer::rts::Season::summer);
    forced = target;
    forced_at = time;
}

std::optional<double> WeatherCycle::next_change() const {
    if (forced) return std::nullopt;
    const double spell_length = spell_seconds(settings);
    auto k = static_cast<std::int64_t>(std::floor((std::max)(time, 0.0) / spell_length));
    for (int i = 0; i < 3; ++i, ++k) {
        const double start = spell_start(k, seed, settings);
        if (start > time) return start - time;
    }
    return std::nullopt;
}

WeatherCycle& weather_cycle() {
    static WeatherCycle cycle;
    return cycle;
}

} // namespace openrts::viewer
