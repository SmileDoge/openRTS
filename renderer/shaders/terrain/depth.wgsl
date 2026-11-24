// Depth only (SceneEffects): each tile's whole mesh in one draw.
struct Draw {
    mvp: mat4x4<f32>,
    offset: vec4<f32>,
};
@group(0) @binding(0) var<uniform> draw: Draw;

@vertex
fn vs_depth(@location(0) position: vec3<f32>) -> @builtin(position) vec4<f32> {
    return draw.mvp * vec4<f32>(position, 1.0);
}
