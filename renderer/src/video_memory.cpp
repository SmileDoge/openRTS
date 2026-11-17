#include <openrts/renderer/video_memory.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <dxgi1_4.h>
#include <windows.h>
#endif

namespace openrts::renderer {

struct VideoMemoryProbe::Impl {
#ifdef _WIN32
    IDXGIAdapter3* adapter = nullptr;
    std::uint64_t total = 0;
    ~Impl() {
        if (adapter) adapter->Release();
    }
#endif
};

VideoMemoryProbe::VideoMemoryProbe(const std::uint32_t vendor_id, const std::uint32_t device_id) : impl_(std::make_unique<Impl>()) {
#ifdef _WIN32
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) return;
    // The adapter with these ids; failing that the first hardware one.
    IDXGIAdapter1* chosen = nullptr;
    IDXGIAdapter1* first = nullptr;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND || !adapter) break;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        const bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        if (!software && !chosen && desc.VendorId == vendor_id && desc.DeviceId == device_id) {
            chosen = adapter;
            impl_->total = desc.DedicatedVideoMemory;
            continue;
        }
        if (!software && !first) {
            first = adapter;
            continue;
        }
        adapter->Release();
    }
    if (!chosen && first) {
        DXGI_ADAPTER_DESC1 desc{};
        first->GetDesc1(&desc);
        impl_->total = desc.DedicatedVideoMemory;
        chosen = first;
        first = nullptr;
    }
    if (first) first->Release();
    if (chosen) {
        chosen->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&impl_->adapter));
        chosen->Release();
    }
    factory->Release();
#else
    (void)vendor_id;
    (void)device_id;
#endif
}

VideoMemoryProbe::~VideoMemoryProbe() = default;

VideoMemory VideoMemoryProbe::query() const {
    VideoMemory out;
#ifdef _WIN32
    if (!impl_->adapter) return out;
    DXGI_QUERY_VIDEO_MEMORY_INFO local{};
    DXGI_QUERY_VIDEO_MEMORY_INFO shared{};
    if (FAILED(impl_->adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local))) return out;
    impl_->adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared);
    out = {.known = true, .usage = local.CurrentUsage, .budget = local.Budget, .shared_usage = shared.CurrentUsage, .total = impl_->total};
#endif
    return out;
}

} // namespace openrts::renderer
