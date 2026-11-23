
struct Frame {
    view_projection: mat4x4<f32>,
    flags: vec4<f32>, // x: 1 the frame is linear (frame_encoding())
};
@group(0) @binding(0) var<uniform> frame: Frame;

struct VertexOut {
    @builtin(position) position: vec4<f32>,
    @location(0) colour: vec4<f32>,
};

@vertex
fn vs_main(@location(0) position: vec3<f32>, @location(1) colour: vec4<f32>) -> VertexOut {
    var out: VertexOut;
    out.position = frame.view_projection * vec4<f32>(position, 1.0);
    out.colour = colour;
    return out;
}

@fragment
fn fs_main(input: VertexOut) -> @location(0) vec4<f32> {
    if (frame.flags.x > 0.5) {
        return vec4<f32>(pow(input.colour.rgb, vec3<f32>(2.2)), input.colour.a);
    }
    return input.colour;
}
