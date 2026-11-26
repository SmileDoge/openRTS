#include "vr_session.hpp"

#ifdef OPENRTS_HAS_XR
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <webgpu/wgpu.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

#include <openrts/renderer/depth.hpp>

namespace openrts::viewer {
namespace {

WGPUStringView name(const char* text) { return {text, std::strlen(text)}; }

const char* mirror_wgsl = R"(
@group(0) @binding(0) var picture: texture_2d<f32>;
@group(0) @binding(1) var smooth_sampler: sampler;
struct Out {
    @builtin(position) clip: vec4<f32>,
    @location(0) uv: vec2<f32>,
};
@vertex
fn vs_main(@builtin(vertex_index) i: u32) -> Out {
    let p = vec2<f32>(f32((i << 1u) & 2u), f32(i & 2u)) * 2.0 - 1.0;
    var out: Out;
    out.clip = vec4<f32>(p, 0.0, 1.0);
    out.uv = vec2<f32>(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    return out;
}
@fragment
fn fs_main(input: Out) -> @location(0) vec4<f32> {
    return textureSample(picture, smooth_sampler, input.uv);
}
)";

glm::mat4 pose_matrix(const XrPosef& p) {
    const glm::quat q{p.orientation.w, p.orientation.x, p.orientation.y, p.orientation.z};
    glm::mat4 m = glm::mat4_cast(q);
    m[3] = glm::vec4{p.position.x, p.position.y, p.position.z, 1.0f};
    return m;
}

bool ok(const XrResult r, const char* what, std::string* error = nullptr) {
    if (XR_SUCCEEDED(r)) return true;
    const auto text = std::string(what) + " failed (" + std::to_string(static_cast<int>(r)) + ")";
    if (error) *error = text;
    std::cerr << "VR: " << text << std::endl;
    return false;
}

} // namespace

struct VrSession::Impl {
    WGPUDevice device = nullptr;
    WGPUInstance wgpu_instance = nullptr;
    WGPUTextureFormat format = WGPUTextureFormat_BGRA8Unorm;
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace space = XR_NULL_HANDLE;
    ID3D11Device* d3d = nullptr;
    ID3D11DeviceContext* d3d_context = nullptr;
    bool running = false, ended = false, frame_begun = false;
    XrFrameState frame_state{XR_TYPE_FRAME_STATE};
    std::string runtime;
    struct EyeData {
        Eye eye;
        XrSwapchain swapchain = XR_NULL_HANDLE;
        std::vector<XrSwapchainImageD3D11KHR> images;
        ID3D11Texture2D* shared = nullptr;
        HANDLE shared_handle = nullptr;
        WGPUTexture depth_texture = nullptr, msaa_texture = nullptr;
        XrView view{XR_TYPE_VIEW};
        bool drawn = false;
    };
    std::vector<EyeData> eyes;
    // recenter(): the headset's space to the seat's, and asked for next frame.
    glm::mat4 centre{1.0f};
    bool recenter_asked = false;
    // The controllers' actions.
    XrActionSet actions = XR_NULL_HANDLE;
    XrAction recenter_action = XR_NULL_HANDLE;
    bool recenter_pressed = false;
    // The targets as last made: remade when the multisampling or HDR changes.
    std::uint32_t made_samples = 0;
    bool made_hdr = false;
    // The window's mirror.
    WGPURenderPipeline mirror = nullptr;
    WGPUSampler sampler = nullptr;
    WGPUBindGroup mirror_group = nullptr;

    void release_targets(EyeData& e) {
        for (auto* view : {&e.eye.depth, &e.eye.msaa, &e.eye.hdr})
            if (*view) {
                wgpuTextureViewRelease(*view);
                *view = nullptr;
            }
        for (auto* texture : {&e.depth_texture, &e.msaa_texture, &e.eye.hdr_texture})
            if (*texture) {
                wgpuTextureRelease(*texture);
                *texture = nullptr;
            }
    }

    // Each eye's depth, multisampled colour and HDR frame, as the window's (PresentationSurface).
    void make_targets() {
        const auto samples = renderer::render_sample_count();
        const bool hdr = renderer::scene_hdr();
        if (samples == made_samples && hdr == made_hdr) return;
        made_samples = samples;
        made_hdr = hdr;
        for (auto& e : eyes) {
            release_targets(e);
            const WGPUExtent3D size{e.eye.width, e.eye.height, 1};
            const WGPUTextureDescriptor depth{.usage = WGPUTextureUsage_RenderAttachment, .dimension = WGPUTextureDimension_2D, .size = size,
                                              .format = renderer::depth_format, .mipLevelCount = 1, .sampleCount = samples};
            e.depth_texture = wgpuDeviceCreateTexture(device, &depth);
            e.eye.depth = wgpuTextureCreateView(e.depth_texture, nullptr);
            const auto scene_format = hdr ? renderer::hdr_format : format;
            if (samples > 1) {
                const WGPUTextureDescriptor msaa{.usage = WGPUTextureUsage_RenderAttachment, .dimension = WGPUTextureDimension_2D, .size = size,
                                                 .format = scene_format, .mipLevelCount = 1, .sampleCount = samples};
                e.msaa_texture = wgpuDeviceCreateTexture(device, &msaa);
                e.eye.msaa = wgpuTextureCreateView(e.msaa_texture, nullptr);
            }
            if (hdr) {
                const WGPUTextureDescriptor frame{.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc,
                                                  .dimension = WGPUTextureDimension_2D, .size = size, .format = renderer::hdr_format,
                                                  .mipLevelCount = 1, .sampleCount = 1};
                e.eye.hdr_texture = wgpuDeviceCreateTexture(device, &frame);
                e.eye.hdr = wgpuTextureCreateView(e.eye.hdr_texture, nullptr);
            }
        }
    }

    ~Impl() {
        if (mirror_group) wgpuBindGroupRelease(mirror_group);
        if (sampler) wgpuSamplerRelease(sampler);
        if (mirror) wgpuRenderPipelineRelease(mirror);
        for (auto& e : eyes) {
            release_targets(e);
            if (e.eye.colour) wgpuTextureViewRelease(e.eye.colour);
            if (e.eye.colour_texture) wgpuTextureRelease(e.eye.colour_texture);
            if (e.swapchain) xrDestroySwapchain(e.swapchain);
            if (e.shared_handle) CloseHandle(e.shared_handle);
            if (e.shared) e.shared->Release();
        }
        if (actions) xrDestroyActionSet(actions);
        if (space) xrDestroySpace(space);
        if (session) xrDestroySession(session);
        if (instance) xrDestroyInstance(instance);
        if (d3d_context) d3d_context->Release();
        if (d3d) d3d->Release();
    }
};

VrSession::VrSession() : impl_(std::make_unique<Impl>()) {}
VrSession::~VrSession() = default;

std::unique_ptr<VrSession> VrSession::start(const WGPUInstance wgpu_instance, const WGPUDevice device, const WGPUTextureFormat colour_format,
                                            const float scale, std::string& error) {
    std::unique_ptr<VrSession> made(new VrSession());
    auto& s = *made->impl_;
    s.device = device;
    s.wgpu_instance = wgpu_instance;
    s.format = colour_format;
    const bool bgra = colour_format == WGPUTextureFormat_BGRA8Unorm;
    if (!bgra && colour_format != WGPUTextureFormat_RGBA8Unorm) {
        error = "the window's format is neither BGRA8 nor RGBA8";
        return nullptr;
    }
    const char* extensions[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo instance_info{XR_TYPE_INSTANCE_CREATE_INFO};
    std::snprintf(instance_info.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "openRTS");
    std::snprintf(instance_info.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "openRTS");
    instance_info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    instance_info.enabledExtensionCount = 1;
    instance_info.enabledExtensionNames = extensions;
    if (!ok(xrCreateInstance(&instance_info, &s.instance), "xrCreateInstance (is the headset's app or SteamVR running?)", &error)) return nullptr;
    XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(s.instance, &properties);
    s.runtime = properties.runtimeName;
    XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!ok(xrGetSystem(s.instance, &system_info, &s.system), "xrGetSystem (is the headset connected?)", &error)) return nullptr;
    PFN_xrGetD3D11GraphicsRequirementsKHR get_requirements = nullptr;
    xrGetInstanceProcAddr(s.instance, "xrGetD3D11GraphicsRequirementsKHR", reinterpret_cast<PFN_xrVoidFunction*>(&get_requirements));
    XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (!get_requirements || !ok(get_requirements(s.instance, s.system, &requirements), "xrGetD3D11GraphicsRequirementsKHR", &error)) return nullptr;
    // D3D11 on the adapter the runtime wants (the same GPU as wgpu's).
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
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const HRESULT created = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                              D3D11_SDK_VERSION, &s.d3d, nullptr, &s.d3d_context);
    if (adapter) adapter->Release();
    if (factory) factory->Release();
    if (FAILED(created)) {
        error = "D3D11CreateDevice failed";
        return nullptr;
    }
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = s.d3d;
    XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
    session_info.next = &binding;
    session_info.systemId = s.system;
    if (!ok(xrCreateSession(s.instance, &session_info, &s.session), "xrCreateSession", &error)) return nullptr;
    XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    space_info.poseInReferenceSpace.orientation.w = 1.0f;
    if (!ok(xrCreateReferenceSpace(s.session, &space_info, &s.space), "xrCreateReferenceSpace", &error)) return nullptr;
    // The controllers: the view centred again by a button (no controllers else).
    {
        XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
        std::snprintf(set_info.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "cab");
        std::snprintf(set_info.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE, "Cab");
        if (ok(xrCreateActionSet(s.instance, &set_info, &s.actions), "xrCreateActionSet")) {
            XrActionCreateInfo action_info{XR_TYPE_ACTION_CREATE_INFO};
            action_info.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
            std::snprintf(action_info.actionName, XR_MAX_ACTION_NAME_SIZE, "recenter");
            std::snprintf(action_info.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "Recenter the view");
            ok(xrCreateAction(s.actions, &action_info, &s.recenter_action), "xrCreateAction");
            const auto path = [&](const char* text) {
                XrPath p = XR_NULL_PATH;
                xrStringToPath(s.instance, text, &p);
                return p;
            };
            const auto suggest = [&](const char* profile, std::initializer_list<const char*> inputs) {
                std::vector<XrActionSuggestedBinding> bindings;
                for (const auto* input : inputs) bindings.push_back({s.recenter_action, path(input)});
                XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
                suggested.interactionProfile = path(profile);
                suggested.countSuggestedBindings = static_cast<std::uint32_t>(bindings.size());
                suggested.suggestedBindings = bindings.data();
                xrSuggestInteractionProfileBindings(s.instance, &suggested);
            };
            suggest("/interaction_profiles/oculus/touch_controller",
                    {"/user/hand/left/input/menu/click", "/user/hand/left/input/thumbstick/click", "/user/hand/right/input/thumbstick/click"});
            suggest("/interaction_profiles/valve/index_controller",
                    {"/user/hand/left/input/thumbstick/click", "/user/hand/right/input/thumbstick/click"});
            suggest("/interaction_profiles/htc/vive_controller", {"/user/hand/left/input/menu/click", "/user/hand/right/input/menu/click"});
            suggest("/interaction_profiles/khr/simple_controller", {"/user/hand/left/input/menu/click", "/user/hand/right/input/menu/click"});
            XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
            attach.countActionSets = 1;
            attach.actionSets = &s.actions;
            ok(xrAttachSessionActionSets(s.session, &attach), "xrAttachSessionActionSets");
        }
    }

    std::uint32_t count = 0;
    xrEnumerateViewConfigurationViews(s.instance, s.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr);
    std::vector<XrViewConfigurationView> config(count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(s.instance, s.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count, &count, config.data());
    std::uint32_t format_count = 0;
    xrEnumerateSwapchainFormats(s.session, 0, &format_count, nullptr);
    std::vector<std::int64_t> formats(format_count);
    xrEnumerateSwapchainFormats(s.session, format_count, &format_count, formats.data());
    // The swapchain's: the same bytes as the eyes' (display values: sRGB).
    const DXGI_FORMAT shared_format = bgra ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    std::int64_t chain_format = 0;
    for (const auto wanted : {bgra ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, shared_format})
        for (const auto f : formats)
            if (!chain_format && f == wanted) chain_format = f;
    if (!chain_format) {
        error = "the runtime offers no 8-bit swapchain of the window's channel order";
        return nullptr;
    }
    s.eyes.resize(count);
    for (std::uint32_t v = 0; v < count; ++v) {
        auto& e = s.eyes[v];
        const auto scaled = [&](const std::uint32_t recommended, const std::uint32_t most) {
            const auto wanted = static_cast<std::uint32_t>(static_cast<float>(recommended) * std::clamp(scale, 0.5f, 2.5f) + 0.5f);
            return (std::min)(wanted, (std::max)(most, recommended));
        };
        e.eye.width = scaled(config[v].recommendedImageRectWidth, config[v].maxImageRectWidth);
        e.eye.height = scaled(config[v].recommendedImageRectHeight, config[v].maxImageRectHeight);
        XrSwapchainCreateInfo chain_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        chain_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        chain_info.format = chain_format;
        chain_info.sampleCount = 1;
        chain_info.width = e.eye.width;
        chain_info.height = e.eye.height;
        chain_info.faceCount = 1;
        chain_info.arraySize = 1;
        chain_info.mipCount = 1;
        if (!ok(xrCreateSwapchain(s.session, &chain_info, &e.swapchain), "xrCreateSwapchain", &error)) return nullptr;
        std::uint32_t images = 0;
        xrEnumerateSwapchainImages(e.swapchain, 0, &images, nullptr);
        e.images.assign(images, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages(e.swapchain, images, &images, reinterpret_cast<XrSwapchainImageBaseHeader*>(e.images.data()));
        // The eye's colour: a D3D11 texture shared with wgpu's device.
        D3D11_TEXTURE2D_DESC shared_desc{};
        shared_desc.Width = e.eye.width;
        shared_desc.Height = e.eye.height;
        shared_desc.MipLevels = 1;
        shared_desc.ArraySize = 1;
        shared_desc.Format = shared_format;
        shared_desc.SampleDesc.Count = 1;
        shared_desc.Usage = D3D11_USAGE_DEFAULT;
        shared_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        shared_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
        IDXGIResource1* resource = nullptr;
        if (SUCCEEDED(s.d3d->CreateTexture2D(&shared_desc, nullptr, &e.shared)) &&
            SUCCEEDED(e.shared->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&resource)))) {
            resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &e.shared_handle);
            resource->Release();
        }
        const WGPUTextureDescriptor colour{
            .usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc,
            .dimension = WGPUTextureDimension_2D,
            .size = {e.eye.width, e.eye.height, 1},
            .format = colour_format,
            .mipLevelCount = 1,
            .sampleCount = 1,
        };
        if (e.shared_handle) e.eye.colour_texture = wgpuDeviceImportD3D11SharedTexture(device, e.shared_handle, &colour);
        if (!e.eye.colour_texture) {
            error = "the eye's shared texture could not be made (wgpu on Vulkan, openRTS's wgpu-native?)";
            return nullptr;
        }
        e.eye.colour = wgpuTextureCreateView(e.eye.colour_texture, nullptr);
    }
    s.make_targets();
    std::cout << "VR: " << s.runtime << ", " << count << " eyes of " << (count ? s.eyes[0].eye.width : 0) << " x "
              << (count ? s.eyes[0].eye.height : 0) << std::endl;
    return made;
}

std::size_t VrSession::eye_count() const noexcept { return impl_->eyes.size(); }
const VrSession::Eye& VrSession::eye(const std::size_t i) const { return impl_->eyes[i].eye; }
bool VrSession::ended() const noexcept { return impl_->ended; }
const std::string& VrSession::runtime() const noexcept { return impl_->runtime; }

bool VrSession::begin_frame() {
    auto& s = *impl_;
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(s.instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto& changed = *reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
            if (changed.state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                s.running = ok(xrBeginSession(s.session, &begin), "xrBeginSession");
            } else if (changed.state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(s.session);
                s.running = false;
            } else if (changed.state == XR_SESSION_STATE_EXITING || changed.state == XR_SESSION_STATE_LOSS_PENDING) {
                s.running = false;
                s.ended = true;
            }
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    if (!s.running) return false;
    s.recenter_pressed = false;
    if (s.actions && s.recenter_action) {
        const XrActiveActionSet active{s.actions, XR_NULL_PATH};
        XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
        sync.countActiveActionSets = 1;
        sync.activeActionSets = &active;
        if (XR_SUCCEEDED(xrSyncActions(s.session, &sync))) {
            XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
            get.action = s.recenter_action;
            XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
            if (XR_SUCCEEDED(xrGetActionStateBoolean(s.session, &get, &state)) && state.isActive && state.changedSinceLastSync &&
                state.currentState) {
                s.recenter_pressed = true;
                s.recenter_asked = true;
            }
        }
    }
    s.frame_state = {XR_TYPE_FRAME_STATE};
    if (!ok(xrWaitFrame(s.session, nullptr, &s.frame_state), "xrWaitFrame")) return false;
    xrBeginFrame(s.session, nullptr);
    s.frame_begun = true;
    for (auto& e : s.eyes) e.drawn = false;
    if (!s.frame_state.shouldRender) {
        end_frame();
        return false;
    }
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = s.frame_state.predictedDisplayTime;
    locate.space = s.space;
    XrViewState view_state{XR_TYPE_VIEW_STATE};
    std::vector<XrView> views(s.eyes.size(), {XR_TYPE_VIEW});
    std::uint32_t located = 0;
    xrLocateViews(s.session, &locate, &view_state, static_cast<std::uint32_t>(views.size()), &located, views.data());
    if (located != s.eyes.size() || !(view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
        end_frame();
        return false;
    }
    s.make_targets();
    if (s.recenter_asked) {
        // The head between the eyes, its turn about the vertical only.
        s.recenter_asked = false;
        glm::vec3 middle{0.0f};
        for (const auto& v : views) middle += glm::vec3{v.pose.position.x, v.pose.position.y, v.pose.position.z};
        middle /= static_cast<float>(views.size());
        const glm::vec3 ahead = glm::mat3{pose_matrix(views[0].pose)} * glm::vec3{0.0f, 0.0f, -1.0f};
        const float yaw = std::atan2(-ahead.x, -ahead.z);
        s.centre = glm::inverse(glm::translate(glm::mat4{1.0f}, middle) * glm::rotate(glm::mat4{1.0f}, yaw, glm::vec3{0.0f, 1.0f, 0.0f}));
    }
    for (std::size_t i = 0; i < s.eyes.size(); ++i) {
        auto& e = s.eyes[i];
        e.view = views[i];
        e.eye.pose = s.centre * pose_matrix(views[i].pose);
        e.eye.left = std::tan(views[i].fov.angleLeft);
        e.eye.right = std::tan(views[i].fov.angleRight);
        e.eye.up = std::tan(views[i].fov.angleUp);
        e.eye.down = std::tan(views[i].fov.angleDown);
    }
    return true;
}

void VrSession::recenter() { impl_->recenter_asked = true; }
bool VrSession::recenter_pressed() const noexcept { return impl_->recenter_pressed; }

void VrSession::eye_done(const std::size_t i, const std::uint64_t submission) {
    auto& s = *impl_;
    auto& e = s.eyes[i];
    // The GPU done with it on wgpu's side, then D3D11 copies it into the
    // swapchain's image on the GPU.
    const WGPUSubmissionIndex index = submission;
    wgpuDevicePoll(s.device, true, &index);
    std::uint32_t image = 0;
    if (!ok(xrAcquireSwapchainImage(e.swapchain, nullptr, &image), "xrAcquireSwapchainImage")) return;
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(e.swapchain, &wait);
    s.d3d_context->CopyResource(e.images[image].texture, e.shared);
    xrReleaseSwapchainImage(e.swapchain, nullptr);
    e.drawn = true;
}

void VrSession::end_frame() {
    auto& s = *impl_;
    if (!s.frame_begun) return;
    s.frame_begun = false;
    std::vector<XrCompositionLayerProjectionView> views(s.eyes.size(), {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
    bool all = !s.eyes.empty();
    for (std::size_t i = 0; i < s.eyes.size(); ++i) {
        const auto& e = s.eyes[i];
        all = all && e.drawn;
        views[i].pose = e.view.pose;
        views[i].fov = e.view.fov;
        views[i].subImage.swapchain = e.swapchain;
        views[i].subImage.imageRect = {{0, 0}, {static_cast<std::int32_t>(e.eye.width), static_cast<std::int32_t>(e.eye.height)}};
    }
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = s.space;
    layer.viewCount = static_cast<std::uint32_t>(views.size());
    layer.views = views.data();
    const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer)};
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = s.frame_state.predictedDisplayTime;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount = all ? 1 : 0;
    end.layers = all ? layers : nullptr;
    ok(xrEndFrame(s.session, &end), "xrEndFrame");
}

void VrSession::mirror(const WGPUCommandEncoder encoder, const WGPUTextureView target) {
    auto& s = *impl_;
    if (s.eyes.empty()) return;
    if (!s.mirror) {
        const WGPUShaderSourceWGSL source{.chain = {.sType = WGPUSType_ShaderSourceWGSL}, .code = name(mirror_wgsl)};
        const WGPUShaderModuleDescriptor shader_descriptor{.nextInChain = &source.chain};
        const auto shader = wgpuDeviceCreateShaderModule(s.device, &shader_descriptor);
        const WGPUColorTargetState colour{.format = s.format, .writeMask = WGPUColorWriteMask_All};
        const WGPUFragmentState fragment{.module = shader, .entryPoint = name("fs_main"), .targetCount = 1, .targets = &colour};
        WGPURenderPipelineDescriptor descriptor{};
        descriptor.vertex = {.module = shader, .entryPoint = name("vs_main")};
        descriptor.primitive = {.topology = WGPUPrimitiveTopology_TriangleList};
        descriptor.multisample = {.count = 1, .mask = ~0u};
        descriptor.fragment = &fragment;
        s.mirror = wgpuDeviceCreateRenderPipeline(s.device, &descriptor);
        wgpuShaderModuleRelease(shader);
        const WGPUSamplerDescriptor sampler{.addressModeU = WGPUAddressMode_ClampToEdge, .addressModeV = WGPUAddressMode_ClampToEdge,
                                            .addressModeW = WGPUAddressMode_ClampToEdge, .magFilter = WGPUFilterMode_Linear,
                                            .minFilter = WGPUFilterMode_Linear, .mipmapFilter = WGPUMipmapFilterMode_Nearest,
                                            .lodMinClamp = 0.0f, .lodMaxClamp = 1.0f, .maxAnisotropy = 1};
        s.sampler = wgpuDeviceCreateSampler(s.device, &sampler);
        const auto layout = wgpuRenderPipelineGetBindGroupLayout(s.mirror, 0);
        WGPUBindGroupEntry entries[2]{};
        entries[0].binding = 0;
        entries[0].textureView = s.eyes[0].eye.colour;
        entries[1].binding = 1;
        entries[1].sampler = s.sampler;
        const WGPUBindGroupDescriptor group{.layout = layout, .entryCount = 2, .entries = entries};
        s.mirror_group = wgpuDeviceCreateBindGroup(s.device, &group);
        wgpuBindGroupLayoutRelease(layout);
    }
    WGPURenderPassColorAttachment colour{};
    colour.view = target;
    colour.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    colour.loadOp = WGPULoadOp_Clear;
    colour.storeOp = WGPUStoreOp_Store;
    const WGPURenderPassDescriptor pass_descriptor{.colorAttachmentCount = 1, .colorAttachments = &colour};
    const auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_descriptor);
    wgpuRenderPassEncoderSetPipeline(pass, s.mirror);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, s.mirror_group, 0, nullptr);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
}

} // namespace openrts::viewer
#endif
