
struct Params {
    to_sun: vec4<f32>,
    sun: vec4<f32>,
    pass_info: vec4<f32>,
};

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var probe: texture_cube<f32>;
@group(0) @binding(2) var linear_sampler: sampler;
@group(0) @binding(3) var<storage, read_write> light: array<vec4<f32>, 12>;
@group(0) @binding(5) var output: texture_storage_2d_array<rgba16float, write>;
@group(0) @binding(6) var source_mip: texture_2d_array<f32>;
@group(0) @binding(7) var source: texture_cube<f32>;

const pi: f32 = 3.14159265;

// A cube face's texel (uv -1..1: right, down) as a way, as WebGPU lays the
// faces out (SkyRenderer::render_reflection_probe draws them so).
fn cube_direction(face: u32, uv: vec2<f32>) -> vec3<f32> {
    switch face {
        case 0u: { return normalize(vec3<f32>(1.0, -uv.y, -uv.x)); }
        case 1u: { return normalize(vec3<f32>(-1.0, -uv.y, uv.x)); }
        case 2u: { return normalize(vec3<f32>(uv.x, 1.0, uv.y)); }
        case 3u: { return normalize(vec3<f32>(uv.x, -1.0, -uv.y)); }
        case 4u: { return normalize(vec3<f32>(uv.x, -uv.y, 1.0)); }
        default: { return normalize(vec3<f32>(-uv.x, -uv.y, -1.0)); }
    }
}

fn basis(d: vec3<f32>) -> array<f32, 9> {
    return array<f32, 9>(0.282095, 0.488603 * d.y, 0.488603 * d.z, 0.488603 * d.x, 1.092548 * d.x * d.y,
                         1.092548 * d.y * d.z, 0.315392 * (3.0 * d.z * d.z - 1.0), 1.092548 * d.x * d.z,
                         0.546274 * (d.x * d.x - d.y * d.y));
}

// The probe's light that way, linear: its colours are gamma 2.2 encoded; a
// part not a finite number (tested by its bits) none; at most the clamp, by
// luminance (the sun's disc is the direct light's).
fn probe_light(direction: vec3<f32>) -> vec3<f32> {
    let encoded = textureSampleLevel(probe, linear_sampler, direction, 0.0).rgb;
    let finite = (bitcast<vec3<u32>>(encoded) & vec3<u32>(0x7f800000u)) != vec3<u32>(0x7f800000u);
    let linear = pow(max(select(vec3<f32>(0.0), encoded, finite), vec3<f32>(0.0)), vec3<f32>(2.2));
    let luminance = dot(linear, vec3<f32>(0.2126, 0.7152, 0.0722));
    return select(linear, linear * (params.sun.w / luminance), luminance > params.sun.w);
}

const sh_grid: u32 = 64u;
var<workgroup> partial: array<array<vec4<f32>, 9>, 64>;
var<workgroup> total: array<vec4<f32>, 9>;

// The SH9 of the sky over the horizon (64 x 64 ways a face, each by its
// solid angle), then the ground under it: a Lambert plane of the ground's
// reflectance lit by the sun and by that sky, added as the lower hemisphere.
@compute @workgroup_size(64)
fn sh(@builtin(local_invocation_index) thread: u32) {
    var sum: array<vec3<f32>, 9>;
    for (var k = 0u; k < 9u; k = k + 1u) {
        sum[k] = vec3<f32>(0.0);
    }
    let per_face = sh_grid * sh_grid;
    for (var i = thread; i < 6u * per_face; i = i + 64u) {
        let face = i / per_face;
        let rest = i % per_face;
        let uv = (vec2<f32>(f32(rest % sh_grid), f32(rest / sh_grid)) + 0.5) / f32(sh_grid) * 2.0 - 1.0;
        let direction = cube_direction(face, uv);
        if (direction.y < 0.0) {
            continue;
        }
        let solid = 4.0 / f32(per_face) / pow(1.0 + dot(uv, uv), 1.5);
        let l = probe_light(direction) * solid;
        var b = basis(direction);
        for (var k = 0u; k < 9u; k = k + 1u) {
            sum[k] = sum[k] + l * b[k];
        }
    }
    for (var k = 0u; k < 9u; k = k + 1u) {
        partial[thread][k] = vec4<f32>(sum[k], 0.0);
    }
    workgroupBarrier();
    if (thread < 9u) {
        var t = vec3<f32>(0.0);
        for (var j = 0u; j < 64u; j = j + 1u) {
            t = t + partial[j][thread].xyz;
        }
        total[thread] = vec4<f32>(t, 0.0);
    }
    workgroupBarrier();
    if (thread == 0u) {
        var band = array<f32, 9>(1.0, 2.0 / 3.0, 2.0 / 3.0, 2.0 / 3.0, 0.25, 0.25, 0.25, 0.25, 0.25);
        var up = basis(vec3<f32>(0.0, 1.0, 0.0));
        var sky_up = vec3<f32>(0.0);
        for (var k = 0u; k < 9u; k = k + 1u) {
            sky_up = sky_up + total[k].xyz * (up[k] * band[k]);
        }
        sky_up = max(sky_up, vec3<f32>(0.0));
        let ground = params.to_sun.w * (params.sun.xyz * max(params.to_sun.y, 0.0) + sky_up);
        var lower = array<f32, 9>(1.7724539, -1.5349900, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
        for (var k = 0u; k < 9u; k = k + 1u) {
            light[k] = vec4<f32>(total[k].xyz + ground * lower[k], 0.0);
        }
        light[9] = vec4<f32>(ground, 0.0);
        light[10] = vec4<f32>(sky_up, 0.0);
        light[11] = vec4<f32>(0.0);
    }
}

// The source: the probe in linear light over the horizon, the ground under it.
@compute @workgroup_size(8, 8, 1)
fn source_light(@builtin(global_invocation_id) id: vec3<u32>) {
    let size = u32(params.pass_info.x);
    if (id.x >= size || id.y >= size) {
        return;
    }
    let uv = (vec2<f32>(id.xy) + 0.5) / f32(size) * 2.0 - 1.0;
    let direction = cube_direction(id.z, uv);
    var l = light[9].xyz;
    if (direction.y >= 0.0) {
        l = probe_light(direction);
    }
    textureStore(output, id.xy, id.z, vec4<f32>(l, 1.0));
}

// The source's next mip: each texel the mean of the four under it.
@compute @workgroup_size(8, 8, 1)
fn downsample(@builtin(global_invocation_id) id: vec3<u32>) {
    let size = textureDimensions(output).x;
    if (id.x >= size || id.y >= size) {
        return;
    }
    let p = vec2<i32>(id.xy) * 2;
    let layer = i32(id.z);
    let sum = textureLoad(source_mip, p, layer, 0) + textureLoad(source_mip, p + vec2<i32>(1, 0), layer, 0)
        + textureLoad(source_mip, p + vec2<i32>(0, 1), layer, 0) + textureLoad(source_mip, p + vec2<i32>(1, 1), layer, 0);
    textureStore(output, id.xy, id.z, sum * 0.25);
}

fn hammersley(i: u32, count: u32) -> vec2<f32> {
    return vec2<f32>((f32(i) + 0.5) / f32(count), f32(reverseBits(i)) * 2.3283064365386963e-10);
}

const prefilter_samples: u32 = 48u;

// A mip of the prefiltered cube: the source as a surface of this roughness
// facing each way reflects it seen along its normal (GGX importance
// sampled, each sample from the source's mip its solid angle fits: Karis,
// Colbert and Krivanek's filtered importance sampling); roughness 0 the
// source itself.
@compute @workgroup_size(8, 8, 1)
fn prefilter(@builtin(global_invocation_id) id: vec3<u32>) {
    let size = u32(params.pass_info.x);
    if (id.x >= size || id.y >= size) {
        return;
    }
    let uv = (vec2<f32>(id.xy) + 0.5) / f32(size) * 2.0 - 1.0;
    let n = cube_direction(id.z, uv);
    let roughness = params.pass_info.y;
    if (roughness <= 0.0) {
        textureStore(output, id.xy, id.z, vec4<f32>(textureSampleLevel(source, linear_sampler, n, 0.0).rgb, 1.0));
        return;
    }
    let alpha = roughness * roughness;
    let a2 = alpha * alpha;
    let helper = select(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 0.0, 1.0), abs(n.z) < 0.999);
    let tangent = normalize(cross(helper, n));
    let bitangent = cross(n, tangent);
    let texel_solid = 4.0 * pi / (6.0 * params.pass_info.z * params.pass_info.z);
    var sum = vec3<f32>(0.0);
    var weight = 0.0;
    for (var i = 0u; i < prefilter_samples; i = i + 1u) {
        let xi = hammersley(i, prefilter_samples);
        let phi = 2.0 * pi * xi.x;
        let cos_theta = sqrt((1.0 - xi.y) / (1.0 + (a2 - 1.0) * xi.y));
        let sin_theta = sqrt(max(1.0 - cos_theta * cos_theta, 0.0));
        let h = normalize(tangent * (sin_theta * cos(phi)) + bitangent * (sin_theta * sin(phi)) + n * cos_theta);
        let l = 2.0 * dot(n, h) * h - n;
        let n_dot_l = dot(n, l);
        if (n_dot_l > 0.0) {
            let d = n_dot_h_ggx(cos_theta, a2);
            let pdf = d * 0.25; // D n.h / (4 v.h), the eye along the normal
            let sample_solid = 1.0 / (f32(prefilter_samples) * pdf + 1e-6);
            let lod = clamp(0.5 * log2(sample_solid / texel_solid) + 1.0, 0.0, params.pass_info.w - 1.0);
            sum = sum + textureSampleLevel(source, linear_sampler, l, lod).rgb * n_dot_l;
            weight = weight + n_dot_l;
        }
    }
    textureStore(output, id.xy, id.z, vec4<f32>(sum / max(weight, 1e-4), 1.0));
}

fn n_dot_h_ggx(n_dot_h: f32, a2: f32) -> f32 {
    let d = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    return a2 / (pi * d * d);
}
