// Each cluster (a screen tile between two depths, as a view-space box) gets
// the lights whose spheres reach it and, a cone's (headlights, searchlights,
// lamps shining down), whose cone does, nearest first, at most `clusters.w`.
// Until 2026-11-17 the sphere alone: a cone's sphere is its whole reach
// round the lamp, so a headlight's 100 m came into every cluster near the
// train, and every pixel there tried it (the night's main pass, testers).
@group(0) @binding(0) var<uniform> effects: Effects;
// Three per light, view space: its sphere (centre, radius); its cone's apex
// and reach from it (w < 0: no cone, the sphere alone); its axis and the
// tangent of its half angle.
@group(0) @binding(1) var<storage, read> spheres: array<vec4<f32>>;
@group(0) @binding(2) var<storage, read_write> counts: array<u32>;
@group(0) @binding(3) var<storage, read_write> indices: array<u32>;

@compute @workgroup_size(64)
fn assign(@builtin(global_invocation_id) id: vec3<u32>) {
    let grid = effects.clusters;
    let total = grid.x * grid.y * grid.z;
    let cluster = id.x;
    if (cluster >= total) { return; }
    let x = cluster % grid.x;
    let y = (cluster / grid.x) % grid.y;
    let z = cluster / (grid.x * grid.y);
    let near = exp((f32(z) - effects.cluster_params.w) / effects.cluster_params.z);
    let far = exp((f32(z + 1u) - effects.cluster_params.w) / effects.cluster_params.z);
    let x0 = min(f32(x) * effects.cluster_params.x * effects.screen.z, 1.0) * 2.0 - 1.0;
    let x1 = min(f32(x + 1u) * effects.cluster_params.x * effects.screen.z, 1.0) * 2.0 - 1.0;
    let y0 = 1.0 - min(f32(y + 1u) * effects.cluster_params.y * effects.screen.w, 1.0) * 2.0;
    let y1 = 1.0 - min(f32(y) * effects.cluster_params.y * effects.screen.w, 1.0) * 2.0;
    let tx = effects.ao.z * effects.ao.w;
    let ty = effects.ao.z;
    let xs = vec4<f32>(x0 * near, x1 * near, x0 * far, x1 * far) * tx;
    let ys = vec4<f32>(y0 * near, y1 * near, y0 * far, y1 * far) * ty;
    let lo = vec3<f32>(min(min(xs.x, xs.y), min(xs.z, xs.w)), min(min(ys.x, ys.y), min(ys.z, ys.w)), -far);
    let hi = vec3<f32>(max(max(xs.x, xs.y), max(xs.z, xs.w)), max(max(ys.x, ys.y), max(ys.z, ys.w)), -near);
    // The cluster's bounding sphere, for the cones.
    let middle = 0.5 * (lo + hi);
    let bound = 0.5 * length(hi - lo);
    var count = 0u;
    let base = cluster * grid.w;
    for (var i = 0u; i < effects.flags.w; i = i + 1u) {
        if (count >= grid.w) { break; }
        let s = spheres[3u * i];
        let closest = clamp(s.xyz, lo, hi);
        let d = closest - s.xyz;
        if (dot(d, d) > s.w * s.w) { continue; }
        let apex = spheres[3u * i + 1u];
        if (apex.w >= 0.0) {
            // The bounding sphere against the cone (Wronski, "Cull that
            // cone!"): its centre's distance to the cone's side, past its
            // reach, or behind its apex.
            let cone = spheres[3u * i + 2u];
            let v = middle - apex.xyz;
            let along = dot(v, cone.xyz);
            let side = sqrt(max(dot(v, v) - along * along, 0.0));
            let cos_half = inverseSqrt(1.0 + cone.w * cone.w);
            if (side * cos_half - along * cone.w * cos_half > bound || along > apex.w + bound || along < -bound) { continue; }
        }
        indices[base + count] = i;
        count = count + 1u;
    }
    counts[cluster] = count;
}
