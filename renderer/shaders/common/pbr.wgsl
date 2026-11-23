
const pbr_pi: f32 = 3.14159265;

// What a surface is for the light: its Lambert albedo (the base colour less
// what a metal reflects), its reflectance at normal incidence (F0: the
// dielectric's, for a metal the base colour), how much of the dielectric's
// reflection there is (KHR_materials_specular's specularFactor; a metal's
// all), alpha (the roughness squared), its normal and the way to the eye.
struct PbrSurface {
    diffuse: vec3<f32>,
    f0: vec3<f32>,
    specular: f32,
    alpha: f32,
    n: vec3<f32>,
    v: vec3<f32>,
    n_dot_v: f32,
};

// `material`: x roughness (perceptual), y metallic, z a dielectric's F0,
// w its specular factor (renderer::SurfaceMaterial).
fn pbr_surface(base: vec3<f32>, n: vec3<f32>, v: vec3<f32>, material: vec4<f32>) -> PbrSurface {
    var s: PbrSurface;
    let roughness = clamp(material.x, 0.045, 1.0);
    let metallic = clamp(material.y, 0.0, 1.0);
    s.diffuse = base * (1.0 - metallic);
    s.f0 = mix(vec3<f32>(material.z), base, metallic);
    s.specular = mix(clamp(material.w, 0.0, 1.0), 1.0, metallic);
    s.alpha = roughness * roughness;
    s.n = n;
    s.v = v;
    s.n_dot_v = clamp(dot(n, v), 1e-4, 1.0);
    return s;
}

fn pbr_d_ggx(n_dot_h: f32, alpha: f32) -> f32 {
    let a2 = alpha * alpha;
    let d = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    return a2 / (pbr_pi * d * d);
}

// The height-correlated Smith visibility, V = G / (4 n.l n.v).
fn pbr_v_smith(n_dot_v: f32, n_dot_l: f32, alpha: f32) -> f32 {
    let a2 = alpha * alpha;
    let gv = n_dot_l * sqrt(n_dot_v * n_dot_v * (1.0 - a2) + a2);
    let gl = n_dot_v * sqrt(n_dot_l * n_dot_l * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}

fn pbr_f_schlick(v_dot_h: f32, f0: vec3<f32>) -> vec3<f32> {
    let f = pow(1.0 - v_dot_h, 5.0);
    return f0 + (vec3<f32>(1.0) - f0) * f;
}

// What a surface sends to the eye of a light from the way `l` (unit, towards
// the light) that lights it as `lit` would a white Lambert surface (n.l in
// it already: the sun's colour times n.l, a lamp as its shape gives it). The
// Lambert part less what is reflected, and, `reflects`, the reflection; a
// light the shape lets past the horizon (RTS's lamps: n.l down to -0.15)
// Lambert alone.
fn pbr_lit(s: PbrSurface, l: vec3<f32>, lit: vec3<f32>, reflects: bool) -> vec3<f32> {
    let n_dot_l = dot(s.n, l);
    if (!reflects || n_dot_l <= 0.0) {
        return s.diffuse * lit;
    }
    let h = normalize(l + s.v);
    let f = s.specular * pbr_f_schlick(clamp(dot(s.v, h), 0.0, 1.0), s.f0);
    let specular = pbr_d_ggx(clamp(dot(s.n, h), 0.0, 1.0), s.alpha) * pbr_v_smith(s.n_dot_v, n_dot_l, s.alpha) * f;
    return (s.diffuse * (vec3<f32>(1.0) - f) + specular * pbr_pi) * lit;
}

// The sky's reflection (PBR stage 2; the effects' sky light, SkyLight): the
// prefiltered sky and ground the way a surface of its roughness reflects
// most (Lagarde's dominant direction, nearer the normal the rougher), at
// least `least` (the environment's own sky light at night, which openRTS's
// sky lacks), times the split sum's scale and bias of F0 and the specular
// factor, less what the ambient occlusion hides of it (Lagarde's specular
// occlusion). `taken`: the share of the light the reflection takes from the
// Lambert part.
struct PbrSkyReflection {
    light: vec3<f32>,
    taken: vec3<f32>,
};

fn pbr_sky_reflection(s: PbrSurface, occlusion: f32, least: vec3<f32>) -> PbrSkyReflection {
    var out: PbrSkyReflection;
    out.light = vec3<f32>(0.0);
    out.taken = vec3<f32>(0.0);
    if (s.specular <= 0.0) {
        return out;
    }
    let roughness = sqrt(s.alpha);
    let mirror = reflect(-s.v, s.n);
    let towards = (1.0 - s.alpha) * (sqrt(1.0 - s.alpha) + s.alpha);
    let direction = normalize(mix(s.n, mirror, towards));
    let radiance = max(effects_sky_radiance(direction, roughness), least);
    let ab = effects_split_sum(s.n_dot_v, roughness);
    out.taken = s.specular * (s.f0 * ab.x + vec3<f32>(ab.y));
    let hidden = clamp(pow(s.n_dot_v + occlusion, exp2(-16.0 * roughness - 1.0)) - 1.0 + occlusion, 0.0, 1.0);
    out.light = radiance * out.taken * hidden;
    return out;
}

// The way to a further light from a world point, in world space. In the
// light's space (its `translation`, world -> light: the inverse of the lamp's
// placement, rigid or uniformly scaled) the light is at the origin, or, an
// orthogonal light (kind 3), shines along +z; a light array's nearest
// element as light_factor takes it.
fn pbr_lamp_way(light: Light, world: vec4<f32>) -> vec3<f32> {
    var p = light.translation * world;
    if (light.kind.y >= 2.0) {
        let count = i32(light.kind.y);
        let projection = p * light.array_step;
        let along = (projection.x + projection.y + projection.z) / light.array_step.w;
        let closest = clamp(i32(round(along)), 0, count - 1);
        p = p - vec4<f32>(light.array_step.xyz, 0.0) * f32(closest);
    }
    var towards = -p.xyz;
    if ((i32(light.kind.x) & 31) == 3) {
        towards = vec3<f32>(0.0, 0.0, -1.0);
    }
    if (dot(towards, towards) < 1e-8) {
        return vec3<f32>(0.0, 1.0, 0.0);
    }
    let turn = mat3x3<f32>(light.translation[0].xyz, light.translation[1].xyz, light.translation[2].xyz);
    return normalize(transpose(turn) * towards);
}
