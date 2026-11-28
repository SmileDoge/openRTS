// The shadows by rays (ray_scene.hpp), from the camera's pre-pass depth, a
// pixel at a time, into the mask the main pass reads (common/ray_shadow):
//   r: the sun's light (a ray towards a point of the sun's disc widened by
//      the shadow softness, another point each pixel and frame: the
//      temporal AA averages them), 1 lit, 0 something between;
//   lamps: of the pixel's cluster, the three lighting it most that have no
//      shadow map, a ray to each.
// Packed (scene_effects.cpp, ray_mask_format): r the lights 0..255 (the sun
// bits 0-7, the lamps 8-15, 16-23, 24-31), g the first two lamps' indices
// + 1 (16 bits each), b the third's (0: none).
// Sky: lit, no lamp.

@group(0) @binding(0) var<uniform> effects: Effects;
@group(0) @binding(1) var scene_depth: texture_depth_2d;
@group(0) @binding(2) var ray_scene: acceleration_structure;
@group(0) @binding(3) var mask: texture_storage_2d<rgba32uint, write>;
@group(0) @binding(4) var<storage, read> scene_lights: array<Light>;
@group(0) @binding(5) var<storage, read> cluster_counts: array<u32>;
@group(0) @binding(6) var<storage, read> cluster_lights: array<u32>;
// The cut-outs (ray_scene.hpp): each cut-out texture's alpha a layer, each
// cut-out geometry's corners' UVs, and by geometry (an instance's custom data
// its first) an entry: x its UVs' first; y its layer (bits 0-15; 0xFFFFFFFF:
// opaque), cut-off (bits 16-23: an 8-bit alpha a texel casts over, as the
// drawing's, formats::cutoff_byte) and kind (bits 24-31: 0 a mask, 1
// transmission, its layer the alpha as it is); z a transmission's RGB.
@group(0) @binding(7) var cut_out_masks: texture_2d_array<f32>;
@group(0) @binding(8) var<storage, read> cut_out_uvs: array<vec2<f32>>;
@group(0) @binding(9) var<storage, read> cut_out_table: array<vec4<u32>>;

// A candidate's texture alpha (0..1) at its hit.
fn cut_out_alpha(c: RayIntersection, entry: vec4<u32>) -> f32 {
    let first = entry.x + c.primitive_index * 3u;
    let b = c.barycentrics;
    let uv = cut_out_uvs[first] * (1.0 - b.x - b.y) + cut_out_uvs[first + 1u] * b.x + cut_out_uvs[first + 2u] * b.y;
    let size = textureDimensions(cut_out_masks);
    let wrapped = uv - floor(uv);
    let texel = min(vec2<u32>(wrapped * vec2<f32>(size)), size - 1u);
    return textureLoad(cut_out_masks, texel, entry.y & 0xFFFFu, 0).r;
}

// Past this many metres from the eye the cut-outs' textures are not read: a
// mask solid, a transmission all of its colour (a shadow's holes there a
// pixel or so). Letting the masks by there instead cost more (2026-11-27): a
// ray let by went on through every tree.
const cut_out_reach = 60.0;
// Transmissions a ray takes at most; past them it counts as blocked (no
// light leaking through a row of glass), as when next to none is left.
const most_layers = 4u;
// The world point (the effects' space) of pixel `p`.
fn world_at(p: vec2<i32>, size: vec2<f32>) -> vec3<f32> {
    let d = textureLoad(scene_depth, p, 0);
    let uv = (vec2<f32>(p) + vec2<f32>(0.5)) / size;
    let ndc = vec4<f32>(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0);
    let v = effects.inv_projection * ndc;
    let view_point = v.xyz / v.w;
    // The view is rigid: its inverse is the transposed rotation.
    let r = mat3x3<f32>(effects.view[0].xyz, effects.view[1].xyz, effects.view[2].xyz);
    return transpose(r) * (view_point - effects.view[3].xyz);
}

// The light let by between `origin` and `origin + towards * reach`, RGB: 0
// blocked (an opaque hit, a mask's candidate where its texture is over the
// cut-off: confirmed, 4 ends the ray there), else each transmission's
// mix(1, its colour, its alpha) (glass), once a geometry of an instance (a
// pane's two faces, a car's windows on both sides darken once). `far`: no
// texture read. (The opaque first in a ray of their own and the cut-outs in
// another cost more, 2026-11-27: most rays meet nothing and went through the
// scene twice.)
fn light_through(origin: vec3<f32>, towards: vec3<f32>, reach: f32, far: bool) -> vec3<f32> {
    if (reach <= 0.0) { return vec3<f32>(1.0); }
    var rq: ray_query;
    rayQueryInitialize(&rq, ray_scene, RayDesc(4u, 0xFFu, 0.0, reach, origin, towards));
    var through = vec3<f32>(1.0);
    var layers = 0u;
    var seen = array<vec2<u32>, 4>(vec2<u32>(0xFFFFFFFFu), vec2<u32>(0xFFFFFFFFu), vec2<u32>(0xFFFFFFFFu), vec2<u32>(0xFFFFFFFFu));
    while (rayQueryProceed(&rq)) {
        let c = rayQueryGetCandidateIntersection(&rq);
        if (c.kind != 1u) { continue; }
        let entry = cut_out_table[c.instance_custom_data + c.geometry_index];
        if (entry.y == 0xFFFFFFFFu) {
            rayQueryConfirmIntersection(&rq);
            continue;
        }
        if ((entry.y >> 24u) == 0u) {
            if (far || u32(round(cut_out_alpha(c, entry) * 255.0)) > ((entry.y >> 16u) & 0xFFu)) { rayQueryConfirmIntersection(&rq); }
            continue;
        }
        // A transmission: once a geometry of an instance.
        let key = vec2<u32>(c.instance_index, c.geometry_index);
        var again = false;
        for (var i = 0u; i < min(layers, most_layers); i = i + 1u) { again = again || all(seen[i] == key); }
        if (again) { continue; }
        if (layers >= most_layers) {
            rayQueryConfirmIntersection(&rq);
            continue;
        }
        seen[layers] = key;
        layers = layers + 1u;
        let colour = vec3<f32>(f32(entry.z & 255u), f32((entry.z >> 8u) & 255u), f32((entry.z >> 16u) & 255u)) / 255.0;
        let alpha = select(cut_out_alpha(c, entry), 1.0, far);
        through = through * mix(vec3<f32>(1.0), colour, alpha);
        if (max(through.x, max(through.y, through.z)) < 0.02) { rayQueryConfirmIntersection(&rq); }
    }
    if (rayQueryGetCommittedIntersection(&rq).kind != 0u) { return vec3<f32>(0.0); }
    return through;
}

// A lamp with a shadow map of its own (SceneEffects::choose_lamp_shadows).
fn has_map(index: u32) -> bool {
    let count = u32(effects.lamp_shadow_params.x);
    for (var s = 0u; s < count; s = s + 1u) {
        if (effects.lamp_shadow_ids[s / 4u][s % 4u] == index + 1u) { return true; }
    }
    return false;
}

@compute @workgroup_size(8, 8)
fn ray_shadow(@builtin(global_invocation_id) id: vec3<u32>) {
    let dims = textureDimensions(scene_depth);
    if (id.x >= dims.x || id.y >= dims.y) { return; }
    let p = vec2<i32>(id.xy);
    let size = vec2<f32>(dims);
    let d = textureLoad(scene_depth, p, 0);
    // Nothing drawn there (the sky): cleared to the far plane.
    if (d == 0.0 || d == 1.0) {
        textureStore(mask, p, vec4<u32>(0xFFFFFFFFu, 0u, 0u, 0xFFFFFFu));
        return;
    }
    let world = world_at(p, size);
    // The surface's normal from the nearer neighbours on each axis.
    let right = world_at(min(p + vec2<i32>(1, 0), vec2<i32>(dims) - 1), size);
    let left = world_at(max(p - vec2<i32>(1, 0), vec2<i32>(0)), size);
    let down = world_at(min(p + vec2<i32>(0, 1), vec2<i32>(dims) - 1), size);
    let up = world_at(max(p - vec2<i32>(0, 1), vec2<i32>(0)), size);
    let dx = select(world - left, right - world, distance(right, world) < distance(left, world));
    let dy = select(world - up, down - world, distance(down, world) < distance(up, world));
    var n = cross(dy, dx);
    let to_eye = normalize(effects.eye.xyz - world);
    n = select(vec3<f32>(0.0, 1.0, 0.0), normalize(n), dot(n, n) > 1e-12);
    n = select(-n, n, dot(n, to_eye) >= 0.0);
    // Off the surface by more far away (the depth's precision, and the
    // normal from neighbours a pixel apart, metres there).
    let far = distance(world, effects.eye.xyz);
    let reach = 0.02 + 0.002 * far;
    let base = world + n * reach + to_eye * (0.003 * far);

    // Interleaved gradient noise, moved each frame (views.w - 1).
    let shift = f32(u32(effects.views.w) - 1u) * 5.588238;
    let q = vec2<f32>(p) + vec2<f32>(shift, shift);
    let u = fract(52.9829189 * fract(dot(q, vec2<f32>(0.06711056, 0.00583715))));
    let v = fract(u * 7.7319 + 0.6180339);

    // The sun (facing away from it: its own shade, the lighting has it).
    var sun_lit = vec3<f32>(1.0);
    let sun = normalize(effects.sun.xyz);
    if (dot(n, sun) > 0.0) {
        let spread = 0.0047 * max(effects.shadow.z, 1.0);
        let axis_up = select(vec3<f32>(0.0, 1.0, 0.0), vec3<f32>(1.0, 0.0, 0.0), abs(sun.y) > 0.9);
        let across = normalize(cross(sun, axis_up));
        let along = cross(across, sun);
        let r = sqrt(u) * spread;
        let a = 6.2831853 * v;
        let towards = normalize(sun + (across * cos(a) + along * sin(a)) * r);
        sun_lit = light_through(base + sun * reach, towards, 3000.0, far > cut_out_reach);
    }

    // The three lamps lighting it most of those without a map (the clustered list).
    var keys = array<u32, 3>(0u, 0u, 0u);
    var lit = array<u32, 3>(255u, 255u, 255u);
    if (effects.flags.x != 0u) {
        let depth = max(dot(world - effects.eye.xyz, effects.forward.xyz), effects.eye.w);
        let slice = u32(clamp(log(depth) * effects.cluster_params.z + effects.cluster_params.w, 0.0, f32(effects.clusters.z - 1u)));
        let cx = min(u32(f32(p.x) / effects.cluster_params.x), effects.clusters.x - 1u);
        let cy = min(u32(f32(p.y) / effects.cluster_params.y), effects.clusters.y - 1u);
        let cluster = (slice * effects.clusters.y + cy) * effects.clusters.x + cx;
        let count = min(cluster_counts[cluster], effects.clusters.w);
        let first = cluster * effects.clusters.w;
        var best = array<f32, 3>(0.0, 0.0, 0.0);
        for (var i = 0u; i < count; i = i + 1u) {
            let index = cluster_lights[first + i];
            let light = scene_lights[index];
            if (has_map(index)) { continue; }
            let strength = physical_light(light, vec4<f32>(world, 1.0), n);
            if (strength <= 0.0) { continue; }
            let colour = select(light.diffuse.xyz, light.ambient.xyz, light.ambient.w > 0.5);
            let seen = strength * dot(colour, vec3<f32>(0.2126, 0.7152, 0.0722));
            // Into the three best, kept in order.
            if (seen > best[2]) {
                var at = 2u;
                while (at > 0u && seen > best[at - 1u]) {
                    best[at] = best[at - 1u];
                    keys[at] = keys[at - 1u];
                    at = at - 1u;
                }
                best[at] = seen;
                keys[at] = index + 1u;
            }
        }
        for (var k = 0u; k < 3u; k = k + 1u) {
            if (keys[k] == 0u) { continue; }
            let light = scene_lights[keys[k] - 1u];
            // Towards the lamp: in its own space the lamp is the origin (a
            // row's nearest lamp, as pbr_lamp_way), turned back to the world.
            var lp = light.translation * vec4<f32>(world, 1.0);
            if (light.kind.y >= 2.0) {
                let projection = lp * light.array_step;
                let along_row = (projection.x + projection.y + projection.z) / light.array_step.w;
                let closest = clamp(i32(round(along_row)), 0, i32(light.kind.y) - 1);
                lp = lp - vec4<f32>(light.array_step.xyz, 0.0) * f32(closest);
            }
            let r = mat3x3<f32>(light.translation[0].xyz, light.translation[1].xyz, light.translation[2].xyz);
            let to_lamp = transpose(r) * (-lp.xyz);
            let length_to = length(to_lamp);
            if (length_to > 1e-3) {
                // Short of the lamp's own body (its source and some).
                let short = max(light.size[3], 0.0) + 0.15;
                // Off the surface towards the lamp too (as the sun's ray): a
                // surface lit at a grazing angle (a hopper's rounded ends)
                // shaded itself (testers 2026-11-27).
                let towards = to_lamp / length_to;
                let start = base + towards * (2.0 * reach);
                let through = light_through(start, towards, length_to - short - 2.0 * reach, far > cut_out_reach);
                lit[k] = u32(round(clamp(dot(through, vec3<f32>(0.2126, 0.7152, 0.0722)), 0.0, 1.0) * 255.0));
            }
        }
    }
    // The sun: its strongest channel in r, its colour (each channel over
    // the strongest, 8 bits each) in a (white: 0xFFFFFF).
    let strongest = max(sun_lit.x, max(sun_lit.y, sun_lit.z));
    let sun_byte = u32(round(clamp(strongest, 0.0, 1.0) * 255.0));
    var tint = 0xFFFFFFu;
    if (strongest > 0.0 && any(sun_lit != vec3<f32>(strongest))) {
        let c = vec3<u32>(round(clamp(sun_lit / max(strongest, 1e-6), vec3<f32>(0.0), vec3<f32>(1.0)) * 255.0));
        tint = c.x | (c.y << 8u) | (c.z << 16u);
    }
    textureStore(mask, p, vec4<u32>(sun_byte | (lit[0] << 8u) | (lit[1] << 16u) | (lit[2] << 24u), keys[0] | (keys[1] << 16u), keys[2], tint));
}
