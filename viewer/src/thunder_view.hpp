#pragma once

// A thunderstorm's thunder (docs/weather.md, "Thunder"): each strike heard
// distance / 343 m/s after its light, a clap for its distance (near within
// 1.5 km, middle to 5 km, far beyond) from SOUND/OPENRTS/THUNDER (openRTS's
// own pack openrts-thunder; a mod may replace or add claps:
// thunder_near_*.wav, thunder_mid_*.wav, thunder_far_*.wav), the one picked
// by the strike's seed, so every player of a game hears the same; panned
// from where it struck.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/vec3.hpp>

#include <openrts/audio/audio_engine.hpp>
#include <openrts/audio/sound_bank.hpp>
#include <openrts/core/vfs.hpp>
#include <openrts/renderer/lightning.hpp>

namespace openrts::viewer {

// The thunder's volume (0..2, 1 as made). Read each frame.
inline float& thunder_volume() {
    static float volume = 1.0f;
    return volume;
}

class ThunderView {
public:
    explicit ThunderView(std::shared_ptr<const core::Vfs> vfs) : vfs_(std::move(vfs)) {}

    // A strike seen: its thunder waits for its time (each strike once).
    // `distance` from the eye (m), `at` where it struck (scene coordinates).
    void strike(const renderer::LightningStrike& strike, float distance, const glm::vec3& at);
    // The claps due at `time` (s of the day) started; the playing ones at
    // `gain` (the share of outside sound reaching the listener: the cab's walls).
    void update(double time, float gain, audio::AudioEngine* audio);
    void stop(audio::AudioEngine& audio);
    [[nodiscard]] std::size_t waiting() const noexcept { return waiting_.size(); }
    // The claps loaded (near, middle, far); none before the first strike.
    [[nodiscard]] std::array<std::size_t, 3> claps() const noexcept {
        return {clips_[0].size(), clips_[1].size(), clips_[2].size()};
    }

    // A strike's clap kind (0 near, 1 middle, 2 far) and its volume (before
    // thunder_volume and the listener's gain).
    [[nodiscard]] static int kind_for(float distance, bool to_ground) noexcept;
    [[nodiscard]] static float volume_for(float distance, float strength, bool to_ground) noexcept;

private:
    struct Waiting {
        std::uint64_t id{};
        double start{};    // s of the day the strike began
        double delay{};    // s until it is heard
        float distance{};
        glm::vec3 at{};
        std::uint32_t seed{};
        float strength = 1.0f;
        bool to_ground = true;
    };
    struct Playing {
        audio::VoiceId voice{};
        float volume{};
        glm::vec3 at{};
        float pitch = 1.0f;
    };
    void load();

    std::shared_ptr<const core::Vfs> vfs_;
    bool loaded_ = false;
    std::array<std::vector<std::shared_ptr<const audio::AudioClip>>, 3> clips_;
    std::array<std::vector<std::string>, 3> names_;
    std::vector<Waiting> waiting_;
    std::unordered_map<std::uint64_t, double> heard_; // strike -> its start, a while after
    std::vector<Playing> playing_;
};

} // namespace openrts::viewer
