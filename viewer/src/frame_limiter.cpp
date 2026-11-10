#include "frame_limiter.hpp"

#include <algorithm>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

namespace openrts::viewer {

int clamp_fps_limit(const int fps) noexcept {
    if (fps <= 0) return 0;
    if (fps > max_fps_limit) return 0;
    return (std::max)(fps, min_fps_limit);
}

int frame_limit(const int setting, const bool loading, const bool background) noexcept {
    if (loading) return 0;
    const int limit = clamp_fps_limit(setting);
    if (!background) return limit;
    return limit == 0 ? background_fps_limit : (std::min)(limit, background_fps_limit);
}

FrameLimiter::FrameLimiter() {
#ifdef _WIN32
    // Windows 10 1803 and later; without it the wait is spun.
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
#endif
}

FrameLimiter::~FrameLimiter() {
#ifdef _WIN32
    if (timer_) CloseHandle(timer_);
#endif
}

void FrameLimiter::set(const int fps) noexcept {
    const int limit = clamp_fps_limit(fps);
    const auto period = limit == 0 ? Clock::duration::zero()
                                   : std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / limit));
    if (period != period_) next_ = {};
    period_ = period;
}

void FrameLimiter::wait() {
    if (period_ == Clock::duration::zero()) return;
    const auto now = Clock::now();
    // Behind by more than a frame (a slow frame, the first one): no hurrying.
    if (next_ < now - period_) next_ = now;
    if (now < next_) sleep_until(next_);
    next_ += period_;
}

void FrameLimiter::sleep_until(const Clock::time_point when) {
    constexpr auto spun = std::chrono::microseconds(300);
#ifdef _WIN32
    if (timer_) {
        const auto left = when - Clock::now() - spun;
        if (left > Clock::duration::zero()) {
            LARGE_INTEGER due{};
            due.QuadPart = -static_cast<LONGLONG>(std::chrono::duration_cast<std::chrono::nanoseconds>(left).count() / 100);
            if (SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0)) WaitForSingleObject(timer_, INFINITE);
        }
    }
#else
    if (const auto left = when - Clock::now() - std::chrono::milliseconds(1); left > Clock::duration::zero()) std::this_thread::sleep_for(left);
#endif
    while (Clock::now() < when) std::this_thread::yield();
}

} // namespace openrts::viewer
