#include "openrts/renderer/opacity_micromap.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace openrts::renderer {

namespace {

std::uint32_t even_bits(std::uint32_t x) {
    x &= 0x55555555u;
    x = (x | (x >> 1)) & 0x33333333u;
    x = (x | (x >> 2)) & 0x0f0f0f0fu;
    x = (x | (x >> 4)) & 0x00ff00ffu;
    x = (x | (x >> 8)) & 0x0000ffffu;
    return x;
}

// The exclusive prefix xor.
std::uint32_t prefix_xor(std::uint32_t x) {
    x ^= (x >> 1) & 0x7fff7fffu;
    x ^= (x >> 2) & 0x3fff3fffu;
    x ^= (x >> 4) & 0x0fff0fffu;
    x ^= (x >> 8) & 0x00ff00ffu;
    return x;
}

} // namespace

std::array<std::array<float, 2>, 3> micro_triangle(const std::uint32_t index, const std::uint32_t level) {
    if (level == 0) return {{{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}}};
    // The discrete barycentrics of the distance along the curve.
    const auto b0 = even_bits(index);
    const auto b1 = even_bits(index >> 1);
    const auto fx = prefix_xor(b0);
    const auto fy = prefix_xor(b0 & ~b1);
    const auto t = fy ^ b1;
    auto iu = (fx & ~t) | (b0 & ~t) | (~b0 & ~fx & t);
    auto iv = fy ^ b0;
    auto iw = (~fx & ~t) | (b0 & ~t) | (~b0 & fx & t);
    const std::uint32_t mask = (1u << level) - 1u;
    iu &= mask;
    iv &= mask;
    iw &= mask;
    const bool upright = ((iu & 1u) ^ (iv & 1u) ^ (iw & 1u)) != 0;
    if (!upright) {
        iu += 1;
        iv += 1;
    }
    const float scale = 1.0f / static_cast<float>(1u << level);
    float du = scale, dv = scale;
    if (!upright) {
        du = -du;
        dv = -dv;
    }
    const float u = static_cast<float>(iu) * scale, v = static_cast<float>(iv) * scale;
    return {{{u, v}, {u + du, v}, {u, v + dv}}};
}

BakedMicromaps bake_opacity_micromaps(const float* uvs, const std::size_t triangles, const std::uint8_t* mask, const std::uint32_t size,
                                      const std::uint32_t level) {
    BakedMicromaps out;
    out.level = level;
    if (!uvs || !mask || size == 0) return out;
    const std::uint32_t micro = 1u << (2 * level);
    const std::size_t bytes = std::max<std::size_t>((static_cast<std::size_t>(micro) * 2 + 7) / 8, 1);
    std::vector<std::array<std::array<float, 2>, 3>> corners(micro);
    for (std::uint32_t i = 0; i < micro; ++i) corners[i] = micro_triangle(i, level);
    const auto solid = [&](long long x, long long y) {
        const auto n = static_cast<long long>(size);
        x = ((x % n) + n) % n;
        y = ((y % n) + n) % n;
        return mask[static_cast<std::size_t>(y) * size + static_cast<std::size_t>(x)] >= 128;
    };
    std::map<std::string, std::int32_t> shared;
    std::vector<std::uint8_t> states(bytes);
    out.indices.reserve(triangles);
    for (std::size_t t = 0; t < triangles; ++t) {
        const float* c = uvs + t * 6;
        std::fill(states.begin(), states.end(), std::uint8_t{0});
        bool any_opaque = false, any_clear = false, any_unknown = false;
        for (std::uint32_t m = 0; m < micro; ++m) {
            // The micro-triangle in mask texels.
            float p[3][2];
            for (int k = 0; k < 3; ++k) {
                const float u = corners[m][k][0], v = corners[m][k][1];
                const float w = 1.0f - u - v;
                p[k][0] = (c[0] * w + c[2] * u + c[4] * v) * static_cast<float>(size);
                p[k][1] = (c[1] * w + c[3] * u + c[5] * v) * static_cast<float>(size);
            }
            const float x0 = std::floor(std::min({p[0][0], p[1][0], p[2][0]}));
            const float x1 = std::floor(std::max({p[0][0], p[1][0], p[2][0]}));
            const float y0 = std::floor(std::min({p[0][1], p[1][1], p[2][1]}));
            const float y1 = std::floor(std::max({p[0][1], p[1][1], p[2][1]}));
            MicroState state = MicroState::unknown_opaque;
            if (std::isfinite(x0) && std::isfinite(y0) && std::isfinite(x1) && std::isfinite(y1) && x1 - x0 <= 64.0f && y1 - y0 <= 64.0f) {
                // Every texel the micro-triangle's box touches (conservative:
                // a texel in the box but outside the triangle counts too).
                bool seen_solid = false, seen_clear = false;
                for (auto y = static_cast<long long>(y0); y <= static_cast<long long>(y1) && !(seen_solid && seen_clear); ++y)
                    for (auto x = static_cast<long long>(x0); x <= static_cast<long long>(x1); ++x) {
                        if (solid(x, y)) seen_solid = true;
                        else seen_clear = true;
                        if (seen_solid && seen_clear) break;
                    }
                if (seen_solid && !seen_clear) state = MicroState::opaque;
                else if (seen_clear && !seen_solid) state = MicroState::transparent;
                else {
                    // Mixed: unknown, leaning as its middle is.
                    const float mx = (p[0][0] + p[1][0] + p[2][0]) / 3.0f, my = (p[0][1] + p[1][1] + p[2][1]) / 3.0f;
                    state = solid(static_cast<long long>(std::floor(mx)), static_cast<long long>(std::floor(my))) ? MicroState::unknown_opaque
                                                                                                                  : MicroState::unknown_transparent;
                }
            }
            any_opaque |= state == MicroState::opaque;
            any_clear |= state == MicroState::transparent;
            any_unknown |= state == MicroState::unknown_opaque || state == MicroState::unknown_transparent;
            states[m / 4] |= static_cast<std::uint8_t>(static_cast<std::uint8_t>(state) << ((m % 4) * 2));
        }
        if (!any_unknown && !any_clear) {
            out.indices.push_back(micromap_fully_opaque);
            continue;
        }
        if (!any_unknown && !any_opaque) {
            out.indices.push_back(micromap_fully_transparent);
            continue;
        }
        const std::string key(states.begin(), states.end());
        const auto [it, added] = shared.emplace(key, static_cast<std::int32_t>(out.count));
        if (added) {
            out.data.insert(out.data.end(), states.begin(), states.end());
            ++out.count;
        }
        out.indices.push_back(it->second);
    }
    return out;
}

namespace {

// The micromaps' worker: one thread, its queue (stopped and joined at exit).
class MicromapWorker {
public:
    MicromapWorker() : thread_([this](const std::stop_token& stop) { run(stop); }) {}
    ~MicromapWorker() {
        thread_.request_stop();
        wake_.notify_all();
    }
    void post(std::function<void()> job) {
        {
            const std::lock_guard lock(mutex_);
            jobs_.push_back(std::move(job));
        }
        wake_.notify_one();
    }

private:
    void run(const std::stop_token& stop) {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, stop, [this] { return !jobs_.empty(); });
                if (stop.stop_requested()) return;
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            job();
        }
    }
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::deque<std::function<void()>> jobs_;
    std::jthread thread_;
};

} // namespace

std::shared_ptr<MicromapJob> bake_opacity_micromaps_async(std::vector<float> uvs, std::vector<std::uint8_t> mask, const std::uint32_t size,
                                                         const std::uint32_t level) {
    static MicromapWorker worker;
    auto job = std::make_shared<MicromapJob>();
    worker.post([job, uvs = std::move(uvs), mask = std::move(mask), size, level] {
        job->result = bake_opacity_micromaps(uvs.data(), uvs.size() / 6, mask.data(), size, level);
        job->done.store(true, std::memory_order_release);
    });
    return job;
}

} // namespace openrts::renderer
