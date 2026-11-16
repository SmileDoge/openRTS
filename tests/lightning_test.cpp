#include <cmath>
#include <cstdio>
#include <set>
#include <string>

#include <glm/geometric.hpp>

#include <openrts/renderer/lightning.hpp>

namespace {

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        std::printf("FAILED: %s\n", what.c_str());
        ++failures;
    }
}

} // namespace

int main() {
    using namespace openrts::renderer;
    const glm::dvec2 eye{40960.0 + 512.0, -20480.0 + 300.0};

    // The same strikes for the same place and time (every client alike).
    {
        const auto a = lightning_strikes(eye, 43210.25, 30.0f);
        const auto b = lightning_strikes(eye, 43210.25, 30.0f);
        bool same = a.size() == b.size();
        for (std::size_t i = 0; same && i < a.size(); ++i) same = a[i].id == b[i].id && a[i].ground == b[i].ground && a[i].start == b[i].start;
        check(same, "the same strikes twice");
    }

    // Over an hour: about `rate` a minute within the radius, each alight for
    // its life only, within the radius, never twice the same.
    {
        constexpr float rate = 6.0f;
        std::set<std::uint64_t> seen;
        int outside = 0;
        double near_sum = 0.0;
        int frames = 0;
        for (double t = 36000.0; t < 36000.0 + 3600.0; t += 0.1) {
            for (const auto& s : lightning_strikes(eye, t, rate)) {
                seen.insert(s.id);
                if (glm::length(s.ground - eye) > lightning_radius + 1.0) ++outside;
                if (t - s.start < 0.0 || t - s.start >= lightning_duration) ++outside;
                near_sum += lightning_flash(s, t - s.start);
            }
            ++frames;
        }
        const double per_minute = static_cast<double>(seen.size()) / 60.0;
        check(outside == 0, "strikes only within the radius and their life");
        // The gathering drifts over the eye: a long while is near the rate.
        check(per_minute > rate * 0.35 && per_minute < rate * 2.2, "about the rate a minute: " + std::to_string(per_minute));
        check(near_sum > 0.0, "some light");
        // A closer radius sees fewer.
        std::set<std::uint64_t> near;
        for (double t = 36000.0; t < 36000.0 + 3600.0; t += 0.1) {
            for (const auto& s : lightning_strikes(eye, t, rate, 3000.0f)) near.insert(s.id);
        }
        check(near.size() < seen.size() / 4, "fewer within 3 km");
        check(lightning_strikes(eye, 36000.0, 0.0f).empty(), "rate 0: none");
    }

    // The gathering: about 1 on average, 0.2..2.9.
    {
        double sum = 0.0;
        float low = 10.0f, high = 0.0f;
        int n = 0;
        for (int i = 0; i < 400; ++i) {
            for (int j = 0; j < 40; ++j) {
                const float g = lightning_gathering({i * 1700.0, j * 9100.0}, 1000.0 + j);
                sum += g;
                low = (std::min)(low, g);
                high = (std::max)(high, g);
                ++n;
            }
        }
        const double mean = sum / n;
        check(mean > 0.85 && mean < 1.15, "gathering averages 1: " + std::to_string(mean));
        check(low >= 0.2f && high <= 2.91f, "gathering within 0.2..2.9");
    }

    // A strike's light: none before or after it, its first stroke near its
    // strength, flickering (dark between the strokes of a multi-stroke one).
    {
        LightningStrike s;
        s.seed = 12345u;
        s.strength = 1.0f;
        float peak = 0.0f;
        for (double age = 0.0; age < lightning_duration; age += 0.001) peak = (std::max)(peak, lightning_flash(s, age));
        check(peak > 0.9f && peak <= 1.01f, "the first stroke at its strength: " + std::to_string(peak));
        check(lightning_flash(s, -0.01) == 0.0f && lightning_flash(s, lightning_duration) == 0.0f, "dark outside its life");
        check(lightning_flash(s, lightning_duration - 0.01) < 0.1f, "faded by its end");
        s.to_ground = false;
        float cloud_peak = 0.0f;
        for (double age = 0.0; age < lightning_duration; age += 0.001) cloud_peak = (std::max)(cloud_peak, lightning_flash(s, age));
        check(cloud_peak > 0.25f && cloud_peak < 0.8f, "within the cloud: softer");
        // At 30 frames a second, whatever the frames' phase, some frame
        // shows the first stroke near its peak.
        s.to_ground = true;
        for (const double phase : {0.0, 0.007, 0.013, 0.021, 0.029}) {
            float seen = 0.0f;
            for (double age = phase; age < 0.2; age += 1.0 / 30.0) seen = (std::max)(seen, lightning_flash_seen(s, age, 1.0 / 30.0));
            check(seen > 0.9f, "a frame shows the first stroke, phase " + std::to_string(phase) + ": " + std::to_string(seen));
        }
        check(lightning_flash_seen(s, 0.5, 0.0) == lightning_flash(s, 0.5), "a frame of no length: the instant");
        int dips = 0;
        for (std::uint32_t seed = 1; seed < 200; ++seed) {
            LightningStrike m;
            m.seed = seed;
            bool was_bright = false, dipped = false;
            for (double age = 0.0; age < lightning_duration; age += 0.002) {
                const float f = lightning_flash(m, age);
                if (f > 0.5f && dipped) {
                    ++dips;
                    break;
                }
                if (f > 0.5f) was_bright = true;
                if (was_bright && f < 0.15f) dipped = true;
            }
        }
        check(dips > 40, "many strikes flicker: " + std::to_string(dips) + " of 199");
    }

    // The channel: from the cloud's base to the ground point, branches below
    // its top, none for a flash within the cloud.
    {
        LightningStrike s;
        s.seed = 777u;
        const auto channel = lightning_channel(s, 1500.0f);
        check(channel.size() > 128, "a channel of many pieces");
        bool grounded = false, topped = false, inside = true;
        for (const auto& piece : channel) {
            if (piece.width == 1.0f && glm::length(piece.b) < 1e-3f) grounded = true;
            if (piece.width == 1.0f && piece.a.y >= 1499.0f && piece.brightness_a == 0.0f) topped = true;
            for (const auto& p : {piece.a, piece.b}) inside = inside && p.y >= -1e-3f && p.y <= 1500.0f * 1.3f;
        }
        check(grounded, "the main channel reaches the ground point");
        check(topped, "its top fades into the cloud");
        check(inside, "between the ground and the cloud");
        const auto again = lightning_channel(s, 1500.0f);
        check(again.size() == channel.size() && again.front().b == channel.front().b, "the same channel twice");
        s.to_ground = false;
        check(lightning_channel(s, 1500.0f).empty(), "no channel within the cloud");
    }

    if (failures == 0) std::printf("lightning: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
