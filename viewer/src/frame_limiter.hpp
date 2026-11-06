#pragma once

// The frame limit (the FPS limit setting): the window loop waits at the start
// of a frame until its time comes. A high-resolution waitable timer on
// Windows (the last fraction of a millisecond spun), sleeps elsewhere. A
// frame that comes late does not make the next ones hurry.

#include <chrono>

namespace openrts::viewer {

// The setting's range; 0 is "no limit".
inline constexpr int min_fps_limit = 30;
inline constexpr int max_fps_limit = 500;

// A remembered or given limit as the setting takes it: 0, or 30..500.
[[nodiscard]] int clamp_fps_limit(int fps) noexcept;

class FrameLimiter {
public:
    FrameLimiter();
    ~FrameLimiter();
    FrameLimiter(const FrameLimiter&) = delete;
    FrameLimiter& operator=(const FrameLimiter&) = delete;

    // Frames per second; 0: no limit.
    void set(int fps) noexcept;
    // Waits until the next frame is due.
    void wait();

private:
    using Clock = std::chrono::steady_clock;
    void sleep_until(Clock::time_point when);

    Clock::duration period_{};
    Clock::time_point next_{};
    void* timer_ = nullptr; // a Windows waitable timer
};

} // namespace openrts::viewer
