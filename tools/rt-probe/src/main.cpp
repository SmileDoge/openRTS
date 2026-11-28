// openrts-rt-probe (docs/ray-tracing.md): a ray query check of openRTS's wgpu-native: one triangle in a BLAS,
// a TLAS of it, a compute shader shooting two rays (one at it, one past it).
#include <cstdio>
#include <cstring>
#include <string>

#include <webgpu/webgpu.h>
#include <webgpu/wgpu.h>

static WGPUAdapter g_adapter = nullptr;
static WGPUDevice g_device = nullptr;
static bool g_mapped = false;

static WGPUStringView sv(const char* s) { return {s, WGPU_STRLEN}; }

static void on_adapter(WGPURequestAdapterStatus, WGPUAdapter a, WGPUStringView msg, void*, void*) {
    g_adapter = a;
    if (!a) std::printf("no adapter: %.*s\n", (int)msg.length, msg.data);
}
static void on_device(WGPURequestDeviceStatus, WGPUDevice d, WGPUStringView msg, void*, void*) {
    g_device = d;
    if (!d) std::printf("no device: %.*s\n", (int)msg.length, msg.data);
}
static void on_error(WGPUDevice const*, WGPUErrorType type, WGPUStringView msg, void*, void*) {
    std::printf("device error %d: %.*s\n", (int)type, (int)msg.length, msg.data);
}
static void on_map(WGPUMapAsyncStatus status, WGPUStringView, void*, void*) { g_mapped = status == WGPUMapAsyncStatus_Success; }

int main(int argc, char** argv) {
    const bool vulkan = argc > 1 && std::strcmp(argv[1], "vulkan") == 0;
    WGPUInstanceExtras extras{};
    extras.chain.sType = (WGPUSType)WGPUSType_InstanceExtras;
    extras.backends = vulkan ? WGPUInstanceBackend_Vulkan : WGPUInstanceBackend_DX12;
    // D3D12's ray queries need Shader Model 6.5: DXC, dxcompiler.dll beside the probe.
    const char* dxc = "dxcompiler.dll";
    if (!vulkan) {
        extras.dx12ShaderCompiler = WGPUDx12Compiler_Dxc;
        extras.dxcPath = sv(dxc);
        extras.dxcMaxShaderModel = WGPUDxcMaxShaderModel_V6_7;
    }
    WGPUInstanceDescriptor idesc{};
    idesc.nextInChain = &extras.chain;
    WGPUInstance instance = wgpuCreateInstance(&idesc);

    WGPURequestAdapterOptions aopt{};
    aopt.powerPreference = WGPUPowerPreference_HighPerformance;
    WGPURequestAdapterCallbackInfo acb{};
    acb.mode = WGPUCallbackMode_AllowSpontaneous;
    acb.callback = on_adapter;
    wgpuInstanceRequestAdapter(instance, &aopt, acb);
    if (!g_adapter) return 1;
    WGPUAdapterInfo info{};
    wgpuAdapterGetInfo(g_adapter, &info);
    std::printf("adapter: %.*s (%s)\n", (int)info.device.length, info.device.data, vulkan ? "Vulkan" : "D3D12");
    if (!wgpuAdapterHasFeature(g_adapter, (WGPUFeatureName)WGPUNativeFeature_RayQuery)) {
        std::printf("RESULT: the adapter has no ray queries\n");
        return 2;
    }
    WGPUFeatureName features[] = {(WGPUFeatureName)WGPUNativeFeature_RayQuery};
    WGPUDeviceDescriptor ddesc{};
    ddesc.requiredFeatureCount = 1;
    ddesc.requiredFeatures = features;
    ddesc.uncapturedErrorCallbackInfo.callback = on_error;
    WGPURequestDeviceCallbackInfo dcb{};
    dcb.mode = WGPUCallbackMode_AllowSpontaneous;
    dcb.callback = on_device;
    wgpuAdapterRequestDevice(g_adapter, &ddesc, dcb);
    if (!g_device) return 1;
    WGPUQueue queue = wgpuDeviceGetQueue(g_device);

    // The triangle: z = 0, round the origin.
    const float vertices[9] = {-1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    WGPUBufferDescriptor vb{};
    vb.size = sizeof vertices;
    vb.usage = WGPUBufferUsage_BlasInput | WGPUBufferUsage_CopyDst | WGPUBufferUsage_Vertex;
    WGPUBuffer vbuf = wgpuDeviceCreateBuffer(g_device, &vb);
    wgpuQueueWriteBuffer(queue, vbuf, 0, vertices, sizeof vertices);

    WGPUBlasTriangleGeometrySize size{};
    size.vertexFormat = WGPUVertexFormat_Float32x3;
    size.vertexCount = 3;
    size.indexFormat = WGPUIndexFormat_Undefined;
    size.flags = WGPUAccelerationStructureGeometryFlag_Opaque;
    WGPUBlasDescriptor bd{};
    bd.label = sv("triangle");
    bd.flags = WGPUAccelerationStructureFlag_PreferFastTrace;
    bd.updateMode = WGPUAccelerationStructureUpdateMode_Build;
    bd.geometryCount = 1;
    bd.geometries = &size;
    WGPUBlas blas = wgpuDeviceCreateBlas(g_device, &bd);
    WGPUTlasDescriptor td{};
    td.label = sv("scene");
    td.maxInstances = 1;
    td.flags = WGPUAccelerationStructureFlag_PreferFastTrace;
    WGPUTlas tlas = wgpuDeviceCreateTlas(g_device, &td);
    if (!blas || !tlas) {
        std::printf("RESULT: no BLAS / TLAS\n");
        return 1;
    }

    WGPUBlasTriangleGeometry geometry{};
    geometry.size = size;
    geometry.vertexBuffer = vbuf;
    geometry.vertexStride = 12;
    WGPUBlasBuildEntry be{blas, 1, &geometry};
    WGPUTlasInstance instance_{};
    instance_.blas = blas;
    const float identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    std::memcpy(instance_.transform, identity, sizeof identity);
    instance_.mask = 0xFF;
    WGPUTlasBuildEntry te{tlas, 1, &instance_};

    const char* code = R"(
@group(0) @binding(0) var acc: acceleration_structure;
@group(0) @binding(1) var<storage, read_write> result: array<u32, 2>;
fn shoot(origin: vec3<f32>) -> u32 {
    var rq: ray_query;
    rayQueryInitialize(&rq, acc, RayDesc(0u, 0xFFu, 0.0, 100.0, origin, vec3<f32>(0.0, 0.0, 1.0)));
    rayQueryProceed(&rq);
    return rayQueryGetCommittedIntersection(&rq).kind;
}
@compute @workgroup_size(1)
fn main() {
    result[0] = shoot(vec3<f32>(0.0, 0.0, -1.0));
    result[1] = shoot(vec3<f32>(5.0, 5.0, -1.0));
}
)";
    WGPUShaderSourceWGSL src{};
    src.chain.sType = WGPUSType_ShaderSourceWGSL;
    src.code = sv(code);
    WGPUShaderModuleDescriptor smd{};
    smd.nextInChain = &src.chain;
    WGPUShaderModule shader = wgpuDeviceCreateShaderModule(g_device, &smd);

    WGPUBindGroupLayoutEntryAccelerationStructure las{};
    las.chain.sType = (WGPUSType)WGPUSType_BindGroupLayoutEntryAccelerationStructure;
    WGPUBindGroupLayoutEntry le[2]{};
    le[0].nextInChain = &las.chain;
    le[0].binding = 0;
    le[0].visibility = WGPUShaderStage_Compute;
    le[1].binding = 1;
    le[1].visibility = WGPUShaderStage_Compute;
    le[1].buffer.type = WGPUBufferBindingType_Storage;
    WGPUBindGroupLayoutDescriptor bgld{};
    bgld.entryCount = 2;
    bgld.entries = le;
    WGPUBindGroupLayout layout = wgpuDeviceCreateBindGroupLayout(g_device, &bgld);

    WGPUBufferDescriptor ob{};
    ob.size = 8;
    ob.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc;
    WGPUBuffer obuf = wgpuDeviceCreateBuffer(g_device, &ob);
    WGPUBufferDescriptor rb{};
    rb.size = 8;
    rb.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    WGPUBuffer rbuf = wgpuDeviceCreateBuffer(g_device, &rb);

    WGPUBindGroupEntryAccelerationStructure eas{};
    eas.chain.sType = (WGPUSType)WGPUSType_BindGroupEntryAccelerationStructure;
    eas.tlas = tlas;
    WGPUBindGroupEntry ge[2]{};
    ge[0].nextInChain = &eas.chain;
    ge[0].binding = 0;
    ge[1].binding = 1;
    ge[1].buffer = obuf;
    ge[1].size = 8;
    WGPUBindGroupDescriptor bgd{};
    bgd.layout = layout;
    bgd.entryCount = 2;
    bgd.entries = ge;
    WGPUBindGroup group = wgpuDeviceCreateBindGroup(g_device, &bgd);

    WGPUPipelineLayoutDescriptor pld{};
    pld.bindGroupLayoutCount = 1;
    pld.bindGroupLayouts = &layout;
    WGPUPipelineLayout pl = wgpuDeviceCreatePipelineLayout(g_device, &pld);
    WGPUComputePipelineDescriptor cpd{};
    cpd.layout = pl;
    cpd.compute.module = shader;
    cpd.compute.entryPoint = sv("main");
    WGPUComputePipeline pipeline = wgpuDeviceCreateComputePipeline(g_device, &cpd);

    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_device, nullptr);
    wgpuCommandEncoderBuildAccelerationStructures(enc, 1, &be, 1, &te);
    WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(enc, nullptr);
    wgpuComputePassEncoderSetPipeline(pass, pipeline);
    wgpuComputePassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(pass, 1, 1, 1);
    wgpuComputePassEncoderEnd(pass);
    wgpuCommandEncoderCopyBufferToBuffer(enc, obuf, 0, rbuf, 0, 8);
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(queue, 1, &cmd);

    WGPUBufferMapCallbackInfo mcb{};
    mcb.mode = WGPUCallbackMode_AllowSpontaneous;
    mcb.callback = on_map;
    wgpuBufferMapAsync(rbuf, WGPUMapMode_Read, 0, 8, mcb);
    for (int i = 0; i < 200 && !g_mapped; ++i) wgpuDevicePoll(g_device, true, nullptr);
    if (!g_mapped) {
        std::printf("RESULT: could not read back\n");
        return 1;
    }
    const auto* r = static_cast<const unsigned*>(wgpuBufferGetConstMappedRange(rbuf, 0, 8));
    std::printf("ray at the triangle: kind %u (1 = triangle), ray past it: kind %u (0 = none)\n", r[0], r[1]);
    const bool ok = r[0] == 1 && r[1] == 0;
    std::printf("RESULT: %s\n", ok ? "ray queries work" : "WRONG");
    return ok ? 0 : 3;
}
