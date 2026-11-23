// The view depth for the occlusion (2026-11-21, as XeGTAO's prefilter):
// the camera's depth made linear once (metres along the view; the sky far
// off), and three more levels each from the one before, each texel the 2 x 2
// below weighed towards the farthest (XeGTAO's DepthMIPFilter: a near
// sliver does not pull a far wall in). Until then GTAO and its blur turned
// every depth they read back through the inverse projection, and the far
// steps read the full-sized depth all over the frame: in a cab at 1440p
// 0.6 ms and 0.22 ms (testers). A ВЛ80С's cab at 720p: GTAO with this 0.19
// -> 0.11 ms, the blur 0.056 -> 0.027 (DirectX 12, a 9800X3D's RTX 4070 Ti
// SUPER; the medians of three runs each in turn).
@group(0) @binding(0) var<uniform> effects: Effects;
@group(0) @binding(1) var depth_texture: texture_depth_2d;
@group(0) @binding(2) var level0: texture_storage_2d<r32float, write>;
@group(0) @binding(3) var level1: texture_storage_2d<r32float, write>;
@group(0) @binding(4) var level2: texture_storage_2d<r32float, write>;
@group(0) @binding(5) var level3: texture_storage_2d<r32float, write>;

fn view_distance(depth: f32) -> f32 {
    if (depth <= 0.0) { return 1.0e6; } // the sky (reversed Z cleared to 0)
    let v = effects.inv_projection * vec4<f32>(0.0, 0.0, depth, 1.0);
    return -v.z / v.w;
}

fn level_filter(d0: f32, d1: f32, d2: f32, d3: f32) -> f32 {
    let far = max(max(d0, d1), max(d2, d3));
    let radius = 0.75 * effects.ao.x;
    let range = 0.615 * radius;
    let from_near = radius * (1.0 - 0.615);
    let mul = -1.0 / range;
    let add = from_near / range + 1.0;
    let w0 = clamp((far - d0) * mul + add, 0.0, 1.0);
    let w1 = clamp((far - d1) * mul + add, 0.0, 1.0);
    let w2 = clamp((far - d2) * mul + add, 0.0, 1.0);
    let w3 = clamp((far - d3) * mul + add, 0.0, 1.0);
    return (w0 * d0 + w1 * d1 + w2 * d2 + w3 * d3) / max(w0 + w1 + w2 + w3, 1e-4);
}

fn put(level: i32, texel: vec2<i32>, value: f32) {
    if (level == 2) {
        if (all(texel < vec2<i32>(textureDimensions(level2)))) { textureStore(level2, texel, vec4<f32>(value)); }
    } else if (all(texel < vec2<i32>(textureDimensions(level3)))) {
        textureStore(level3, texel, vec4<f32>(value));
    }
}

// Level 1's 8 x 8 of the group's tile, then each smaller level from it.
var<workgroup> tile: array<f32, 64>;

// A group a 16 x 16 tile, a thread a 2 x 2 block of it: its four texels of
// level 0, one of level 1; then 16 and 4 of its threads levels 2 and 3 from
// the shared memory. Four levels: the farthest step (96 pixels) reads level
// 3 (and four storage textures are WebGPU's least a stage).
@compute @workgroup_size(8, 8)
fn prefilter(@builtin(global_invocation_id) id: vec3<u32>, @builtin(local_invocation_id) local: vec3<u32>,
             @builtin(workgroup_id) group: vec3<u32>) {
    let size = vec2<i32>(textureDimensions(depth_texture));
    let base = vec2<i32>(id.xy) * 2;
    var d: array<f32, 4>;
    for (var k = 0; k < 4; k = k + 1) {
        let p = base + vec2<i32>(k & 1, k >> 1);
        let q = min(p, size - vec2<i32>(1));
        d[k] = view_distance(textureLoad(depth_texture, q, 0));
        if (p.x < size.x && p.y < size.y) { textureStore(level0, p, vec4<f32>(d[k])); }
    }
    let one = level_filter(d[0], d[1], d[2], d[3]);
    if (all(vec2<i32>(id.xy) < vec2<i32>(textureDimensions(level1)))) { textureStore(level1, vec2<i32>(id.xy), vec4<f32>(one)); }
    let at = vec2<i32>(local.xy);
    tile[at.y * 8 + at.x] = one;
    workgroupBarrier();
    // Levels 2 and 3: 4 x 4 and 2 x 2 of the tile, in place.
    var span = 8;
    for (var level = 2; level <= 3; level = level + 1) {
        let half_span = span / 2;
        var value = 0.0;
        let working = at.x < half_span && at.y < half_span;
        if (working) {
            let b = at * 2;
            value = level_filter(tile[b.y * 8 + b.x], tile[b.y * 8 + b.x + 1], tile[(b.y + 1) * 8 + b.x], tile[(b.y + 1) * 8 + b.x + 1]);
            put(level, vec2<i32>(group.xy) * half_span + at, value);
        }
        workgroupBarrier();
        if (working) { tile[at.y * 8 + at.x] = value; }
        workgroupBarrier();
        span = half_span;
    }
}
