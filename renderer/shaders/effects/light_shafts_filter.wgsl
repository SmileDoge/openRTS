
// A 9-tap Gaussian along x or y over texels at about the same distance.
fn blur(position: vec4<f32>, axis: vec2<i32>) -> vec4<f32> {
    let size = vec2<i32>(textureDimensions(input_image));
    let centre = vec2<i32>(position.xy);
    let middle = textureLoad(input_image, centre, 0);
    let d0 = middle.a;
    var sum = vec3<f32>(0.0);
    var weight = 0.0;
    for (var k = -4; k <= 4; k = k + 1) {
        let texel = clamp(centre + axis * k, vec2<i32>(0), size - vec2<i32>(1));
        let tap = textureLoad(input_image, texel, 0);
        let w = exp(-f32(k * k) / 8.0) * exp(-abs(tap.a - d0) / (0.1 * d0 + 0.2));
        sum = sum + tap.rgb * w;
        weight = weight + w;
    }
    return vec4<f32>(sum / max(weight, 1e-5), d0);
}

@fragment
fn fs_blur_x(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> {
    return blur(position, vec2<i32>(1, 0));
}

@fragment
fn fs_blur_y(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> {
    return blur(position, vec2<i32>(0, 1));
}

// Up to the frame's size: the four half-size texels round it, bilinearly,
// those at about this texel's distance (a tree's edge against the sky keeps
// its own).
@fragment
fn fs_up(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> {
    let d = distance_at(vec2<i32>(position.xy));
    let half_size = vec2<i32>(textureDimensions(input_image));
    let at = position.xy * 0.5 - vec2<f32>(0.5);
    let origin = vec2<i32>(floor(at));
    let f = at - floor(at);
    var sum = vec3<f32>(0.0);
    var weight = 0.0;
    var closest = vec3<f32>(0.0);
    var closest_gap = 1.0e9;
    for (var k = 0; k < 4; k = k + 1) {
        let offset = vec2<i32>(k & 1, k >> 1);
        let texel = clamp(origin + offset, vec2<i32>(0), half_size - vec2<i32>(1));
        let tap = textureLoad(input_image, texel, 0);
        let bilinear = select(1.0 - f.x, f.x, offset.x == 1) * select(1.0 - f.y, f.y, offset.y == 1);
        let gap = abs(tap.a - d);
        let w = bilinear * exp(-gap / (0.1 * d + 0.2)) + 1e-6 * bilinear;
        sum = sum + tap.rgb * w;
        weight = weight + w;
        if (gap < closest_gap) {
            closest_gap = gap;
            closest = tap.rgb;
        }
    }
    if (weight < 1e-3) { return vec4<f32>(closest, 1.0); }
    return vec4<f32>(sum / weight, 1.0);
}
