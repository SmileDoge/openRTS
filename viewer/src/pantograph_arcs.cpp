#include "pantograph_arcs.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

#include <glm/geometric.hpp>

namespace openrts::viewer {

float PantographArcs::icy_rate(const float speed) noexcept {
    // None at a walk; about one a second at 60 km/h, two at 120 and over.
    if (speed < 2.0f) return 0.0f;
    return (std::min)(speed / 16.7f, 2.0f);
}

void PantographArcs::begin(const ArcPantograph& p, const float length, const float size, const bool drawn_out) {
    const auto id = next_id_++;
    arcs_.push_back({id, p.key, p.position, p.back, length, length, size, drawn_out, p.speed});
    started_.push_back({id, p.key, p.position, length, size, p.ac});
}

void PantographArcs::update(const float elapsed, const std::span<const ArcPantograph> pantographs, const bool icy) {
    started_.clear();
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    std::unordered_set<std::uint64_t> seen;
    for (const auto& p : pantographs) {
        seen.insert(p.key);
        const auto it = contact_.find(p.key);
        // A pantograph first seen (a train come into view, loaded) as it is.
        if (it == contact_.end()) {
            contact_.emplace(p.key, p.contact);
            continue;
        }
        const bool was = it->second;
        it->second = p.contact;
        // Touching the wire: a short flash; leaving it: the arc drawn out.
        if (p.live && was != p.contact) {
            if (p.contact) begin(p, 0.12f + 0.18f * unit(random_), 1.0f, false);
            else begin(p, 0.3f + 0.4f * unit(random_), 1.0f, true);
            continue;
        }
        if (!(p.live && p.contact && icy && p.leading)) continue;
        // Ice: the head jumps off it now and then, a spark (and now and then
        // a few in a row).
        if (unit(random_) < icy_rate(p.speed) * elapsed) {
            begin(p, 0.04f + 0.2f * unit(random_) * unit(random_), 0.45f + 0.4f * unit(random_), false);
        }
    }
    for (auto it = contact_.begin(); it != contact_.end();) {
        it = seen.contains(it->first) ? std::next(it) : contact_.erase(it);
    }
    // The arcs follow their pantographs; they flicker; each frame a new thread.
    shown_.clear();
    for (auto& arc : arcs_) {
        for (const auto& p : pantographs) {
            if (p.key != arc.key) continue;
            arc.position = p.position;
            arc.back = p.back;
            arc.speed = p.speed;
        }
        arc.left -= elapsed;
        if (arc.left <= 0.0f) continue;
        const float age = 1.0f - arc.left / (std::max)(arc.length, 1e-3f);
        // The gap from the wire to the head: a few cm touching or jumping on
        // ice, growing as the head drops away (to some 0.5 m).
        const float gap = arc.drawn_out ? 0.04f + 0.5f * age * age : (0.02f + 0.08f * unit(random_)) * (0.5f + arc.size);
        PantographArc shown{.id = arc.id, .key = arc.key, .position = arc.position, .level = 0.35f + 0.65f * unit(random_), .size = arc.size};
        // Blown back on the run, bowed sideways at random.
        const glm::vec3 up{0.0f, 1.0f, 0.0f};
        glm::vec3 side = glm::cross(up, arc.back);
        if (glm::length(side) < 1e-3f) side = {1.0f, 0.0f, 0.0f};
        side = glm::normalize(side);
        const float blow = (std::min)(arc.speed / 30.0f, 1.0f) * gap * 1.5f;
        const float bow = (unit(random_) - 0.5f) * gap * 0.8f;
        const int pieces = arc.drawn_out ? 9 : 6;
        for (int k = 0; k <= pieces; ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(pieces);
            const float belly = std::sin(t * 3.14159265f);
            glm::vec3 p = arc.position - up * (gap * t) + arc.back * (blow * belly) + side * (bow * belly);
            if (k > 0 && k < pieces) {
                const float jag = gap * 0.18f + 0.004f;
                p += glm::vec3{(unit(random_) - 0.5f) * jag, (unit(random_) - 0.5f) * jag * 0.5f, (unit(random_) - 0.5f) * jag};
            }
            shown.path.push_back(p);
        }
        shown_.push_back(std::move(shown));
    }
    std::erase_if(arcs_, [](const Arc& a) { return a.left <= 0.0f; });
}

namespace {

constexpr std::uint32_t arc_rate = 44100;
constexpr double pi = 3.14159265358979;

struct Noise {
    std::uint32_t state;
    float next() {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / 8388608.0f - 1.0f;
    }
};

// A one-pole high-pass and low-pass, by their corner (Hz).
struct HighPass {
    float a, in = 0.0f, out = 0.0f;
    explicit HighPass(const float hz) : a(static_cast<float>(1.0 / (1.0 + 2.0 * pi * hz / arc_rate))) {}
    float operator()(const float x) {
        out = a * (out + x - in);
        in = x;
        return out;
    }
};
struct LowPass {
    float a, y = 0.0f;
    explicit LowPass(const float hz) : a(static_cast<float>(1.0 - std::exp(-2.0 * pi * hz / arc_rate))) {}
    float operator()(const float x) { return y += a * (x - y); }
};

std::shared_ptr<audio::AudioClip> clip_of(std::vector<float> samples) {
    float peak = 1e-6f;
    for (const float s : samples) peak = (std::max)(peak, std::abs(s));
    for (float& s : samples) s = s / peak * 0.85f;
    auto clip = std::make_shared<audio::AudioClip>();
    clip->channels = 1;
    clip->sample_rate = arc_rate;
    clip->frames = samples.size();
    clip->samples = std::move(samples);
    return clip;
}

} // namespace

std::shared_ptr<const audio::AudioClip> make_arc_snap(const std::uint32_t seed) {
    // A crack (a few ms of broadband noise, very loud, falling off fast),
    // then a short sizzle of high noise flickering in strength.
    const auto n = static_cast<std::size_t>(0.35 * arc_rate);
    std::vector<float> out(n);
    Noise noise{seed * 2654435761u + 11u};
    HighPass crack_hp(800.0f), sizzle_hp(2500.0f);
    LowPass flutter(60.0f);
    for (std::size_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / arc_rate;
        const float crack = crack_hp(noise.next()) * std::exp(-t / 0.004f) * 3.0f;
        const float strength = std::clamp(flutter(noise.next() * 3.0f) + 0.6f, 0.0f, 1.5f);
        const float sizzle = sizzle_hp(noise.next()) * strength * std::exp(-t / 0.09f) * 0.9f;
        out[i] = crack + sizzle;
    }
    return clip_of(std::move(out));
}

std::shared_ptr<const audio::AudioClip> make_arc_buzz(const bool ac, const std::uint32_t seed) {
    // A rough hiss (noise over 1.5 kHz) and a buzz (the arc's harmonics),
    // its strength flickering; on AC both pulse at 100 Hz (the arc dies and
    // strikes again each half wave). A second long, its ends matched to loop.
    const auto n = static_cast<std::size_t>(arc_rate);
    std::vector<float> out(n);
    Noise noise{seed * 2246822519u + 5u};
    HighPass hiss_hp(1500.0f);
    LowPass flutter(25.0f), body(4000.0f);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / arc_rate;
        const float strength = std::clamp(flutter(noise.next() * 2.5f) + 0.75f, 0.2f, 1.4f);
        float pulse = 1.0f;
        if (ac) {
            const float s = static_cast<float>(std::abs(std::sin(2.0 * pi * 50.0 * t)));
            pulse = 0.25f + 0.75f * s * s * s;
        }
        // Harmonics of 100 Hz, a sawtooth's (rough), whole cycles in the second.
        float buzz = 0.0f;
        for (int h = 1; h <= 12; ++h) buzz += static_cast<float>(std::sin(2.0 * pi * 100.0 * h * t)) / static_cast<float>(h);
        const float hiss = hiss_hp(noise.next());
        out[i] = body(hiss * 0.9f * strength * pulse + buzz * 0.25f * pulse);
    }
    // The loop's seam: the last 20 ms faded into the first.
    const std::size_t fade = arc_rate / 50;
    for (std::size_t k = 0; k < fade; ++k) {
        const float w = static_cast<float>(k) / static_cast<float>(fade);
        out[k] = out[k] * w + out[n - fade + k] * (1.0f - w);
    }
    out.resize(n - fade);
    auto clip = clip_of(std::move(out));
    clip->loop_start = 0;
    clip->loop_end = clip->frames;
    return clip;
}

} // namespace openrts::viewer
