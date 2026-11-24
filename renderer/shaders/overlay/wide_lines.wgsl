// Vertices come in clip space: the strip is built on the CPU each frame.
struct VertexOut {
    @builtin(position) position: vec4<f32>,
    @location(0) colour: vec4<f32>,
};

@vertex
fn vs_main(@location(0) position: vec4<f32>, @location(1) colour: vec4<f32>) -> VertexOut {
    var out: VertexOut;
    out.position = position;
    out.colour = colour;
    return out;
}

@fragment
fn fs_main(input: VertexOut) -> @location(0) vec4<f32> {
    return input.colour;
}
