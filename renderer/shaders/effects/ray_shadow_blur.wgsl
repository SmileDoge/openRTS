// The ray shadows' mask blurred (effects/ray_shadow): a 5 x 5 box of the
// pixels at about the same depth, so a ray a pixel (another point of the
// sun's disc each) does not crackle far away; a caster's edge stays where
// the depth jumps. Each lamp's light among the pixels that traced the same
// lamp in the same place only. Packed as the rays' (ray_mask_format).

@group(0) @binding(0) var<uniform> effects: Effects;
@group(0) @binding(1) var scene_depth: texture_depth_2d;
@group(0) @binding(2) var raw: texture_2d<u32>;
@group(0) @binding(3) var mask: texture_storage_2d<rgba32uint, write>;

// The view depth of a pre-pass depth value.
fn view_depth_of(d: f32) -> f32 {
    let v = effects.inv_projection * vec4<f32>(0.0, 0.0, d, 1.0);
    return abs(v.z / v.w);
}

@compute @workgroup_size(8, 8)
fn ray_shadow_blur(@builtin(global_invocation_id) id: vec3<u32>) {
    let dims = vec2<i32>(textureDimensions(raw));
    let p = vec2<i32>(id.xy);
    if (p.x >= dims.x || p.y >= dims.y) { return; }
    let centre = view_depth_of(textureLoad(scene_depth, p, 0));
    let own = textureLoad(raw, p, 0);
    let own_keys = vec3<u32>(own.g & 65535u, own.g >> 16u, own.b);
    var sums = vec4<f32>(0.0);
    var weights = vec4<f32>(0.0);
    for (var y = -2; y <= 2; y = y + 1) {
        for (var x = -2; x <= 2; x = x + 1) {
            let q = clamp(p + vec2<i32>(x, y), vec2<i32>(0), dims - 1);
            let depth = view_depth_of(textureLoad(scene_depth, q, 0));
            // Within 2% of the depth: the same surface.
            if (abs(depth - centre) > 0.02 * centre + 0.05) { continue; }
            let texel = textureLoad(raw, q, 0);
            let keys = vec3<u32>(texel.g & 65535u, texel.g >> 16u, texel.b);
            sums.x = sums.x + f32(texel.r & 255u);
            weights.x = weights.x + 1.0;
            for (var k = 0u; k < 3u; k = k + 1u) {
                if (own_keys[k] == 0u) { continue; }
                // The same lamp wherever it stands in the neighbour's three.
                for (var m = 0u; m < 3u; m = m + 1u) {
                    if (keys[m] == own_keys[k]) {
                        sums[k + 1u] = sums[k + 1u] + f32((texel.r >> (8u * (m + 1u))) & 255u);
                        weights[k + 1u] = weights[k + 1u] + 1.0;
                    }
                }
            }
        }
    }
    var packed = u32(round(sums.x / max(weights.x, 1.0))) & 255u;
    for (var k = 0u; k < 3u; k = k + 1u) {
        var value = (own.r >> (8u * (k + 1u))) & 255u;
        if (weights[k + 1u] > 0.0) { value = u32(round(sums[k + 1u] / weights[k + 1u])) & 255u; }
        packed = packed | (value << (8u * (k + 1u)));
    }
    // The sun's colour through glass (a) the pixel's own.
    textureStore(mask, p, vec4<u32>(packed, own.g, own.b, own.a));
}
