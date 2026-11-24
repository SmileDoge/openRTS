// Ground-truth ambient occlusion (Jimenez et al. 2016, as XeGTAO lays it
// out): two slices a pixel, six steps each way, horizons weighted down
// beyond the radius; normals from the depth. The steps read the view depth
// (prefilter_wgsl), the far ones from its smaller levels (XeGTAO: the level
// log2 of the step's pixels less 3.3, at most 3), the place a step's from
// its ray.
// Then a 5 x 5 blur that keeps to surfaces of about the same depth.
@group(0) @binding(0) var<uniform> effects: Effects;
@group(0) @binding(1) var view_depth: texture_2d<f32>;
@group(0) @binding(2) var ao_out: texture_storage_2d<r32float, write>;

// High: every pixel, two slices of six steps. Low: a texel for each 2 x 2
// pixels (the depth pixel at twice its place), one slice of four steps,
// the steps' reach capped nearer.
override slices: i32 = 2;
override steps: i32 = 6;
override scale: i32 = 1;
override max_radius: f32 = 96.0;

// The view ray through a point of the screen (NDC), at a metre's distance:
// ray_mul * ndc + ray_add (from the inverse projection, per thread).
var<private> ray_mul: vec2<f32>;
var<private> ray_add: vec2<f32>;
var<private> depth_size: vec2<i32>;

fn ray_at(ndc: vec2<f32>) -> vec2<f32> {
    let v = effects.inv_projection * vec4<f32>(ndc, 0.5, 1.0);
    return v.xy / -v.z;
}

fn view_position(pixel: vec2<i32>, level: i32) -> vec3<f32> {
    let p = clamp(pixel, vec2<i32>(0), depth_size - vec2<i32>(1));
    // A level's size: the frame's halved, at least a texel (no array with
    // a dynamic index: GPUs keep those in memory, not registers).
    let shift = vec2<u32>(u32(level));
    let last = max(depth_size >> shift, vec2<i32>(1)) - vec2<i32>(1);
    let distance = textureLoad(view_depth, min(p >> shift, last), level).r;
    let uv = (vec2<f32>(p) + vec2<f32>(0.5)) / vec2<f32>(depth_size);
    let ndc = vec2<f32>(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    return vec3<f32>((ray_mul * ndc + ray_add) * distance, -distance);
}

@compute @workgroup_size(8, 8)
fn gtao(@builtin(global_invocation_id) id: vec3<u32>) {
    depth_size = vec2<i32>(textureDimensions(view_depth, 0));
    let texel = vec2<i32>(id.xy);
    let out_size = vec2<i32>(textureDimensions(ao_out));
    if (texel.x >= out_size.x || texel.y >= out_size.y) { return; }
    let pixel = min(texel * scale, depth_size - vec2<i32>(1));
    if (textureLoad(view_depth, pixel, 0).r >= 1.0e5) { // the sky
        textureStore(ao_out, texel, vec4<f32>(1.0));
        return;
    }
    ray_add = ray_at(vec2<f32>(0.0));
    ray_mul = vec2<f32>(ray_at(vec2<f32>(1.0, 0.0)).x, ray_at(vec2<f32>(0.0, 1.0)).y) - ray_add;
    let p = view_position(pixel, 0);
    let l = view_position(pixel - vec2<i32>(1, 0), 0);
    let r = view_position(pixel + vec2<i32>(1, 0), 0);
    let u = view_position(pixel - vec2<i32>(0, 1), 0);
    let d = view_position(pixel + vec2<i32>(0, 1), 0);
    let dx = select(p - l, r - p, abs(r.z - p.z) < abs(p.z - l.z));
    let dy = select(p - u, d - p, abs(d.z - p.z) < abs(p.z - u.z));
    let v = normalize(-p);
    var n = normalize(cross(dy, dx));
    if (dot(n, v) < 0.0) { n = -n; }

    let radius = effects.ao.x;
    // Pixels a metre covers at this depth.
    let pixels_per_metre = f32(depth_size.y) * 0.5 / (effects.ao.z * max(-p.z, 0.01));
    let radius_px = min(radius * pixels_per_metre, max_radius);
    if (radius_px < 1.0) {
        textureStore(ao_out, texel, vec4<f32>(1.0));
        return;
    }
    let levels = 3;
    let noise = fract(52.9829189 * fract(0.06711056 * f32(texel.x) + 0.00583715 * f32(texel.y)));
    let noise2 = fract(noise * 7.13 + 0.37);
    let falloff_range = 0.615 * radius;
    let falloff_from = radius * (1.0 - 0.615);
    let falloff_mul = -1.0 / falloff_range;
    let falloff_add = falloff_from / falloff_range + 1.0;
    const PI = 3.14159265;
    var visibility = 0.0;
    for (var slice = 0; slice < slices; slice = slice + 1) {
        let phi = (f32(slice) + noise) * PI / f32(slices);
        let omega = vec2<f32>(cos(phi), -sin(phi)); // screen, y down
        let direction = vec3<f32>(omega.x, -omega.y, 0.0);
        let ortho = direction - dot(direction, v) * v;
        let axis = normalize(cross(ortho, v));
        let projected = n - axis * dot(n, axis);
        let projected_length = length(projected);
        if (projected_length < 1e-4) { continue; }
        let sign_n = sign(dot(ortho, projected));
        let cos_n = clamp(dot(projected, v) / projected_length, 0.0, 1.0);
        let angle_n = sign_n * acos(cos_n);
        let low0 = cos(angle_n + PI * 0.5);
        let low1 = cos(angle_n - PI * 0.5);
        var horizon0 = low0;
        var horizon1 = low1;
        for (var step = 0; step < steps; step = step + 1) {
            var s = (f32(step) + noise2) / f32(steps);
            s = s * s;
            let offset = omega * max(s * radius_px, f32(step) + 1.0);
            let level = clamp(i32(round(log2(length(offset)) - 3.3)), 0, levels);
            let q0 = view_position(pixel + vec2<i32>(round(offset)), level);
            let q1 = view_position(pixel - vec2<i32>(round(offset)), level);
            let delta0 = q0 - p;
            let delta1 = q1 - p;
            let dist0 = length(delta0);
            let dist1 = length(delta1);
            let weight0 = clamp(dist0 * falloff_mul + falloff_add, 0.0, 1.0);
            let weight1 = clamp(dist1 * falloff_mul + falloff_add, 0.0, 1.0);
            let cos0 = mix(low0, dot(delta0 / max(dist0, 1e-4), v), weight0);
            let cos1 = mix(low1, dot(delta1 / max(dist1, 1e-4), v), weight1);
            horizon0 = max(horizon0, cos0);
            horizon1 = max(horizon1, cos1);
        }
        var h0 = -acos(clamp(horizon1, -1.0, 1.0));
        var h1 = acos(clamp(horizon0, -1.0, 1.0));
        h0 = angle_n + clamp(h0 - angle_n, -PI * 0.5, PI * 0.5);
        h1 = angle_n + clamp(h1 - angle_n, -PI * 0.5, PI * 0.5);
        let arc0 = (cos_n + 2.0 * h0 * sin(angle_n) - cos(2.0 * h0 - angle_n)) * 0.25;
        let arc1 = (cos_n + 2.0 * h1 * sin(angle_n) - cos(2.0 * h1 - angle_n)) * 0.25;
        visibility = visibility + projected_length * (arc0 + arc1);
    }
    // A slice of a tilted surface can see more than 1; the average over
    // slices (the blur) is 1 unoccluded, so it is clamped only after it (XeGTAO).
    visibility = max(visibility / f32(slices), 0.0);
    textureStore(ao_out, texel, vec4<f32>(pow(visibility, effects.ao.y)));
}
