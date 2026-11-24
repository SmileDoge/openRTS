
struct Params {
    texel: vec2<f32>,  // the input's texel
    threshold: f32,    // what is over it blooms (scene-linear)
    knee: f32,         // the threshold's soft start
    night: f32,        // 0 day .. 1 night: the brightest texels bloom (see light_at)
    linear: f32,       // 1: the scene and the lights' source hold linear light, 0: gamma 2.2
    pad1: vec2<f32>,
};
@group(0) @binding(0) var input_a: texture_2d<f32>; // the scene (the first pass), else the larger image
@group(0) @binding(1) var input_b: texture_2d<f32>; // the lights' source (the first pass)
@group(0) @binding(2) var linear_clamp: sampler;
@group(0) @binding(3) var<uniform> params: Params;

struct VertexOut {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> VertexOut {
    let p = vec2<f32>(f32((index << 1u) & 2u), f32(index & 2u));
    var out: VertexOut;
    out.position = vec4<f32>(p * 2.0 - 1.0, 0.0, 1.0);
    out.uv = vec2<f32>(p.x, 1.0 - p.y);
    return out;
}

fn luma(c: vec3<f32>) -> f32 {
    return dot(c, vec3<f32>(0.2126, 0.7152, 0.0722));
}

// What is over the threshold, its brightest channel's share, softly.
fn over(c: vec3<f32>) -> vec3<f32> {
    let bright = max(c.r, max(c.g, c.b));
    var soft = clamp(bright - params.threshold + params.knee, 0.0, 2.0 * params.knee);
    soft = soft * soft / (4.0 * params.knee + 1e-5);
    let w = max(soft, bright - params.threshold) / max(bright, 1e-5);
    return c * max(w, 0.0);
}

// The light that blooms at uv: the scene over the threshold; at night also
// the brightest of what the textures paint (a lamp's bulb, a lit window, a
// sign: drawn near white, 0.8 to 1 as displayed; a dark wall, a lamp's pool
// painted on a platform stay below), as the night's eye sees them; and the
// lights' glows.
fn light_at(uv: vec2<f32>) -> vec3<f32> {
    let stored = max(textureSampleLevel(input_a, linear_clamp, uv, 0.0).rgb, vec3<f32>(0.0));
    let linear_frame = params.linear > 0.5;
    let scene = select(pow(stored, vec3<f32>(2.2)), stored, linear_frame);
    // How bright it is as displayed (the texture's own brightness).
    let encoded = select(stored, pow(stored, vec3<f32>(1.0 / 2.2)), linear_frame);
    var light = over(scene);
    if (params.night > 0.0) {
        let top = max(encoded.r, max(encoded.g, encoded.b));
        light = max(light, scene * smoothstep(0.8, 1.0, top) * params.night);
    }
    let glow_stored = max(textureSampleLevel(input_b, linear_clamp, uv, 0.0).rgb, vec3<f32>(0.0));
    let glow = select(pow(glow_stored, vec3<f32>(2.2)), glow_stored, linear_frame);
    return min(light + glow, vec3<f32>(1000.0));
}

fn take(uv: vec2<f32>) -> vec3<f32> {
    return textureSampleLevel(input_a, linear_clamp, uv, 0.0).rgb;
}

// The first step down (13 taps, Jimenez's), each of its five boxes weighed
// by 1 / (1 + luma) (Karis): a lone bright texel does not flicker.
@fragment
fn fs_prefilter(input: VertexOut) -> @location(0) vec4<f32> {
    let t = params.texel;
    let uv = input.uv;
    let a = light_at(uv + t * vec2<f32>(-2.0, -2.0));
    let b = light_at(uv + t * vec2<f32>(0.0, -2.0));
    let c = light_at(uv + t * vec2<f32>(2.0, -2.0));
    let d = light_at(uv + t * vec2<f32>(-2.0, 0.0));
    let e = light_at(uv);
    let f = light_at(uv + t * vec2<f32>(2.0, 0.0));
    let g = light_at(uv + t * vec2<f32>(-2.0, 2.0));
    let h = light_at(uv + t * vec2<f32>(0.0, 2.0));
    let i = light_at(uv + t * vec2<f32>(2.0, 2.0));
    let j = light_at(uv + t * vec2<f32>(-1.0, -1.0));
    let k = light_at(uv + t * vec2<f32>(1.0, -1.0));
    let l = light_at(uv + t * vec2<f32>(-1.0, 1.0));
    let m = light_at(uv + t * vec2<f32>(1.0, 1.0));
    let g0 = (j + k + l + m) * 0.25;
    let g1 = (a + b + d + e) * 0.25;
    let g2 = (b + c + e + f) * 0.25;
    let g3 = (d + e + g + h) * 0.25;
    let g4 = (e + f + h + i) * 0.25;
    let w0 = 0.5 / (1.0 + luma(g0));
    let w1 = 0.125 / (1.0 + luma(g1));
    let w2 = 0.125 / (1.0 + luma(g2));
    let w3 = 0.125 / (1.0 + luma(g3));
    let w4 = 0.125 / (1.0 + luma(g4));
    let sum = g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4;
    return vec4<f32>(sum / (w0 + w1 + w2 + w3 + w4), 1.0);
}

// A step down: the 13 taps, plainly weighed.
@fragment
fn fs_down(input: VertexOut) -> @location(0) vec4<f32> {
    let t = params.texel;
    let uv = input.uv;
    let a = take(uv + t * vec2<f32>(-2.0, -2.0));
    let b = take(uv + t * vec2<f32>(0.0, -2.0));
    let c = take(uv + t * vec2<f32>(2.0, -2.0));
    let d = take(uv + t * vec2<f32>(-2.0, 0.0));
    let e = take(uv);
    let f = take(uv + t * vec2<f32>(2.0, 0.0));
    let g = take(uv + t * vec2<f32>(-2.0, 2.0));
    let h = take(uv + t * vec2<f32>(0.0, 2.0));
    let i = take(uv + t * vec2<f32>(2.0, 2.0));
    let j = take(uv + t * vec2<f32>(-1.0, -1.0));
    let k = take(uv + t * vec2<f32>(1.0, -1.0));
    let l = take(uv + t * vec2<f32>(-1.0, 1.0));
    let m = take(uv + t * vec2<f32>(1.0, 1.0));
    let sum = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
    return vec4<f32>(sum, 1.0);
}

// A step up: the smaller image through a 3 x 3 tent, added to the larger.
@fragment
fn fs_up(input: VertexOut) -> @location(0) vec4<f32> {
    let t = params.texel;
    let uv = input.uv;
    var sum = take(uv) * 4.0;
    sum = sum + (take(uv + vec2<f32>(0.0, -t.y)) + take(uv + vec2<f32>(-t.x, 0.0)) + take(uv + vec2<f32>(t.x, 0.0))
                 + take(uv + vec2<f32>(0.0, t.y))) * 2.0;
    sum = sum + take(uv + vec2<f32>(-t.x, -t.y)) + take(uv + vec2<f32>(t.x, -t.y)) + take(uv + vec2<f32>(-t.x, t.y))
              + take(uv + vec2<f32>(t.x, t.y));
    return vec4<f32>(sum / 16.0, 1.0);
}
