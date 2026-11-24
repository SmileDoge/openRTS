
@group(0) @binding(0) var source: texture_depth_multisampled_2d;

// One band of the screen per draw (vertices 6 * band ...): two triangles
// each, see DepthExport::record.
const bands = 16u;

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> @builtin(position) vec4<f32> {
    var corners = array<vec2<f32>, 6>(
        vec2<f32>(-1.0, 0.0), vec2<f32>(1.0, 0.0), vec2<f32>(-1.0, 1.0),
        vec2<f32>(-1.0, 1.0), vec2<f32>(1.0, 0.0), vec2<f32>(1.0, 1.0));
    let corner = corners[index % 6u];
    let band = f32(index / 6u);
    let height = 2.0 / f32(bands);
    return vec4<f32>(corner.x, -1.0 + (band + corner.y) * height, 0.0, 1.0);
}

@fragment
fn fs_main(@builtin(position) at: vec4<f32>) -> @builtin(frag_depth) f32 {
    return textureLoad(source, vec2<i32>(at.xy), 0);
}
