// The blur (2026-11-21): a group's tile of the occlusion and of the view
// depth read once into its shared memory, the taps from there.
@group(0) @binding(0) var<uniform> effects: Effects;
@group(0) @binding(1) var view_depth: texture_2d<f32>;
@group(0) @binding(2) var ao_in: texture_2d<f32>;
@group(0) @binding(3) var ao_out: texture_storage_2d<r32float, write>;

// High: 5 x 5 of the frame-sized occlusion. Low: 3 x 3 texels of the
// half-sized one round the pixel (as far as high's 5 x 5), each weighed by
// its depth pixel's (twice its place) against the pixel's, so a texel of
// another surface does not bleed over an edge.
override scale: i32 = 1;
override reach: i32 = 2;

// A group's 8 x 8 pixels: 8 / scale texels of ao_in a side, `reach` round
// (12 x 12 at most).
var<workgroup> tile_ao: array<f32, 144>;
var<workgroup> tile_depth: array<f32, 144>;

@compute @workgroup_size(8, 8)
fn blur(@builtin(global_invocation_id) id: vec3<u32>, @builtin(local_invocation_index) index: u32,
        @builtin(workgroup_id) group: vec3<u32>) {
    let size = vec2<i32>(textureDimensions(ao_out));
    let in_size = vec2<i32>(textureDimensions(ao_in));
    let depth_size = vec2<i32>(textureDimensions(view_depth, 0));
    let span = 8 / scale + 2 * reach;
    let origin = vec2<i32>(group.xy) * (8 / scale) - vec2<i32>(reach);
    for (var k = i32(index); k < span * span; k = k + 64) {
        let q = clamp(origin + vec2<i32>(k % span, k / span), vec2<i32>(0), in_size - vec2<i32>(1));
        tile_ao[k] = textureLoad(ao_in, q, 0).r;
        tile_depth[k] = textureLoad(view_depth, min(q * scale, depth_size - vec2<i32>(1)), 0).r;
    }
    workgroupBarrier();
    let pixel = vec2<i32>(id.xy);
    if (pixel.x >= size.x || pixel.y >= size.y) { return; }
    let centre = textureLoad(view_depth, min(pixel, depth_size - vec2<i32>(1)), 0).r;
    let at = pixel / scale - origin;
    // Joined up, no texel may be of the pixel's own surface (a thin pole):
    // then they count alike rather than not at all.
    let least = select(0.0, 0.001, scale > 1);
    var sum = 0.0;
    var weights = 0.0;
    for (var y = -reach; y <= reach; y = y + 1) {
        for (var x = -reach; x <= reach; x = x + 1) {
            let t = at + vec2<i32>(x, y);
            let k = t.y * span + t.x;
            let w = max(exp(-abs(tile_depth[k] - centre) / max(centre * 0.03, 0.02)), least);
            sum = sum + tile_ao[k] * w;
            weights = weights + w;
        }
    }
    textureStore(ao_out, pixel, vec4<f32>(clamp(sum / max(weights, 1e-4), 0.03, 1.0)));
}
