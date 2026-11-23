
struct Shafts {
    inv_view_projection: mat4x4<f32>,
    to_room: mat4x4<f32>,
    eye: vec4<f32>,        // xyz: the camera, w: 1 with a room
    room_lower: vec4<f32>, // w: the room's dust (per metre)
    room_upper: vec4<f32>, // w: the outdoor dust (per metre)
    sizes: vec4<f32>,      // the half image's width, height; the frame's width, height
    march: vec4<f32>,      // x: the cascades' end (m), y: the clouds' march's end (m), z: the light's strength
    beams: array<vec4<f32>, 12>, // the lamps' beams in snow or rain: apex + reach, axis + tangent, colour
    beam: vec4<f32>,       // x: beams, y: the air's scattering in them (per metre)
};
@group(0) @binding(0) var<uniform> shafts: Shafts;
@group(0) @binding(1) var scene_depth: texture_depth_2d;
@group(0) @binding(2) var input_image: texture_2d<f32>;

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> @builtin(position) vec4<f32> {
    let p = vec2<f32>(f32((index << 1u) & 2u), f32(index & 2u));
    return vec4<f32>(p * 2.0 - 1.0, 0.0, 1.0);
}

// A point of the frame (uv, reversed-Z depth) in the world.
fn world_at(uv: vec2<f32>, depth: f32) -> vec3<f32> {
    let w = shafts.inv_view_projection * vec4<f32>(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    return w.xyz / w.w;
}

// How far from the eye the frame shows something at a full-size texel (the
// sky: far).
fn distance_at(texel: vec2<i32>) -> f32 {
    let depth = textureLoad(scene_depth, texel, 0);
    if (depth <= 0.0) { return 1.0e6; }
    let uv = (vec2<f32>(texel) + vec2<f32>(0.5)) / shafts.sizes.zw;
    return min(distance(world_at(uv, depth), shafts.eye.xyz), 1.0e6);
}
