// Temporal anti-aliasing (renderer/temporal_aa.hpp): this frame's pixel
// with the history where its point was last frame, clipped to the colours
// round the pixel.
struct Params {
    inverse_view_projection: mat4x4<f32>, // this frame's clip (jittered) -> route space
    previous_view_projection: mat4x4<f32>, // the last frame's (steady) route space -> clip
    steady_view_projection: mat4x4<f32>,   // this frame's without the jitter
    eye_shift: vec4<f32>,                  // xyz: the player's train's move since; w: 1 a history to take
    size: vec4<f32>,                       // width, height, 1 / width, 1 / height
    view: vec4<f32>,                       // x: what to show (TemporalAAView), 0 the result
};

// The views for looking into it (TemporalAAView).
fn shown(mode: f32, motion_pixels: vec2<f32>, is_moving: bool, depth: f32, raw: vec3<f32>, clipped: vec3<f32>) -> vec4<f32> {
    if (mode < 1.5) {
        // Motion: red / green its x / y (a pixel = 0.25), blue the moving models.
        return vec4<f32>(min(abs(motion_pixels) * 0.25, vec2<f32>(1.0)), select(0.0, 0.6, is_moving), 1.0);
    }
    if (mode < 2.5) {
        // The depth TAA reads (near white, far black; the sky black), the
        // moving models' in blue.
        let d = clamp(pow(depth * 200.0, 0.35), 0.0, 1.0);
        return vec4<f32>(d, d, select(d, 1.0, is_moving), 1.0);
    }
    if (mode < 3.5) {
        return vec4<f32>(unsqueeze(raw), 1.0); // the history as found, not clipped
    }
    // How much of the history the clip let go (white: all of it).
    let gone = length(raw - clipped) * 4.0;
    return vec4<f32>(gone, gone, gone, 1.0);
}

@group(0) @binding(0) var scene: texture_2d<f32>;
@group(0) @binding(1) var history: texture_2d<f32>;
@group(0) @binding(2) var depth: texture_depth_2d;
@group(0) @binding(3) var moving: texture_depth_2d;
@group(0) @binding(4) var linear_sampler: sampler;
@group(0) @binding(5) var<uniform> params: Params;

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> @builtin(position) vec4<f32> {
    let uv = vec2<f32>(f32((index << 1u) & 2u), f32(index & 2u));
    return vec4<f32>(uv * 2.0 - 1.0, 0.0, 1.0);
}

fn luma(c: vec3<f32>) -> f32 {
    return dot(c, vec3<f32>(0.2126, 0.7152, 0.0722));
}

// Luma and chroma (YCoCg): the neighbours' box is tighter round a colour
// than in RGB (fewer wrong colours kept, less flicker).
fn to_ycocg(c: vec3<f32>) -> vec3<f32> {
    return vec3<f32>(dot(c, vec3<f32>(0.25, 0.5, 0.25)), dot(c, vec3<f32>(0.5, 0.0, -0.5)), dot(c, vec3<f32>(-0.25, 0.5, -0.25)));
}

fn from_ycocg(c: vec3<f32>) -> vec3<f32> {
    return vec3<f32>(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// `past` taken towards the box's centre until it is inside the box (not
// each channel cut alone: the colour keeps its hue).
fn clip_to_box(past: vec3<f32>, low: vec3<f32>, high: vec3<f32>) -> vec3<f32> {
    let centre = 0.5 * (high + low);
    let half_size = 0.5 * (high - low) + vec3<f32>(1e-5);
    let offset = past - centre;
    let units = abs(offset / half_size);
    let most = max(units.x, max(units.y, units.z));
    if (most > 1.0) {
        return centre + offset / most;
    }
    return past;
}

// Blended in a space where the bright do not outweigh the rest (HDR).
fn squeeze(c: vec3<f32>) -> vec3<f32> {
    return c / (1.0 + luma(c));
}

fn unsqueeze(c: vec3<f32>) -> vec3<f32> {
    return c / max(1.0 - luma(c), 1e-4);
}

// A colour fit to keep: no NaN, no infinity, nothing below 0 (one such
// texel would stay in the history for good and spread: the clip's
// comparisons are false for a NaN).
fn sane(c: vec3<f32>) -> vec3<f32> {
    let sum = c.x + c.y + c.z;
    if (!(sum >= 0.0 && sum <= 60000.0) || !(c.x >= 0.0) || !(c.y >= 0.0) || !(c.z >= 0.0)) {
        return clamp(select(vec3<f32>(0.0), c, c >= vec3<f32>(0.0)), vec3<f32>(0.0), vec3<f32>(20000.0));
    }
    return c;
}

fn load_scene(p: vec2<i32>) -> vec3<f32> {
    let size = vec2<i32>(params.size.xy);
    return sane(textureLoad(scene, clamp(p, vec2<i32>(0), size - 1), 0).rgb);
}

// The history at `uv` by Catmull-Rom (five bilinear taps): sharper than
// bilinear, which would soften it a little more every frame.
fn history_at(uv: vec2<f32>) -> vec3<f32> {
    let position = uv * params.size.xy;
    let centre = floor(position - 0.5) + 0.5;
    let f = position - centre;
    let w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    let w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    let w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    let w3 = f * f * (-0.5 + 0.5 * f);
    let w12 = w1 + w2;
    let offset12 = w2 / w12;
    let texel = params.size.zw;
    let p0 = (centre - 1.0) * texel;
    let p3 = (centre + 2.0) * texel;
    let p12 = (centre + offset12) * texel;
    var sum = vec3<f32>(0.0);
    var weight = 0.0;
    let corners = array<vec3<f32>, 5>(vec3<f32>(p12.x, p0.y, w12.x * w0.y), vec3<f32>(p0.x, p12.y, w0.x * w12.y),
                                      vec3<f32>(p12.x, p12.y, w12.x * w12.y), vec3<f32>(p3.x, p12.y, w3.x * w12.y),
                                      vec3<f32>(p12.x, p3.y, w12.x * w3.y));
    for (var i = 0; i < 5; i = i + 1) {
        let c = corners[i];
        sum = sum + textureSampleLevel(history, linear_sampler, c.xy, 0.0).rgb * c.z;
        weight = weight + c.z;
    }
    return sane(sum / max(weight, 1e-4));
}

// The result (the history of the next frame) and what the panel shows
// (the result, or a view into it): the views never go into the history.
struct Out {
    @location(0) result: vec4<f32>,
    @location(1) shown: vec4<f32>,
};

fn both(result: vec4<f32>) -> Out {
    return Out(result, result);
}

@fragment
fn fs_main(@builtin(position) position: vec4<f32>) -> Out {
    let p = vec2<i32>(position.xy);
    let size = vec2<i32>(params.size.xy);
    let current = squeeze(load_scene(p));
    // The neighbours' colours (their mean and spread) and the nearest point
    // round the pixel (an edge's motion is its front's).
    var m1 = vec3<f32>(0.0);
    var m2 = vec3<f32>(0.0);
    var low = vec3<f32>(1e9);
    var high = vec3<f32>(-1e9);
    var nearest = 0.0;
    var at = p;
    for (var y = -1; y <= 1; y = y + 1) {
        for (var x = -1; x <= 1; x = x + 1) {
            let q = clamp(p + vec2<i32>(x, y), vec2<i32>(0), size - 1);
            let c = to_ycocg(squeeze(load_scene(q)));
            m1 = m1 + c;
            m2 = m2 + c * c;
            low = min(low, c);
            high = max(high, c);
            let d = textureLoad(depth, q, 0); // reversed Z: larger is nearer
            if (d > nearest) {
                nearest = d;
                at = q;
            }
        }
    }
    if (params.eye_shift.w < 0.5) {
        return both(vec4<f32>(unsqueeze(current), 1.0));
    }
    // Where the point was: from this frame's depth back to route space (in
    // homogeneous form: the sky at infinity too), through the last frame's
    // view. A moving model's point moved with the camera's place.
    let uv = (vec2<f32>(at) + 0.5) * params.size.zw;
    let clip = vec4<f32>(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, nearest, 1.0);
    var place = params.inverse_view_projection * clip;
    let moving_depth = textureLoad(moving, at, 0);
    let is_moving = moving_depth > 0.0 && moving_depth >= nearest * 0.9999;
    if (is_moving) {
        place = vec4<f32>(place.xyz - params.eye_shift.xyz * place.w, place.w);
    }
    // The point's place now and then, both without the jitter: the motion
    // the history follows (the jitter's own shift left out).
    let before = params.previous_view_projection * place;
    let now = params.steady_view_projection * (params.inverse_view_projection * clip);
    if (before.w <= 1e-6 || now.w <= 1e-6) {
        return both(vec4<f32>(unsqueeze(current), 1.0));
    }
    let before_ndc = before.xy / before.w;
    let now_ndc = now.xy / now.w;
    let motion = vec2<f32>(before_ndc.x - now_ndc.x, now_ndc.y - before_ndc.y) * 0.5;
    let previous_uv = (position.xy * params.size.zw) + motion;
    if (any(previous_uv < vec2<f32>(0.0)) || any(previous_uv > vec2<f32>(1.0))) {
        return both(vec4<f32>(unsqueeze(current), 1.0));
    }
    let raw = squeeze(history_at(previous_uv));
    // Clipped (YCoCg) to the neighbours' box (the mean and 1.25 of the
    // spread, within their range): what was not here is let go.
    let mean = m1 / 9.0;
    let spread = sqrt(max(m2 / 9.0 - mean * mean, vec3<f32>(0.0)));
    let box_low = max(low, mean - 1.25 * spread);
    let box_high = min(high, mean + 1.25 * spread);
    let past = from_ycocg(clip_to_box(to_ycocg(raw), box_low, box_high));
    // The new frame's share: small where the history and the frame agree
    // (still things stay still, thin wires do not flicker), larger where
    // they differ, the point moved fast or is a train's (its guessed past).
    let a = luma(current);
    let b = luma(past);
    let differ = abs(a - b) / max(max(a, b), 0.2);
    let agree = (1.0 - differ) * (1.0 - differ);
    var weight = mix(0.15, 0.05, agree);
    let pixels = length(motion * params.size.xy);
    weight = mix(weight, 0.25, clamp(pixels / 24.0, 0.0, 1.0));
    if (is_moving) {
        weight = max(weight, 0.15);
    }
    if (params.view.x > 0.5) {
        return Out(vec4<f32>(sane(unsqueeze(mix(past, current, weight))), 1.0),
                   shown(params.view.x, motion * params.size.xy, is_moving, nearest, raw, past));
    }
    return both(vec4<f32>(sane(unsqueeze(mix(past, current, weight))), 1.0));
}
