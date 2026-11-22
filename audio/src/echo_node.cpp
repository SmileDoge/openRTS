#include "echo_node.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace openrts::audio {

namespace {

// Freeverb's lengths at 44.1 kHz, the right channel's 23 samples longer.
constexpr std::uint32_t comb_lengths[4] = {1116, 1188, 1277, 1356};
constexpr std::uint32_t allpass_lengths[2] = {556, 441};
constexpr std::uint32_t stereo_spread = 23;
constexpr float allpass_feedback = 0.5f;
constexpr float input_gain = 0.03f;   // four combs summed
constexpr float max_predelay = 0.06f; // s

std::size_t scaled(const std::uint32_t length, const std::uint32_t rate) {
    return (std::max)(std::size_t{1}, static_cast<std::size_t>(std::lround(static_cast<double>(length) * rate / 44100.0)));
}

} // namespace

void EchoNode::on_process(ma_node* node, const float** in, ma_uint32* frames_in, float** out, ma_uint32* frames_out) {
    (void)frames_in;
    auto* body = reinterpret_cast<Body*>(node); // its ma_node_base first
    body->self->process(in[0], out[0], *frames_out);
}

EchoNode::~EchoNode() {
    if (initialized_) ma_node_uninit(&body_.base, nullptr);
}

bool EchoNode::init(ma_node_graph* graph, ma_node* out, const std::uint32_t channels, const std::uint32_t sample_rate) {
    channels_ = (std::max)(channels, 1u);
    sample_rate_ = (std::max)(sample_rate, 8000u);
    state_.assign(channels_, {});
    for (std::uint32_t c = 0; c < channels_; ++c) {
        const std::uint32_t spread = (c % 2) * stereo_spread;
        for (int k = 0; k < 4; ++k) state_[c].combs[k].buffer.assign(scaled(comb_lengths[k] + spread, sample_rate_), 0.0f);
        for (int k = 0; k < 2; ++k) state_[c].allpasses[k].buffer.assign(scaled(allpass_lengths[k] + spread, sample_rate_), 0.0f);
    }
    predelay_.assign(static_cast<std::size_t>(max_predelay * sample_rate_) + 1, 0.0f);
    if (!graph || !out) return false;
    body_.self = this;
    static ma_node_vtable vtable = {&EchoNode::on_process, nullptr, 1, 1, 0};
    auto config = ma_node_config_init();
    config.vtable = &vtable;
    config.pInputChannels = &channels_;
    config.pOutputChannels = &channels_;
    if (ma_node_init(graph, &config, nullptr, &body_.base) != MA_SUCCESS) return false;
    initialized_ = true;
    ma_node_attach_output_bus(&body_.base, 0, out, 0);
    return true;
}

void EchoNode::process(const float* in, float* out, const std::uint32_t frames) {
    const std::size_t n = static_cast<std::size_t>(frames) * channels_;
    const float target = std::clamp(amount_.load(std::memory_order_relaxed), 0.0f, 1.0f);
    // About half a second to follow a change: in and out of a tunnel.
    const float ease = 1.0f - std::exp(-static_cast<float>(frames) / (0.5f * static_cast<float>(sample_rate_)));
    const float from = eased_;
    eased_ += (target - eased_) * ease;
    if (eased_ < 1e-3f && target < 1e-3f && silent_) {
        if (out != in) std::memcpy(out, in, n * sizeof(float));
        return;
    }
    // The tail longer and the walls further, the more closed in.
    const float a = (std::max)(eased_, target);
    const float feedback = 0.70f + 0.20f * a; // the combs' (the room size)
    const float damping = 0.45f - 0.2f * a;   // highs die out faster in the open
    constexpr float wet_scale = 0.55f; // at the amount 1
    const std::size_t predelay =
        (std::min)(predelay_.size() - 1, static_cast<std::size_t>((0.01f + 0.05f * a) * static_cast<float>(sample_rate_)));
    float loudest = 0.0f;
    for (std::uint32_t f = 0; f < frames; ++f) {
        // Mono into the pre-delay.
        float sum = 0.0f;
        for (std::uint32_t c = 0; c < channels_; ++c) sum += in[f * channels_ + c];
        predelay_[predelay_at_] = sum / static_cast<float>(channels_);
        const std::size_t read = (predelay_at_ + predelay_.size() - predelay) % predelay_.size();
        predelay_at_ = (predelay_at_ + 1) % predelay_.size();
        const float input = predelay_[read] * input_gain;
        float dry[8]{};
        for (std::uint32_t c = 0; c < channels_ && c < 8; ++c) dry[c] = in[f * channels_ + c];
        const float level = from + (eased_ - from) * (static_cast<float>(f) + 1.0f) / static_cast<float>(frames);
        for (std::uint32_t c = 0; c < channels_; ++c) {
            auto& ch = state_[c];
            float acc = 0.0f;
            for (auto& comb : ch.combs) {
                const float y = comb.buffer[comb.at];
                comb.store = y * (1.0f - damping) + comb.store * damping;
                comb.buffer[comb.at] = input + comb.store * feedback;
                comb.at = (comb.at + 1) % comb.buffer.size();
                acc += y;
            }
            for (auto& ap : ch.allpasses) {
                const float b = ap.buffer[ap.at];
                const float y = -acc + b;
                ap.buffer[ap.at] = acc + b * allpass_feedback;
                ap.at = (ap.at + 1) % ap.buffer.size();
                acc = y;
            }
            out[f * channels_ + c] = (c < 8 ? dry[c] : 0.0f) + acc * wet_scale * level;
            loudest = (std::max)(loudest, std::abs(acc));
        }
    }
    // Asleep once the tail is gone and nothing comes in.
    if (target < 1e-3f && loudest < 1e-5f) {
        quiet_frames_ += frames;
        if (quiet_frames_ > sample_rate_) silent_ = true;
    } else {
        quiet_frames_ = 0;
        silent_ = false;
    }
}

} // namespace openrts::audio
