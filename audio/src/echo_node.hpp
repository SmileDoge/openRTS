#pragma once

// openRTS (beyond RTS, the "Echo" sound setting): a small reverberation for
// the sounds outside, by how closed in the listener is (a tunnel, a
// cutting, a platform's roof). A miniaudio node between the outside voices'
// group and the output: the sound passes through as it is, the
// reverberation added (AudioEngine).
//
// Schroeder's form as in Freeverb: four damped feedback combs in parallel,
// two allpasses after them, each channel its own lengths (the right a little
// longer, so the tail is wide); a pre-delay before them, longer in a long
// enclosure (the walls further off along it).

#include <atomic>
#include <cstdint>
#include <vector>

#include "miniaudio_config.hpp"

namespace openrts::audio {

class EchoNode {
public:
    EchoNode() = default;
    EchoNode(const EchoNode&) = delete;
    EchoNode& operator=(const EchoNode&) = delete;
    ~EchoNode();

    // Into `graph`, its output to `out`; `channels` in and out.
    bool init(ma_node_graph* graph, ma_node* out, std::uint32_t channels, std::uint32_t sample_rate);
    [[nodiscard]] ma_node* node() noexcept { return initialized_ ? &body_.base : nullptr; }
    // 0: none (the sound passes as it is once the tail has died out), 1: deep in
    // a tunnel. Read by the audio thread; changes are eased there.
    void set_amount(float amount) noexcept { amount_.store(amount, std::memory_order_relaxed); }
    [[nodiscard]] float amount() const noexcept { return amount_.load(std::memory_order_relaxed); }

    // The processing on interleaved frames, `channels` wide (the node's
    // callback; tests call it directly).
    void process(const float* in, float* out, std::uint32_t frames);

private:
    struct Comb {
        std::vector<float> buffer;
        std::size_t at = 0;
        float store = 0.0f;
    };
    struct Allpass {
        std::vector<float> buffer;
        std::size_t at = 0;
    };
    struct Channel {
        Comb combs[4];
        Allpass allpasses[2];
    };
    struct Body {
        ma_node_base base; // first: miniaudio's node
        EchoNode* self = nullptr;
    };
    static void on_process(ma_node* node, const float** in, ma_uint32* frames_in, float** out, ma_uint32* frames_out);

    Body body_{};
    bool initialized_ = false;
    std::uint32_t channels_ = 2;
    std::uint32_t sample_rate_ = 48000;
    std::vector<Channel> state_;
    std::vector<float> predelay_; // mono, the longest pre-delay
    std::size_t predelay_at_ = 0;
    std::atomic<float> amount_{0.0f};
    float eased_ = 0.0f; // the audio thread's amount
    bool silent_ = true; // the tail has died out: nothing to compute
    std::uint32_t quiet_frames_ = 0;
};

} // namespace openrts::audio
