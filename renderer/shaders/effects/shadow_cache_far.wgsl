
@vertex
fn vs_full(@builtin(vertex_index) v: u32) -> @builtin(position) vec4<f32> {
    let xy = vec2<f32>(f32((v << 1u) & 2u), f32(v & 2u)) * 2.0 - 1.0;
    return vec4<f32>(xy, 0.0, 1.0);
}

@fragment
fn fs_far() -> @builtin(frag_depth) f32 {
    return 1.0;
}
