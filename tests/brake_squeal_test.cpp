// openRTS's braking sound (viewer/src/train_extra_sounds.hpp): an 11 kHz
// recording's highs remade; a train braking to a stop squeals car by car,
// its stop's moan begins to end with the stop and the squeak comes at the
// end; standing, all is quiet.

#include "../viewer/src/train_extra_sounds.hpp"

#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

#include <openrts/audio/audio_engine.hpp>

using namespace openrts;

namespace {

int failures = 0;

void check(const bool condition, const char* what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

std::shared_ptr<audio::AudioClip> tone(const std::uint32_t rate, const double hz, const double seconds) {
    auto clip = std::make_shared<audio::AudioClip>();
    clip->channels = 1;
    clip->sample_rate = rate;
    clip->frames = static_cast<std::uint64_t>(seconds * rate);
    for (std::uint64_t i = 0; i < clip->frames; ++i)
        clip->samples.push_back(0.5f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * hz * static_cast<double>(i) / rate)));
    return clip;
}

// The share of a clip's energy above `hz` (a plain DFT over a stretch).
double share_above(const audio::AudioClip& clip, const double hz) {
    const std::size_t n = 4096;
    if (clip.samples.size() < 2 * n) return 0.0;
    const std::size_t from = clip.samples.size() / 2; // the middle, away from the loop's seam
    double above = 0.0, all = 0.0;
    for (std::size_t k = 1; k < n / 2; ++k) {
        double re = 0.0, im = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double w = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * static_cast<double>(i) / n); // Hann
            const double a = 2.0 * 3.14159265358979 * static_cast<double>(k * i) / n;
            re += w * clip.samples[from + i] * std::cos(a);
            im -= w * clip.samples[from + i] * std::sin(a);
        }
        const double e = re * re + im * im;
        all += e;
        if (static_cast<double>(k) * clip.sample_rate / n > hz) above += e;
    }
    return all > 0.0 ? above / all : 0.0;
}

} // namespace

int main() {
    check(viewer::brake_squeal_level(30.0f / 3.6f, 3.0f) == 0.0f, "no squeal at 30 km/h");
    check(viewer::brake_squeal_level(5.0f / 3.6f, 3.0f) > 0.5f, "loud at a few km/h, braked");
    check(viewer::brake_squeal_level(5.0f / 3.6f, 0.3f) == 0.0f, "none with the brakes off");
    check(viewer::brake_squeal_level(0.0f, 3.0f) == 0.0f, "none standing");

    // An 11025 Hz squeal: 4.5 kHz, nothing above 5.5 kHz.
    const auto recorded = tone(11025, 4500.0, 2.0);
    const auto lifted = viewer::lift_squeal(*recorded);
    check(lifted && lifted->sample_rate == 44100 && lifted->channels == 1, "lifted to 44.1 kHz mono");
    if (lifted) check(share_above(*lifted, 5600.0) > 0.05, "its highs remade above the old limit");

    // A train of three braked cars slowing from 25 km/h to a stop.
    audio::AudioEngine engine({.device = false, .channels = 2, .sample_rate = 22050});
    const auto moan = tone(44100, 700.0, 6.0);
    const auto squeak = tone(44100, 4450.0, 2.0);
    viewer::TrainExtraSounds sounds([&](const core::VirtualPath& path) -> std::shared_ptr<const audio::AudioClip> {
        const auto name = path.str();
        if (name.find("stop_moan") != std::string::npos) return moan;
        if (name.find("stop_squeak") != std::string::npos) return squeak;
        if (name.find("x_uk_brakesqueal02a") != std::string::npos) return recorded;
        return nullptr;
    });
    engine.set_listener({0, 0, 0}, {0, 0, 1});
    const float dt = 1.0f / 30.0f;
    float speed = 25.0f / 3.6f;
    const float decel = 0.6f;
    bool squealed = false;
    std::vector<float> mix(2 * 22050 / 30 + 8);
    std::size_t voices_after_stop = 99;
    for (int frame = 0; frame < 30 * 20; ++frame) {
        viewer::ExtraSoundTrain train;
        train.id = 7;
        train.speed = speed;
        train.deceleration = speed > 0.0f ? decel : 0.0f;
        for (int c = 0; c < 3; ++c) train.cars.push_back({.position = {5.0f, 0.0f, 15.0f * c}, .speed = speed, .cylinder = 3.0f});
        sounds.update(dt, std::span(&train, 1), {0, 0, 0}, 1.0f, true, engine);
        engine.render(mix.data(), 22050 / 30);
        engine.update();
        const auto heard = engine.audible_voices();
        if (speed > 0.0f && speed * 3.6f < 20.0f && heard > 0) squealed = true;
        speed = (std::max)(0.0f, speed - decel * dt);
        if (speed == 0.0f && frame % 30 == 0) voices_after_stop = engine.active_voices();
    }
    check(squealed, "the cars squeal slowing below 20 km/h");
    check(voices_after_stop == 0, "standing, every voice let go");
    check(sounds.squeal_source() == "SOUND/x_uk_brakesqueal02a.wav", "the content's squeal taken");

    // No recording at all: silent, no made squeal.
    {
        audio::AudioEngine e({.device = false, .channels = 2, .sample_rate = 22050});
        viewer::TrainExtraSounds none([](const core::VirtualPath&) -> std::shared_ptr<const audio::AudioClip> { return nullptr; });
        std::vector<float> buffer(2 * 22050 / 30 + 8);
        std::size_t most = 0;
        for (int frame = 0; frame < 30 * 5; ++frame) {
            viewer::ExtraSoundTrain train;
            train.id = 2;
            train.speed = 8.0f / 3.6f;
            train.deceleration = 0.3f;
            train.cars.push_back({.position = {5.0f, 0.0f, 0.0f}, .speed = train.speed, .cylinder = 3.0f});
            none.update(dt, std::span(&train, 1), {0, 0, 0}, 1.0f, true, e);
            e.render(buffer.data(), 22050 / 30);
            most = (std::max)(most, e.active_voices());
        }
        check(most == 0 && none.squeal_source() == "none", "without a recording, no squeal");
    }

    // The moan alone: begun when the stop is its length away.
    {
        audio::AudioEngine e({.device = false, .channels = 2, .sample_rate = 22050});
        viewer::TrainExtraSounds only_moan([&](const core::VirtualPath& path) -> std::shared_ptr<const audio::AudioClip> {
            return path.str().find("stop_moan") != std::string::npos ? moan : nullptr;
        });
        float v = 15.0f / 3.6f;
        float began_at = -1.0f;
        std::vector<float> buffer(2 * 22050 / 30 + 8);
        for (int frame = 0; frame < 30 * 15 && v > 0.0f; ++frame) {
            viewer::ExtraSoundTrain train;
            train.id = 1;
            train.speed = v;
            train.deceleration = 0.5f;
            train.cars.push_back({.position = {5.0f, 0.0f, 0.0f}, .speed = v, .cylinder = 3.0f});
            const auto before = e.active_voices();
            only_moan.update(dt, std::span(&train, 1), {0, 0, 0}, 1.0f, true, e);
            // No recorded squeal here: the moan is the only voice.
            if (began_at < 0.0f && e.active_voices() > before && v / 0.5f <= 6.05f) began_at = v / 0.5f;
            e.render(buffer.data(), 22050 / 30);
            e.update();
            v = (std::max)(0.0f, v - 0.5f * dt);
        }
        check(began_at > 5.5f && began_at <= 6.05f, "the moan begins its length before the stop");
    }
    if (failures == 0) std::cout << "brake squeal: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
