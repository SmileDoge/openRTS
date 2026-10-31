#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <vector>

#include <glm/vec3.hpp>

#include <openrts/audio/sound_bank.hpp>
#include <openrts/formats/sound.hpp>

namespace openrts::audio {

using VoiceId = std::uint32_t;

enum class VoicePlayback : std::uint8_t {
    once,        // play to the end
    loop,        // loop the whole clip
    loop_region, // play into the clip's loop region and loop it (whole clip when it has none)
};

// Where sound scripts send their voices: the miniaudio engine, or a recorder
// in tests. Positions are in scene coordinates.
class VoiceSink {
public:
    virtual ~VoiceSink() = default;
    // Returns 0 when the voice could not be started.
    virtual VoiceId play(std::shared_ptr<const AudioClip> clip, VoicePlayback playback, bool spatial) = 0;
    // Leaves the loop: the voice plays to the end of its clip and finishes.
    virtual void release(VoiceId voice) = 0;
    virtual void stop(VoiceId voice) = 0;
    // volume 0..; pitch 1 = the clip's own rate.
    virtual void set(VoiceId voice, float volume, float pitch, const glm::vec3& position) = 0;
    [[nodiscard]] virtual bool playing(VoiceId voice) const = 0;
};

enum class SoundCamera : std::uint8_t { external, cab, passenger };

// What a sound object reacts to in one update.
struct SoundInputs {
    std::array<float, formats::sound_variable_count> variables{};
    SoundCamera camera = SoundCamera::external;
    glm::vec3 position{}; // of the sound source, scene coordinates
    std::span<const std::uint32_t> discrete;    // trigger codes since the last update
    std::span<const formats::SoundEvent> events; // events since the last update

    [[nodiscard]] float& operator[](formats::SoundVariable variable) {
        return variables[static_cast<std::size_t>(variable)];
    }
};

// Runs one ScalabiltyGroup of a sound script: activation, triggers, curves,
// loops and one-shots. Semantics: docs/sound.md.
class SoundInstance {
public:
    using ClipLookup = std::function<std::shared_ptr<const AudioClip>(const std::string& file)>;

    SoundInstance(std::shared_ptr<const formats::SoundScript> script, const formats::SoundGroup& group,
                  formats::SoundOwner owner, ClipLookup clips, std::uint32_t seed);
    SoundInstance(const SoundInstance&) = delete;
    SoundInstance& operator=(const SoundInstance&) = delete;
    SoundInstance(SoundInstance&&) noexcept;
    SoundInstance& operator=(SoundInstance&&) noexcept;
    ~SoundInstance();

    void update(float dt, const SoundInputs& inputs, VoiceSink& sink);
    // Stops every voice (deactivation, unloading).
    void stop(VoiceSink& sink);

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] std::size_t voices() const noexcept;
    [[nodiscard]] const formats::SoundGroup& group() const noexcept { return *group_; }

private:
    struct Stream;
    void activate(const SoundInputs& inputs, VoiceSink& sink);
    void fire(Stream& stream, std::size_t trigger, float volume, const SoundInputs& inputs, VoiceSink& sink);
    [[nodiscard]] bool camera_listed(const formats::SoundActivation& activation, SoundCamera camera) const;

    std::shared_ptr<const formats::SoundScript> script_;
    const formats::SoundGroup* group_;
    ClipLookup clips_;
    std::mt19937 random_;
    std::vector<Stream> streams_;
    formats::SoundOwner owner_;
    std::array<float, formats::sound_variable_count> previous_{};
    bool has_previous_ = false;
    std::vector<bool> pair_on_; // RTS DiscreteTriggerPairs state
    std::vector<std::uint32_t> posted_; // PostTrigger codes for the next update
    // SetVariableValue / IncreaseVariableValue: replace the input from then on.
    std::array<std::optional<float>, formats::sound_variable_count> overrides_{};
    bool active_ = false;
};

// Value of a curve at `x`: linear between points, clamped at the ends.
[[nodiscard]] float evaluate_curve(const formats::SoundCurve& curve, float x);

} // namespace openrts::audio
