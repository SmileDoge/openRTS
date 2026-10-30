#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>

namespace openrts::renderer {

// Deferred release of GPU resources (docs/vfs.md, "Safe GPU release").
// A resource retired while frame N is recorded may still be referenced by the
// command buffers of frame N and earlier; it is freed only after the GPU has
// finished frame N and no CPU job holds it any more. Backend-independent: the
// renderer reports submitted and completed frames.
class GpuRetireQueue {
public:
    // Frame currently being recorded (first frame is 1).
    [[nodiscard]] std::uint64_t recording_frame() const noexcept { return recording_; }
    [[nodiscard]] std::uint64_t completed_frame() const noexcept { return completed_; }

    void retire(std::shared_ptr<const void> resource) {
        if (!resource) return;
        const std::scoped_lock lock(mutex_);
        retired_.emplace_back(recording_, std::move(resource));
    }

    // Closes the recording frame; returns its number for the completion
    // notification.
    std::uint64_t submit() noexcept { return recording_++; }

    // The GPU finished `frame` (and everything before it). May be called from
    // a backend callback thread.
    void complete(const std::uint64_t frame) noexcept {
        auto current = completed_.load();
        while (frame > current && !completed_.compare_exchange_weak(current, frame)) {
        }
    }

    // Frees what is safe to free; returns how many resources were released.
    std::size_t collect() {
        const auto done = completed_.load();
        std::deque<std::pair<std::uint64_t, std::shared_ptr<const void>>> released;
        {
            const std::scoped_lock lock(mutex_);
            std::deque<std::pair<std::uint64_t, std::shared_ptr<const void>>> keep;
            for (auto& item : retired_) {
                const bool safe = item.first <= done && item.second.use_count() == 1;
                (safe ? released : keep).push_back(std::move(item));
            }
            retired_.swap(keep);
        }
        return released.size(); // destructors run here, outside the lock
    }

    // Releases everything; only valid once the device is idle.
    void drain() {
        const std::scoped_lock lock(mutex_);
        retired_.clear();
    }

    [[nodiscard]] std::size_t pending() const {
        const std::scoped_lock lock(mutex_);
        return retired_.size();
    }

private:
    mutable std::mutex mutex_;
    std::deque<std::pair<std::uint64_t, std::shared_ptr<const void>>> retired_;
    std::uint64_t recording_ = 1;
    std::atomic<std::uint64_t> completed_{0};
};

} // namespace openrts::renderer
