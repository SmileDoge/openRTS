// The shadows by rays (ray_scene.hpp): effects/ray_shadow's mask, a value a
// pixel of the camera's pre-pass, packed (scene_effects.cpp, ray_mask_format).
@group(2) @binding(14) var ray_mask: texture_2d<u32>;

fn effects_ray_texel(frag: vec4<f32>) -> vec4<u32> {
    let size = vec2<i32>(textureDimensions(ray_mask));
    let p = clamp(vec2<i32>(frag.xy), vec2<i32>(0), size - 1);
    return textureLoad(ray_mask, p, 0);
}

// 1 lit, 0 something between the point and the sun. Off (views.w 0): 1.
// A point in front of the pre-pass's surface (glass, a blended body) takes
// what is behind it: the light that reaches there.
fn effects_ray_shadow(world: vec3<f32>, normal: vec3<f32>, frag: vec4<f32>) -> f32 {
    if (effects.views.w < 0.5) { return 1.0; }
    return f32(effects_ray_texel(frag).r & 255u) / 255.0;
}

// The sun's colour through glass by rays (1, 1, 1: none, or white light).
fn effects_ray_sun_tint(frag: vec4<f32>) -> vec3<f32> {
    if (effects.views.w < 0.5) { return vec3<f32>(1.0); }
    let a = effects_ray_texel(frag).a;
    return vec3<f32>(f32(a & 255u), f32((a >> 8u) & 255u), f32((a >> 16u) & 255u)) / 255.0;
}

// A lamp's light by rays (`index` in scene_lights): the mask's when the
// pixel traced this lamp (one of three), else none known (1).
fn effects_ray_lamp(index: u32, frag: vec4<f32>) -> f32 {
    if (effects.views.w < 0.5) { return 1.0; }
    let t = effects_ray_texel(frag);
    let key = index + 1u;
    if ((t.g & 65535u) == key) { return f32((t.r >> 8u) & 255u) / 255.0; }
    if ((t.g >> 16u) == key) { return f32((t.r >> 16u) & 255u) / 255.0; }
    if ((t.b & 65535u) == key) { return f32(t.r >> 24u) / 255.0; }
    return 1.0;
}
