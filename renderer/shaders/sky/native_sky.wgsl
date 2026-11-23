
struct Sky {
    inverse_view_projection: mat4x4<f32>,
    sun: vec4<f32>,
    eye: vec4<f32>,
    clouds: vec4<f32>,
    field: vec4<f32>,
    sun_at_clouds: vec4<f32>,
    ambient_top: vec4<f32>,
    ambient_bottom: vec4<f32>,
    previous_view_projection: mat4x4<f32>,
    shadow: vec4<f32>,
    flash: vec4<f32>,
    flash_light: vec4<f32>,
};

override hdr_output: bool = false;

@group(0) @binding(0) var<uniform> sky: Sky;
@group(0) @binding(1) var shape_noise: texture_3d<f32>;
@group(0) @binding(2) var detail_noise: texture_3d<f32>;
@group(0) @binding(3) var weather_map: texture_2d<f32>;
@group(0) @binding(4) var noise_sampler: sampler;
@group(0) @binding(5) var sky_table: texture_2d<f32>;
@group(0) @binding(6) var table_sampler: sampler;
@group(0) @binding(7) var cloud_texture: texture_2d<f32>;
@group(0) @binding(8) var clamp_sampler: sampler;
@group(0) @binding(9) var flash_texture: texture_2d<f32>;

const planet: f32 = 6360000.0;
const atmosphere_top: f32 = 6460000.0;
const rayleigh: vec3<f32> = vec3<f32>(5.802e-6, 13.558e-6, 33.1e-6);
const mie_scattering: f32 = 3.996e-6;
const mie_extinction: f32 = 4.40e-6;
const ozone: vec3<f32> = vec3<f32>(0.650e-6, 1.881e-6, 0.085e-6);
const pi: f32 = 3.14159265;

struct VertexOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) ndc: vec2<f32>,
};

// A triangle over the whole target, on the far plane (reversed Z: 0).
fn fullscreen(index: u32) -> VertexOut {
    let corner = vec2<f32>(f32((index << 1u) & 2u) * 2.0 - 1.0, f32(index & 2u) * 2.0 - 1.0);
    var out: VertexOut;
    out.clip = vec4<f32>(corner, 0.0, 1.0);
    out.ndc = corner;
    return out;
}

@vertex
fn vs_sky(@builtin(vertex_index) index: u32) -> VertexOut {
    return fullscreen(index);
}

fn ray_sphere(origin: vec3<f32>, direction: vec3<f32>, radius: f32) -> vec2<f32> {
    let b = dot(origin, direction);
    let r = length(origin);
    let c = (r - radius) * (r + radius);
    let d = b * b - c;
    if (d < 0.0) {
        return vec2<f32>(-1.0, -1.0);
    }
    let s = sqrt(d);
    return vec2<f32>(-b - s, -b + s);
}

fn densities(height: f32) -> vec3<f32> {
    let h = max(height, 0.0);
    return vec3<f32>(exp(-h / 8000.0), exp(-h / 1200.0), max(0.0, 1.0 - abs(h - 25000.0) / 15000.0));
}

fn extinction_at(height: f32) -> vec3<f32> {
    let d = densities(height);
    return rayleigh * d.x + vec3<f32>(mie_extinction * d.y) + ozone * d.z;
}

fn transmittance_from(point: vec3<f32>, to_sun: vec3<f32>) -> vec3<f32> {
    if (ray_sphere(point, to_sun, planet).x > 0.0) {
        return vec3<f32>(0.0);
    }
    let top = ray_sphere(point, to_sun, atmosphere_top).y;
    let dt = top / 8.0;
    var depth = vec3<f32>(0.0);
    for (var k = 0; k < 8; k = k + 1) {
        let q = point + to_sun * ((f32(k) + 0.5) * dt);
        depth = depth + extinction_at(length(q) - planet) * dt;
    }
    return exp(-depth);
}

// Single scattering along the view, samples crowded near the eye (t = s^2).
fn atmosphere(view: vec3<f32>) -> vec3<f32> {
    let origin = vec3<f32>(0.0, planet + max(sky.eye.x, 1.0), 0.0);
    var end = ray_sphere(origin, view, atmosphere_top).y;
    let ground = ray_sphere(origin, view, planet);
    if (ground.x > 0.0) {
        end = ground.x;
    }
    end = min(end, 300000.0);
    let to_sun = sky.sun.xyz;
    let mu = dot(view, to_sun);
    let phase_r = 3.0 / (16.0 * pi) * (1.0 + mu * mu);
    let g = 0.8;
    let phase_m = 3.0 / (8.0 * pi) * ((1.0 - g * g) * (1.0 + mu * mu)) / ((2.0 + g * g) * pow(1.0 + g * g - 2.0 * g * mu, 1.5));
    var light = vec3<f32>(0.0);
    var through = vec3<f32>(1.0);
    for (var k = 0; k < 16; k = k + 1) {
        let s0 = f32(k) / 16.0;
        let s1 = f32(k + 1) / 16.0;
        let t = end * 0.5 * (s0 * s0 + s1 * s1);
        let dt = end * (s1 * s1 - s0 * s0);
        let q = origin + view * t;
        let h = length(q) - planet;
        let d = densities(h);
        let e = extinction_at(h);
        let scatter = rayleigh * d.x * phase_r + vec3<f32>(mie_scattering * d.y * phase_m);
        let segment = exp(-e * dt);
        light = light + through * transmittance_from(q, to_sun) * scatter * (vec3<f32>(1.0) - segment) / max(e, vec3<f32>(1e-12));
        through = through * segment;
    }
    return light * sky.eye.w;
}

// The table (texture coordinates, its first row on top): longitude across,
// the zenith on top, the nadir at the bottom, rows crowded at the horizon.
fn table_direction(uv: vec2<f32>) -> vec3<f32> {
    let azimuth = (uv.x - 0.5) * 2.0 * pi;
    let s = 1.0 - 2.0 * uv.y; // 1 at the zenith
    let elevation = sign(s) * s * s * 0.5 * pi;
    return vec3<f32>(cos(elevation) * sin(azimuth), sin(elevation), cos(elevation) * cos(azimuth));
}

fn table_uv(view: vec3<f32>) -> vec2<f32> {
    let azimuth = atan2(view.x, view.z);
    let elevation = asin(clamp(view.y, -1.0, 1.0));
    let s = sign(elevation) * sqrt(abs(elevation) / (0.5 * pi));
    return vec2<f32>(azimuth / (2.0 * pi) + 0.5, 0.5 - 0.5 * s);
}

@vertex
fn vs_table(@builtin(vertex_index) index: u32) -> VertexOut {
    return fullscreen(index);
}

@fragment
fn fs_table(input: VertexOut) -> @location(0) vec4<f32> {
    // NDC y up is the target's first row.
    let uv = vec2<f32>(input.ndc.x * 0.5 + 0.5, 0.5 - input.ndc.y * 0.5);
    return vec4<f32>(atmosphere(table_direction(uv)), 1.0);
}

fn remap(value: f32, low: f32, high: f32, to_low: f32, to_high: f32) -> f32 {
    return to_low + (value - low) / max(high - low, 1e-5) * (to_high - to_low);
}

// The clouds' density at a point (planet-centred, m): a layer between base
// and top, cumulus-shaped (a rounded base, the top narrowing), the cover
// varying over tens of kilometres, the edges eaten by finer noise.
fn cloud_density(p: vec3<f32>, detailed: bool) -> f32 {
    let height = length(p) - planet;
    let fraction = (height - sky.clouds.y) / (sky.clouds.z - sky.clouds.y);
    if (fraction <= 0.0 || fraction >= 1.0) {
        return 0.0;
    }
    let at = sky.field.xy + vec2<f32>(p.x, p.z);
    let weather = textureSampleLevel(weather_map, noise_sampler, at / 65536.0, 0.0).r;
    let cover = clamp(sky.clouds.x + (weather - 0.5) * 0.6, 0.0, 1.0);
    let profile = smoothstep(0.0, 0.12, fraction) * (1.0 - smoothstep(0.5, 1.0, fraction));
    let shape = textureSampleLevel(shape_noise, noise_sampler, vec3<f32>(at.x / 16384.0, height / 4096.0, at.y / 16384.0), 0.0).r;
    var density = remap(shape * profile, 1.0 - cover, 1.0, 0.0, 1.0);
    if (density <= 0.0) {
        return 0.0;
    }
    if (detailed) {
        let erosion = textureSampleLevel(detail_noise, noise_sampler, vec3<f32>(at.x, height, at.y) / 2048.0, 0.0).r;
        density = remap(density, erosion * 0.4, 1.0, 0.0, 1.0);
    }
    return clamp(density, 0.0, 1.0);
}

fn henyey_greenstein(mu: f32, g: f32) -> f32 {
    return (1.0 - g * g) / (4.0 * pi * pow(max(1.0 + g * g - 2.0 * g * mu, 1e-4), 1.5));
}

// Extinction per metre of the densest cloud.
const cloud_sigma: f32 = 0.03;

// The sunlight's way out of the clouds towards the sun: optical depth
// (the first step shifted by the frame's jitter).
fn sun_depth(p: vec3<f32>, jitter: f32) -> f32 {
    var depth = 0.0;
    var stride = 80.0;
    var q = p + sky.sun.xyz * (stride * (jitter - 0.5));
    for (var j = 0; j < 6; j = j + 1) {
        q = q + sky.sun.xyz * stride;
        depth = depth + cloud_density(q, false) * stride;
        stride = stride * 1.7;
    }
    return depth * cloud_sigma;
}

// Where a view crosses the clouds' layer (m from the eye: in, out); out
// not past in: nowhere (below the horizon under the layer). Out to 60 km.
fn cloud_span(view: vec3<f32>) -> vec2<f32> {
    let altitude = sky.eye.x;
    let origin = vec3<f32>(0.0, planet + altitude, 0.0);
    let inner = ray_sphere(origin, view, planet + sky.clouds.y);
    let outer = ray_sphere(origin, view, planet + sky.clouds.z);
    var t0 = 0.0;
    var t1 = 0.0;
    if (altitude < sky.clouds.y) {
        if (ray_sphere(origin, view, planet).x > 0.0) {
            return vec2<f32>(0.0);
        }
        t0 = inner.y;
        t1 = outer.y;
    } else if (altitude < sky.clouds.z) {
        t1 = outer.y;
        if (inner.x > 0.0) {
            t1 = inner.x;
        }
    } else {
        if (outer.x < 0.0) {
            return vec2<f32>(0.0);
        }
        t0 = outer.x;
        t1 = outer.y;
        if (inner.x > 0.0) {
            t1 = inner.x;
        }
    }
    t0 = max(t0, 0.0);
    return vec2<f32>(t0, min(t1, 60000.0));
}

// How much of far clouds is seen: they sink into the haze behind them.
fn cloud_fade(span: vec2<f32>) -> f32 {
    return exp(-span.x / 40000.0);
}

// light.rgb: the clouds' light along the view (faded into the haze);
// light.a: the light they let through, not faded (the sun or the moon behind
// a far cloud is hidden still). flash: a lightning flash's light on them
// for a flash of 1 (faded alike). `jitter` (0..1) shifts the samples: each
// frame its own, the frames averaged (the clouds' texture), so the steps
// leave no bands.
struct March {
    light: vec4<f32>,
    flash: f32,
};

// A flash's light reaching a point of the cloud, for a flash of 1: the
// cloud scatters it on and on, so it spreads round the flash and fades over
// about flash_spread (sky.flash.w its inverse), a faint share far through the
// whole layer.
fn flash_glow(p: vec3<f32>) -> f32 {
    let to_flash = sky.flash.xyz - p;
    let d2 = dot(to_flash, to_flash);
    let d = sqrt(d2);
    return exp(-d * sky.flash.w) / (d2 * 1e-6 + 0.25) + 0.02 * exp(-d / 8000.0);
}

fn clouds(view: vec3<f32>, jitter: f32) -> March {
    var out: March;
    out.light = vec4<f32>(0.0, 0.0, 0.0, 1.0);
    out.flash = 0.0;
    let span = cloud_span(view);
    if (span.y <= span.x) {
        return out;
    }
    let flashing = sky.flash.w > 0.0;
    var flash = 0.0;
    let origin = vec3<f32>(0.0, planet + sky.eye.x, 0.0);
    let base = planet + sky.clouds.y;
    let top = planet + sky.clouds.z;
    let size = span.y - span.x;
    // Few steps a frame: the frames, each its own jitter, add up to many.
    let steps = i32(clamp(size / 200.0, 12.0, 32.0));
    let dt = size / f32(steps);
    // Multiple scattering as octaves (Wrenninge): each half as strong, its
    // phase flatter, the light going twice as far. The phases are the
    // view's, the same at every step.
    let mu = dot(view, sky.sun.xyz);
    let phase0 = mix(henyey_greenstein(mu, 0.8), henyey_greenstein(mu, -0.25), 0.35);
    let phase1 = 0.5 * mix(henyey_greenstein(mu, 0.4), henyey_greenstein(mu, -0.125), 0.35);
    let phase2 = 0.25 * mix(henyey_greenstein(mu, 0.2), henyey_greenstein(mu, -0.0625), 0.35);
    var through = 1.0;
    var light = vec3<f32>(0.0);
    var t = span.x + dt * jitter;
    for (var i = 0; i < steps; i = i + 1) {
        let p = origin + view * t;
        let density = cloud_density(p, true);
        if (density > 0.0) {
            let depth = sun_depth(p, jitter);
            let sun_part = phase0 * exp(-depth) + phase1 * exp(-depth * 0.5) + phase2 * exp(-depth * 0.25);
            let fraction = clamp((length(p) - base) / (top - base), 0.0, 1.0);
            let ambient = mix(sky.ambient_bottom.xyz, sky.ambient_top.xyz, fraction);
            let source = sky.sun_at_clouds.xyz * sun_part + ambient;
            let step_through = exp(-density * cloud_sigma * dt);
            light = light + through * source * (1.0 - step_through);
            if (flashing) {
                flash = flash + through * flash_glow(p) * (1.0 - step_through);
            }
            through = through * step_through;
            if (through < 0.01) {
                through = 0.0;
                break;
            }
        }
        t = t + dt;
    }
    out.light = vec4<f32>(light * cloud_fade(span), through);
    out.flash = flash * cloud_fade(span);
    return out;
}

fn stars(view: vec3<f32>) -> vec3<f32> {
    if (sky.eye.y <= 0.0 || view.y < -0.05) {
        return vec3<f32>(0.0);
    }
    let c = view * 220.0;
    let cell = floor(c);
    let h = fract(sin(dot(cell, vec3<f32>(127.1, 311.7, 74.7))) * 43758.5453);
    if (h < 0.996) {
        return vec3<f32>(0.0);
    }
    let centre = cell + vec3<f32>(0.5) + (vec3<f32>(fract(h * 13.1), fract(h * 71.7), fract(h * 37.3)) - vec3<f32>(0.5)) * 0.5;
    let shine = 1.0 - smoothstep(0.0, 0.45, length(c - centre));
    return vec3<f32>(0.9, 0.95, 1.0) * shine * (0.02 + 0.1 * fract(h * 157.3)) * sky.eye.y;
}

fn sun_disc(view: vec3<f32>) -> vec3<f32> {
    let size = 0.0093; // radians, twice the real sun's radius
    let mu = dot(view, sky.sun.xyz);
    let edge = cos(size);
    if (mu < edge) {
        return vec3<f32>(0.0);
    }
    let across = clamp((mu - edge) / (1.0 - edge), 0.0, 1.0);
    let origin = vec3<f32>(0.0, planet + max(sky.eye.x, 1.0), 0.0);
    return transmittance_from(origin, sky.sun.xyz) * sky.eye.w * 30.0 * smoothstep(0.0, 0.3, across);
}

fn shoulder(c: vec3<f32>) -> vec3<f32> {
    let knee = 0.6;
    let room = 1.0 - knee;
    let over = max(c - vec3<f32>(knee), vec3<f32>(0.0));
    let rolled = vec3<f32>(knee) + room * (vec3<f32>(1.0) - exp(-over / room));
    return select(c, rolled, c > vec3<f32>(knee));
}

// The view through a pixel (NDC), from the eye.
fn view_of(ndc: vec2<f32>) -> vec3<f32> {
    let far = sky.inverse_view_projection * vec4<f32>(ndc, 0.5, 1.0);
    return normalize(far.xyz / far.w);
}

// A pixel's jitter: interleaved gradient noise, moved on each frame.
fn pixel_jitter(pixel: vec2<f32>) -> f32 {
    let noise = fract(52.9829189 * fract(dot(floor(pixel), vec2<f32>(0.06711056, 0.00583715))));
    return fract(noise + sky.field.z);
}

fn texture_uv(ndc: vec2<f32>) -> vec2<f32> {
    return vec2<f32>(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

// The clouds' texture (half the view's size): this frame's march blended
// into the last frames' (found again by direction: the clouds are far); the
// flash's alike while it is the same flash.
struct CloudsOut {
    @location(0) light: vec4<f32>,
    @location(1) flash: vec4<f32>,
};

// Light as the clouds' textures may keep it: each part that is not a finite
// number (its exponent's bits all set: NaN, infinity; tested by its bits, as
// a comparison may be assumed true) the fallback's, the rest within a half
// float's range (an overflow would be stored as infinity) and not negative.
fn finite_light(v: vec4<f32>, fallback: vec4<f32>) -> vec4<f32> {
    let exponent = bitcast<vec4<u32>>(v) & vec4<u32>(0x7f800000u);
    let kept = select(fallback, v, exponent != vec4<u32>(0x7f800000u));
    return clamp(kept, vec4<f32>(0.0), vec4<f32>(60000.0));
}

@fragment
fn fs_clouds(input: VertexOut) -> CloudsOut {
    let view = view_of(input.ndc);
    var current = clouds(view, pixel_jitter(input.clip.xy));
    // Only finite light goes into the textures: a NaN or an infinity once
    // in them was kept by every later frame's blend and spread by the
    // reprojection's filtering (a GTX 1050 on Vulkan, 2026-11-16: after a
    // flight of two tiles the sky black, with green blocks flickering).
    current.light = finite_light(current.light, vec4<f32>(0.0, 0.0, 0.0, 1.0));
    current.flash = finite_light(vec4<f32>(current.flash), vec4<f32>(0.0)).x;
    var out: CloudsOut;
    out.light = current.light;
    out.flash = vec4<f32>(current.flash, 0.0, 0.0, 1.0);
    let weight = sky.field.w;
    let flash_weight = max(weight, sky.flash_light.w);
    if (weight >= 1.0 && flash_weight >= 1.0) {
        return out;
    }
    let before = sky.previous_view_projection * vec4<f32>(view, 1.0);
    if (before.w <= 1e-6) {
        return out;
    }
    let ndc = before.xy / before.w;
    if (abs(ndc.x) > 1.0 || abs(ndc.y) > 1.0) {
        return out;
    }
    let uv = texture_uv(ndc);
    if (weight < 1.0) {
        let history = finite_light(textureSampleLevel(cloud_texture, clamp_sampler, uv, 0.0), current.light);
        out.light = mix(history, current.light, weight);
    }
    if (flash_weight < 1.0) {
        let history = finite_light(vec4<f32>(textureSampleLevel(flash_texture, clamp_sampler, uv, 0.0).r), vec4<f32>(current.flash)).x;
        out.flash = vec4<f32>(mix(history, current.flash, flash_weight), 0.0, 0.0, 1.0);
    }
    return out;
}

// The air's colour is looked up at least half a degree up: below it, where
// the land has ended (past the loaded tiles, the eye high above them), the
// far land would be lost in the haze, and the air towards the ground (a
// short way, no ground lit) showed as a dark blue band. The horizon's own
// colour there, as the fog turns the land's end into (FogRing, as high).
fn above_horizon(view: vec3<f32>) -> vec3<f32> {
    let low = 0.0087265; // sin(0.5 degrees)
    if (view.y >= low) {
        return view;
    }
    let flat = length(view.xz);
    let across = select(vec2<f32>(0.0, 1.0), view.xz / max(flat, 1e-6), flat > 1e-5);
    let c = sqrt(1.0 - low * low);
    return vec3<f32>(across.x * c, low, across.y * c);
}

// The sky, its alpha what the clouds let through: the moon and the other
// satellites drawn before it are kept by that much (the pipeline's blend).
// The clouds' shadow on the ground (cloud_shadow.hpp): over a square at the
// layer's middle height, the light the whole layer lets through along the
// sun's way through each point, from its base to its top in steps of about
// 100 m (16 to 40). The clouds as the sky shows them, their fine noise too,
// their density times the sharpness (sky.shadow.w): the edges' soft fall-off
// narrows. How dark under a thick one is SceneEffects' (the strength).
@fragment
fn fs_cloud_shadow(input: VertexOut) -> @location(0) vec4<f32> {
    let uv = vec2<f32>(input.ndc.x * 0.5 + 0.5, 0.5 - input.ndc.y * 0.5);
    let q = sky.shadow.xy + (uv * 2.0 - 1.0) * sky.shadow.z;
    let sun = sky.sun.xyz;
    let rise = max(sun.y, 0.05);
    let middle = (sky.clouds.y + sky.clouds.z) * 0.5;
    let first = (sky.clouds.y - middle) / rise;
    let way = (sky.clouds.z - sky.clouds.y) / rise;
    let steps = i32(clamp(ceil(way / 100.0), 16.0, 40.0));
    let stride = way / f32(steps);
    let origin = vec3<f32>(q.x, planet + middle, q.y);
    var depth = 0.0;
    for (var i = 0; i < steps; i = i + 1) {
        depth = depth + cloud_density(origin + sun * (first + (f32(i) + 0.5) * stride), true) * stride;
    }
    return vec4<f32>(exp(-depth * cloud_sigma * sky.shadow.w), 0.0, 0.0, 1.0);
}

@fragment
fn fs_sky(input: VertexOut) -> @location(0) vec4<f32> {
    let view = view_of(input.ndc);
    var colour = textureSampleLevel(sky_table, table_sampler, table_uv(above_horizon(view)), 0.0).xyz;
    // The night's own faint light (airglow, starlight) and the stars.
    colour = colour + vec3<f32>(0.004, 0.006, 0.012) * sky.eye.y + stars(view);
    var cloud: vec4<f32>;
    var flash = 0.0;
    if (sky.clouds.w > 0.5) {
        cloud = textureSampleLevel(cloud_texture, clamp_sampler, texture_uv(input.ndc), 0.0);
        flash = textureSampleLevel(flash_texture, clamp_sampler, texture_uv(input.ndc), 0.0).r;
    } else {
        let march = clouds(view, pixel_jitter(input.clip.xy));
        cloud = march.light;
        flash = march.flash;
    }
    let through = clamp(cloud.a, 0.0, 1.0);
    colour = colour * mix(1.0, through, cloud_fade(cloud_span(view))) + cloud.xyz;
    // A lightning flash lights the clouds round it.
    colour = colour + sky.flash_light.xyz * max(flash, 0.0);
    // The sun's disc is thousands of times the sky's light: through a
    // cloud that lets a hundredth through it still glared white, so it
    // shows only through a thin veil.
    colour = colour + sun_disc(view) * through * through * through;
    // A linear frame (frame_encoding()): the light as it is.
    if (sky.eye.z > 0.5) {
        return vec4<f32>(max(colour, vec3<f32>(0.0)), through);
    }
    let shaped = select(shoulder(colour), max(colour, vec3<f32>(0.0)), hdr_output);
    return vec4<f32>(pow(shaped, vec3<f32>(1.0 / 2.2)), through);
}
