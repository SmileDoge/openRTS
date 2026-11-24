#include "train_extra_sounds.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>

#include <glm/geometric.hpp>

namespace openrts::viewer {

namespace {

constexpr std::uint32_t out_rate = 44100;
constexpr double pi = 3.141592653589793;

// A biquad (RBJ's cookbook), run over a whole signal.
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    static Biquad low(double f, double rate, double q = 0.7071) {
        const double w = 2 * pi * f / rate, c = std::cos(w), alpha = std::sin(w) / (2 * q), a0 = 1 + alpha;
        return {(1 - c) / 2 / a0, (1 - c) / a0, (1 - c) / 2 / a0, -2 * c / a0, (1 - alpha) / a0};
    }
    static Biquad high(double f, double rate, double q = 0.7071) {
        const double w = 2 * pi * f / rate, c = std::cos(w), alpha = std::sin(w) / (2 * q), a0 = 1 + alpha;
        return {(1 + c) / 2 / a0, -(1 + c) / a0, (1 + c) / 2 / a0, -2 * c / a0, (1 - alpha) / a0};
    }
    void run(std::vector<float>& x) const {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (float& v : x) {
            const double y = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1;
            x1 = v;
            y2 = y1;
            y1 = y;
            v = static_cast<float>(y);
        }
    }
};

std::vector<float> filtered(std::vector<float> x, std::initializer_list<Biquad> stages) {
    for (const auto& s : stages) s.run(x);
    return x;
}

float peak(const std::vector<float>& x) {
    float p = 0.0f;
    for (const float v : x) p = (std::max)(p, std::abs(v));
    return p;
}

void scale(std::vector<float>& x, const float to) {
    const float p = peak(x);
    if (p > 0.0f)
        for (float& v : x) v *= to / p;
}

// A small LCG's noise, -1..1.
struct Noise {
    std::uint32_t state;
    float next() {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / 8388608.0f - 1.0f;
    }
};

// Mono at out_rate: a whole-number step up through a windowed sinc, else
// straight lines between samples.
std::vector<float> mono_at_out_rate(const audio::AudioClip& clip) {
    std::vector<float> mono(static_cast<std::size_t>(clip.frames));
    const auto ch = (std::max)(clip.channels, 1u);
    for (std::size_t f = 0; f < mono.size(); ++f) {
        float sum = 0.0f;
        for (std::uint32_t c = 0; c < ch; ++c) sum += clip.samples[f * ch + c];
        mono[f] = sum / static_cast<float>(ch);
    }
    if (clip.sample_rate == out_rate || clip.sample_rate == 0) return mono;
    const auto step = out_rate / clip.sample_rate;
    std::vector<float> out;
    if (step >= 2 && step * clip.sample_rate == out_rate) {
        const int half = 8 * static_cast<int>(step);
        std::vector<float> taps(static_cast<std::size_t>(2 * half + 1));
        const double cut = 0.47 / step; // of the new rate
        for (int k = -half; k <= half; ++k) {
            const double s = k == 0 ? 2 * cut : std::sin(2 * pi * cut * k) / (pi * k);
            const double w = 0.42 + 0.5 * std::cos(pi * k / half) + 0.08 * std::cos(2 * pi * k / half); // Blackman
            taps[static_cast<std::size_t>(k + half)] = static_cast<float>(s * w * step);
        }
        out.assign(mono.size() * step, 0.0f);
        for (std::size_t n = 0; n < out.size(); ++n) {
            double acc = 0.0;
            // Only the taps that land on a real sample (the others are zeros).
            const long long first = static_cast<long long>(n) - half;
            long long k0 = first % static_cast<long long>(step);
            if (k0 < 0) k0 += step;
            for (long long m = first + (k0 ? static_cast<long long>(step) - k0 : 0); m <= static_cast<long long>(n) + half;
                 m += static_cast<long long>(step)) {
                if (m < 0) continue;
                const auto src = static_cast<std::size_t>(m / static_cast<long long>(step));
                if (src >= mono.size()) break;
                acc += mono[src] * taps[static_cast<std::size_t>(m - first)];
            }
            out[n] = static_cast<float>(acc);
        }
        return out;
    }
    const double ratio = static_cast<double>(clip.sample_rate) / out_rate;
    out.resize(static_cast<std::size_t>(static_cast<double>(mono.size()) / ratio));
    for (std::size_t n = 0; n < out.size(); ++n) {
        const double at = static_cast<double>(n) * ratio;
        const auto i = static_cast<std::size_t>(at);
        const auto t = static_cast<float>(at - static_cast<double>(i));
        out[n] = mono[i] * (1 - t) + mono[(std::min)(i + 1, mono.size() - 1)] * t;
    }
    return out;
}

} // namespace

float brake_squeal_level(const float speed, const float cylinder) noexcept {
    const float kmh = speed * 3.6f;
    if (kmh < 0.3f || kmh > 26.0f) return 0.0f;
    const float pressed = std::clamp((cylinder - 0.5f) / 2.0f, 0.0f, 1.0f);
    const float by_speed = kmh < 6.0f ? kmh / 6.0f : 1.0f - (kmh - 6.0f) / 20.0f;
    return pressed * std::clamp(by_speed, 0.0f, 1.0f);
}

std::shared_ptr<const audio::AudioClip> lift_squeal(const audio::AudioClip& recording, const std::uint32_t seed) {
    const bool narrow = recording.sample_rate <= 22050;
    auto x = mono_at_out_rate(recording);
    if (x.size() < out_rate / 4) return nullptr;
    const double r = out_rate;
    x = filtered(std::move(x), {Biquad::high(120, r)});
    scale(x, 1.0f);
    Noise noise{seed * 2654435761u + 1u};
    if (narrow) {
        // Harmonics of its own tone above the old limit: an uneven
        // saturation of its top band, only what lands above 5.6 kHz kept.
        auto top = filtered(x, {Biquad::high(2500, r), Biquad::high(2500, r), Biquad::low(5400, r), Biquad::low(5400, r)});
        const float tp = (std::max)(peak(top), 1e-6f);
        for (float& v : top) v = std::tanh(3.0f * v / tp + 0.4f) - std::tanh(0.4f);
        top = filtered(std::move(top), {Biquad::high(5600, r), Biquad::high(5600, r), Biquad::high(5600, r), Biquad::low(15000, r)});
        scale(top, 1.0f);
        // Air up high, following its loudness.
        std::vector<float> env(x.size());
        for (std::size_t i = 0; i < x.size(); ++i) env[i] = std::abs(x[i]);
        env = filtered(std::move(env), {Biquad::low(30, r), Biquad::low(30, r)});
        const float ep = (std::max)(peak(env), 1e-6f);
        std::vector<float> air(x.size());
        for (float& v : air) v = noise.next();
        air = filtered(std::move(air), {Biquad::high(7000, r), Biquad::low(15000, r)});
        scale(air, 1.0f);
        for (std::size_t i = 0; i < x.size(); ++i) x[i] += 0.5f * top[i] + 0.05f * air[i] * env[i] / ep;
        // A gentle shelf over 4 kHz.
        const auto bright = filtered(x, {Biquad::high(4000, r), Biquad::high(4000, r)});
        for (std::size_t i = 0; i < x.size(); ++i) x[i] += 0.25f * bright[i];
    }
    // Its loudest stretch (the squeal, not the run-up).
    std::vector<float> env(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) env[i] = std::abs(x[i]);
    env = filtered(std::move(env), {Biquad::low(5, r), Biquad::low(5, r)});
    const float ep = peak(env);
    std::size_t a = 0, b = x.size();
    while (a < b && env[a] < 0.45f * ep) ++a;
    while (b > a && env[b - 1] < 0.45f * ep) --b;
    if (b - a < out_rate / 4) {
        a = 0;
        b = x.size();
    }
    std::vector<float> body(x.begin() + static_cast<std::ptrdiff_t>(a), x.begin() + static_cast<std::ptrdiff_t>(b));
    // Grit: a slight uneven flutter and a metallic scrape following it.
    std::vector<float> flutter(body.size()), scrape(body.size()), benv(body.size());
    for (float& v : flutter) v = noise.next();
    flutter = filtered(std::move(flutter), {Biquad::low(70, r), Biquad::low(70, r)});
    scale(flutter, 1.0f);
    for (float& v : scrape) v = noise.next();
    scrape = filtered(std::move(scrape), {Biquad::high(1400, r), Biquad::low(7000, r)});
    scale(scrape, 1.0f);
    for (std::size_t i = 0; i < body.size(); ++i) benv[i] = std::abs(body[i]);
    benv = filtered(std::move(benv), {Biquad::low(25, r)});
    const float bp = (std::max)(peak(benv), 1e-6f);
    for (std::size_t i = 0; i < body.size(); ++i) body[i] = body[i] * (0.93f + 0.07f * flutter[i]) + 0.08f * scrape[i] * benv[i] / bp;
    // Seamless: its end crossfaded over its start.
    const std::size_t fade = (std::min)(body.size() / 4, static_cast<std::size_t>(0.4 * out_rate));
    std::vector<float> loop(body.begin(), body.end() - static_cast<std::ptrdiff_t>(fade));
    for (std::size_t i = 0; i < fade; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(fade);
        loop[i] = loop[i] * t + body[body.size() - fade + i] * (1 - t);
    }
    scale(loop, 0.7f);
    auto clip = std::make_shared<audio::AudioClip>();
    clip->channels = 1;
    clip->sample_rate = out_rate;
    clip->frames = loop.size();
    clip->samples = std::move(loop);
    return clip;
}

float TrainExtraSounds::random(const float lo, const float hi) {
    random_ = random_ * 1664525u + 1013904223u;
    return lo + (hi - lo) * static_cast<float>(random_ >> 8) / 16777216.0f;
}

void TrainExtraSounds::load_clips() {
    loaded_ = true;
    for (const char* name : {"TRAINS/TRAINSET/gkl_TU6-Alias/Sound/braking_squeal.wav", "SOUND/x_uk_brakesqueal02a.wav", "SOUND/squeal.wav"}) {
        const auto path = core::VirtualPath::parse(name);
        if (!path) continue;
        if (const auto recording = load_(*path)) {
            if ((squeal_ = lift_squeal(*recording))) {
                squeal_source_ = name;
                break;
            }
        }
    }
    // No recording, no squeal: a made one sounded cheap (testers).
    if (!squeal_) squeal_source_ = "none";
    if (const auto path = core::VirtualPath::parse("SOUND/OPENRTS/BRAKES/stop_moan.wav")) moan_ = load_(*path);
    if (const auto path = core::VirtualPath::parse("SOUND/OPENRTS/BRAKES/stop_squeak.wav")) squeak_ = load_(*path);
    if (moan_ && moan_->sample_rate > 0) moan_seconds_ = static_cast<float>(moan_->frames) / static_cast<float>(moan_->sample_rate);
    std::cout << "brake squeal: " << (squeal_ ? squeal_source_ : std::string("none (no recorded squeal in the content: silent)")) << (moan_ ? ", the stop's moan" : ", no stop's moan (no SOUND/OPENRTS/BRAKES/stop_moan.wav)")
              << (squeak_ ? ", the stop's squeak" : "") << std::endl;
}

void TrainExtraSounds::update(const float elapsed, std::span<const ExtraSoundTrain> trains, const glm::vec3& eye, const float inside_gain,
                              const bool squeal, audio::AudioEngine& sound) {
    if (squeal && !loaded_) load_clips();
    for (auto& [id, state] : trains_) state.seen = false;
    constexpr std::size_t heard_cars = 4;
    const float dt = (std::max)(elapsed, 0.0f);
    for (const auto& train : trains) {
        auto& state = trains_[train.id];
        state.seen = true;
        const float kmh = train.speed * 3.6f;
        const float room = train.inside ? (std::max)(inside_gain, 0.4f) : inside_gain;
        // The braked cars nearest the listener.
        std::vector<std::pair<float, std::size_t>> near;
        bool braking = false;
        float nearest = 1e30f;
        for (std::size_t i = 0; i < train.cars.size(); ++i) {
            const auto& car = train.cars[i];
            if (!car.squeals || car.cylinder < 0.5f) continue;
            braking = true;
            const float d = glm::distance(car.position, eye);
            if (d < nearest) {
                nearest = d;
                state.at = car.position + glm::vec3{0.0f, 0.6f, 0.0f};
            }
            if (squeal && brake_squeal_level(car.speed, car.cylinder) > 0.0f) near.emplace_back(d, i);
        }
        std::sort(near.begin(), near.end());
        if (near.size() > heard_cars) near.resize(heard_cars);
        // The stop's moan: once, begun to end with the stop.
        if (kmh > 6.0f || !squeal) state.moan_done = false;
        if (state.moan && !sound.playing(state.moan)) state.moan = 0;
        if (squeal && moan_ && !state.moan && !state.moan_done && braking && kmh < 20.0f && kmh > 0.5f && train.deceleration > 0.05f
            && train.speed / train.deceleration <= moan_seconds_) {
            state.moan = sound.play(moan_, audio::VoicePlayback::once, audio::VoiceSpace::positional);
            state.moan_age = 0.0f;
            state.moan_level = 0.0f;
            state.moan_done = true;
        }
        float duck = 1.0f;
        if (state.moan) {
            state.moan_age += dt;
            // In over half a second; out fast once it stands or runs on.
            const bool going = kmh > 0.05f && train.deceleration > -0.1f && braking && squeal;
            const float target = going ? 0.85f * (std::min)(state.moan_age / 0.5f, 1.0f) * room : 0.0f;
            const float rate = target > state.moan_level ? 6.0f : 7.0f;
            state.moan_level += (target - state.moan_level) * (1.0f - std::exp(-rate * dt));
            if (!going && state.moan_level < 0.01f) {
                sound.stop(state.moan);
                state.moan = 0;
            } else {
                sound.set(state.moan, state.moan_level, 1.0f, state.at);
                duck = 1.0f - 0.6f * (std::min)(state.moan_age / 0.5f, 1.0f);
            }
        }
        // The quiet squeak from about 3 km/h to the stop.
        {
            const float on = braking && squeal && squeak_ ? std::clamp((3.0f - kmh) / 1.5f, 0.0f, 1.0f) * std::clamp(kmh / 0.2f, 0.0f, 1.0f) : 0.0f;
            const float target = 0.3f * on * room;
            state.squeak_level += (target - state.squeak_level) * (1.0f - std::exp(-(target > state.squeak_level ? 3.0f : 6.0f) * dt));
            if (state.squeak_level < 0.004f && target <= 0.0f) {
                if (state.squeak) sound.stop(state.squeak);
                state.squeak = 0;
            } else {
                if (!state.squeak) {
                    state.squeak = sound.play(squeak_, audio::VoicePlayback::loop, audio::VoiceSpace::positional);
                    state.squeak_pitch = random(0.95f, 1.05f);
                }
                if (state.squeak) sound.set(state.squeak, state.squeak_level, state.squeak_pitch, state.at);
            }
        }
        // The squeal, car by car.
        for (auto& [i, car] : state.cars) car.seen = false;
        for (const auto& [d, i] : near) {
            auto [it, fresh] = state.cars.try_emplace(i);
            auto& car = it->second;
            car.seen = true;
            if (fresh) {
                car.pitch0 = random(0.88f, 1.15f);
                car.timer = random(0.2f, 1.5f);
                car.mode_timer = random(0.5f, 2.5f);
            }
            car.at = train.cars[i].position + glm::vec3{0.0f, 0.5f, 0.0f};
        }
        for (auto it = state.cars.begin(); it != state.cars.end();) {
            auto& car = it->second;
            const auto& c = it->first < train.cars.size() ? train.cars[it->first] : ExtraSoundCar{};
            const float ck = c.speed * 3.6f;
            const float level = car.seen ? brake_squeal_level(c.speed, c.cylinder) : 0.0f;
            const float slow = std::clamp(1.0f - ck / 26.0f, 0.0f, 1.0f);
            // Catching and letting go.
            car.timer -= dt;
            if (car.timer <= 0.0f) {
                car.on = !car.on && level > 0.0f;
                car.timer = car.on ? random(0.6f, 1.4f) * (0.8f + 2.4f * slow) : random(0.2f, 1.0f) * (1.6f - 1.3f * slow);
            }
            // The shoe's mode: a step now and then.
            car.mode_timer -= dt;
            if (car.mode_timer <= 0.0f) {
                car.mode_timer = random(0.5f, 2.5f);
                if (random(0.0f, 1.0f) < 0.6f) {
                    const float steps[] = {0.94f, 0.97f, 1.0f, 1.03f, 1.06f};
                    car.mode = steps[static_cast<int>(random(0.0f, 4.999f))];
                }
            }
            car.wobble_timer -= dt;
            if (car.wobble_timer <= 0.0f) {
                car.wobble_timer = random(0.15f, 0.35f);
                car.wobble_target = random(0.7f, 1.0f);
            }
            car.wobble += (car.wobble_target - car.wobble) * (1.0f - std::exp(-5.0f * dt));
            const float last = ck > 0.05f ? std::clamp((2.0f - ck) / 2.0f, 0.0f, 1.0f) : 0.0f;
            const float target = car.on ? level * 0.75f * car.wobble * (1.0f + 0.2f * last) * duck * room : 0.0f;
            car.level += (target - car.level) * (1.0f - std::exp(-25.0f * dt));
            const float pitch = car.pitch0 * car.mode * (1.0f + 0.06f * slow + 0.03f * last);
            if (car.level < 0.003f && target <= 0.0f) {
                if (car.voice) sound.stop(car.voice);
                car.voice = 0;
                if (!car.seen) {
                    it = state.cars.erase(it);
                    continue;
                }
            } else {
                if (!car.voice && squeal_) car.voice = sound.play(squeal_, audio::VoicePlayback::loop, audio::VoiceSpace::positional);
                if (car.voice) sound.set(car.voice, car.level, pitch, car.at);
            }
            ++it;
        }
    }
    // Trains gone: their voices too.
    for (auto it = trains_.begin(); it != trains_.end();) {
        if (it->second.seen) {
            ++it;
            continue;
        }
        for (auto& [i, car] : it->second.cars)
            if (car.voice) sound.stop(car.voice);
        if (it->second.moan) sound.stop(it->second.moan);
        if (it->second.squeak) sound.stop(it->second.squeak);
        it = trains_.erase(it);
    }
}

void TrainExtraSounds::stop(audio::AudioEngine& sound) {
    for (auto& [id, state] : trains_) {
        for (auto& [i, car] : state.cars)
            if (car.voice) sound.stop(car.voice);
        if (state.moan) sound.stop(state.moan);
        if (state.squeak) sound.stop(state.squeak);
    }
    trains_.clear();
}

} // namespace openrts::viewer
