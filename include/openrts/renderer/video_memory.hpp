#pragma once

#include <cstdint>
#include <memory>

namespace openrts::renderer {

// The video card's memory as Windows counts it for this process (DXGI's
// QueryVideoMemoryInfo; WDDM counts every API's allocations, Vulkan's too):
// what is in the card's own memory, what Windows lets the process keep there
// (its budget: the card's memory less what other programs and the desktop
// hold), and what spilled to the shared system memory. Past the budget
// Windows moves the process's allocations out to system memory, read over
// PCIe: passes over big render targets then take tens of times longer
// (testers, 2026-11-17: an RTX 4060 with 8 GB at Ultra, the tone curve 6 ms
// instead of 0.1). Not known elsewhere (known false).
struct VideoMemory {
    bool known = false;
    std::uint64_t usage = 0;        // bytes in the card's memory
    std::uint64_t budget = 0;       // bytes Windows lets the process keep there
    std::uint64_t shared_usage = 0; // bytes in the shared system memory
    std::uint64_t total = 0;        // the card's own memory
    [[nodiscard]] bool over_budget() const noexcept { return known && budget > 0 && usage > budget; }
};

class VideoMemoryProbe {
public:
    // The adapter by its PCI vendor and device ids (WGPUAdapterInfo's).
    VideoMemoryProbe(std::uint32_t vendor_id, std::uint32_t device_id);
    ~VideoMemoryProbe();
    VideoMemoryProbe(const VideoMemoryProbe&) = delete;
    VideoMemoryProbe& operator=(const VideoMemoryProbe&) = delete;

    [[nodiscard]] VideoMemory query() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace openrts::renderer
