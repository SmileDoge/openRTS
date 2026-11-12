#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <glm/vec3.hpp>

#include <openrts/audio/sound_instance.hpp>

namespace openrts::audio {

// Sound output through a miniaudio engine on the default playback device.
// Each voice is a miniaudio sound node reading a decoded clip in place; the
// node graph is where later effects (spatial audio) attach.
//
// Scene coordinates are MSTS (left-handed); positions are mirrored in X for
// miniaudio's right-handed space, as the renderer mirrors the camera.
// Volume follows the scripts' curves only: miniaudio distance attenuation is
// off, spatialisation gives direction.
struct AudioEngineOptions {
    // Voices heard at once. The silent ones held (AudioEngine::set) take no
    // place: a big station's standing cars hold hundreds of looping voices
    // within their scripts' 500 m, unheard.
    std::uint32_t max_voices = 1024;
    // All voices, held ones too: only against voices never let go.
    std::uint32_t max_held_voices = 8192;
    // Without a device the engine is rendered with render(): tests, offline mixing.
    bool device = true;
    std::uint32_t channels = 2;       // offline only
    std::uint32_t sample_rate = 48000; // offline only
};

// Distance (m) up to which positional voices play at full volume; beyond it
// they fall off as reference / distance. openRTS choice: RTS places no sound
// (docs/sound.md), its rolling stock fades only by its scripts' curves.
inline constexpr float positional_reference_distance = 15.0f;

// How far a voice placed in space is panned (openRTS: RTS pans nothing):
// straight ahead or behind both sides at full level, as RTS plays every
// sound; fully to one side the other side at 1 - pan_depth.
inline constexpr float pan_depth = 0.7f;

class AudioEngine final : public VoiceSink {
public:
    explicit AudioEngine(const AudioEngineOptions& options = {});
    ~AudioEngine() override;
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    [[nodiscard]] bool ok() const noexcept;
    [[nodiscard]] const std::string& error() const noexcept;

    void set_listener(const glm::vec3& position, const glm::vec3& forward);
    void set_volume(float volume);
    [[nodiscard]] float volume() const noexcept;
    // Stops the output (every voice holds where it is) or starts it again.
    void set_paused(bool paused);
    [[nodiscard]] bool paused() const noexcept;
    // Releases finished voices; call once per frame.
    void update();
    void seek(VoiceId voice, std::uint64_t frame) override;
    [[nodiscard]] std::optional<std::uint64_t> cursor(VoiceId voice) const override;
    [[nodiscard]] std::size_t active_voices() const noexcept;
    // The voices the mixer plays (not held silent): max_voices of them.
    [[nodiscard]] std::size_t audible_voices() const noexcept;
    // Offline engines: mixes `frames` interleaved float frames into `out`.
    void render(float* out, std::uint64_t frames);

    VoiceId play(std::shared_ptr<const AudioClip> clip, VoicePlayback playback, VoiceSpace space) override;
    void release(VoiceId voice) override;
    void stop(VoiceId voice) override;
    // A looping voice set silent is held where it is (the mixer skips it:
    // silent voices had cost as much as playing ones; nor does it take a
    // place of max_voices) and goes on from there once it is heard again,
    // when a place is free (else it waits, silent); a one-shot plays on.
    void set(VoiceId voice, float volume, float pitch, const glm::vec3& position) override;
    [[nodiscard]] bool playing(VoiceId voice) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool paused_ = false;
};

} // namespace openrts::audio
