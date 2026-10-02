#include <iostream>
#include <filesystem>
#include <string_view>

#include <openrts/assets/assets.hpp>
#include <openrts/renderer/renderer.hpp>
#ifdef OPENRTS_HAS_GLFW
#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif
#endif
#ifdef OPENRTS_HAS_WGPU
#include <openrts/formats/ace_file.hpp>
#include <openrts/renderer/webgpu_context.hpp>
#include <openrts/renderer/webgpu_texture.hpp>

static WGPURenderPipeline create_pipeline(WGPUDevice d, WGPUTextureFormat f, WGPUBindGroupLayout* out) {
 const char* s="@group(0) @binding(0) var t:texture_2d<f32>; @group(0) @binding(1) var sm:sampler; struct O{@builtin(position)p:vec4f,@location(0)u:vec2f}; @vertex fn vs(@builtin(vertex_index)i:u32)->O{var p=array<vec2f,3>(vec2f(-1.,-1.),vec2f(3.,-1.),vec2f(-1.,3.));var o:O;o.p=vec4f(p[i],0.,1.);o.u=(p[i]+vec2f(1.))/2.;return o;} @fragment fn fs(i:O)->@location(0) vec4f{return textureSample(t,sm,i.u);}";
 WGPUShaderSourceWGSL wg{.chain={.next=nullptr,.sType=WGPUSType_ShaderSourceWGSL},.code={.data=s,.length=WGPU_STRLEN}}; WGPUShaderModuleDescriptor sd{.nextInChain=&wg.chain}; WGPUShaderModule m=wgpuDeviceCreateShaderModule(d,&sd);
 WGPUBindGroupLayoutEntry e[2]{}; e[0].binding=0;e[0].visibility=WGPUShaderStage_Fragment;e[0].texture.sampleType=WGPUTextureSampleType_Float;e[0].texture.viewDimension=WGPUTextureViewDimension_2D;e[1].binding=1;e[1].visibility=WGPUShaderStage_Fragment;e[1].sampler.type=WGPUSamplerBindingType_Filtering; WGPUBindGroupLayoutDescriptor bd{.entryCount=2,.entries=e};*out=wgpuDeviceCreateBindGroupLayout(d,&bd); WGPUPipelineLayoutDescriptor pd{.bindGroupLayoutCount=1,.bindGroupLayouts=out};WGPUPipelineLayout l=wgpuDeviceCreatePipelineLayout(d,&pd);WGPUColorTargetState ct{.format=f,.writeMask=WGPUColorWriteMask_All};WGPUFragmentState fs{.module=m,.entryPoint={.data="fs",.length=2},.targetCount=1,.targets=&ct};WGPURenderPipelineDescriptor rp{.layout=l,.vertex={.module=m,.entryPoint={.data="vs",.length=2}},.primitive={.topology=WGPUPrimitiveTopology_TriangleList},.multisample={.count=1,.mask=~0u},.fragment=&fs};auto r=wgpuDeviceCreateRenderPipeline(d,&rp);wgpuPipelineLayoutRelease(l);wgpuShaderModuleRelease(m);return r;
}
#endif

int main(const int argc, char* argv[]) {
    std::cout << "openRTS viewer skeleton\n"
              << "assets: " << openrts::assets::module_name() << '\n'
              << "renderer backend: " << openrts::renderer::backend_name() << '\n';
#ifdef OPENRTS_HAS_WGPU
    if (argc == 3 && std::string_view(argv[1]) == "--ace") {
        const std::filesystem::path file = argv[2];
        const auto decoded = openrts::formats::decode_structured_ace_rgba_path(file);
        if (!decoded.ok()) {
            std::cerr << "ACE decode failed: " << decoded.error << '\n';
            return 2;
        }
        auto device = openrts::renderer::create_headless_wgpu_device();
        if (!device.ok()) {
            std::cerr << "WebGPU device failed: " << device.error << '\n';
            return 3;
        }
        const auto upload = openrts::renderer::upload_rgba8_texture(
            device.context.device(),
            {.width = decoded.image->width, .height = decoded.image->height, .pixels = &decoded.image->pixels},
            file.filename().string());
        if (!upload.ok()) {
            std::cerr << "WebGPU texture upload failed: " << upload.error << '\n';
            return 4;
        }
        std::cout << "uploaded RGBA8 texture: " << decoded.image->width << 'x' << decoded.image->height << '\n';
    }
#else
    static_cast<void>(argc);
    static_cast<void>(argv);
#endif
#ifdef OPENRTS_HAS_GLFW
    if ((argc == 2 && std::string_view(argv[1]) == "--window")
        || (argc == 3 && std::string_view(argv[1]) == "--window")) {
        if (!glfwInit()) {
            std::cerr << "GLFW initialization failed.\n";
            return 5;
        }
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        GLFWwindow* window = glfwCreateWindow(1280, 720, "openRTS viewer", nullptr, nullptr);
        if (!window) {
            glfwTerminate();
            std::cerr << "GLFW window creation failed.\n";
            return 6;
        }
#if defined(OPENRTS_HAS_WGPU) && defined(_WIN32)
        auto context = openrts::renderer::create_headless_wgpu_device();
        if (!context.ok()) {
            std::cerr << "WebGPU device failed: " << context.error << '\n';
            glfwDestroyWindow(window); glfwTerminate(); return 7;
        }
        const WGPUSurfaceSourceWindowsHWND source{
            .chain = {.next = nullptr, .sType = WGPUSType_SurfaceSourceWindowsHWND},
            .hinstance = GetModuleHandle(nullptr), .hwnd = glfwGetWin32Window(window)};
        const WGPUSurfaceDescriptor descriptor{.nextInChain = &source.chain, .label = {.data = "openRTS", .length = 7}};
        WGPUSurface surface = wgpuInstanceCreateSurface(context.context.instance(), &descriptor);
        WGPUSurfaceCapabilities capabilities{};
        wgpuSurfaceGetCapabilities(surface, context.context.adapter(), &capabilities);
        if (!surface || capabilities.formatCount == 0) {
            std::cerr << "WebGPU surface creation failed.\n";
            if (surface) wgpuSurfaceRelease(surface);
            glfwDestroyWindow(window); glfwTerminate(); return 8;
        }
        WGPUSurfaceConfiguration config{.nextInChain = nullptr, .device = context.context.device(),
            .format = capabilities.formats[0], .usage = WGPUTextureUsage_RenderAttachment,
            .width = 1280, .height = 720, .viewFormatCount = 0, .viewFormats = nullptr,
            .alphaMode = capabilities.alphaModes[0]};
        wgpuSurfaceCapabilitiesFreeMembers(capabilities);
        wgpuSurfaceConfigure(surface, &config);
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            WGPUSurfaceTexture frame{};
            wgpuSurfaceGetCurrentTexture(surface, &frame);
            if (frame.status == WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal
                || frame.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
                WGPUTextureView view = wgpuTextureCreateView(frame.texture, nullptr);
                WGPURenderPassColorAttachment color{.nextInChain = nullptr, .view = view,
                    .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED, .resolveTarget = nullptr,
                    .loadOp = WGPULoadOp_Clear, .storeOp = WGPUStoreOp_Store,
                    .clearValue = {.r = 0.03, .g = 0.06, .b = 0.12, .a = 1.0}};
                WGPURenderPassDescriptor pass{.nextInChain = nullptr, .label = {.data = "clear", .length = 5},
                    .colorAttachmentCount = 1, .colorAttachments = &color, .depthStencilAttachment = nullptr,
                    .occlusionQuerySet = nullptr, .timestampWrites = nullptr};
                WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(context.context.device(), nullptr);
                WGPURenderPassEncoder render_pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass);
                wgpuRenderPassEncoderEnd(render_pass); wgpuRenderPassEncoderRelease(render_pass);
                WGPUCommandBuffer command = wgpuCommandEncoderFinish(encoder, nullptr);
                WGPUQueue queue = wgpuDeviceGetQueue(context.context.device());
                wgpuQueueSubmit(queue, 1, &command);
                wgpuQueueRelease(queue); wgpuCommandBufferRelease(command); wgpuCommandEncoderRelease(encoder);
                wgpuTextureViewRelease(view); wgpuSurfacePresent(surface); wgpuTextureRelease(frame.texture);
            }
        }
        wgpuSurfaceUnconfigure(surface);
        wgpuSurfaceRelease(surface);
#else
        while (!glfwWindowShouldClose(window)) glfwWaitEvents();
#endif
        glfwDestroyWindow(window);
        glfwTerminate();
    }
#endif
    return 0;
}
