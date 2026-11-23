// A mip level from the one above: the bilinear tap in the middle of each
// 2 x 2 block is their average, in linear light (an sRGB image).
@group(0) @binding(0) var above: texture_2d<f32>;
@group(0) @binding(1) var above_sampler: sampler;

struct VertexOut {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn vs_full(@builtin(vertex_index) index: u32) -> VertexOut {
    let p = vec2<f32>(f32((index << 1u) & 2u), f32(index & 2u)) * 2.0 - 1.0;
    var out: VertexOut;
    out.position = vec4<f32>(p, 0.0, 1.0);
    out.uv = vec2<f32>(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    return out;
}

@fragment
fn fs_down(input: VertexOut) -> @location(0) vec4<f32> {
    return textureSampleLevel(above, above_sampler, input.uv, 0.0);
}
