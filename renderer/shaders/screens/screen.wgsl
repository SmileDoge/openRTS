// Straight onto the panel, as RTS draws them (and the supersampled path off).
struct Frame {
    view_projection: mat4x4<f32>,
    eye: vec4<f32>,
};
@group(0) @binding(0) var<uniform> frame: Frame;
@group(0) @binding(1) var screen_texture: texture_2d<f32>;
@group(0) @binding(2) var screen_sampler: sampler;

struct VertexOut {
    @builtin(position) position: vec4<f32>,
    @location(0) colour: vec4<f32>,
    @location(1) uv: vec2<f32>,
};

@vertex
fn vs_main(@location(0) position: vec3<f32>, @location(1) uv: vec2<f32>, @location(2) colour: vec4<f32>) -> VertexOut {
    let to_eye = frame.eye.xyz - position;
    let length_to_eye = max(length(to_eye), 0.001);
    let lifted = position + to_eye / length_to_eye * 0.001;
    var out: VertexOut;
    out.position = frame.view_projection * vec4<f32>(lifted, 1.0);
    out.colour = colour;
    out.uv = uv;
    return out;
}

// Half a mip level sharper than the plain choice: a screen's small print
// is read from the seat (testers, 2026-11-12: the ЭП20's БЛОК was blurry).
// eye.w 1: the frame is linear (frame_encoding()): RTS's display colours decoded.
fn encoded(c: vec3<f32>) -> vec3<f32> {
    return select(c, pow(max(c, vec3<f32>(0.0)), vec3<f32>(2.2)), frame.eye.w > 0.5);
}

@fragment
fn fs_blend(input: VertexOut) -> @location(0) vec4<f32> {
    let texel = textureSampleBias(screen_texture, screen_sampler, input.uv, -0.5) * input.colour;
    return vec4<f32>(encoded(texel.rgb), texel.a);
}

@fragment
fn fs_opaque(input: VertexOut) -> @location(0) vec4<f32> {
    let texel = textureSampleBias(screen_texture, screen_sampler, input.uv, -0.5) * input.colour;
    return vec4<f32>(encoded(texel.rgb), 1.0);
}
