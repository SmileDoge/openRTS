
struct Frame {
    view_projection: mat4x4<f32>,
    viewport: vec4<f32>, // xy: pixels, z: 1 the frame is linear (frame_encoding())
};
@group(0) @binding(0) var<uniform> frame: Frame;

struct VertexOut {
    @builtin(position) position: vec4<f32>,
    @location(0) light: vec4<f32>,
    @location(1) @interpolate(linear) across: vec2<f32>, // x: pixels from the middle line, y: the core's width
};

const halo_factor: f32 = 6.0;

@vertex
fn vs_main(@builtin(vertex_index) index: u32, @location(0) a: vec4<f32>, @location(1) b: vec4<f32>,
           @location(2) light_a: vec4<f32>, @location(3) light_b: vec4<f32>) -> VertexOut {
    var out: VertexOut;
    let corner = index % 6u;
    let at_b = corner == 1u || corner == 2u || corner == 4u;
    let side = select(-1.0, 1.0, corner == 2u || corner == 4u || corner == 5u);
    let ca = frame.view_projection * vec4<f32>(a.xyz, 1.0);
    let cb = frame.view_projection * vec4<f32>(b.xyz, 1.0);
    if (ca.w < 1.0 || cb.w < 1.0) {
        out.position = vec4<f32>(0.0, 0.0, -1.0, 1.0);
        out.light = vec4<f32>(0.0);
        out.across = vec2<f32>(0.0, 1.0);
        return out;
    }
    let half_viewport = frame.viewport.xy * 0.5;
    let sa = ca.xy / ca.w * half_viewport;
    let sb = cb.xy / cb.w * half_viewport;
    var way = sb - sa;
    if (length(way) < 1e-3) {
        way = vec2<f32>(0.0, 1.0);
    }
    way = normalize(way);
    let normal = vec2<f32>(-way.y, way.x);
    let core = max(a.w, 1.2);
    let halo = core * halo_factor;
    var clip = select(ca, cb, at_b);
    // Out to the halo's width each side, half of it past each end (the
    // pieces overlap at their joints).
    let offset = normal * side * halo + way * select(-1.0, 1.0, at_b) * halo * 0.5;
    clip = vec4<f32>(clip.xy + offset / half_viewport * clip.w, clip.z, clip.w);
    // Beyond the far plane: kept on it (reversed Z: 0 there).
    clip.z = max(clip.z, 0.0);
    out.position = clip;
    out.light = select(light_a, light_b, at_b);
    out.across = vec2<f32>(side * halo, core);
    return out;
}

@fragment
fn fs_main(input: VertexOut) -> @location(0) vec4<f32> {
    let x = abs(input.across.x) / input.across.y;
    let shine = exp(-x * x) + 0.08 * exp(-x * x / (halo_factor * halo_factor * 0.25));
    var light = input.light.rgb * shine;
    // The strips' light is given gamma 2.2 encoded: a linear frame decodes it.
    if (frame.viewport.z > 0.5) {
        light = pow(max(light, vec3<f32>(0.0)), vec3<f32>(2.2));
    }
    return vec4<f32>(light, 0.0);
}
