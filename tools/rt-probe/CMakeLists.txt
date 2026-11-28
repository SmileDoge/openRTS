# openrts-rt-probe (docs/ray-tracing.md): one triangle in a BLAS, a TLAS of
# it and two rays from a compute shader, on D3D12 (default) or Vulkan
# ("openrts-rt-probe vulkan"). Needs openRTS's wgpu-native with ray queries.
add_executable(openrts_rt_probe src/main.cpp)
set_target_properties(openrts_rt_probe PROPERTIES OUTPUT_NAME openrts-rt-probe)
openrts_configure_target(openrts_rt_probe)
target_link_libraries(openrts_rt_probe PRIVATE openrts_renderer)
openrts_copy_wgpu_runtime(openrts_rt_probe)
