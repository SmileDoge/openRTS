
@group(1) @binding(0) var<uniform> effects: Effects;
@group(1) @binding(1) var shadow_map: texture_depth_2d_array;
@group(1) @binding(2) var shadow_sampler: sampler_comparison;
@group(1) @binding(8) var cloud_shadow_map: texture_2d<f32>;
@group(1) @binding(9) var cloud_sampler: sampler;

// The clouds' shadow at a point (as effects_cloud_shadow in the models).
fn cloud_light(world: vec3<f32>) -> f32 {
    let strength = effects.cloud_sun.w;
    if (strength <= 0.0) { return effects.cloud_edge.z; }
    let sun = effects.cloud_sun.xyz;
    let d = world - effects.eye.xyz;
    let q = d.xz + sun.xz * ((effects.cloud_area.w - d.y) / max(sun.y, 0.05));
    let uv = (q - effects.cloud_area.xy) / (2.0 * effects.cloud_area.z) + vec2<f32>(0.5);
    let lit = smoothstep(effects.cloud_edge.x, effects.cloud_edge.y, textureSampleLevel(cloud_shadow_map, cloud_sampler, uv, 0.0).r);
    return effects.cloud_edge.z - strength * (1.0 - lit);
}

// Whether the sun reaches a point of the air: the cab's map where it covers
// it, else the cascade of its view depth (one filtered compare); 1 past the
// last cascade.
fn sun_reaches(p: vec3<f32>) -> f32 {
    let count = u32(effects.shadow.x);
    if (effects.focus_params.x > 0.0) {
        let c = effects.focus * vec4<f32>(p, 1.0);
        if (all(abs(c.xy) < vec2<f32>(1.0 - effects.focus_params.z)) && c.z >= 0.0 && c.z <= 1.0) {
            let uv = c.xy * vec2<f32>(0.5, -0.5) + vec2<f32>(0.5);
            return textureSampleCompareLevel(shadow_map, shadow_sampler, uv, u32(effects.focus_params.x) - 1u, c.z);
        }
    }
    let depth = dot(p - effects.eye.xyz, effects.forward.xyz);
    for (var i = 0u; i < count; i = i + 1u) {
        if (depth < effects.splits[i]) {
            let c = effects.cascades[i] * vec4<f32>(p, 1.0);
            let uv = c.xy * vec2<f32>(0.5, -0.5) + vec2<f32>(0.5);
            if (any(uv < vec2<f32>(0.0)) || any(uv > vec2<f32>(1.0)) || c.z > 1.0 || c.z < 0.0) { return 1.0; }
            return textureSampleCompareLevel(shadow_map, shadow_sampler, uv, i, c.z);
        }
    }
    return 1.0;
}

// The dust at a point: thin outdoors, thinning with height above the eye;
// thicker in the room.
fn dust(p: vec3<f32>) -> f32 {
    var d = shafts.room_upper.w * exp(-max(p.y - shafts.eye.y, 0.0) / 300.0);
    if (shafts.eye.w > 0.0) {
        let q = (shafts.to_room * vec4<f32>(p, 1.0)).xyz;
        if (all(q >= shafts.room_lower.xyz) && all(q <= shafts.room_upper.xyz)) { d = d + shafts.room_lower.w; }
    }
    return d;
}

@fragment
fn fs_march(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> {
    // The nearest of the four full-size texels under this one.
    let size = vec2<i32>(textureDimensions(scene_depth));
    let base = vec2<i32>(position.xy) * 2;
    var nearest = 1.0e6;
    for (var k = 0; k < 4; k = k + 1) {
        let texel = min(base + vec2<i32>(k & 1, k >> 1), size - vec2<i32>(1));
        nearest = min(nearest, distance_at(texel));
    }
    let count = u32(effects.shadow.x);
    if (count == 0u || shafts.march.z <= 0.0) { return vec4<f32>(0.0, 0.0, 0.0, nearest); }
    let uv = (vec2<f32>(base) + vec2<f32>(1.0)) / shafts.sizes.zw;
    let eye = shafts.eye.xyz;
    let dir = normalize(world_at(uv, 1.0) - eye);
    // Henyey-Greenstein (g 0.7) and a little of an even scatter, times pi:
    // the scene lights a surface with its albedo x the sun (no 1 / pi), so
    // the air's light is weighed the same way.
    let c = dot(dir, effects.sun.xyz);
    let g = 0.7;
    let hg = (1.0 - g * g) / (12.566371 * pow(max(1.0 + g * g - 2.0 * g * c, 1e-4), 1.5));
    let phase = 3.14159265 * (0.85 * hg + 0.15 / 12.566371);
    // Interleaved gradient noise: each pixel's steps start apart (the blur
    // joins them).
    let jitter = fract(52.9829189 * fract(dot(floor(position.xy), vec2<f32>(0.06711056, 0.00583715))));
    let steps = 32u;
    let near_end = min(nearest * 0.98, shafts.march.x);
    var light = 0.0;
    var optical = 0.0;
    for (var i = 0u; i < steps; i = i + 1u) {
        // Closer together near the eye: t = end x ((i + jitter) / n)^2.
        let a = (f32(i) + jitter) / f32(steps);
        let t = near_end * a * a;
        let dt = near_end * (2.0 * f32(i) + 1.0) / f32(steps * steps);
        let p = eye + dir * t;
        let sigma = dust(p);
        light = light + sigma * exp(-optical) * sun_reaches(p) * cloud_light(p) * dt;
        optical = optical + sigma * dt;
    }
    // On under the clouds' shadow only (no cascade covers it there).
    let far_end = min(nearest * 0.98, shafts.march.y);
    if (effects.cloud_sun.w > 0.0 && far_end > near_end) {
        let far_steps = 8u;
        let dt = (far_end - near_end) / f32(far_steps);
        for (var i = 0u; i < far_steps; i = i + 1u) {
            let p = eye + dir * (near_end + (f32(i) + jitter) * dt);
            let sigma = 0.5 * dust(p);
            light = light + sigma * exp(-optical) * cloud_light(p) * dt;
            optical = optical + sigma * dt;
        }
    }
    let colour = effects.sun_colour.rgb * (phase * light * effects.shadow.y * shafts.march.z);
    return vec4<f32>(min(colour, vec3<f32>(16.0)), nearest);
}
