// openrts-xr-probe: the OpenXR prototype (docs/vr.md). A scene drawn by
// wgpu, as the game's renderer draws, shown in a headset through OpenXR:
// each eye its own view and projection from the runtime, the head tracked,
// 1:1 scale. The frame: with openRTS's wgpu-native fork (OPENRTS_XR_SHARED),
// wgpu draws into a D3D11 texture shared with its Vulkan device and D3D11
// copies it into the OpenXR swapchain's image on the GPU; else (or
// --cpu) wgpu -> the CPU -> that image. The scene: a floor grid 1.6 m under the start, markers at
// 2 m ahead (red), left (green) and right (blue), a sky.
//
//   openrts-xr-probe [--seconds N] [--cpu]   (Esc or the headset's quit ends it)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <webgpu/webgpu.h>
#include <webgpu/wgpu.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>


namespace {

bool check(const XrResult r, const char* what, XrInstance instance = XR_NULL_HANDLE) {
    if (XR_SUCCEEDED(r)) return true;
    char text[XR_MAX_RESULT_STRING_SIZE]{};
    if (instance) xrResultToString(instance, r, text);
    std::fprintf(stderr, "openrts-xr-probe: %s failed: %d %s\n", what, static_cast<int>(r), text);
    return false;
}

WGPUStringView wgsl_name(const char* text) { return {text, std::strlen(text)}; }

const char* scene_wgsl = R"(
struct Eye {
    inverse_view_projection: mat4x4<f32>,
    eye: vec4<f32>,
};
@group(0) @binding(0) var<uniform> u: Eye;

struct Out {
    @builtin(position) clip: vec4<f32>,
    @location(0) ndc: vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) i: u32) -> Out {
    let p = vec2<f32>(f32((i << 1u) & 2u), f32(i & 2u)) * 2.0 - 1.0;
    var out: Out;
    out.clip = vec4<f32>(p, 0.0, 1.0);
    out.ndc = p;
    return out;
}

// A box of half size h at c: the distance along the ray, or -1.
fn box_hit(o: vec3<f32>, d: vec3<f32>, c: vec3<f32>, h: vec3<f32>) -> f32 {
    let inv = 1.0 / d;
    let t0 = (c - h - o) * inv;
    let t1 = (c + h - o) * inv;
    let near = max(max(min(t0.x, t1.x), min(t0.y, t1.y)), min(t0.z, t1.z));
    let far = min(min(max(t0.x, t1.x), max(t0.y, t1.y)), max(t0.z, t1.z));
    if (far < max(near, 0.0)) { return -1.0; }
    return near;
}

@fragment
fn fs_main(input: Out) -> @location(0) vec4<f32> {
    let far = u.inverse_view_projection * vec4<f32>(input.ndc, 1.0, 1.0);
    let o = u.eye.xyz;
    let d = normalize(far.xyz / far.w - o);
    // The sky.
    var colour = mix(vec3<f32>(0.55, 0.65, 0.8), vec3<f32>(0.12, 0.25, 0.55), clamp(d.y * 2.0, 0.0, 1.0));
    var best = 1e9;
    // The floor 1.6 m under the start: a 1 m grid fading far off.
    if (d.y < -1e-4) {
        let t = (-1.6 - o.y) / d.y;
        if (t > 0.0) {
            best = t;
            let p = o + d * t;
            let g = abs(fract(p.xz + 0.5) - 0.5);
            let line = 1.0 - smoothstep(0.0, 0.02 + t * 0.002, min(g.x, g.y));
            let fade = exp(-t * 0.03);
            colour = mix(colour, mix(vec3<f32>(0.2), vec3<f32>(0.9), line), fade);
        }
    }
    // Markers 2 m away: ahead red, left green, right blue, a white one behind.
    let centres = array<vec3<f32>, 4>(vec3<f32>(0.0, 0.0, -2.0), vec3<f32>(-2.0, 0.0, 0.0), vec3<f32>(2.0, 0.0, 0.0), vec3<f32>(0.0, 0.0, 2.0));
    let colours = array<vec3<f32>, 4>(vec3<f32>(0.9, 0.15, 0.1), vec3<f32>(0.1, 0.8, 0.2), vec3<f32>(0.15, 0.3, 0.95), vec3<f32>(0.9, 0.9, 0.9));
    for (var k = 0; k < 4; k = k + 1) {
        let t = box_hit(o, d, centres[k], vec3<f32>(0.15, 0.15, 0.15));
        if (t > 0.0 && t < best) {
            best = t;
            let p = o + d * t - centres[k];
            let a = abs(p);
            var n = vec3<f32>(sign(p.x), 0.0, 0.0);
            if (a.y > a.x && a.y > a.z) { n = vec3<f32>(0.0, sign(p.y), 0.0); }
            if (a.z > a.x && a.z > a.y) { n = vec3<f32>(0.0, 0.0, sign(p.z)); }
            colour = colours[k] * (0.45 + 0.55 * max(dot(n, normalize(vec3<f32>(0.4, 0.8, 0.3))), 0.0));
        }
    }
    return vec4<f32>(colour, 1.0);
}
)";

// An OpenXR pose as a rigid transform (head-to-space).
glm::mat4 pose_matrix(const XrPosef& p) {
    const glm::quat q{p.orientation.w, p.orientation.x, p.orientation.y, p.orientation.z};
    glm::mat4 m = glm::mat4_cast(q);
    m[3] = glm::vec4{p.position.x, p.position.y, p.position.z, 1.0f};
    return m;
}

// The eye's asymmetric projection (right-handed, view down -Z, depth 0..1).
glm::mat4 projection(const XrFovf& f, const float near_z, const float far_z) {
    const float l = std::tan(f.angleLeft), r = std::tan(f.angleRight), d = std::tan(f.angleDown), u = std::tan(f.angleUp);
    glm::mat4 m{0.0f};
    m[0][0] = 2.0f / (r - l);
    m[1][1] = 2.0f / (u - d);
    m[2][0] = (r + l) / (r - l);
    m[2][1] = (u + d) / (u - d);
    m[2][2] = -far_z / (far_z - near_z);
    m[2][3] = -1.0f;
    m[3][2] = -(far_z * near_z) / (far_z - near_z);
    return m;
}

// wgpu on Vulkan, asking for the D3D11 shared-texture import when the fork
// has it: the instance, the adapter, the device.
struct Gpu {
    WGPUInstance instance = nullptr;
    WGPUAdapter adapter = nullptr;
    WGPUDevice device = nullptr;
};
Gpu make_gpu(const bool shared) {
    Gpu g;
    WGPUInstanceExtras extras{};
    extras.chain.sType = static_cast<WGPUSType>(WGPUSType_InstanceExtras);
    extras.backends = WGPUInstanceBackend_Vulkan;
    WGPUInstanceDescriptor instance_descriptor{};
    instance_descriptor.nextInChain = &extras.chain;
    g.instance = wgpuCreateInstance(&instance_descriptor);
    if (!g.instance) return g;
    WGPURequestAdapterOptions options{};
    options.powerPreference = WGPUPowerPreference_HighPerformance;
    const WGPURequestAdapterCallbackInfo adapter_callback{
        .mode = WGPUCallbackMode_AllowProcessEvents,
        .callback = [](const WGPURequestAdapterStatus status, const WGPUAdapter adapter, WGPUStringView, void* data, void*) {
            if (status == WGPURequestAdapterStatus_Success) *static_cast<WGPUAdapter*>(data) = adapter;
        },
        .userdata1 = &g.adapter,
    };
    wgpuInstanceRequestAdapter(g.instance, &options, adapter_callback);
    for (int k = 0; k < 1000 && !g.adapter; ++k) wgpuInstanceProcessEvents(g.instance);
    if (!g.adapter) return g;
    WGPUFeatureName features[1]{};
    std::size_t feature_count = 0;
#ifdef OPENRTS_XR_SHARED
    if (shared) features[feature_count++] = static_cast<WGPUFeatureName>(WGPUNativeFeature_VulkanExternalMemoryWin32);
#else
    (void)shared;
#endif
    WGPUDeviceDescriptor device_descriptor{};
    device_descriptor.requiredFeatureCount = feature_count;
    device_descriptor.requiredFeatures = features;
    const WGPURequestDeviceCallbackInfo device_callback{
        .mode = WGPUCallbackMode_AllowProcessEvents,
        .callback = [](const WGPURequestDeviceStatus status, const WGPUDevice device, WGPUStringView message, void* data, void*) {
            if (status == WGPURequestDeviceStatus_Success) *static_cast<WGPUDevice*>(data) = device;
            else std::fprintf(stderr, "openrts-xr-probe: no device: %.*s\n", static_cast<int>(message.length), message.data);
        },
        .userdata1 = &g.device,
    };
    wgpuAdapterRequestDevice(g.adapter, &device_descriptor, device_callback);
    for (int k = 0; k < 1000 && !g.device; ++k) wgpuInstanceProcessEvents(g.instance);
    return g;
}

struct EyeTarget {
    std::uint32_t width{}, height{};
    // The shared path: the D3D11 texture wgpu draws into (its NT handle).
    ID3D11Texture2D* shared = nullptr;
    HANDLE shared_handle = nullptr;
    WGPUTexture texture = nullptr;
    WGPUTextureView view = nullptr;
    WGPUBuffer readback = nullptr;
    std::uint32_t row_bytes{}; // 256-aligned
    WGPUBuffer uniform = nullptr;
    WGPUBindGroup group = nullptr;
    XrSwapchain swapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images;
};

} // namespace

int main(int argc, char** argv) {
    double seconds = 0.0; // 0: until quit
    bool cpu = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--cpu") == 0) cpu = true;
    }
#ifndef OPENRTS_XR_SHARED
    cpu = true; // the official wgpu-native: no shared textures
#endif

    // --- OpenXR: the instance, the headset, its D3D11 needs.
    const char* extensions[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo instance_info{XR_TYPE_INSTANCE_CREATE_INFO};
    std::snprintf(instance_info.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "openRTS xr probe");
    std::snprintf(instance_info.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "openRTS");
    instance_info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    instance_info.enabledExtensionCount = 1;
    instance_info.enabledExtensionNames = extensions;
    XrInstance instance = XR_NULL_HANDLE;
    if (!check(xrCreateInstance(&instance_info, &instance), "xrCreateInstance (is the Oculus / Meta Link app running and set as the OpenXR runtime?)"))
        return 1;
    XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(instance, &properties);
    std::printf("OpenXR runtime: %s %u.%u.%u\n", properties.runtimeName, XR_VERSION_MAJOR(properties.runtimeVersion),
                XR_VERSION_MINOR(properties.runtimeVersion), XR_VERSION_PATCH(properties.runtimeVersion));
    XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    if (!check(xrGetSystem(instance, &system_info, &system), "xrGetSystem (is the headset connected through Link?)", instance)) return 1;
    XrSystemProperties system_properties{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(instance, system, &system_properties);
    std::printf("Headset: %s\n", system_properties.systemName);

    PFN_xrGetD3D11GraphicsRequirementsKHR get_requirements = nullptr;
    xrGetInstanceProcAddr(instance, "xrGetD3D11GraphicsRequirementsKHR", reinterpret_cast<PFN_xrVoidFunction*>(&get_requirements));
    XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (!get_requirements || !check(get_requirements(instance, system, &requirements), "xrGetD3D11GraphicsRequirementsKHR", instance)) return 1;

    // --- D3D11 on the adapter the runtime asks for (only to fill its images).
    IDXGIFactory1* factory = nullptr;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
    IDXGIAdapter1* adapter = nullptr;
    for (UINT k = 0; factory && factory->EnumAdapters1(k, &adapter) != DXGI_ERROR_NOT_FOUND; ++k) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (std::memcmp(&desc.AdapterLuid, &requirements.adapterLuid, sizeof(LUID)) == 0) break;
        adapter->Release();
        adapter = nullptr;
    }
    ID3D11Device* d3d = nullptr;
    ID3D11DeviceContext* d3d_context = nullptr;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &d3d,
                                 nullptr, &d3d_context))) {
        std::fprintf(stderr, "openrts-xr-probe: D3D11CreateDevice failed\n");
        return 1;
    }

    // --- The session.
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = d3d;
    XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
    session_info.next = &binding;
    session_info.systemId = system;
    XrSession session = XR_NULL_HANDLE;
    if (!check(xrCreateSession(instance, &session_info, &session), "xrCreateSession", instance)) return 1;
    XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    space_info.poseInReferenceSpace.orientation.w = 1.0f;
    XrSpace space = XR_NULL_HANDLE;
    if (!check(xrCreateReferenceSpace(session, &space_info, &space), "xrCreateReferenceSpace", instance)) return 1;

    std::uint32_t view_count = 0;
    xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &view_count, nullptr);
    std::vector<XrViewConfigurationView> config(view_count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, view_count, &view_count, config.data());
    std::uint32_t format_count = 0;
    xrEnumerateSwapchainFormats(session, 0, &format_count, nullptr);
    std::vector<std::int64_t> formats(format_count);
    xrEnumerateSwapchainFormats(session, format_count, &format_count, formats.data());
    const std::int64_t wanted = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    bool has_wanted = false;
    for (const auto f : formats) has_wanted = has_wanted || f == wanted;
    if (!has_wanted) {
        std::fprintf(stderr, "openrts-xr-probe: the runtime offers no R8G8B8A8_UNORM_SRGB swapchain\n");
        return 1;
    }

    // --- wgpu on Vulkan.
    const auto gpu = make_gpu(!cpu);
    if (!gpu.device) {
        std::fprintf(stderr, "openrts-xr-probe: no wgpu device on Vulkan\n");
        return 1;
    }
    const auto device = gpu.device;
    const auto queue = wgpuDeviceGetQueue(device);
    std::printf("wgpu on Vulkan, the frame %s\n", cpu ? "through the CPU" : "through a shared texture (GPU only)");
    const WGPUShaderSourceWGSL source{.chain = {.sType = WGPUSType_ShaderSourceWGSL}, .code = wgsl_name(scene_wgsl)};
    const WGPUShaderModuleDescriptor shader_descriptor{.nextInChain = &source.chain};
    const auto shader = wgpuDeviceCreateShaderModule(device, &shader_descriptor);
    WGPUBindGroupLayoutEntry entry{};
    entry.binding = 0;
    entry.visibility = WGPUShaderStage_Fragment;
    entry.buffer.type = WGPUBufferBindingType_Uniform;
    entry.buffer.minBindingSize = 80;
    const WGPUBindGroupLayoutDescriptor layout_descriptor{.entryCount = 1, .entries = &entry};
    const auto group_layout = wgpuDeviceCreateBindGroupLayout(device, &layout_descriptor);
    const WGPUPipelineLayoutDescriptor pipeline_layout_descriptor{.bindGroupLayoutCount = 1, .bindGroupLayouts = &group_layout};
    const auto pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &pipeline_layout_descriptor);
    const WGPUColorTargetState target{.format = WGPUTextureFormat_RGBA8UnormSrgb, .writeMask = WGPUColorWriteMask_All};
    const WGPUFragmentState fragment{.module = shader, .entryPoint = wgsl_name("fs_main"), .targetCount = 1, .targets = &target};
    WGPURenderPipelineDescriptor pipeline_descriptor{};
    pipeline_descriptor.layout = pipeline_layout;
    pipeline_descriptor.vertex = {.module = shader, .entryPoint = wgsl_name("vs_main")};
    pipeline_descriptor.primitive = {.topology = WGPUPrimitiveTopology_TriangleList};
    pipeline_descriptor.multisample = {.count = 1, .mask = ~0u};
    pipeline_descriptor.fragment = &fragment;
    const auto pipeline = wgpuDeviceCreateRenderPipeline(device, &pipeline_descriptor);
    if (!pipeline) {
        std::fprintf(stderr, "openrts-xr-probe: the pipeline failed\n");
        return 1;
    }

    // --- Each eye: its swapchain, its wgpu target and readback.
    std::vector<EyeTarget> eyes(view_count);
    for (std::uint32_t v = 0; v < view_count; ++v) {
        auto& e = eyes[v];
        e.width = config[v].recommendedImageRectWidth;
        e.height = config[v].recommendedImageRectHeight;
        XrSwapchainCreateInfo chain_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        chain_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        chain_info.format = wanted;
        chain_info.sampleCount = 1;
        chain_info.width = e.width;
        chain_info.height = e.height;
        chain_info.faceCount = 1;
        chain_info.arraySize = 1;
        chain_info.mipCount = 1;
        if (!check(xrCreateSwapchain(session, &chain_info, &e.swapchain), "xrCreateSwapchain", instance)) return 1;
        std::uint32_t image_count = 0;
        xrEnumerateSwapchainImages(e.swapchain, 0, &image_count, nullptr);
        e.images.assign(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages(e.swapchain, image_count, &image_count, reinterpret_cast<XrSwapchainImageBaseHeader*>(e.images.data()));
        const WGPUTextureDescriptor texture_descriptor{
            .usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc,
            .dimension = WGPUTextureDimension_2D,
            .size = {e.width, e.height, 1},
            .format = WGPUTextureFormat_RGBA8UnormSrgb,
            .mipLevelCount = 1,
            .sampleCount = 1,
        };
#ifdef OPENRTS_XR_SHARED
        if (!cpu) {
            // A D3D11 texture shared by an NT handle, wgpu's over it.
            D3D11_TEXTURE2D_DESC shared_desc{};
            shared_desc.Width = e.width;
            shared_desc.Height = e.height;
            shared_desc.MipLevels = 1;
            shared_desc.ArraySize = 1;
            shared_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            shared_desc.SampleDesc.Count = 1;
            shared_desc.Usage = D3D11_USAGE_DEFAULT;
            shared_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            shared_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
            IDXGIResource1* resource = nullptr;
            if (SUCCEEDED(d3d->CreateTexture2D(&shared_desc, nullptr, &e.shared)) &&
                SUCCEEDED(e.shared->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&resource)))) {
                resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &e.shared_handle);
                resource->Release();
            }
            if (e.shared_handle) e.texture = wgpuDeviceImportD3D11SharedTexture(device, e.shared_handle, &texture_descriptor);
            if (!e.texture) {
                std::fprintf(stderr, "openrts-xr-probe: the shared texture could not be made; through the CPU\n");
                cpu = true;
            }
        }
#endif
        if (!e.texture) e.texture = wgpuDeviceCreateTexture(device, &texture_descriptor);
        e.view = wgpuTextureCreateView(e.texture, nullptr);
        e.row_bytes = (e.width * 4 + 255) / 256 * 256;
        const WGPUBufferDescriptor read{.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst,
                                        .size = static_cast<std::uint64_t>(e.row_bytes) * e.height};
        e.readback = wgpuDeviceCreateBuffer(device, &read);
        const WGPUBufferDescriptor uniform{.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, .size = 80};
        e.uniform = wgpuDeviceCreateBuffer(device, &uniform);
        WGPUBindGroupEntry group_entry{};
        group_entry.binding = 0;
        group_entry.buffer = e.uniform;
        group_entry.size = 80;
        const WGPUBindGroupDescriptor group{.layout = group_layout, .entryCount = 1, .entries = &group_entry};
        e.group = wgpuDeviceCreateBindGroup(device, &group);
        std::printf("eye %u: %u x %u, %u images\n", v, e.width, e.height, image_count);
    }

    // --- The frame loop.
    bool running = false, quit = false;
    const auto started = std::chrono::steady_clock::now();
    std::size_t frames = 0;
    double draw_ms_sum = 0.0;
    std::vector<XrView> views(view_count, {XR_TYPE_VIEW});
    while (!quit) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) quit = true;
        if (seconds > 0.0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() > seconds) quit = true;
        if (quit && running) xrRequestExitSession(session);
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(instance, &event) == XR_SUCCESS) {
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const auto& changed = *reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
                std::printf("session state %d\n", static_cast<int>(changed.state));
                if (changed.state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                    begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    running = check(xrBeginSession(session, &begin), "xrBeginSession", instance);
                } else if (changed.state == XR_SESSION_STATE_STOPPING) {
                    xrEndSession(session);
                    running = false;
                    quit = true;
                } else if (changed.state == XR_SESSION_STATE_EXITING || changed.state == XR_SESSION_STATE_LOSS_PENDING) {
                    running = false;
                    quit = true;
                }
            }
            event = {XR_TYPE_EVENT_DATA_BUFFER};
        }
        if (!running) {
            if (!quit) Sleep(10);
            continue;
        }
        XrFrameState frame_state{XR_TYPE_FRAME_STATE};
        if (!check(xrWaitFrame(session, nullptr, &frame_state), "xrWaitFrame", instance)) break;
        xrBeginFrame(session, nullptr);
        std::vector<XrCompositionLayerProjectionView> layer_views(view_count, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
        bool drawn = false;
        if (frame_state.shouldRender) {
            XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
            locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            locate.displayTime = frame_state.predictedDisplayTime;
            locate.space = space;
            XrViewState view_state{XR_TYPE_VIEW_STATE};
            std::uint32_t located = 0;
            xrLocateViews(session, &locate, &view_state, view_count, &located, views.data());
            const auto t0 = std::chrono::steady_clock::now();
            for (std::uint32_t v = 0; v < view_count && v < located; ++v) {
                auto& e = eyes[v];
                // The eye's matrices: the world to the eye's clip space, inverted for the rays.
                const glm::mat4 eye_to_space = pose_matrix(views[v].pose);
                const glm::mat4 view_projection = projection(views[v].fov, 0.05f, 1000.0f) * glm::inverse(eye_to_space);
                float data[20];
                const glm::mat4 inverse = glm::inverse(view_projection);
                std::memcpy(data, &inverse[0][0], 64);
                data[16] = views[v].pose.position.x;
                data[17] = views[v].pose.position.y;
                data[18] = views[v].pose.position.z;
                data[19] = 1.0f;
                wgpuQueueWriteBuffer(queue, e.uniform, 0, data, sizeof data);
                const auto encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);
                WGPURenderPassColorAttachment colour{};
                colour.view = e.view;
                colour.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
                colour.loadOp = WGPULoadOp_Clear;
                colour.storeOp = WGPUStoreOp_Store;
                colour.clearValue = {0.0, 0.0, 0.0, 1.0};
                const WGPURenderPassDescriptor pass_descriptor{.colorAttachmentCount = 1, .colorAttachments = &colour};
                const auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_descriptor);
                wgpuRenderPassEncoderSetPipeline(pass, pipeline);
                wgpuRenderPassEncoderSetBindGroup(pass, 0, e.group, 0, nullptr);
                wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
                wgpuRenderPassEncoderEnd(pass);
                wgpuRenderPassEncoderRelease(pass);
                if (cpu) {
                    const WGPUTexelCopyTextureInfo from{.texture = e.texture, .mipLevel = 0, .origin = {0, 0, 0}, .aspect = WGPUTextureAspect_All};
                    const WGPUTexelCopyBufferInfo to{.layout = {.offset = 0, .bytesPerRow = e.row_bytes, .rowsPerImage = e.height}, .buffer = e.readback};
                    const WGPUExtent3D size{e.width, e.height, 1};
                    wgpuCommandEncoderCopyTextureToBuffer(encoder, &from, &to, &size);
                }
                const auto commands = wgpuCommandEncoderFinish(encoder, nullptr);
                const auto submitted = wgpuQueueSubmitForIndex(queue, 1, &commands);
                wgpuCommandBufferRelease(commands);
                wgpuCommandEncoderRelease(encoder);
                if (!cpu) {
                    // The GPU done with it (wgpu's Vulkan queue), then D3D11 copies
                    // it into the swapchain's image, on the GPU.
                    wgpuDevicePoll(device, true, &submitted);
                    std::uint32_t index = 0;
                    xrAcquireSwapchainImage(e.swapchain, nullptr, &index);
                    XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                    wait_info.timeout = XR_INFINITE_DURATION;
                    xrWaitSwapchainImage(e.swapchain, &wait_info);
                    d3d_context->CopyResource(e.images[index].texture, e.shared);
                    xrReleaseSwapchainImage(e.swapchain, nullptr);
                    auto& lv = layer_views[v];
                    lv.pose = views[v].pose;
                    lv.fov = views[v].fov;
                    lv.subImage.swapchain = e.swapchain;
                    lv.subImage.imageRect = {{0, 0}, {static_cast<std::int32_t>(e.width), static_cast<std::int32_t>(e.height)}};
                    continue;
                }
                // Its pixels back on the CPU.
                bool done = false, mapped = false;
                struct Wait {
                    bool* done;
                    bool* mapped;
                } wait{&done, &mapped};
                const WGPUBufferMapCallbackInfo callback{
                    .mode = WGPUCallbackMode_AllowProcessEvents,
                    .callback = [](const WGPUMapAsyncStatus status, WGPUStringView, void* data, void*) {
                        auto* w = static_cast<Wait*>(data);
                        *w->mapped = status == WGPUMapAsyncStatus_Success;
                        *w->done = true;
                    },
                    .userdata1 = &wait,
                };
                wgpuBufferMapAsync(e.readback, WGPUMapMode_Read, 0, static_cast<std::uint64_t>(e.row_bytes) * e.height, callback);
                for (int k = 0; k < 10000 && !done; ++k) {
                    wgpuDevicePoll(device, true, nullptr);
                    wgpuInstanceProcessEvents(gpu.instance);
                }
                // Into the swapchain's image.
                std::uint32_t index = 0;
                xrAcquireSwapchainImage(e.swapchain, nullptr, &index);
                XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wait_info.timeout = XR_INFINITE_DURATION;
                xrWaitSwapchainImage(e.swapchain, &wait_info);
                if (mapped) {
                    const auto* pixels = wgpuBufferGetConstMappedRange(e.readback, 0, static_cast<std::size_t>(e.row_bytes) * e.height);
                    d3d_context->UpdateSubresource(e.images[index].texture, 0, nullptr, pixels, e.row_bytes, 0);
                    wgpuBufferUnmap(e.readback);
                }
                xrReleaseSwapchainImage(e.swapchain, nullptr);
                auto& lv = layer_views[v];
                lv.pose = views[v].pose;
                lv.fov = views[v].fov;
                lv.subImage.swapchain = e.swapchain;
                lv.subImage.imageRect = {{0, 0}, {static_cast<std::int32_t>(e.width), static_cast<std::int32_t>(e.height)}};
            }
            draw_ms_sum += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            ++frames;
            drawn = located == view_count;
        }
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        layer.space = space;
        layer.viewCount = view_count;
        layer.views = layer_views.data();
        const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer)};
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.displayTime = frame_state.predictedDisplayTime;
        end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end.layerCount = drawn ? 1 : 0;
        end.layers = drawn ? layers : nullptr;
        check(xrEndFrame(session, &end), "xrEndFrame", instance);
        if (frames > 0 && frames % 300 == 0)
            std::printf("%zu frames, both eyes drawn and copied in %.1f ms a frame\n", frames, draw_ms_sum / static_cast<double>(frames));
    }
    if (frames > 0) std::printf("done: %zu frames, %.1f ms a frame for both eyes\n", frames, draw_ms_sum / static_cast<double>(frames));
    for (auto& e : eyes) {
        if (e.swapchain) xrDestroySwapchain(e.swapchain);
        if (e.group) wgpuBindGroupRelease(e.group);
        if (e.uniform) wgpuBufferRelease(e.uniform);
        if (e.readback) wgpuBufferRelease(e.readback);
        if (e.view) wgpuTextureViewRelease(e.view);
        if (e.texture) wgpuTextureRelease(e.texture);
        if (e.shared_handle) CloseHandle(e.shared_handle);
        if (e.shared) e.shared->Release();
    }
    xrDestroySpace(space);
    xrDestroySession(session);
    xrDestroyInstance(instance);
    d3d_context->Release();
    d3d->Release();
    if (adapter) adapter->Release();
    if (factory) factory->Release();
    return 0;
}
