// The sun's shadow cache (2026-11-17): a cascade's working map from the
// still casters' layer, kept toroidally (each global texel at its place
// modulo the size), then the moving casters over it; a piece of the layer
// drawn again first set to the far plane.
struct Unwrap {
    at: array<vec4<u32>, 4>, // the window's offset in the layer (x, y), the layer, the size
};
@group(0) @binding(0) var<uniform> unwrap: Unwrap;
@group(0) @binding(1) var still: texture_depth_2d_array;

struct FullOut {
    @builtin(position) position: vec4<f32>,
    @location(0) @interpolate(flat) which: u32,
};

@vertex
fn vs_full(@builtin(vertex_index) v: u32, @builtin(instance_index) i: u32) -> FullOut {
    let xy = vec2<f32>(f32((v << 1u) & 2u), f32(v & 2u)) * 2.0 - 1.0;
    var out: FullOut;
    out.position = vec4<f32>(xy, 0.0, 1.0);
    out.which = i;
    return out;
}

@fragment
fn fs_unwrap(input: FullOut) -> @builtin(frag_depth) f32 {
    let a = unwrap.at[input.which];
    let p = vec2<u32>(input.position.xy);
    let at = vec2<u32>((p.x + a.x) % a.w, (p.y + a.y) % a.w);
    return textureLoad(still, vec2<i32>(at), i32(a.z), 0);
}
