#include <openrts/audio/audio_engine.hpp>

#include <openrts/core/profile.hpp>

#include <algorithm>
#include <map>
#include <unordered_map>

#include "miniaudio_config.hpp"

namespace openrts::audio {

struct AudioEngine::Impl {
    struct Voice {
        std::shared_ptr<const AudioClip> clip;
        ma_audio_buffer buffer{};
        ma_sound sound{};
        bool loops = false; // looping, not released
        bool held = false;  // stopped while silent (set)
    };

    ma_engine engine{};
    bool initialized = false;
    std::string error;
    std::uint32_t max_voices = 1024;
    std::uint32_t max_held_voices = 8192;
    std::size_t audible = 0; // voices not held
    VoiceId next_id = 1;
    std::unordered_map<VoiceId, std::unique_ptr<Voice>> voices;
    // Mono copies of stereo clips for the voices placed in space, by clip.
    std::map<const AudioClip*, std::pair<std::weak_ptr<const AudioClip>, std::shared_ptr<const AudioClip>>> mono;

    std::shared_ptr<const AudioClip> mono_of(const std::shared_ptr<const AudioClip>& clip) {
        if (!clip || clip->channels <= 1) return clip;
        auto& entry = mono[clip.get()];
        if (entry.second && !entry.first.expired()) return entry.second;
        auto copy = std::make_shared<AudioClip>();
        copy->channels = 1;
        copy->sample_rate = clip->sample_rate;
        copy->frames = clip->frames;
        copy->loop_start = clip->loop_start;
        copy->loop_end = clip->loop_end;
        copy->samples.resize(static_cast<std::size_t>(clip->frames));
        const auto channels = clip->channels;
        for (std::size_t f = 0; f < copy->samples.size(); ++f) {
            float sum = 0.0f;
            for (std::uint32_t c = 0; c < channels; ++c) sum += clip->samples[f * channels + c];
            copy->samples[f] = sum / static_cast<float>(channels);
        }
        entry = {clip, copy};
        // Forget the copies of clips no longer held.
        std::erase_if(mono, [](const auto& item) { return item.second.first.expired(); });
        return copy;
    }

    void destroy(Voice& voice) {
        ma_sound_uninit(&voice.sound);
        ma_audio_buffer_uninit(&voice.buffer);
    }

    // Destroys a voice and frees its place.
    std::unordered_map<VoiceId, std::unique_ptr<Voice>>::iterator remove(std::unordered_map<VoiceId, std::unique_ptr<Voice>>::iterator it) {
        if (!it->second->held) --audible;
        destroy(*it->second);
        return voices.erase(it);
    }
};

AudioEngine::AudioEngine(const AudioEngineOptions& options) : impl_(std::make_unique<Impl>()) {
    impl_->max_voices = options.max_voices;
    impl_->max_held_voices = (std::max)(options.max_held_voices, options.max_voices);
    auto config = ma_engine_config_init();
    config.listenerCount = 1;
    if (!options.device) {
        config.noDevice = MA_TRUE;
        config.channels = options.channels;
        config.sampleRate = options.sample_rate;
    }
    if (ma_engine_init(&config, &impl_->engine) != MA_SUCCESS) {
        impl_->error = "no audio playback device";
        return;
    }
    impl_->initialized = true;
    ma_engine_listener_set_world_up(&impl_->engine, 0, 0.0f, 1.0f, 0.0f);
}

AudioEngine::~AudioEngine() {
    if (!impl_->initialized) return;
    for (auto& [_, voice] : impl_->voices) impl_->destroy(*voice);
    impl_->voices.clear();
    ma_engine_uninit(&impl_->engine);
}

bool AudioEngine::ok() const noexcept {
    return impl_->initialized;
}

const std::string& AudioEngine::error() const noexcept {
    return impl_->error;
}

void AudioEngine::set_listener(const glm::vec3& position, const glm::vec3& forward) {
    if (!ok()) return;
    ma_engine_listener_set_position(&impl_->engine, 0, -position.x, position.y, position.z);
    ma_engine_listener_set_direction(&impl_->engine, 0, -forward.x, forward.y, forward.z);
}

void AudioEngine::set_paused(const bool paused) {
    if (!ok() || paused == paused_) return;
    paused_ = paused;
    if (paused) ma_engine_stop(&impl_->engine);
    else ma_engine_start(&impl_->engine);
}

bool AudioEngine::paused() const noexcept { return paused_; }

void AudioEngine::set_volume(const float volume) {
    if (ok()) ma_engine_set_volume(&impl_->engine, volume);
}

float AudioEngine::volume() const noexcept {
    return ok() ? ma_engine_get_volume(&impl_->engine) : 0.0f;
}

void AudioEngine::update() {
    OPENRTS_ZONE("audio");
    for (auto it = impl_->voices.begin(); it != impl_->voices.end();) {
        if (ma_sound_at_end(&it->second->sound)) {
            it = impl_->remove(it);
        } else {
            ++it;
        }
    }
}

void AudioEngine::render(float* out, const std::uint64_t frames) {
    if (ok()) ma_engine_read_pcm_frames(&impl_->engine, out, frames, nullptr);
}

std::size_t AudioEngine::active_voices() const noexcept {
    return impl_->voices.size();
}

std::size_t AudioEngine::audible_voices() const noexcept {
    return impl_->audible;
}

VoiceId AudioEngine::play(std::shared_ptr<const AudioClip> clip, const VoicePlayback playback, const VoiceSpace space) {
    // The places are the heard voices' (held silent ones take none).
    if (!ok() || !clip || clip->frames == 0 || impl_->audible >= impl_->max_voices || impl_->voices.size() >= impl_->max_held_voices) {
        return 0;
    }
    auto voice = std::make_unique<Impl::Voice>();
    // A voice placed in space plays a stereo file as mono (openRTS: RTS
    // places no sound at all, docs/sound.md); miniaudio pans a stereo
    // source's channels only weakly, and a crossing's stereo bell sounded
    // as if inside the head.
    voice->clip = space == VoiceSpace::flat ? std::move(clip) : impl_->mono_of(clip);
    const auto& data = *voice->clip;
    auto config = ma_audio_buffer_config_init(ma_format_f32, data.channels, data.frames, data.samples.data(), nullptr);
    config.sampleRate = data.sample_rate;
    if (ma_audio_buffer_init(&config, &voice->buffer) != MA_SUCCESS) return 0;
    if (playback == VoicePlayback::loop_region && data.has_loop()) {
        ma_data_source_set_loop_point_in_pcm_frames(&voice->buffer, data.loop_start, data.loop_end);
    }
    if (ma_sound_init_from_data_source(&impl_->engine, &voice->buffer, 0, nullptr, &voice->sound) != MA_SUCCESS) {
        ma_audio_buffer_uninit(&voice->buffer);
        return 0;
    }
    ma_sound_set_looping(&voice->sound, playback != VoicePlayback::once);
    voice->loops = playback != VoicePlayback::once;
    ma_sound_set_spatialization_enabled(&voice->sound, space != VoiceSpace::flat);
    // Panned voices: a model with zero rolloff, no distance attenuation (the
    // scripts' curves set the volume) but still panning;
    // ma_attenuation_model_none would skip the spatializer altogether.
    // Positional voices: inverse distance from positional_reference_distance.
    ma_sound_set_attenuation_model(&voice->sound, ma_attenuation_model_inverse);
    if (space == VoiceSpace::positional) {
        ma_sound_set_rolloff(&voice->sound, 1.0f);
        ma_sound_set_min_distance(&voice->sound, positional_reference_distance);
    } else {
        ma_sound_set_rolloff(&voice->sound, 0.0f);
    }
    ma_sound_set_volume(&voice->sound, 0.0f);
    ma_sound_start(&voice->sound);
    const auto id = impl_->next_id++;
    impl_->voices.emplace(id, std::move(voice));
    ++impl_->audible;
    return id;
}

void AudioEngine::release(const VoiceId id) {
    if (const auto it = impl_->voices.find(id); it != impl_->voices.end()) {
        // A held loop would wait for ever to play out, unheard: it ends now.
        if (it->second->held) {
            impl_->remove(it);
            return;
        }
        ma_sound_set_looping(&it->second->sound, false);
        it->second->loops = false;
    }
}

void AudioEngine::stop(const VoiceId id) {
    if (const auto it = impl_->voices.find(id); it != impl_->voices.end()) impl_->remove(it);
}

void AudioEngine::set(const VoiceId id, const float volume, const float pitch, const glm::vec3& position) {
    const auto it = impl_->voices.find(id);
    if (it == impl_->voices.end()) return;
    auto& voice = *it->second;
    auto& sound = voice.sound;
    const bool silent = volume <= 1e-4f;
    if (voice.loops && silent && !voice.held) {
        ma_sound_stop(&sound); // held at its frame, not rewound
        voice.held = true;
        --impl_->audible;
    } else if (voice.held && !silent && impl_->audible < impl_->max_voices) {
        ma_sound_start(&sound);
        voice.held = false;
        ++impl_->audible;
    }
    ma_sound_set_volume(&sound, volume);
    ma_sound_set_pitch(&sound, pitch);
    ma_sound_set_position(&sound, -position.x, position.y, position.z);
}

void AudioEngine::seek(const VoiceId id, const std::uint64_t frame) {
    const auto it = impl_->voices.find(id);
    if (it == impl_->voices.end() || !it->second->clip || it->second->clip->frames == 0) return;
    ma_sound_seek_to_pcm_frame(&it->second->sound, frame % it->second->clip->frames);
}

std::optional<std::uint64_t> AudioEngine::cursor(const VoiceId id) const {
    const auto it = impl_->voices.find(id);
    if (it == impl_->voices.end()) return std::nullopt;
    ma_uint64 frame = 0;
    if (ma_sound_get_cursor_in_pcm_frames(&it->second->sound, &frame) != MA_SUCCESS) return std::nullopt;
    return static_cast<std::uint64_t>(frame);
}

bool AudioEngine::playing(const VoiceId id) const {
    const auto it = impl_->voices.find(id);
    return it != impl_->voices.end() && !ma_sound_at_end(&it->second->sound);
}

} // namespace openrts::audio
