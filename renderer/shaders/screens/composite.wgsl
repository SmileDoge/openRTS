// A screen's image on the panel: linear premultiplied back to the display's
// encoding, blended as RTS blends (straight alpha, display values).
struct Frame {
    view_projection: mat4x4<f32>,
    eye: vec4<f32>,
};
@group(0) @binding(0) var<uniform> frame: Frame;
@group(1) @binding(0) var image: texture_2d<f32>;
@group(1) @binding(1) var image_sampler: sampler;

struct VertexOut {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn vs_composite(@location(0) position: vec3<f32>, @location(1) uv: vec2<f32>) -> VertexOut {
    let to_eye = frame.eye.xyz - position;
    let length_to_eye = max(length(to_eye), 0.001);
    let lifted = position + to_eye / length_to_eye * 0.001;
    var out: VertexOut;
    out.position = frame.view_projection * vec4<f32>(lifted, 1.0);
    out.uv = uv;
    return out;
}

fn to_display(c: vec3<f32>) -> vec3<f32> {
    return select(1.055 * pow(c, vec3<f32>(1.0 / 2.4)) - 0.055, c * 12.92, c <= vec3<f32>(0.0031308));
}

// Half a mip level sharper, as the straight path.
@fragment
fn fs_composite(input: VertexOut) -> @location(0) vec4<f32> {
    let texel = textureSampleBias(image, image_sampler, input.uv, -0.5);
    let straight = min(texel.rgb / max(texel.a, 0.00001), vec3<f32>(1.0));
    var display = to_display(straight);
    // A linear frame (eye.w 1): as the gamma 2.2 frame would show it, decoded.
    if (frame.eye.w > 0.5) {
        display = pow(display, vec3<f32>(2.2));
    }
    return vec4<f32>(display, texel.a);
}
