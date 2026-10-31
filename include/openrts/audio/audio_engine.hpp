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
    std::uint32_t max_voices = 256;
    // Without a device the engine is rendered with render(): tests, offline mixing.
    bool device = true;
    std::uint32_t channels = 2;       // offline only
    std::uint32_t sample_rate = 48000; // offline only
};

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
    // Releases finished voices; call once per frame.
    void update();
    [[nodiscard]] std::size_t active_voices() const noexcept;
    // Offline engines: mixes `frames` interleaved float frames into `out`.
    void render(float* out, std::uint64_t frames);

    VoiceId play(std::shared_ptr<const AudioClip> clip, VoicePlayback playback, bool spatial) override;
    void release(VoiceId voice) override;
    void stop(VoiceId voice) override;
    void set(VoiceId voice, float volume, float pitch, const glm::vec3& position) override;
    [[nodiscard]] bool playing(VoiceId voice) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openrts::audio
