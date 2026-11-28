// The panels over the HDR output (hdr_output.hpp): their texture holds
// display-encoded colours premultiplied by their coverage (ImGui's blending
// over a transparent clear); out linear at the paper white, premultiplied.
@group(0) @binding(0) var panels: texture_2d<f32>;
@group(0) @binding(1) var<uniform> scale: vec4<f32>; // x the paper white over 80 nits

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> @builtin(position) vec4<f32> {
    let uv = vec2<f32>(f32((index << 1u) & 2u), f32(index & 2u));
    return vec4<f32>(uv * 2.0 - 1.0, 0.0, 1.0);
}

fn to_linear(c: vec3<f32>) -> vec3<f32> {
    let low = c / 12.92;
    let high = pow((c + 0.055) / 1.055, vec3<f32>(2.4));
    return select(high, low, c <= vec3<f32>(0.04045));
}

@fragment
fn fs_main(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> {
    let p = textureLoad(panels, vec2<i32>(position.xy), 0);
    if (p.a <= 0.0) {
        discard;
    }
    let straight = clamp(p.rgb / p.a, vec3<f32>(0.0), vec3<f32>(1.0));
    return vec4<f32>(to_linear(straight) * scale.x * p.a, p.a);
}
