#include "dlss_upscaler.hpp"

#if defined(OPENRTS_HAS_DLSS) && defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <thread>

#include <glm/matrix.hpp>
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_security.h>
#include <webgpu/wgpu.h>
#endif

namespace openrts::viewer {

#if defined(OPENRTS_HAS_DLSS) && defined(_WIN32)
namespace {

// openRTS's project id for NGX (no application id from NVIDIA: the engine
// "custom" with a version and this GUID).
constexpr const char* project_id = "6b3c9d2e-4f1a-4c8b-9e2d-7a5f0b1c3d84";

void log_message(const sl::LogType type, const char* text) {
    if (type == sl::LogType::eInfo) return;
    std::cout << "DLSS (Streamline): " << (text ? text : "") << std::flush;
}

// A D3D11 texture shared with wgpu: made here, imported there.
struct Shared {
    ID3D11Texture2D* d3d = nullptr;
    HANDLE handle = nullptr;
    WGPUTexture texture = nullptr;
    WGPUTextureView view = nullptr;

    void release() {
        if (view) wgpuTextureViewRelease(view);
        if (texture) wgpuTextureRelease(texture);
        if (handle) CloseHandle(handle);
        if (d3d) d3d->Release();
        *this = {};
    }
};

constexpr const char* depth_copy_wgsl = R"(
@group(0) @binding(0) var depth: texture_depth_2d;
@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> @builtin(position) vec4<f32> {
    let uv = vec2<f32>(f32((index << 1u) & 2u), f32(index & 2u));
    return vec4<f32>(uv * 2.0 - 1.0, 0.0, 1.0);
}
@fragment
fn fs_main(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> {
    return vec4<f32>(textureLoad(depth, vec2<i32>(position.xy), 0), 0.0, 0.0, 1.0);
}
)";

} // namespace

struct DlssUpscaler::Impl {
    WGPUDevice device = nullptr;
    std::string error;
    bool available = false;
    HMODULE module = nullptr;
    bool initialised = false;
    PFun_slInit* sl_init = nullptr;
    PFun_slShutdown* sl_shutdown = nullptr;
    PFun_slSetD3DDevice* sl_set_device = nullptr;
    PFun_slIsFeatureSupported* sl_supported = nullptr;
    PFun_slGetNewFrameToken* sl_frame_token = nullptr;
    PFun_slSetTagForFrame* sl_set_tags = nullptr;
    PFun_slSetConstants* sl_set_constants = nullptr;
    PFun_slEvaluateFeature* sl_evaluate = nullptr;
    PFun_slGetFeatureFunction* sl_feature_function = nullptr;
    PFun_slFreeResources* sl_free = nullptr;
    PFun_slDLSSSetOptions* sl_dlss_options = nullptr;
    PFun_slUpgradeInterface* sl_upgrade = nullptr;
    // Streamline's frame bookkeeping runs at a swapchain's Present (its
    // presentCommon()); the frame goes to the screen through wgpu, so a
    // hidden 1 x 1 window's swapchain, upgraded by Streamline, presents
    // once a frame.
    HWND window = nullptr;
    IDXGISwapChain1* swapchain = nullptr;
    ID3D11Device* d3d = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Query* done = nullptr;
    Shared colour, motion, depth, output;
    std::uint32_t width = 0, height = 0;
    std::uint32_t frame = 0;
    WGPUBindGroupLayout depth_layout = nullptr;
    WGPURenderPipeline depth_pipeline = nullptr;

    ~Impl() {
        release_textures();
        if (initialised && sl_free) sl_free(sl::kFeatureDLSS, sl::ViewportHandle(0));
        if (initialised && sl_shutdown) sl_shutdown();
        if (swapchain) swapchain->Release();
        if (window) DestroyWindow(window);
        if (done) done->Release();
        if (context) context->Release();
        if (d3d) d3d->Release();
        if (depth_pipeline) wgpuRenderPipelineRelease(depth_pipeline);
        if (depth_layout) wgpuBindGroupLayoutRelease(depth_layout);
        if (module) FreeLibrary(module);
    }

    void release_textures() {
        for (auto* s : {&colour, &motion, &depth, &output}) s->release();
        width = height = 0;
    }

    template <typename T> bool find(T*& fn, const char* name) {
        fn = reinterpret_cast<T*>(GetProcAddress(module, name));
        if (!fn) error = std::string("sl.interposer.dll has no ") + name;
        return fn != nullptr;
    }

    bool make_shared(Shared& s, const DXGI_FORMAT format, const UINT bind, const WGPUTextureFormat wgpu_format, const WGPUTextureUsage usage,
                     const std::uint32_t w, const std::uint32_t h, const char* what) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = bind;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
        IDXGIResource1* resource = nullptr;
        if (FAILED(d3d->CreateTexture2D(&desc, nullptr, &s.d3d)) ||
            FAILED(s.d3d->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&resource)))) {
            error = std::string("the shared texture for ") + what + " could not be made";
            return false;
        }
        resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &s.handle);
        resource->Release();
        const WGPUTextureDescriptor descriptor{
            .usage = usage,
            .dimension = WGPUTextureDimension_2D,
            .size = {w, h, 1},
            .format = wgpu_format,
            .mipLevelCount = 1,
            .sampleCount = 1,
        };
        if (s.handle) s.texture = wgpuDeviceImportD3D11SharedTexture(device, s.handle, &descriptor);
        if (!s.texture) {
            error = std::string("wgpu could not take the shared texture for ") + what + " (wgpu on Vulkan, openRTS's wgpu-native?)";
            return false;
        }
        s.view = wgpuTextureCreateView(s.texture, nullptr);
        return true;
    }

    bool ensure(const std::uint32_t w, const std::uint32_t h) {
        if (w == width && h == height && output.view) return true;
        release_textures();
        const auto hdr = DXGI_FORMAT_R16G16B16A16_FLOAT;
        const bool made =
            make_shared(colour, hdr, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, WGPUTextureFormat_RGBA16Float,
                        WGPUTextureUsage_CopyDst | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_RenderAttachment, w, h, "the frame")
            && make_shared(motion, hdr, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, WGPUTextureFormat_RGBA16Float,
                           WGPUTextureUsage_CopyDst | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_RenderAttachment, w, h, "the motion")
            && make_shared(depth, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, WGPUTextureFormat_R32Float,
                           WGPUTextureUsage_TextureBinding | WGPUTextureUsage_RenderAttachment, w, h, "the depth")
            && make_shared(output, hdr, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET,
                           WGPUTextureFormat_RGBA16Float, WGPUTextureUsage_TextureBinding | WGPUTextureUsage_RenderAttachment, w, h,
                           "the output");
        if (!made) {
            std::cout << "DLSS: " << error << std::endl;
            release_textures();
            return false;
        }
        width = w;
        height = h;
        return true;
    }

    void make_depth_pipeline() {
        WGPUBindGroupLayoutEntry entry{.binding = 0, .visibility = WGPUShaderStage_Fragment};
        entry.texture.sampleType = WGPUTextureSampleType_Depth;
        entry.texture.viewDimension = WGPUTextureViewDimension_2D;
        const WGPUBindGroupLayoutDescriptor layout_descriptor{.entryCount = 1, .entries = &entry};
        depth_layout = wgpuDeviceCreateBindGroupLayout(device, &layout_descriptor);
        const WGPUShaderSourceWGSL source{.chain = {.sType = WGPUSType_ShaderSourceWGSL},
                                          .code = {.data = depth_copy_wgsl, .length = WGPU_STRLEN}};
        const WGPUShaderModuleDescriptor shader_descriptor{.nextInChain = &source.chain};
        const auto shader = wgpuDeviceCreateShaderModule(device, &shader_descriptor);
        const WGPUPipelineLayoutDescriptor pipeline_layout_descriptor{.bindGroupLayoutCount = 1, .bindGroupLayouts = &depth_layout};
        const auto pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &pipeline_layout_descriptor);
        const WGPUColorTargetState target{.format = WGPUTextureFormat_R32Float, .writeMask = WGPUColorWriteMask_All};
        const WGPUFragmentState fragment{.module = shader, .entryPoint = {.data = "fs_main", .length = WGPU_STRLEN}, .targetCount = 1, .targets = &target};
        const WGPURenderPipelineDescriptor descriptor{
            .layout = pipeline_layout,
            .vertex = {.module = shader, .entryPoint = {.data = "vs_main", .length = WGPU_STRLEN}},
            .primitive = {.topology = WGPUPrimitiveTopology_TriangleList},
            .multisample = {.count = 1, .mask = ~0u},
            .fragment = &fragment,
        };
        depth_pipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor);
        wgpuPipelineLayoutRelease(pipeline_layout);
        wgpuShaderModuleRelease(shader);
    }
};

DlssUpscaler::DlssUpscaler(const WGPUDevice device, const WGPUAdapter adapter) : impl_(std::make_unique<Impl>()) {
    auto& s = *impl_;
    s.device = device;
    // wgpu's video card: NVIDIA's, on Vulkan (the shared textures' import).
    WGPUAdapterInfo info{};
    if (adapter) wgpuAdapterGetInfo(adapter, &info);
    const auto vendor = info.vendorID;
    const auto device_id = info.deviceID;
    const auto backend = info.backendType;
    if (adapter) wgpuAdapterInfoFreeMembers(info);
    if (vendor != 0x10DE) {
        s.error = "not an NVIDIA video card";
        return;
    }
    if (backend != WGPUBackendType_Vulkan) {
        s.error = "wgpu on Vulkan only (the graphics API in the launcher)";
        return;
    }
    // Streamline beside the game, NVIDIA's signature checked.
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto folder = std::filesystem::path(exe).parent_path();
    const auto interposer = folder / L"sl.interposer.dll";
    if (!std::filesystem::exists(interposer)) {
        s.error = "sl.interposer.dll is not beside the game";
        return;
    }
    if (!sl::security::verifyEmbeddedSignature(interposer.c_str())) {
        s.error = "sl.interposer.dll is not signed by NVIDIA";
        return;
    }
    s.module = LoadLibraryW(interposer.c_str());
    if (!s.module) {
        s.error = "sl.interposer.dll could not be loaded";
        return;
    }
    if (!s.find(s.sl_init, "slInit") || !s.find(s.sl_shutdown, "slShutdown") || !s.find(s.sl_set_device, "slSetD3DDevice")
        || !s.find(s.sl_supported, "slIsFeatureSupported") || !s.find(s.sl_frame_token, "slGetNewFrameToken")
        || !s.find(s.sl_set_tags, "slSetTagForFrame") || !s.find(s.sl_set_constants, "slSetConstants")
        || !s.find(s.sl_evaluate, "slEvaluateFeature") || !s.find(s.sl_feature_function, "slGetFeatureFunction")
        || !s.find(s.sl_free, "slFreeResources") || !s.find(s.sl_upgrade, "slUpgradeInterface"))
        return;
    const std::wstring plugins = folder.wstring();
    const wchar_t* plugin_paths[] = {plugins.c_str()};
    const sl::Feature features[] = {sl::kFeatureDLSS};
    sl::Preferences preferences{};
    preferences.showConsole = false;
    preferences.logLevel = sl::LogLevel::eDefault;
    preferences.pathsToPlugins = plugin_paths;
    preferences.numPathsToPlugins = 1;
    preferences.logMessageCallback = log_message;
    // No updates over the network; openRTS restores no command list state
    // (D3D11's context is its own here).
    preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking
                        | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    preferences.featuresToLoad = features;
    preferences.numFeaturesToLoad = 1;
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = "openRTS";
    preferences.projectId = project_id;
    preferences.renderAPI = sl::RenderAPI::eD3D11;
    if (const auto result = s.sl_init(preferences, sl::kSDKVersion); result != sl::Result::eOk) {
        s.error = "slInit failed (" + std::to_string(static_cast<int>(result)) + ")";
        return;
    }
    s.initialised = true;
    // D3D11 on wgpu's video card.
    IDXGIFactory1* factory = nullptr;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
    IDXGIAdapter1* chosen = nullptr;
    for (UINT k = 0; factory; ++k) {
        IDXGIAdapter1* each = nullptr;
        if (factory->EnumAdapters1(k, &each) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        each->GetDesc1(&desc);
        if (desc.VendorId == vendor && (device_id == 0 || desc.DeviceId == device_id) && !chosen) {
            chosen = each;
            continue;
        }
        each->Release();
    }
    if (factory) factory->Release();
    if (!chosen) {
        s.error = "wgpu's video card not found by DXGI";
        return;
    }
    DXGI_ADAPTER_DESC1 desc{};
    chosen->GetDesc1(&desc);
    sl::AdapterInfo adapter_info{};
    adapter_info.deviceLUID = reinterpret_cast<std::uint8_t*>(&desc.AdapterLuid);
    adapter_info.deviceLUIDSizeInBytes = sizeof(LUID);
    if (const auto result = s.sl_supported(sl::kFeatureDLSS, adapter_info); result != sl::Result::eOk) {
        s.error = "DLSS is not supported on this video card or driver (" + std::to_string(static_cast<int>(result)) + ")";
        chosen->Release();
        return;
    }
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const HRESULT created = D3D11CreateDevice(chosen, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &s.d3d,
                                              nullptr, &s.context);
    chosen->Release();
    if (FAILED(created) || !s.d3d) {
        s.error = "D3D11CreateDevice failed";
        return;
    }
    if (s.sl_set_device(s.d3d) != sl::Result::eOk) {
        s.error = "slSetD3DDevice failed";
        return;
    }
    void* options = nullptr;
    if (s.sl_feature_function(sl::kFeatureDLSS, "slDLSSSetOptions", options) != sl::Result::eOk || !options) {
        s.error = "DLSS's options not found (sl.dlss.dll, nvngx_dlss.dll beside the game?)";
        return;
    }
    s.sl_dlss_options = reinterpret_cast<PFun_slDLSSSetOptions*>(options);
    {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = DefWindowProcW;
        window_class.hInstance = instance;
        window_class.lpszClassName = L"openRTS DLSS";
        RegisterClassW(&window_class);
        s.window = CreateWindowExW(WS_EX_TOOLWINDOW, L"openRTS DLSS", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
        IDXGIDevice* dxgi_device = nullptr;
        IDXGIAdapter* dxgi_adapter = nullptr;
        IDXGIFactory2* factory2 = nullptr;
        if (s.window && SUCCEEDED(s.d3d->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_device)))
            && SUCCEEDED(dxgi_device->GetAdapter(&dxgi_adapter))
            && SUCCEEDED(dxgi_adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory2)))) {
            DXGI_SWAP_CHAIN_DESC1 chain{};
            chain.Width = 1;
            chain.Height = 1;
            chain.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            chain.SampleDesc.Count = 1;
            chain.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            chain.BufferCount = 2;
            chain.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            if (SUCCEEDED(factory2->CreateSwapChainForHwnd(s.d3d, s.window, &chain, nullptr, nullptr, &s.swapchain)))
                s.sl_upgrade(reinterpret_cast<void**>(&s.swapchain));
        }
        if (factory2) factory2->Release();
        if (dxgi_adapter) dxgi_adapter->Release();
        if (dxgi_device) dxgi_device->Release();
        if (!s.swapchain) std::cout << "DLSS: no presenting swapchain (Streamline's bookkeeping may grow)" << std::endl;
    }
    const D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
    s.d3d->CreateQuery(&query, &s.done);
    s.make_depth_pipeline();
    s.available = s.done && s.depth_pipeline;
    if (!s.available) s.error = "the D3D11 query or the depth copy could not be made";
}

DlssUpscaler::~DlssUpscaler() = default;

bool DlssUpscaler::available() const noexcept {
    return impl_->available;
}

const std::string& DlssUpscaler::error() const noexcept {
    return impl_->error;
}

bool DlssUpscaler::record_inputs(const WGPUCommandEncoder encoder, const WGPUTexture scene, const WGPUTexture motion,
                                 const WGPUTextureView depth, const std::uint32_t width, const std::uint32_t height) {
    auto& s = *impl_;
    if (!s.available || !scene || !motion || !depth || width == 0 || height == 0) return false;
    if (!s.ensure(width, height)) return false;
    const WGPUExtent3D size{width, height, 1};
    const WGPUTexelCopyTextureInfo from_scene{.texture = scene};
    const WGPUTexelCopyTextureInfo to_colour{.texture = s.colour.texture};
    wgpuCommandEncoderCopyTextureToTexture(encoder, &from_scene, &to_colour, &size);
    const WGPUTexelCopyTextureInfo from_motion{.texture = motion};
    const WGPUTexelCopyTextureInfo to_motion{.texture = s.motion.texture};
    wgpuCommandEncoderCopyTextureToTexture(encoder, &from_motion, &to_motion, &size);
    // The depth into a colour texture D3D11 can read (R32_FLOAT).
    const WGPUBindGroupEntry entry{.binding = 0, .textureView = depth};
    const WGPUBindGroupDescriptor group_descriptor{.layout = s.depth_layout, .entryCount = 1, .entries = &entry};
    const auto group = wgpuDeviceCreateBindGroup(s.device, &group_descriptor);
    const WGPURenderPassColorAttachment colour{
        .view = s.depth.view, .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED, .loadOp = WGPULoadOp_Clear, .storeOp = WGPUStoreOp_Store};
    const WGPURenderPassDescriptor pass_descriptor{.colorAttachmentCount = 1, .colorAttachments = &colour};
    const auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_descriptor);
    wgpuRenderPassEncoderSetPipeline(pass, s.depth_pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
    wgpuBindGroupRelease(group);
    return true;
}

WGPUTextureView DlssUpscaler::evaluate(const DlssFrame& frame) {
    auto& s = *impl_;
    if (!s.available || !s.output.view || frame.width != s.width || frame.height != s.height) return nullptr;
    const sl::ViewportHandle viewport(0);
    sl::FrameToken* token = nullptr;
    const std::uint32_t index = ++s.frame;
    if (s.sl_frame_token(token, &index) != sl::Result::eOk || !token) return nullptr;

    sl::DLSSOptions options{};
    options.mode = sl::DLSSMode::eDLAA;
    options.outputWidth = frame.width;
    options.outputHeight = frame.height;
    options.colorBuffersHDR = sl::Boolean::eTrue;
    options.useAutoExposure = sl::Boolean::eTrue;
    s.sl_dlss_options(viewport, options);

    // glm's column-major matrices (column vectors) are Streamline's
    // row-major ones (row vectors) as they lie in memory.
    const auto matrix = [](const glm::mat4& m) {
        sl::float4x4 out;
        std::memcpy(&out, &m, sizeof(out));
        return out;
    };
    const glm::mat4 now = frame.projection * frame.view;
    sl::Constants constants{};
    constants.cameraViewToClip = matrix(frame.projection);
    constants.clipToCameraView = matrix(glm::inverse(frame.projection));
    constants.clipToLensClip = matrix(glm::mat4{1.0f});
    constants.clipToPrevClip = matrix(frame.previous_view_projection * glm::inverse(now));
    constants.prevClipToClip = matrix(now * glm::inverse(frame.previous_view_projection));
    // MotionVectors' jitter moved the picture by (-x, +y) pixels.
    static const bool flip = GetEnvironmentVariableW(L"OPENRTS_DLSS_JITTER_FLIP", nullptr, 0) > 0;
    constants.jitterOffset = flip ? sl::float2(-frame.jitter.x, frame.jitter.y) : sl::float2(frame.jitter.x, -frame.jitter.y);
    // MotionVectors' motion is in texture coordinates already (0..1 across).
    constants.mvecScale = sl::float2(1.0f, 1.0f);
    constants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
    const glm::mat4 inverse_view = glm::inverse(frame.view);
    constants.cameraPos = sl::float3(frame.eye.x, frame.eye.y, frame.eye.z);
    constants.cameraRight = sl::float3(inverse_view[0][0], inverse_view[0][1], inverse_view[0][2]);
    constants.cameraUp = sl::float3(inverse_view[1][0], inverse_view[1][1], inverse_view[1][2]);
    constants.cameraFwd = sl::float3(-inverse_view[2][0], -inverse_view[2][1], -inverse_view[2][2]);
    constants.cameraNear = frame.near_plane;
    constants.cameraFar = frame.far_plane;
    constants.cameraFOV = frame.vertical_fov;
    constants.cameraAspectRatio = frame.aspect;
    constants.motionVectorsInvalidValue = 0.0f;
    constants.depthInverted = sl::Boolean::eTrue;
    constants.cameraMotionIncluded = sl::Boolean::eTrue;
    constants.motionVectors3D = sl::Boolean::eFalse;
    constants.reset = frame.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    constants.orthographicProjection = sl::Boolean::eFalse;
    constants.motionVectorsDilated = sl::Boolean::eFalse;
    constants.motionVectorsJittered = sl::Boolean::eFalse;
    s.sl_set_constants(constants, *token, viewport);

    sl::Resource colour{sl::ResourceType::eTex2d, s.colour.d3d, nullptr, nullptr};
    sl::Resource motion{sl::ResourceType::eTex2d, s.motion.d3d, nullptr, nullptr};
    sl::Resource depth{sl::ResourceType::eTex2d, s.depth.d3d, nullptr, nullptr};
    sl::Resource output{sl::ResourceType::eTex2d, s.output.d3d, nullptr, nullptr};
    const sl::Extent extent{0, 0, frame.width, frame.height};
    const sl::ResourceTag tags[] = {
        sl::ResourceTag(&colour, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent, &extent),
        sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &extent),
        sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &extent),
        sl::ResourceTag(&output, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, &extent),
    };
    if (const auto result = s.sl_set_tags(*token, viewport, tags, 4, reinterpret_cast<sl::CommandBuffer*>(s.context));
        result != sl::Result::eOk) {
        std::cout << "DLSS: slSetTagForFrame failed (" << static_cast<int>(result) << ")" << std::endl;
        return nullptr;
    }
    const sl::BaseStructure* inputs[] = {&viewport};
    if (const auto result = s.sl_evaluate(sl::kFeatureDLSS, *token, inputs, 1, reinterpret_cast<sl::CommandBuffer*>(s.context));
        result != sl::Result::eOk) {
        std::cout << "DLSS: slEvaluateFeature failed (" << static_cast<int>(result) << ")" << std::endl;
        return nullptr;
    }
    // Done on the GPU before wgpu reads the output.
    s.context->End(s.done);
    s.context->Flush();
    while (s.context->GetData(s.done, nullptr, 0, 0) == S_FALSE) std::this_thread::yield();
    // The frame's end for Streamline.
    if (s.swapchain) s.swapchain->Present(0, 0);
    return s.output.view;
}

#else

struct DlssUpscaler::Impl {
    std::string error = "this build has no DLSS (openRTS's wgpu-native and Streamline wanted)";
};

DlssUpscaler::DlssUpscaler(WGPUDevice, WGPUAdapter) : impl_(std::make_unique<Impl>()) {}
DlssUpscaler::~DlssUpscaler() = default;
bool DlssUpscaler::available() const noexcept {
    return false;
}
const std::string& DlssUpscaler::error() const noexcept {
    return impl_->error;
}
bool DlssUpscaler::record_inputs(WGPUCommandEncoder, WGPUTexture, WGPUTexture, WGPUTextureView, std::uint32_t, std::uint32_t) {
    return false;
}
WGPUTextureView DlssUpscaler::evaluate(const DlssFrame&) {
    return nullptr;
}

#endif

} // namespace openrts::viewer
