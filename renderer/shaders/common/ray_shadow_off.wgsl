// No ray queries (ray_scene.hpp): the shadow maps alone.
fn effects_ray_shadow(world: vec3<f32>, normal: vec3<f32>, frag: vec4<f32>) -> f32 {
    return 1.0;
}

fn effects_ray_lamp(index: u32, frag: vec4<f32>) -> f32 {
    return 1.0;
}

fn effects_ray_sun_tint(frag: vec4<f32>) -> vec3<f32> {
    return vec3<f32>(1.0);
}
