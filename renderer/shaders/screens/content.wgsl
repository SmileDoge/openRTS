// A screen's objects into its own image (RGBA8UnormSrgb: the blending and
// the filtering in linear light), colour premultiplied by alpha.
struct Surface {
    area: vec4<f32>, // the part of the screen the image covers: x0 y0 x1 y1 in its 0..1
};
@group(0) @binding(0) var<uniform> surface: Surface;
@group(1) @binding(0) var source: texture_2d<f32>;
@group(1) @binding(1) var source_sampler: sampler;

struct VertexOut {
    @builtin(position) position: vec4<f32>,
    @location(0) colour: vec4<f32>,
    @location(1) uv: vec2<f32>,
};

@vertex
fn vs_content(@location(1) uv: vec2<f32>, @location(2) colour: vec4<f32>, @location(3) local: vec2<f32>) -> VertexOut {
    let t = (local - surface.area.xy) / (surface.area.zw - surface.area.xy);
    var out: VertexOut;
    out.position = vec4<f32>(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);
    out.colour = colour;
    out.uv = uv;
    return out;
}

// The display's encoding to linear light (sRGB), as the image stores it.
fn to_linear(c: vec3<f32>) -> vec3<f32> {
    return select(pow((c + 0.055) / 1.055, vec3<f32>(2.4)), c / 12.92, c <= vec3<f32>(0.04045));
}

// The texture premultiplied in linear light (add_texture's premultiplied);
// the tint is display-encoded, as RTS's vertex colour.
@fragment
fn fs_content_blend(input: VertexOut) -> @location(0) vec4<f32> {
    let texel = textureSample(source, source_sampler, input.uv);
    let tint = to_linear(input.colour.rgb);
    return vec4<f32>(texel.rgb * tint * input.colour.a, texel.a * input.colour.a);
}

// Opaque shaders (Tex): the plain texture's colour, alpha left out.
@fragment
fn fs_content_opaque(input: VertexOut) -> @location(0) vec4<f32> {
    let texel = textureSample(source, source_sampler, input.uv);
    return vec4<f32>(to_linear(texel.rgb * input.colour.rgb), 1.0);
}
