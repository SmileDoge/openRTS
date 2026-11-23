
struct Uniform {
    view_projection: mat4x4<f32>,
    view: mat4x4<f32>,
    eye: vec4<f32>,
    flow: vec4<f32>,
    eye_vel: vec4<f32>,
    light: vec4<f32>,
    params: vec4<f32>,
    to_room: mat4x4<f32>,
    room_lower: vec4<f32>,
    room_upper: vec4<f32>,
    origin: vec4<f32>,
    lamps: array<vec4<f32>, 12>, // per lamp: apex, reach; axis, tangent; colour
    lamp_count: vec4<f32>,
};
@group(0) @binding(0) var<uniform> u: Uniform;

struct Out {
    @builtin(position) clip: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) alpha: f32,
    @location(2) @interpolate(flat) snow: f32,
    @location(3) lamp: vec3<f32>, // the headlights' light on it (display)
};

fn hash(n: u32) -> vec4<f32> {
    var x = n * 747796405u + 2891336453u;
    var r: vec4<f32>;
    for (var k = 0u; k < 4u; k = k + 1u) {
        x = (x ^ (x >> 16u)) * 2246822519u;
        x = (x ^ (x >> 13u)) * 3266489917u;
        x = x ^ (x >> 16u);
        r[k] = f32(x & 0xFFFFFFu) / 16777216.0;
    }
    return r;
}

@vertex
fn vs_main(@builtin(vertex_index) vertex: u32) -> Out {
    var out: Out;
    let i = vertex / 6u;
    let corner = vertex % 6u;
    let h = hash(i);
    let h2 = hash(i + 7919u);
    let snow = u.flow.w > 0.5;
    out.snow = select(0.0, 1.0, snow);
    let t = u.eye.w;
    let half = vec3<f32>(u.params.x, u.params.y, u.params.x);
    let box = half * 2.0;
    // Its own fall: rain 7..9.5 m/s, snow 0.7..1.4 m/s tumbling.
    var v = u.flow.xyz;
    var drift = vec3<f32>(0.0);
    if (snow) {
        v.y = v.y - (0.7 + 0.7 * h.w);
        let ph = h2.xyz * 6.2831853;
        drift = vec3<f32>(sin(t * (0.9 + h2.w) + ph.x), 0.0, cos(t * (0.7 + h2.w * 0.8) + ph.y)) * (0.25 + 0.3 * h2.z);
    } else {
        v.y = v.y - (7.0 + 2.5 * h.w);
    }
    let raw = h.xyz * box + v * t + drift - u.origin.xyz;
    let p = u.eye.xyz + (fract((raw - u.eye.xyz) / box) - vec3<f32>(0.5)) * box;
    let rel = v - u.eye_vel.xyz;
    // Fading in near the eye and out at the box's edge.
    let away = length(p - u.eye.xyz);
    var fade = clamp((away - 0.35) / 0.6, 0.0, 1.0) * clamp((u.params.x - away) / (u.params.x * 0.35), 0.0, 1.0);
    // None inside the cab he is in (its glass stops them).
    if (u.room_lower.w > 0.5) {
        let r = (u.to_room * vec4<f32>(p, 1.0)).xyz;
        if (all(r > u.room_lower.xyz - vec3<f32>(0.05)) && all(r < u.room_upper.xyz + vec3<f32>(0.05))) {
            fade = 0.0;
        }
    }
    if (fade <= 0.0) {
        out.clip = vec4<f32>(2.0, 2.0, 2.0, 1.0);
        out.alpha = 0.0;
        return out;
    }

    let right = vec3<f32>(u.view[0][0], u.view[1][0], u.view[2][0]);
    let up = vec3<f32>(u.view[0][1], u.view[1][1], u.view[2][1]);
    var a: vec3<f32>;
    var b: vec3<f32>;
    var width: f32;
    if (snow) {
        // Finer than they were (1.2 .. 2.8 cm; testers 2026-11-23).
        let r = 0.007 + 0.01 * h2.y;
        let along = select(up, normalize(rel), length(rel) > 0.1);
        // Drawn out by the speed against it: a 1/60 s exposure, at most
        // 0.8 m (on the move, streaks flying at the eye, a blizzard's).
        let stretch = min(length(rel) * (1.0 / 60.0), 0.8);
        a = p + along * r;
        b = p - along * (r + stretch);
        width = r * 2.0;
    } else {
        // A 1/40 s exposure, at most 3 m.
        var trail = rel * (1.0 / 40.0);
        let l = length(trail);
        if (l > 3.0) {
            trail = trail * (3.0 / l);
        }
        a = p;
        b = p - trail;
        width = 0.004 + 0.003 * h2.y; // wider than a drop: its blur
    }
    let ca = u.view_projection * vec4<f32>(a, 1.0);
    let cb = u.view_projection * vec4<f32>(b, 1.0);
    if (ca.w < 0.05 || cb.w < 0.05) {
        out.clip = vec4<f32>(2.0, 2.0, 2.0, 1.0);
        out.alpha = 0.0;
        return out;
    }
    let na = ca.xy / ca.w;
    let nb = cb.xy / cb.w;
    // The width in pixels (at least one, its light shrinking with it).
    let pixels_per_unit = u.eye_vel.w * u.params.w * 0.5;
    let wa = width * pixels_per_unit / ca.w;
    let wb = width * pixels_per_unit / cb.w;
    let wpx = max(max(wa, wb), 1.0);
    fade = fade * sqrt(min((wa + wb) * 0.5 / wpx, 1.0));
    var dir = nb - na;
    let aspect = u.params.z;
    var d2 = vec2<f32>(dir.x * aspect, dir.y);
    if (length(d2) < 1e-6) {
        d2 = vec2<f32>(0.0, 1.0);
    }
    let n2 = normalize(vec2<f32>(-d2.y, d2.x));
    let offset = vec2<f32>(n2.x / aspect, n2.y) * (wpx / u.params.w);
    // Corners: 0 a-, 1 a+, 2 b+, 3 a-, 4 b+, 5 b-.
    let at_b = corner == 2u || corner == 4u || corner == 5u;
    let side = select(-1.0, 1.0, corner == 1u || corner == 2u || corner == 4u);
    let c = select(ca, cb, at_b);
    let n = select(na, nb, at_b);
    out.clip = vec4<f32>((n + offset * side) * c.w, c.z, c.w);
    out.uv = vec2<f32>(side, select(-1.0, 1.0, at_b));
    out.alpha = fade;
    // The headlights' beams: a flake or drop inside a cone shines in its
    // colour, brightest near the lamp, softly to the cone's edge and its reach.
    var lamp = vec3<f32>(0.0);
    let lamps = u32(u.lamp_count.x);
    for (var k = 0u; k < lamps; k = k + 1u) {
        let apex = u.lamps[k * 3u];
        let axis = u.lamps[k * 3u + 1u];
        let v = p - apex.xyz;
        let along = dot(v, axis.xyz);
        if (along <= 0.0 || along >= apex.w) {
            continue;
        }
        let off = length(v - axis.xyz * along) / max(along * axis.w, 1e-3);
        let edge = 1.0 - smoothstep(0.6, 1.0, off);
        let reach = 1.0 - along / apex.w;
        lamp = lamp + u.lamps[k * 3u + 2u].xyz * edge * reach * (6.0 / (1.0 + along * 0.12));
    }
    out.lamp = lamp;
    return out;
}

fn encode(colour: vec3<f32>) -> vec3<f32> {
    return select(colour, pow(max(colour, vec3<f32>(0.0)), vec3<f32>(2.2)), u.light.w > 0.5);
}

@fragment
fn fs_main(input: Out) -> @location(0) vec4<f32> {
    var a: f32;
    var colour: vec3<f32>;
    if (input.snow > 0.5) {
        // A soft disk.
        let r = length(input.uv);
        a = (1.0 - smoothstep(0.35, 1.0, r)) * 0.9 * input.alpha;
        colour = min(u.light.xyz * 1.1 + input.lamp, vec3<f32>(1.0));
    } else {
        // A thin streak, soft across, its ends fading.
        let across = 1.0 - abs(input.uv.x);
        let along = 1.0 - abs(input.uv.y);
        a = across * smoothstep(0.0, 0.35, along) * 0.55 * input.alpha;
        colour = min(u.light.xyz * 1.1 + input.lamp, vec3<f32>(1.0)); // the scene's light (dark at night) and the beams'
    }
    if (a <= 0.002) {
        discard;
    }
    return vec4<f32>(encode(colour) * a, a);
}
