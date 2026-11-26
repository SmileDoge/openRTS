// The screen's motion of every pixel since the last frame
// (renderer/motion_vectors.hpp): xy to where it was (texture coordinates),
// z 1 a train's, w 1 where there is a last frame.
struct Params {
    inverse_view_projection: mat4x4<f32>,  // this frame's clip (jittered) -> route space
    previous_view_projection: mat4x4<f32>, // the last frame's (steady) route space -> clip
    steady_view_projection: mat4x4<f32>,   // this frame's without the jitter
    train_shift: vec4<f32>,                // xyz: the player's train's move since; w: 1 a last frame
    size: vec4<f32>,                       // width, height, 1 / width, 1 / height
};

@group(0) @binding(0) var depth: texture_depth_2d;
@group(0) @binding(1) var moving: texture_depth_2d;
@group(0) @binding(2) var<uniform> params: Params;

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> @builtin(position) vec4<f32> {
    let uv = vec2<f32>(f32((index << 1u) & 2u), f32(index & 2u));
    return vec4<f32>(uv * 2.0 - 1.0, 0.0, 1.0);
}

@fragment
fn fs_main(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> {
    let p = vec2<i32>(position.xy);
    let d = textureLoad(depth, p, 0); // reversed Z: larger is nearer
    let moving_depth = textureLoad(moving, p, 0);
    let is_train = moving_depth > 0.0 && moving_depth >= d * 0.9999;
    let train = select(0.0, 1.0, is_train);
    if (params.train_shift.w < 0.5) {
        return vec4<f32>(0.0, 0.0, train, 0.0);
    }
    // From this frame's depth back to route space (homogeneous: the sky at
    // infinity too), then through the last frame's view; both places
    // without the jitter.
    let uv = (vec2<f32>(p) + 0.5) * params.size.zw;
    let clip = vec4<f32>(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0);
    let place = params.inverse_view_projection * clip;
    var then_place = place;
    if (is_train) {
        then_place = vec4<f32>(place.xyz - params.train_shift.xyz * place.w, place.w);
    }
    let before = params.previous_view_projection * then_place;
    let now = params.steady_view_projection * place;
    if (before.w <= 1e-6 || now.w <= 1e-6) {
        return vec4<f32>(0.0, 0.0, train, 0.0);
    }
    let before_ndc = before.xy / before.w;
    let now_ndc = now.xy / now.w;
    let motion = vec2<f32>(before_ndc.x - now_ndc.x, now_ndc.y - before_ndc.y) * 0.5;
    return vec4<f32>(motion, train, 1.0);
}
