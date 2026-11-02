#include <openrts/audio/audio_engine.hpp>

#include <unordered_map>

#include "miniaudio_config.hpp"

namespace openrts::audio {

struct AudioEngine::Impl {
    struct Voice {
        std::shared_ptr<const AudioClip> clip;
        ma_audio_buffer buffer{};
        ma_sound sound{};
    };

    ma_engine engine{};
    bool initialized = false;
    std::string error;
    std::uint32_t max_voices = 256;
    VoiceId next_id = 1;
    std::unordered_map<VoiceId, std::unique_ptr<Voice>> voices;

    void destroy(Voice& voice) {
        ma_sound_uninit(&voice.sound);
        ma_audio_buffer_uninit(&voice.buffer);
    }
};

AudioEngine::AudioEngine(const AudioEngineOptions& options) : impl_(std::make_unique<Impl>()) {
    impl_->max_voices = options.max_voices;
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
    for (auto it = impl_->voices.begin(); it != impl_->voices.end();) {
        if (ma_sound_at_end(&it->second->sound)) {
            impl_->destroy(*it->second);
            it = impl_->voices.erase(it);
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

VoiceId AudioEngine::play(std::shared_ptr<const AudioClip> clip, const VoicePlayback playback, const VoiceSpace space) {
    if (!ok() || !clip || clip->frames == 0 || impl_->voices.size() >= impl_->max_voices) return 0;
    auto voice = std::make_unique<Impl::Voice>();
    voice->clip = std::move(clip);
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
    return id;
}

void AudioEngine::release(const VoiceId id) {
    if (const auto it = impl_->voices.find(id); it != impl_->voices.end()) {
        ma_sound_set_looping(&it->second->sound, false);
    }
}

void AudioEngine::stop(const VoiceId id) {
    if (const auto it = impl_->voices.find(id); it != impl_->voices.end()) {
        impl_->destroy(*it->second);
        impl_->voices.erase(it);
    }
}

void AudioEngine::set(const VoiceId id, const float volume, const float pitch, const glm::vec3& position) {
    const auto it = impl_->voices.find(id);
    if (it == impl_->voices.end()) return;
    auto& sound = it->second->sound;
    ma_sound_set_volume(&sound, volume);
    ma_sound_set_pitch(&sound, pitch);
    ma_sound_set_position(&sound, -position.x, position.y, position.z);
}

void AudioEngine::seek(const VoiceId id, const std::uint64_t frame) {
    const auto it = impl_->voices.find(id);
    if (it == impl_->voices.end() || !it->second->clip || it->second->clip->frames == 0) return;
    ma_sound_seek_to_pcm_frame(&it->second->sound, frame % it->second->clip->frames);
}

bool AudioEngine::playing(const VoiceId id) const {
    const auto it = impl_->voices.find(id);
    return it != impl_->voices.end() && !ma_sound_at_end(&it->second->sound);
}

} // namespace openrts::audio
