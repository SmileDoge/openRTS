
struct Frame {
    view_projection: mat4x4<f32>,
    right: vec4<f32>,  // xyz camera right, w: a pixel's size a metre away (m)
    up: vec4<f32>,     // xyz camera up, w: 1 the frame is linear (frame_encoding())
    eye: vec4<f32>,    // xyz eye, w: field of view / 45 degrees
};
@group(0) @binding(0) var<uniform> frame: Frame;
@group(0) @binding(1) var light_texture: texture_2d<f32>;
@group(0) @binding(2) var light_sampler: sampler;

struct VertexOut {
    @builtin(position) position: vec4<f32>,
    @location(0) colour: vec4<f32>,
    @location(1) uv: vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) index: u32, @location(0) centre: vec3<f32>, @location(1) radius: f32,
           @location(2) uv_rect: vec4<f32>, @location(3) colour: vec4<f32>, @location(4) far_scale: f32,
           @location(5) axis_x: vec3<f32>, @location(6) axis_y: vec3<f32>, @location(7) min_pixels: f32,
           @location(8) spread_light: f32) -> VertexOut {
    var corners = array<vec2<f32>, 6>(vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                                      vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
    let corner = corners[index];
    let to_eye = frame.eye.xyz - centre;
    let distance = length(to_eye);
    let own = max(radius, distance * far_scale) * frame.eye.w;
    let size = max(own, distance * min_pixels * frame.right.w);
    var x = axis_x;
    var y = axis_y;
    if (dot(x, x) < 0.5) {
        x = frame.right.xyz;
        y = frame.up.xyz;
    }
    let world = centre + (x * corner.x + y * corner.y) * size;
    var out: VertexOut;
    out.position = frame.view_projection * vec4<f32>(world, 1.0);
    out.colour = colour;
    // Made bigger than its own size: its light spread over the bigger quad.
    let kept = own / max(size, 1e-6);
    out.colour.a = colour.a * mix(1.0, kept * kept, clamp(spread_light, 0.0, 1.0));
    let t = corner * 0.5 + vec2(0.5, 0.5);
    out.uv = vec2(mix(uv_rect.x, uv_rect.z, t.x), mix(uv_rect.w, uv_rect.y, t.y));
    return out;
}

// Added (rgb x alpha): into a linear frame the light it adds on black as the
// display shows it, decoded.
@fragment
fn fs_main(input: VertexOut) -> @location(0) vec4<f32> {
    let texel = textureSample(light_texture, light_sampler, input.uv);
    let intensity = texel.a * input.colour.a;
    var light = texel.rgb * input.colour.rgb * intensity;
    if (frame.up.w > 0.5) {
        light = pow(max(light, vec3<f32>(0.0)), vec3<f32>(2.2));
    }
    return vec4<f32>(light, intensity);
}

// Laid over by its alpha (premultiplied): into a linear frame its colour
// decoded, then premultiplied.
@fragment
fn fs_over(input: VertexOut) -> @location(0) vec4<f32> {
    let texel = textureSample(light_texture, light_sampler, input.uv);
    let intensity = texel.a * input.colour.a;
    var colour = texel.rgb * input.colour.rgb;
    if (frame.up.w > 0.5) {
        colour = pow(max(colour, vec3<f32>(0.0)), vec3<f32>(2.2));
    }
    return vec4<f32>(colour * intensity, intensity);
}
