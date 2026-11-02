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

// How a voice is placed: flat (weather, environment), panned by direction
// only (world sounds: their scripts carry distance curves), or positional
// with distance rolloff (rolling stock: scripts rely on the engine's 3D
// attenuation, as RTS with DirectSound 3D buffers).
enum class VoiceSpace : std::uint8_t { flat, panned, positional };

// Where sound scripts send their voices: the miniaudio engine, or a recorder
// in tests. Positions are in scene coordinates.
class VoiceSink {
public:
    virtual ~VoiceSink() = default;
    // Returns 0 when the voice could not be started.
    virtual VoiceId play(std::shared_ptr<const AudioClip> clip, VoicePlayback playback, VoiceSpace space) = 0;
    // Leaves the loop: the voice plays to the end of its clip and finishes.
    virtual void release(VoiceId voice) = 0;
    virtual void stop(VoiceId voice) = 0;
    // volume 0..; pitch 1 = the clip's own rate.
    virtual void set(VoiceId voice, float volume, float pitch, const glm::vec3& position) = 0;
    [[nodiscard]] virtual bool playing(VoiceId voice) const = 0;
    // Moves a voice to `frame` of its clip (a looped voice starting
    // elsewhere than at its beginning).
    virtual void seek(VoiceId voice, std::uint64_t frame) {
        (void)voice;
        (void)frame;
    }
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
    // Stops every voice (deactivation, unloading). As RTS (0x4A9CF4), the
    // loop each stream was playing is remembered with its position and
    // starts again there when the group activates again (0x4A9BA8).
    void stop(VoiceSink& sink);
    // Fetches every clip the group can play, so the first update does not
    // decode files while voices already started elsewhere keep playing (one
    // car of a train running ahead of the others).
    void preload();
    // StartLoop voices (whole-clip loops) begin at a random frame instead of
    // the first: the same loop on many cars of a train does not add up in
    // phase (one loud point, comb filtering). openRTS choice; RTS starts its
    // buffers at 0 (PROBABLE, docs/sound.md).
    void set_random_loop_phase(bool on) noexcept { random_loop_phase_ = on; }

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
    bool activated_once_ = false; // Initial_Triggers fire on the first activation only
    // Discrete codes that came while inactive: RTS keeps them in the object's
    // 96-entry queue and runs them at its next active update.
    std::vector<std::uint32_t> queued_;
    bool random_loop_phase_ = false;
};

// Value of a curve at `x`: linear between points, clamped at the ends.
[[nodiscard]] float evaluate_curve(const formats::SoundCurve& curve, float x);

} // namespace openrts::audio
