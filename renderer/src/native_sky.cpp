#include <openrts/renderer/native_sky.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <future>
#include <string>
#include <vector>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>
#include <glm/trigonometric.hpp>

namespace openrts::renderer {
namespace {

// The planet and its atmosphere (m, 1/m), as the shader has them.
constexpr double planet = 6360000.0;
constexpr double atmosphere_top = 6460000.0;
constexpr double rayleigh[3]{5.802e-6, 13.558e-6, 33.1e-6};
constexpr double mie_scattering = 3.996e-6;
constexpr double mie_extinction = 4.40e-6;
constexpr double ozone[3]{0.650e-6, 1.881e-6, 0.085e-6};
constexpr double pi = 3.14159265358979323846;
constexpr int view_steps = 16;
constexpr int light_steps = 8;

struct DVec3 {
    double x, y, z;
};
DVec3 add(const DVec3& a, const DVec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
DVec3 scale(const DVec3& a, const double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(const DVec3& a, const DVec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double length(const DVec3& a) { return std::sqrt(dot(a, a)); }

// Where a ray meets a sphere round the planet's centre (near, far; -1 none).
std::array<double, 2> ray_sphere(const DVec3& origin, const DVec3& direction, const double radius) {
    const double b = dot(origin, direction);
    const double r = length(origin);
    const double c = (r - radius) * (r + radius);
    const double d = b * b - c;
    if (d < 0.0) return {-1.0, -1.0};
    const double s = std::sqrt(d);
    return {-b - s, -b + s};
}

std::array<double, 3> densities(const double height) {
    const double h = (std::max)(height, 0.0);
    return {std::exp(-h / 8000.0), std::exp(-h / 1200.0), (std::max)(0.0, 1.0 - std::abs(h - 25000.0) / 15000.0)};
}

std::array<double, 3> extinction(const double height) {
    const auto d = densities(height);
    return {rayleigh[0] * d[0] + mie_extinction * d[1] + ozone[0] * d[2], rayleigh[1] * d[0] + mie_extinction * d[1] + ozone[1] * d[2],
            rayleigh[2] * d[0] + mie_extinction * d[1] + ozone[2] * d[2]};
}

std::array<double, 3> transmittance_from(const DVec3& point, const DVec3& to_sun) {
    if (const auto ground = ray_sphere(point, to_sun, planet); ground[0] > 0.0) return {0.0, 0.0, 0.0};
    const double top = ray_sphere(point, to_sun, atmosphere_top)[1];
    const double dt = top / light_steps;
    std::array<double, 3> depth{};
    for (int k = 0; k < light_steps; ++k) {
        const auto q = add(point, scale(to_sun, (k + 0.5) * dt));
        const auto e = extinction(length(q) - planet);
        for (int i = 0; i < 3; ++i) depth[i] += e[i] * dt;
    }
    return {std::exp(-depth[0]), std::exp(-depth[1]), std::exp(-depth[2])};
}

DVec3 unit(const glm::vec3& v) {
    const auto n = glm::normalize(v);
    return {n.x, n.y, n.z};
}

// Smooth roll-off above 0.6 towards 1, as the scene's (water, terrain).
float shoulder(const float c) {
    constexpr float knee = 0.6f;
    constexpr float room = 1.0f - knee;
    return c > knee ? knee + room * (1.0f - std::exp(-(c - knee) / room)) : c;
}

} // namespace

glm::vec3 atmosphere_transmittance(const float altitude, const glm::vec3& to_sun) {
    const auto t = transmittance_from({0.0, planet + (std::max)(altitude, 1.0f), 0.0}, unit(to_sun));
    return {static_cast<float>(t[0]), static_cast<float>(t[1]), static_cast<float>(t[2])};
}

glm::vec3 atmosphere_radiance(const float altitude, const glm::vec3& view_direction, const glm::vec3& to_sun_direction) {
    const DVec3 origin{0.0, planet + (std::max)(altitude, 1.0f), 0.0};
    const auto view = unit(view_direction);
    const auto sun = unit(to_sun_direction);
    double end = ray_sphere(origin, view, atmosphere_top)[1];
    if (const auto ground = ray_sphere(origin, view, planet); ground[0] > 0.0) end = ground[0];
    end = (std::min)(end, 300000.0);
    const double mu = dot(view, sun);
    const double phase_r = 3.0 / (16.0 * pi) * (1.0 + mu * mu);
    constexpr double g = 0.8;
    const double phase_m = 3.0 / (8.0 * pi) * ((1.0 - g * g) * (1.0 + mu * mu)) / ((2.0 + g * g) * std::pow(1.0 + g * g - 2.0 * g * mu, 1.5));
    std::array<double, 3> light{}, through{1.0, 1.0, 1.0};
    // Samples crowded near the eye, where the air is densest: t = end * s^2.
    for (int k = 0; k < view_steps; ++k) {
        const double s0 = static_cast<double>(k) / view_steps;
        const double s1 = static_cast<double>(k + 1) / view_steps;
        const double t = end * 0.5 * (s0 * s0 + s1 * s1);
        const double dt = end * (s1 * s1 - s0 * s0);
        const auto q = add(origin, scale(view, t));
        const double h = length(q) - planet;
        const auto d = densities(h);
        const auto e = extinction(h);
        const auto sun_t = transmittance_from(q, sun);
        for (int i = 0; i < 3; ++i) {
            const double scatter = rayleigh[i] * d[0] * phase_r + mie_scattering * d[1] * phase_m;
            const double step = std::exp(-e[i] * dt);
            light[i] += through[i] * sun_t[i] * scatter * (1.0 - step) / (std::max)(e[i], 1e-12);
            through[i] *= step;
        }
    }
    return glm::vec3{static_cast<float>(light[0]), static_cast<float>(light[1]), static_cast<float>(light[2])} * native_sky_sun;
}

} // namespace openrts::renderer

#ifdef OPENRTS_HAS_WGPU
#include <openrts/renderer/depth.hpp>

namespace openrts::renderer {
namespace {

constexpr std::uint64_t uniform_stride = 256;
constexpr std::uint32_t uniform_slots = 9; // the frame, six probe faces, the table, the clouds
constexpr std::uint32_t table_slot = 7;
constexpr std::uint32_t cloud_slot = 8;
// The clouds' texture: half the view's size each way, rgb their light, a
// the light they let through.
constexpr WGPUTextureFormat cloud_format = WGPUTextureFormat_RGBA16Float;
// A new frame's share of the clouds' texture: about the last eight frames,
// each marched with its own jitter, so the steps' bands average out.
constexpr float cloud_history_weight = 0.12f;
constexpr std::uint32_t table_width = 256;
constexpr std::uint32_t table_height = 128;
constexpr WGPUTextureFormat table_format = WGPUTextureFormat_RGBA16Float;
constexpr std::uint32_t shape_size = 64;  // the clouds' shapes, tiling every 16384 m
constexpr std::uint32_t detail_size = 32; // their edges, every 2048 m
constexpr std::uint32_t weather_size = 128; // where they gather, every 65536 m
// The clouds' shadow map: 1024 x 1024 texels over 16 km (16 m each), the
// light let through; drawn every fourth frame (the clouds drift a few metres
// meanwhile, the map is read where it was drawn).
constexpr std::uint32_t cloud_shadow_size = 1024;
constexpr std::uint64_t cloud_shadow_every = 4;
constexpr float cloud_shadow_half = 8192.0f;
constexpr WGPUTextureFormat cloud_shadow_format = WGPUTextureFormat_R8Unorm;

struct SkyUniform {
    glm::mat4 inverse_view_projection{1.0f};
    float sun[4]{};            // xyz: towards the sun
    float eye[4]{};            // x: altitude (m), y: night (0..1), z: -, w: the sun's light (native_sky_sun)
    float clouds[4]{};         // x: cover, y: base (m above sea), z: top, w: 1: the sky reads the clouds' texture
    float field[4]{};          // xy: the eye in the clouds' field (m, wraps at 65536), z: this frame's jitter (0..1), w: the clouds' history: the new frame's weight (1: none)
    float sun_at_clouds[4]{};  // rgb: sunlight at the clouds (linear, scaled)
    float ambient_top[4]{};    // rgb: sky light on their tops
    float ambient_bottom[4]{}; // rgb: light from below
    glm::mat4 previous_view_projection{1.0f}; // the clouds' last frame: directions -> clip
    float shadow[4]{}; // the clouds' shadow map: xy its middle from the eye (m), z its half width (m), w its sharpness
};
static_assert(sizeof(SkyUniform) <= uniform_stride);

constexpr char sky_shader[] = R"(
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

// Rgb: the clouds' light along the view (faded into the haze); a: the light
// they let through, not faded (the sun or the moon behind a far cloud is
// hidden still). `jitter` (0..1) shifts the samples: each frame its own,
// the frames averaged (the clouds' texture), so the steps leave no bands.
fn clouds(view: vec3<f32>, jitter: f32) -> vec4<f32> {
    let span = cloud_span(view);
    if (span.y <= span.x) {
        return vec4<f32>(0.0, 0.0, 0.0, 1.0);
    }
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
            through = through * step_through;
            if (through < 0.01) {
                through = 0.0;
                break;
            }
        }
        t = t + dt;
    }
    return vec4<f32>(light * cloud_fade(span), through);
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
// into the last frames' (found again by direction: the clouds are far).
@fragment
fn fs_clouds(input: VertexOut) -> @location(0) vec4<f32> {
    let view = view_of(input.ndc);
    let current = clouds(view, pixel_jitter(input.clip.xy));
    let weight = sky.field.w;
    if (weight >= 1.0) {
        return current;
    }
    let before = sky.previous_view_projection * vec4<f32>(view, 1.0);
    if (before.w <= 1e-6) {
        return current;
    }
    let ndc = before.xy / before.w;
    if (abs(ndc.x) > 1.0 || abs(ndc.y) > 1.0) {
        return current;
    }
    let history = textureSampleLevel(cloud_texture, clamp_sampler, texture_uv(ndc), 0.0);
    return mix(history, current, weight);
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
    if (sky.clouds.w > 0.5) {
        cloud = textureSampleLevel(cloud_texture, clamp_sampler, texture_uv(input.ndc), 0.0);
    } else {
        cloud = clouds(view, pixel_jitter(input.clip.xy));
    }
    let through = clamp(cloud.a, 0.0, 1.0);
    colour = colour * mix(1.0, through, cloud_fade(cloud_span(view))) + cloud.xyz;
    // The sun's disc is thousands of times the sky's light: through a
    // cloud that lets a hundredth through it still glared white, so it
    // shows only through a thin veil.
    colour = colour + sun_disc(view) * through * through * through;
    let shaped = select(shoulder(colour), max(colour, vec3<f32>(0.0)), hdr_output);
    return vec4<f32>(pow(shaped, vec3<f32>(1.0 / 2.2)), through);
}
)";

[[nodiscard]] WGPUStringView wgsl_name(const char* name) {
    return {.data = name, .length = WGPU_STRLEN};
}

// Integer hashes for the noise (any good mix will do; fixed, so the sky is
// the same every run).
[[nodiscard]] std::uint32_t mix_bits(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

[[nodiscard]] float hash01(const int x, const int y, const int z, const std::uint32_t seed) {
    const auto h = mix_bits(static_cast<std::uint32_t>(x) * 0x8DA6B343u ^ static_cast<std::uint32_t>(y) * 0xD8163841u
                            ^ static_cast<std::uint32_t>(z) * 0xCB1AB31Fu ^ seed);
    return static_cast<float>(h & 0xFFFFFFu) / 16777216.0f;
}

[[nodiscard]] int wrap(const int v, const int period) {
    const int m = v % period;
    return m < 0 ? m + period : m;
}

// Gradient noise tiling every `period` lattice cells, about -1..1.
[[nodiscard]] float perlin(const float x, const float y, const float z, const int period, const std::uint32_t seed) {
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int z0 = static_cast<int>(std::floor(z));
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);
    const float fz = z - static_cast<float>(z0);
    const auto fade = [](const float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); };
    const auto corner = [&](const int dx, const int dy, const int dz) {
        const int cx = wrap(x0 + dx, period);
        const int cy = wrap(y0 + dy, period);
        const int cz = wrap(z0 + dz, period);
        // A gradient on the unit sphere from two hashes.
        const float u = hash01(cx, cy, cz, seed) * 2.0f - 1.0f;
        const float a = hash01(cx, cy, cz, seed ^ 0x9E3779B9u) * 6.2831853f;
        const float s = std::sqrt((std::max)(0.0f, 1.0f - u * u));
        return s * std::cos(a) * (fx - static_cast<float>(dx)) + s * std::sin(a) * (fy - static_cast<float>(dy)) + u * (fz - static_cast<float>(dz));
    };
    const float u = fade(fx), v = fade(fy), w = fade(fz);
    const auto lerp = [](const float a, const float b, const float t) { return a + (b - a) * t; };
    const float x00 = lerp(corner(0, 0, 0), corner(1, 0, 0), u);
    const float x10 = lerp(corner(0, 1, 0), corner(1, 1, 0), u);
    const float x01 = lerp(corner(0, 0, 1), corner(1, 0, 1), u);
    const float x11 = lerp(corner(0, 1, 1), corner(1, 1, 1), u);
    return lerp(lerp(x00, x10, v), lerp(x01, x11, v), w) * 1.6f;
}

// Cellular noise tiling every `cells`: 1 at a feature point, 0 a cell away.
[[nodiscard]] float worley(const float x, const float y, const float z, const int cells, const std::uint32_t seed) {
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int z0 = static_cast<int>(std::floor(z));
    float nearest = 4.0f;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const int cx = x0 + dx, cy = y0 + dy, cz = z0 + dz;
                const int hx = wrap(cx, cells), hy = wrap(cy, cells), hz = wrap(cz, cells);
                const float px = static_cast<float>(cx) + hash01(hx, hy, hz, seed);
                const float py = static_cast<float>(cy) + hash01(hx, hy, hz, seed ^ 0x68E31DA4u);
                const float pz = static_cast<float>(cz) + hash01(hx, hy, hz, seed ^ 0xB5297A4Du);
                const float d = (px - x) * (px - x) + (py - y) * (py - y) + (pz - z) * (pz - z);
                nearest = (std::min)(nearest, d);
            }
        }
    }
    return 1.0f - (std::min)(std::sqrt(nearest), 1.0f);
}

// Fills `size`^3 bytes, z slices on several threads.
template <typename Value>
[[nodiscard]] std::vector<std::uint8_t> volume(const std::uint32_t size, Value value) {
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size) * size * size);
    std::vector<std::future<void>> jobs;
    constexpr std::uint32_t parts = 8;
    for (std::uint32_t part = 0; part < parts; ++part) {
        jobs.push_back(std::async(std::launch::async, [&, part] {
            for (std::uint32_t z = part; z < size; z += parts) {
                for (std::uint32_t y = 0; y < size; ++y) {
                    for (std::uint32_t x = 0; x < size; ++x) {
                        const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
                        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
                        const float w = (static_cast<float>(z) + 0.5f) / static_cast<float>(size);
                        const float result = std::clamp(value(u, v, w), 0.0f, 1.0f);
                        bytes[(static_cast<std::size_t>(z) * size + y) * size + x] = static_cast<std::uint8_t>(std::lround(result * 255.0f));
                    }
                }
            }
        }));
    }
    for (auto& job : jobs) job.wait();
    return bytes;
}

// The clouds' shapes: billowing cells (Worley, three octaves) over a larger
// gradient noise, their contrast raised; about half above 0.5.
[[nodiscard]] float shape_value(const float u, const float v, const float w) {
    float fbm = 0.0f;
    float amplitude = 0.5f;
    for (int octave = 0; octave < 4; ++octave) {
        const int period = 4 << octave;
        fbm += amplitude * perlin(u * static_cast<float>(period), v * static_cast<float>(period), w * static_cast<float>(period), period, 0x1234u + octave);
        amplitude *= 0.5f;
    }
    const float smooth = fbm * 0.5f + 0.5f;
    const float cells = worley(u * 4.0f, v * 4.0f, w * 4.0f, 4, 0xA11u) * 0.625f + worley(u * 8.0f, v * 8.0f, w * 8.0f, 8, 0xB22u) * 0.25f
                        + worley(u * 16.0f, v * 16.0f, w * 16.0f, 16, 0xC33u) * 0.125f;
    const float shape = smooth * 0.55f + cells * 0.45f;
    return (shape - 0.5f) * 1.8f + 0.5f;
}

[[nodiscard]] float detail_value(const float u, const float v, const float w) {
    return worley(u * 4.0f, v * 4.0f, w * 4.0f, 4, 0xD44u) * 0.625f + worley(u * 8.0f, v * 8.0f, w * 8.0f, 8, 0xE55u) * 0.25f
           + worley(u * 16.0f, v * 16.0f, w * 16.0f, 16, 0xF66u) * 0.125f;
}

[[nodiscard]] std::vector<std::uint8_t> weather_bytes() {
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(weather_size) * weather_size);
    for (std::uint32_t y = 0; y < weather_size; ++y) {
        for (std::uint32_t x = 0; x < weather_size; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(weather_size);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(weather_size);
            float fbm = 0.0f, amplitude = 0.5f;
            for (int octave = 0; octave < 3; ++octave) {
                const int period = 4 << octave;
                fbm += amplitude * perlin(u * static_cast<float>(period), v * static_cast<float>(period), 0.5f, period, 0x5EA7u + octave);
                amplitude *= 0.5f;
            }
            bytes[static_cast<std::size_t>(y) * weather_size + x] = static_cast<std::uint8_t>(std::lround(std::clamp(fbm * 0.7f + 0.5f, 0.0f, 1.0f) * 255.0f));
        }
    }
    return bytes;
}

[[nodiscard]] WGPUTexture make_texture(const WGPUDevice device, const WGPUTextureDimension dimension, const std::uint32_t size,
                                       const std::uint32_t depth, const WGPUTextureFormat format, const WGPUTextureUsage usage,
                                       const char* label) {
    const WGPUTextureDescriptor descriptor{
        .label = {.data = label, .length = WGPU_STRLEN},
        .usage = usage,
        .dimension = dimension,
        .size = {.width = size, .height = dimension == WGPUTextureDimension_3D ? size : depth, .depthOrArrayLayers = dimension == WGPUTextureDimension_3D ? depth : 1},
        .format = format,
        .mipLevelCount = 1,
        .sampleCount = 1,
    };
    return wgpuDeviceCreateTexture(device, &descriptor);
}

void write_bytes(const WGPUDevice device, const WGPUTexture texture, const std::vector<std::uint8_t>& bytes, const std::uint32_t width,
                 const std::uint32_t height, const std::uint32_t depth) {
    const auto queue = wgpuDeviceGetQueue(device);
    const WGPUTexelCopyTextureInfo target{.texture = texture, .mipLevel = 0, .origin = {}, .aspect = WGPUTextureAspect_All};
    const WGPUTexelCopyBufferLayout layout{.offset = 0, .bytesPerRow = width, .rowsPerImage = height};
    const WGPUExtent3D extent{.width = width, .height = height, .depthOrArrayLayers = depth};
    wgpuQueueWriteTexture(queue, &target, bytes.data(), bytes.size(), &layout, &extent);
    wgpuQueueRelease(queue);
}

} // namespace

struct NativeSky::Impl {
    WGPUDevice device = nullptr;
    WGPUBindGroupLayout table_layout = nullptr;
    WGPUBindGroupLayout sky_layout = nullptr;
    WGPURenderPipeline table_pipeline = nullptr;
    WGPURenderPipeline sky_pipeline = nullptr;
    WGPURenderPipeline cloud_pipeline = nullptr;
    WGPURenderPipeline shadow_pipeline = nullptr;
    WGPUTexture shadow_map = nullptr;
    WGPUTextureView shadow_view = nullptr;
    bool shadow_drawn = false; // drawn, and still good to read
    bool shadow_due = false;   // to be drawn this frame
    std::uint64_t shadow_frame = 0;   // the update() it was (last) drawn for
    glm::dvec2 eye_field{0.0};        // the eye in the clouds' field, not wrapped
    glm::dvec2 shadow_field{0.0};     // that when it was drawn
    glm::vec2 shadow_centre{0.0f};    // its middle from the eye then (m)
    glm::vec3 shadow_sun{0.0f, 1.0f, 0.0f};
    float shadow_sharpness = 0.0f;
    float shadow_above = 0.0f;        // the layer's middle above the eye now (m)
    float shadow_strength = 0.0f;
    WGPUBuffer uniforms = nullptr;
    WGPUTexture shape = nullptr, detail = nullptr, weather = nullptr, table = nullptr;
    WGPUTextureView shape_view = nullptr, detail_view = nullptr, weather_view = nullptr, table_view = nullptr;
    WGPUSampler noise_sampler = nullptr, table_sampler = nullptr, clamp_sampler = nullptr;
    WGPUBindGroup table_binding = nullptr;
    // The clouds' texture, two: one drawn each frame from the other. The
    // sky's bindings: [k] reads clouds[k].
    std::array<WGPUTexture, 2> clouds{};
    std::array<WGPUTextureView, 2> cloud_views{};
    std::array<WGPUBindGroup, 2> sky_bindings{};
    std::uint32_t cloud_width = 0, cloud_height = 0;
    std::uint32_t cloud_current = 0;   // the one this frame's clouds are in
    bool clouds_this_frame = false;    // render_clouds() ran for this frame's view
    std::uint64_t frames = 0;          // update() calls
    std::uint64_t traced_at = ~0ull;   // the frame render_clouds() last ran in
    glm::mat4 traced_directions{1.0f}; // its view's directions -> clip
    glm::vec3 traced_sun{0.0f};
    glm::dvec2 traced_field{0.0};

    SkyUniform frame{};
    glm::dvec2 drift{0.0};
    glm::dvec2 field{0.0};
    glm::vec3 to_sun{0.0f, 1.0f, 0.0f};
    glm::vec3 horizon{0.6f};
    FogRing fog_ring;

    void release_clouds() {
        for (auto*& group : sky_bindings) {
            if (group) wgpuBindGroupRelease(group);
            group = nullptr;
        }
        for (auto*& view : cloud_views) {
            if (view) wgpuTextureViewRelease(view);
            view = nullptr;
        }
        for (auto*& texture : clouds) {
            if (texture) wgpuTextureRelease(texture);
            texture = nullptr;
        }
        cloud_width = cloud_height = 0;
    }

    // The clouds' textures at a size, and the sky's bindings reading them.
    [[nodiscard]] bool make_clouds(std::uint32_t width, std::uint32_t height);

    ~Impl() {
        release_clouds();
        if (table_binding) wgpuBindGroupRelease(table_binding);
        if (shadow_view) wgpuTextureViewRelease(shadow_view);
        if (shadow_map) wgpuTextureRelease(shadow_map);
        if (shadow_pipeline) wgpuRenderPipelineRelease(shadow_pipeline);
        for (auto* view : {shape_view, detail_view, weather_view, table_view}) {
            if (view) wgpuTextureViewRelease(view);
        }
        for (auto* texture : {shape, detail, weather, table}) {
            if (texture) wgpuTextureRelease(texture);
        }
        for (auto* sampler : {noise_sampler, table_sampler, clamp_sampler}) {
            if (sampler) wgpuSamplerRelease(sampler);
        }
        if (uniforms) wgpuBufferRelease(uniforms);
        for (auto* pipeline : {table_pipeline, sky_pipeline, cloud_pipeline}) {
            if (pipeline) wgpuRenderPipelineRelease(pipeline);
        }
        for (auto* layout : {table_layout, sky_layout}) {
            if (layout) wgpuBindGroupLayoutRelease(layout);
        }
    }

    void write_slot(const std::uint32_t slot) const {
        const auto queue = wgpuDeviceGetQueue(device);
        wgpuQueueWriteBuffer(queue, uniforms, slot * uniform_stride, &frame, sizeof(frame));
        wgpuQueueRelease(queue);
    }
};

bool NativeSky::Impl::make_clouds(const std::uint32_t width, const std::uint32_t height) {
    release_clouds();
    for (std::size_t k = 0; k < clouds.size(); ++k) {
        const WGPUTextureDescriptor descriptor{
            .label = {.data = "sky-clouds", .length = WGPU_STRLEN},
            .usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_RenderAttachment,
            .dimension = WGPUTextureDimension_2D,
            .size = {.width = width, .height = height, .depthOrArrayLayers = 1},
            .format = cloud_format,
            .mipLevelCount = 1,
            .sampleCount = 1,
        };
        clouds[k] = wgpuDeviceCreateTexture(device, &descriptor);
        if (!clouds[k]) return false;
        cloud_views[k] = wgpuTextureCreateView(clouds[k], nullptr);
    }
    for (std::size_t k = 0; k < sky_bindings.size(); ++k) {
        std::array<WGPUBindGroupEntry, 9> entries{};
        entries[0] = {.binding = 0, .buffer = uniforms, .offset = 0, .size = sizeof(SkyUniform)};
        entries[1].binding = 1;
        entries[1].textureView = shape_view;
        entries[2].binding = 2;
        entries[2].textureView = detail_view;
        entries[3].binding = 3;
        entries[3].textureView = weather_view;
        entries[4].binding = 4;
        entries[4].sampler = noise_sampler;
        entries[5].binding = 5;
        entries[5].textureView = table_view;
        entries[6].binding = 6;
        entries[6].sampler = table_sampler;
        entries[7].binding = 7;
        entries[7].textureView = cloud_views[k];
        entries[8].binding = 8;
        entries[8].sampler = clamp_sampler;
        const WGPUBindGroupDescriptor descriptor{.layout = sky_layout, .entryCount = entries.size(), .entries = entries.data()};
        sky_bindings[k] = wgpuDeviceCreateBindGroup(device, &descriptor);
        if (!sky_bindings[k]) return false;
    }
    cloud_width = width;
    cloud_height = height;
    return true;
}

NativeSky::NativeSky(const WGPUDevice device, const WGPUTextureFormat target_format) : impl_(std::make_unique<Impl>()) {
    auto& impl = *impl_;
    impl.device = device;

    // The noise, made once (a few tenths of a second, on several threads).
    const auto shape_bytes = volume(shape_size, shape_value);
    const auto detail_bytes = volume(detail_size, detail_value);
    const auto weather = weather_bytes();
    constexpr auto sampled = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    impl.shape = make_texture(device, WGPUTextureDimension_3D, shape_size, shape_size, WGPUTextureFormat_R8Unorm, sampled, "sky-cloud-shapes");
    impl.detail = make_texture(device, WGPUTextureDimension_3D, detail_size, detail_size, WGPUTextureFormat_R8Unorm, sampled, "sky-cloud-detail");
    impl.weather = make_texture(device, WGPUTextureDimension_2D, weather_size, weather_size, WGPUTextureFormat_R8Unorm, sampled, "sky-cloud-cover");
    impl.table = make_texture(device, WGPUTextureDimension_2D, table_width, table_height, table_format,
                              WGPUTextureUsage_TextureBinding | WGPUTextureUsage_RenderAttachment, "sky-atmosphere");
    if (!impl.shape || !impl.detail || !impl.weather || !impl.table) return;
    write_bytes(device, impl.shape, shape_bytes, shape_size, shape_size, shape_size);
    write_bytes(device, impl.detail, detail_bytes, detail_size, detail_size, detail_size);
    write_bytes(device, impl.weather, weather, weather_size, weather_size, 1);
    impl.shape_view = wgpuTextureCreateView(impl.shape, nullptr);
    impl.detail_view = wgpuTextureCreateView(impl.detail, nullptr);
    impl.weather_view = wgpuTextureCreateView(impl.weather, nullptr);
    impl.table_view = wgpuTextureCreateView(impl.table, nullptr);

    const WGPUSamplerDescriptor noise_sampler{
        .addressModeU = WGPUAddressMode_Repeat,
        .addressModeV = WGPUAddressMode_Repeat,
        .addressModeW = WGPUAddressMode_Repeat,
        .magFilter = WGPUFilterMode_Linear,
        .minFilter = WGPUFilterMode_Linear,
        .mipmapFilter = WGPUMipmapFilterMode_Nearest,
        .lodMaxClamp = 1.0f,
        .maxAnisotropy = 1,
    };
    impl.noise_sampler = wgpuDeviceCreateSampler(device, &noise_sampler);
    const WGPUSamplerDescriptor table_sampler{
        .addressModeU = WGPUAddressMode_Repeat,
        .addressModeV = WGPUAddressMode_ClampToEdge,
        .addressModeW = WGPUAddressMode_ClampToEdge,
        .magFilter = WGPUFilterMode_Linear,
        .minFilter = WGPUFilterMode_Linear,
        .mipmapFilter = WGPUMipmapFilterMode_Nearest,
        .lodMaxClamp = 1.0f,
        .maxAnisotropy = 1,
    };
    impl.table_sampler = wgpuDeviceCreateSampler(device, &table_sampler);
    const WGPUSamplerDescriptor clamp_sampler{
        .addressModeU = WGPUAddressMode_ClampToEdge,
        .addressModeV = WGPUAddressMode_ClampToEdge,
        .addressModeW = WGPUAddressMode_ClampToEdge,
        .magFilter = WGPUFilterMode_Linear,
        .minFilter = WGPUFilterMode_Linear,
        .mipmapFilter = WGPUMipmapFilterMode_Nearest,
        .lodMaxClamp = 1.0f,
        .maxAnisotropy = 1,
    };
    impl.clamp_sampler = wgpuDeviceCreateSampler(device, &clamp_sampler);

    const WGPUBufferDescriptor buffer{.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, .size = uniform_stride * uniform_slots};
    impl.uniforms = wgpuDeviceCreateBuffer(device, &buffer);

    // Layouts: the table reads the uniform only; the sky the noise and the table too.
    WGPUBindGroupLayoutEntry uniform_entry{};
    uniform_entry.binding = 0;
    uniform_entry.visibility = WGPUShaderStage_Fragment;
    uniform_entry.buffer.type = WGPUBufferBindingType_Uniform;
    uniform_entry.buffer.hasDynamicOffset = true;
    uniform_entry.buffer.minBindingSize = sizeof(SkyUniform);
    {
        const WGPUBindGroupLayoutDescriptor descriptor{.entryCount = 1, .entries = &uniform_entry};
        impl.table_layout = wgpuDeviceCreateBindGroupLayout(device, &descriptor);
    }
    {
        std::array<WGPUBindGroupLayoutEntry, 9> entries{};
        entries[0] = uniform_entry;
        const auto texture = [&](const std::uint32_t binding, const WGPUTextureViewDimension dimension) {
            entries[binding].binding = binding;
            entries[binding].visibility = WGPUShaderStage_Fragment;
            entries[binding].texture.sampleType = WGPUTextureSampleType_Float;
            entries[binding].texture.viewDimension = dimension;
        };
        texture(1, WGPUTextureViewDimension_3D);
        texture(2, WGPUTextureViewDimension_3D);
        texture(3, WGPUTextureViewDimension_2D);
        entries[4].binding = 4;
        entries[4].visibility = WGPUShaderStage_Fragment;
        entries[4].sampler.type = WGPUSamplerBindingType_Filtering;
        texture(5, WGPUTextureViewDimension_2D);
        entries[6].binding = 6;
        entries[6].visibility = WGPUShaderStage_Fragment;
        entries[6].sampler.type = WGPUSamplerBindingType_Filtering;
        texture(7, WGPUTextureViewDimension_2D);
        entries[8].binding = 8;
        entries[8].visibility = WGPUShaderStage_Fragment;
        entries[8].sampler.type = WGPUSamplerBindingType_Filtering;
        const WGPUBindGroupLayoutDescriptor descriptor{.entryCount = entries.size(), .entries = entries.data()};
        impl.sky_layout = wgpuDeviceCreateBindGroupLayout(device, &descriptor);
    }
    {
        const WGPUBindGroupEntry entry{.binding = 0, .buffer = impl.uniforms, .offset = 0, .size = sizeof(SkyUniform)};
        const WGPUBindGroupDescriptor descriptor{.layout = impl.table_layout, .entryCount = 1, .entries = &entry};
        impl.table_binding = wgpuDeviceCreateBindGroup(device, &descriptor);
    }
    if (!impl.make_clouds(1, 1)) impl.release_clouds();

    const WGPUShaderSourceWGSL source{.chain = {.sType = WGPUSType_ShaderSourceWGSL}, .code = wgsl_name(sky_shader)};
    const WGPUShaderModuleDescriptor module_descriptor{.nextInChain = &source.chain};
    const auto shader = wgpuDeviceCreateShaderModule(device, &module_descriptor);

    // The table: one triangle over it, no depth, single sample.
    {
        const WGPUPipelineLayoutDescriptor layout_descriptor{.bindGroupLayoutCount = 1, .bindGroupLayouts = &impl.table_layout};
        const auto layout = wgpuDeviceCreatePipelineLayout(device, &layout_descriptor);
        const WGPUColorTargetState target{.format = table_format, .writeMask = WGPUColorWriteMask_All};
        const WGPUFragmentState fragment{.module = shader, .entryPoint = wgsl_name("fs_table"), .targetCount = 1, .targets = &target};
        const WGPURenderPipelineDescriptor descriptor{
            .layout = layout,
            .vertex = {.module = shader, .entryPoint = wgsl_name("vs_table")},
            .primitive = {.topology = WGPUPrimitiveTopology_TriangleList},
            .multisample = {.count = 1, .mask = ~0u, .alphaToCoverageEnabled = false},
            .fragment = &fragment,
        };
        impl.table_pipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor);
        wgpuPipelineLayoutRelease(layout);
    }
    // The sky: where the depth is still the far plane's (reversed Z: 0), no writes.
    {
        const WGPUPipelineLayoutDescriptor layout_descriptor{.bindGroupLayoutCount = 1, .bindGroupLayouts = &impl.sky_layout};
        const auto layout = wgpuDeviceCreatePipelineLayout(device, &layout_descriptor);
        constexpr WGPUStencilFaceState keep{
            .compare = WGPUCompareFunction_Always,
            .failOp = WGPUStencilOperation_Keep,
            .depthFailOp = WGPUStencilOperation_Keep,
            .passOp = WGPUStencilOperation_Keep,
        };
        const WGPUDepthStencilState depth{
            .format = depth_format,
            .depthWriteEnabled = WGPUOptionalBool_False,
            .depthCompare = WGPUCompareFunction_Equal,
            .stencilFront = keep,
            .stencilBack = keep,
            .stencilReadMask = ~0u,
            .stencilWriteMask = ~0u,
        };
        // Over the satellites drawn before it (the moon): they keep what
        // the clouds let through (the sky's alpha); their target's alpha kept.
        const WGPUBlendState over_satellites{
            .color = {.operation = WGPUBlendOperation_Add, .srcFactor = WGPUBlendFactor_One, .dstFactor = WGPUBlendFactor_SrcAlpha},
            .alpha = {.operation = WGPUBlendOperation_Add, .srcFactor = WGPUBlendFactor_Zero, .dstFactor = WGPUBlendFactor_One},
        };
        const WGPUColorTargetState target{.format = target_format, .blend = &over_satellites, .writeMask = WGPUColorWriteMask_All};
        const WGPUConstantEntry hdr{.key = wgsl_name("hdr_output"), .value = scene_hdr() ? 1.0 : 0.0};
        const WGPUFragmentState fragment{.module = shader,
                                         .entryPoint = wgsl_name("fs_sky"),
                                         .constantCount = 1,
                                         .constants = &hdr,
                                         .targetCount = 1,
                                         .targets = &target};
        const WGPURenderPipelineDescriptor descriptor{
            .layout = layout,
            .vertex = {.module = shader, .entryPoint = wgsl_name("vs_sky")},
            .primitive = {.topology = WGPUPrimitiveTopology_TriangleList},
            .depthStencil = &depth,
            .multisample = scene_multisample(),
            .fragment = &fragment,
        };
        impl.sky_pipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor);
        wgpuPipelineLayoutRelease(layout);
    }
    // The clouds' texture: one triangle over it, no depth, single sample.
    {
        const WGPUPipelineLayoutDescriptor layout_descriptor{.bindGroupLayoutCount = 1, .bindGroupLayouts = &impl.sky_layout};
        const auto layout = wgpuDeviceCreatePipelineLayout(device, &layout_descriptor);
        const WGPUColorTargetState target{.format = cloud_format, .writeMask = WGPUColorWriteMask_All};
        const WGPUFragmentState fragment{.module = shader, .entryPoint = wgsl_name("fs_clouds"), .targetCount = 1, .targets = &target};
        const WGPURenderPipelineDescriptor descriptor{
            .layout = layout,
            .vertex = {.module = shader, .entryPoint = wgsl_name("vs_sky")},
            .primitive = {.topology = WGPUPrimitiveTopology_TriangleList},
            .multisample = {.count = 1, .mask = ~0u, .alphaToCoverageEnabled = false},
            .fragment = &fragment,
        };
        impl.cloud_pipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor);
        wgpuPipelineLayoutRelease(layout);
    }
    // The clouds' shadow map: one triangle over it, no depth, single sample.
    impl.shadow_map = make_texture(device, WGPUTextureDimension_2D, cloud_shadow_size, cloud_shadow_size, cloud_shadow_format,
                                   WGPUTextureUsage_TextureBinding | WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopyDst,
                                   "sky-cloud-shadow");
    if (impl.shadow_map) {
        // Lit until first drawn (a texture starts at 0: all in shadow).
        write_bytes(device, impl.shadow_map, std::vector<std::uint8_t>(std::size_t{cloud_shadow_size} * cloud_shadow_size, 255),
                    cloud_shadow_size, cloud_shadow_size, 1);
        impl.shadow_view = wgpuTextureCreateView(impl.shadow_map, nullptr);
        const WGPUPipelineLayoutDescriptor layout_descriptor{.bindGroupLayoutCount = 1, .bindGroupLayouts = &impl.sky_layout};
        const auto layout = wgpuDeviceCreatePipelineLayout(device, &layout_descriptor);
        const WGPUColorTargetState target{.format = cloud_shadow_format, .writeMask = WGPUColorWriteMask_All};
        const WGPUFragmentState fragment{.module = shader, .entryPoint = wgsl_name("fs_cloud_shadow"), .targetCount = 1, .targets = &target};
        const WGPURenderPipelineDescriptor descriptor{
            .layout = layout,
            .vertex = {.module = shader, .entryPoint = wgsl_name("vs_table")},
            .primitive = {.topology = WGPUPrimitiveTopology_TriangleList},
            .multisample = {.count = 1, .mask = ~0u, .alphaToCoverageEnabled = false},
            .fragment = &fragment,
        };
        impl.shadow_pipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor);
        wgpuPipelineLayoutRelease(layout);
    }
    wgpuShaderModuleRelease(shader);
}

NativeSky::~NativeSky() = default;

bool NativeSky::ready() const noexcept {
    return impl_ && impl_->table_pipeline && impl_->sky_pipeline && impl_->cloud_pipeline && impl_->sky_bindings[0]
        && impl_->sky_bindings[1] && impl_->table_binding && impl_->uniforms;
}

void NativeSky::update(const NativeSkyFrame& input) {
    auto& impl = *impl_;
    const auto to_sun = glm::normalize(input.to_sun);
    const float altitude = (std::max)(input.altitude, 1.0f);
    impl.drift += glm::dvec2{input.wind} * static_cast<double>(input.elapsed);
    // The clouds' field wraps at 65536 m (every noise tiles within it).
    const auto field = glm::mod(input.eye_route + impl.drift, glm::dvec2{65536.0});
    impl.field = field;
    impl.to_sun = to_sun;
    ++impl.frames;
    // The layer: cumulus from 1.5 km above sea, or 1 km above the eye's
    // ground where the route lies high.
    const float base = (std::max)(1500.0f, altitude + 600.0f);
    const float top = base + 2200.0f;
    const float night = 1.0f - glm::smoothstep(-0.12f, 0.02f, to_sun.y);

    auto& f = impl.frame;
    f.sun[0] = to_sun.x;
    f.sun[1] = to_sun.y;
    f.sun[2] = to_sun.z;
    f.eye[0] = altitude;
    f.eye[1] = night;
    f.eye[3] = native_sky_sun;
    f.clouds[0] = std::clamp(input.coverage, 0.0f, 1.0f);
    f.clouds[1] = base;
    f.clouds[2] = top;
    // The shadow map's square: round the point where the sun's way from the
    // eye's own ground meets the layer's middle, so the land round the eye
    // falls near its middle at any sun height. Faded in as the sun rises.
    // Drawn every fourth frame, at once when the sun turns, the eye jumps or
    // the sharpness changes; read in between where it was drawn.
    {
        const float rise = (std::max)(to_sun.y, 0.05f);
        const float above = (base + top) * 0.5f - altitude;
        const float sharpness = std::clamp(cloud_shadow_sharpness(), 1.0f, 10.0f);
        impl.eye_field = input.eye_route + impl.drift;
        impl.shadow_above = above;
        impl.shadow_strength = std::clamp((to_sun.y - 0.02f) / 0.10f, 0.0f, 1.0f);
        if (!cloud_shadows() || impl.shadow_strength <= 0.0f) impl.shadow_drawn = false;
        impl.shadow_due = !impl.shadow_drawn || impl.frames - impl.shadow_frame >= cloud_shadow_every
                          || glm::dot(to_sun, impl.shadow_sun) < 0.99999f || glm::length(impl.eye_field - impl.shadow_field) > 300.0
                          || sharpness != impl.shadow_sharpness;
        if (impl.shadow_due) {
            impl.shadow_frame = impl.frames;
            impl.shadow_field = impl.eye_field;
            impl.shadow_centre = glm::vec2{to_sun.x, to_sun.z} * (above / rise);
            impl.shadow_sun = to_sun;
            impl.shadow_sharpness = sharpness;
        }
        f.shadow[0] = impl.shadow_centre.x;
        f.shadow[1] = impl.shadow_centre.y;
        f.shadow[2] = cloud_shadow_half;
        f.shadow[3] = impl.shadow_sharpness;
    }
    f.field[0] = static_cast<float>(field.x);
    f.field[1] = static_cast<float>(field.y);
    // Each frame's samples shifted on by the golden ratio's fraction.
    f.field[2] = static_cast<float>(std::fmod(static_cast<double>(impl.frames) * 0.6180339887498949, 1.0));

    // The clouds' light: the sun through the air above their middle, the
    // sky above them (its average over the upper half), the ground below
    // (a fifth of the sun on it, lit by the sky too); the night's own glow.
    const float middle = (base + top) * 0.5f;
    const auto sun_light = atmosphere_transmittance(middle, to_sun) * native_sky_sun;
    glm::vec3 above{0.0f};
    int count = 0;
    for (const float elevation : {15.0f, 45.0f, 80.0f}) {
        for (int k = 0; k < 4; ++k) {
            const float a = glm::radians(45.0f + 90.0f * static_cast<float>(k));
            const float e = glm::radians(elevation);
            above += atmosphere_radiance(middle, {std::cos(e) * std::sin(a), std::sin(e), std::cos(e) * std::cos(a)}, to_sun);
            ++count;
        }
    }
    above /= static_cast<float>(count);
    const glm::vec3 night_glow = glm::vec3{0.004f, 0.006f, 0.012f} * night;
    const auto ground_sun = atmosphere_transmittance(altitude, to_sun) * native_sky_sun * (std::max)(to_sun.y, 0.0f);
    const auto below = (ground_sun * 0.2f / 3.14159265f + above * 0.3f) * 0.5f;
    for (int i = 0; i < 3; ++i) {
        f.sun_at_clouds[i] = sun_light[i];
        f.ambient_top[i] = above[i] + night_glow[i];
        f.ambient_bottom[i] = below[i] + night_glow[i];
    }

    // Display encoded as the sky is drawn (the HDR frame: no soft clip).
    const bool hdr = scene_hdr();
    const auto display = [hdr](const glm::vec3& linear) {
        glm::vec3 result;
        for (int i = 0; i < 3; ++i) {
            const float c = (std::max)(linear[i], 0.0f);
            result[i] = std::pow(hdr ? c : shoulder(c), 1.0f / 2.2f);
        }
        return result;
    };

    // The horizon's average colour: RTS's fog colour while this sky is
    // drawn (what has no direction: the clear colour, the water's sky).
    glm::vec3 horizon{0.0f};
    for (int k = 0; k < 8; ++k) {
        const float a = glm::radians(22.5f + 45.0f * static_cast<float>(k));
        const float e = glm::radians(2.0f);
        horizon += atmosphere_radiance(altitude, {std::cos(e) * std::sin(a), std::sin(e), std::cos(e) * std::cos(a)}, to_sun);
    }
    impl.horizon = display(horizon / 8.0f + night_glow);

    // The fog's colour by direction (FogRing): the horizon half a degree up
    // (what a long way through the air turns to; the sky below it is that
    // too, fs_sky) at the ring's angles from the sun's way.
    glm::vec2 way{to_sun.x, to_sun.z};
    way = glm::length(way) > 1e-4f ? glm::normalize(way) : glm::vec2{0.0f, 1.0f};
    impl.fog_ring.on = true;
    impl.fog_ring.sun = way;
    const float up = glm::radians(0.5f);
    for (std::size_t i = 0; i < FogRing::count; ++i) {
        const float a = fog_ring_angle(i);
        const glm::vec2 d{way.x * std::cos(a) - way.y * std::sin(a), way.x * std::sin(a) + way.y * std::cos(a)};
        const glm::vec3 view{std::cos(up) * d.x, std::sin(up), std::cos(up) * d.y};
        impl.fog_ring.colours[i] = display(atmosphere_radiance(altitude, view, to_sun) + night_glow);
    }
}

void NativeSky::render_table(const WGPUCommandEncoder encoder) {
    if (!ready()) return;
    auto& impl = *impl_;
    impl.write_slot(table_slot);
    const WGPURenderPassColorAttachment colour{
        .view = impl.table_view,
        .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
        .loadOp = WGPULoadOp_Clear,
        .storeOp = WGPUStoreOp_Store,
        .clearValue = {.r = 0.0, .g = 0.0, .b = 0.0, .a = 1.0},
    };
    const WGPURenderPassDescriptor descriptor{.colorAttachmentCount = 1, .colorAttachments = &colour};
    const auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
    const std::uint32_t offset = table_slot * static_cast<std::uint32_t>(uniform_stride);
    wgpuRenderPassEncoderSetPipeline(pass, impl.table_pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, impl.table_binding, 1, &offset);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
}

void NativeSky::render_clouds(const WGPUCommandEncoder encoder, const glm::mat4& projection, const glm::mat4& view,
                              const std::uint32_t width, const std::uint32_t height) {
    if (!ready() || width == 0 || height == 0) return;
    auto& impl = *impl_;
    const std::uint32_t w = (width + 1) / 2;
    const std::uint32_t h = (height + 1) / 2;
    // The last frames' clouds count only when they were the frame just
    // before, at this size, under nearly the same sun, the eye not jumped.
    bool fresh = impl.traced_at + 1 != impl.frames;
    if (w != impl.cloud_width || h != impl.cloud_height) {
        if (!impl.make_clouds(w, h)) {
            // Back to a size that works; the sky marches its clouds itself.
            if (!impl.make_clouds(1, 1)) impl.release_clouds();
            return;
        }
        fresh = true;
    }
    if (glm::dot(impl.to_sun, impl.traced_sun) < 0.99996f || glm::length(impl.field - impl.traced_field) > 300.0) fresh = true;

    // Directions only: the view's rotation.
    const auto directions = projection * glm::mat4{glm::mat3{view}};
    impl.cloud_current ^= 1u;
    impl.frame.inverse_view_projection = glm::inverse(directions);
    impl.frame.previous_view_projection = impl.traced_directions;
    impl.frame.field[3] = fresh ? 1.0f : cloud_history_weight;
    impl.frame.clouds[3] = 0.0f;
    impl.write_slot(cloud_slot);
    impl.frame.field[3] = 1.0f;

    const WGPURenderPassColorAttachment colour{
        .view = impl.cloud_views[impl.cloud_current],
        .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
        .loadOp = WGPULoadOp_Clear,
        .storeOp = WGPUStoreOp_Store,
        .clearValue = {.r = 0.0, .g = 0.0, .b = 0.0, .a = 1.0},
    };
    const WGPURenderPassDescriptor descriptor{.colorAttachmentCount = 1, .colorAttachments = &colour};
    const auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
    const std::uint32_t offset = cloud_slot * static_cast<std::uint32_t>(uniform_stride);
    wgpuRenderPassEncoderSetPipeline(pass, impl.cloud_pipeline);
    // Reading the other texture: the last frames' clouds.
    wgpuRenderPassEncoderSetBindGroup(pass, 0, impl.sky_bindings[impl.cloud_current ^ 1u], 1, &offset);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);

    // The clouds' shadow on the ground, for this frame's main pass.
    if (impl.shadow_pipeline && impl.shadow_due && cloud_shadows() && impl.shadow_strength > 0.0f) {
        const WGPURenderPassColorAttachment target{
            .view = impl.shadow_view,
            .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
            .loadOp = WGPULoadOp_Clear,
            .storeOp = WGPUStoreOp_Store,
            .clearValue = {.r = 1.0, .g = 0.0, .b = 0.0, .a = 1.0},
        };
        const WGPURenderPassDescriptor shadow_descriptor{.colorAttachmentCount = 1, .colorAttachments = &target};
        const auto shadow_pass = wgpuCommandEncoderBeginRenderPass(encoder, &shadow_descriptor);
        wgpuRenderPassEncoderSetPipeline(shadow_pass, impl.shadow_pipeline);
        wgpuRenderPassEncoderSetBindGroup(shadow_pass, 0, impl.sky_bindings[impl.cloud_current], 1, &offset);
        wgpuRenderPassEncoderDraw(shadow_pass, 3, 1, 0, 0);
        wgpuRenderPassEncoderEnd(shadow_pass);
        wgpuRenderPassEncoderRelease(shadow_pass);
        impl.shadow_drawn = true;
        impl.shadow_due = false;
    }

    impl.traced_at = impl.frames;
    impl.traced_directions = directions;
    impl.traced_sun = impl.to_sun;
    impl.traced_field = impl.field;
    impl.clouds_this_frame = true;
}

void NativeSky::render(const WGPURenderPassEncoder pass, const glm::mat4& projection, const glm::mat4& view, const std::uint32_t slot) {
    if (!ready() || slot >= table_slot) return;
    auto& impl = *impl_;
    // Directions only: the view's rotation.
    impl.frame.inverse_view_projection = glm::inverse(projection * glm::mat4{glm::mat3{view}});
    // The frame's view reads the clouds render_clouds() drew for it; the
    // probe's faces march their own.
    const bool from_texture = slot == frame_slot && impl.clouds_this_frame;
    impl.frame.clouds[3] = from_texture ? 1.0f : 0.0f;
    impl.write_slot(slot);
    impl.frame.clouds[3] = 0.0f;
    if (slot == frame_slot) impl.clouds_this_frame = false;
    const std::uint32_t offset = slot * static_cast<std::uint32_t>(uniform_stride);
    wgpuRenderPassEncoderSetPipeline(pass, impl.sky_pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, impl.sky_bindings[impl.cloud_current], 1, &offset);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
}

CloudShadow NativeSky::cloud_shadow() const noexcept {
    if (!impl_ || !impl_->shadow_view) return {};
    const auto& impl = *impl_;
    // Drawn this frame (it is due: render_clouds() draws it before the main
    // pass), or before and read where it was drawn: the eye and the clouds
    // have moved on in the field since.
    const bool usable = impl.shadow_drawn || impl.shadow_due;
    const float strength = usable && cloud_shadows() ? impl.shadow_strength * std::clamp(cloud_shadow_darkness(), 0.0f, 1.0f) : 0.0f;
    const glm::dvec2 moved = impl.eye_field - impl.shadow_field;
    // The edge band: half of 1 / sqrt(sharpness) either side of a half let
    // through (sharpness 1: the whole range, 3: 0.21..0.79, 10: 0.34..0.66).
    const float band = 0.5f / std::sqrt((std::max)(impl.shadow_sharpness, 1.0f));
    return {.map = impl.shadow_view,
            .area = {impl.shadow_centre.x - static_cast<float>(moved.x), impl.shadow_centre.y - static_cast<float>(moved.y), cloud_shadow_half,
                     impl.shadow_above},
            .sun = glm::vec4{impl.shadow_sun, strength},
            .edge = {0.5f - band, 0.5f + band}};
}

FogRing NativeSky::fog_ring() const noexcept {
    return impl_ ? impl_->fog_ring : FogRing{};
}

glm::vec3 NativeSky::horizon_colour() const noexcept {
    return impl_ ? impl_->horizon : glm::vec3{0.6f};
}

} // namespace openrts::renderer
#endif
