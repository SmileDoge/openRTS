// --shadow-cache-check: the texels of the working maps that differ from the
// cascades drawn whole.
@group(0) @binding(0) var working: texture_depth_2d_array;
@group(0) @binding(1) var whole: texture_depth_2d_array;
@group(0) @binding(2) var<storage, read_write> counts: array<atomic<u32>, 8>;

@compute @workgroup_size(8, 8)
fn compare(@builtin(global_invocation_id) id: vec3<u32>) {
    let size = textureDimensions(whole);
    if (id.x >= size.x || id.y >= size.y) { return; }
    let a = textureLoad(working, vec2<i32>(id.xy), i32(id.z), 0);
    let b = textureLoad(whole, vec2<i32>(id.xy), i32(id.z), 0);
    if (abs(a - b) > 2e-5) {
        atomicAdd(&counts[id.z], 1u);
    }
    if (b < 1.0) {
        atomicAdd(&counts[id.z + 4u], 1u);
    }
}
