#include "ui_image.hpp"

#include <cstring>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincodec.h>
#endif

namespace openrts::game::menu {
namespace {

#ifdef _WIN32
template <typename T>
void release(T*& object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

// PNG (or any WIC format) as 8-bit RGBA, rows top to bottom.
bool decode(const std::filesystem::path& file, std::vector<std::uint8_t>& pixels, int& width, int& height) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize = SUCCEEDED(initialized);
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(reinterpret_cast<UINT*>(&width), reinterpret_cast<UINT*>(&height))) &&
        width > 0 && height > 0 &&
        SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                        WICBitmapPaletteTypeCustom))) {
        pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u);
        ok = SUCCEEDED(converter->CopyPixels(nullptr, static_cast<UINT>(width * 4), static_cast<UINT>(pixels.size()), pixels.data()));
    }
    release(converter);
    release(frame);
    release(decoder);
    release(factory);
    if (uninitialize) CoUninitialize();
    return ok;
}
#else
bool decode(const std::filesystem::path&, std::vector<std::uint8_t>&, int&, int&) { return false; }
#endif

} // namespace

ImageCache::Image::~Image() {
    if (view) wgpuTextureViewRelease(view);
    if (texture) wgpuTextureRelease(texture);
}

void ImageCache::set_device(const WGPUDevice device) { device_ = device; }

ImageCache::~ImageCache() { clear(); }

void ImageCache::clear() { images_.clear(); }

ImageCache::Image* ImageCache::find(const std::filesystem::path& file) {
    if (!device_) return nullptr;
    const auto key = file.string();
    if (const auto it = images_.find(key); it != images_.end()) return it->second.get();

    std::vector<std::uint8_t> pixels;
    int width = 0, height = 0;
    if (!decode(file, pixels, width, height)) return nullptr;

    auto image = std::make_unique<Image>();
    WGPUTextureDescriptor descriptor{};
    descriptor.dimension = WGPUTextureDimension_2D;
    descriptor.format = WGPUTextureFormat_RGBA8Unorm;
    descriptor.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    descriptor.size = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 1};
    descriptor.mipLevelCount = 1;
    descriptor.sampleCount = 1;
    image->texture = wgpuDeviceCreateTexture(device_, &descriptor);
    if (!image->texture) return nullptr;
    image->width = width;
    image->height = height;

    if (const auto queue = wgpuDeviceGetQueue(device_)) {
        const WGPUTexelCopyTextureInfo target{.texture = image->texture, .mipLevel = 0, .origin = {}, .aspect = WGPUTextureAspect_All};
        const WGPUTexelCopyBufferLayout layout{.offset = 0, .bytesPerRow = static_cast<std::uint32_t>(width * 4),
                                               .rowsPerImage = static_cast<std::uint32_t>(height)};
        const WGPUExtent3D extent{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 1};
        wgpuQueueWriteTexture(queue, &target, pixels.data(), pixels.size(), &layout, &extent);
    }
    image->view = wgpuTextureCreateView(image->texture, nullptr);
    if (!image->view) return nullptr;

    auto& slot = images_[key];
    slot = std::move(image);
    return slot.get();
}

WGPUTextureView ImageCache::load(const std::filesystem::path& file) {
    const auto* image = find(file);
    return image ? image->view : nullptr;
}

std::pair<int, int> ImageCache::size(const std::filesystem::path& file) {
    const auto* image = find(file);
    return image ? std::pair{image->width, image->height} : std::pair{0, 0};
}

} // namespace openrts::game::menu
