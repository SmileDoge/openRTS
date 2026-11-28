// The openRTS water (RTS had nothing of it): waves from three octaves of a
// wave map, the sky reflected by Fresnel (Schlick, water's 0.02), the sun's
// highlight (normalised Blinn-Phong, wider where the far map averages its
// ripples out: Toksvig), the water's own colour lit by the sun and the sky,
// the ground seen through by the light left along the view's path under the
// surface, and a soft shore. Laid over what is drawn by alpha.
struct Draw {
    mvp: mat4x4<f32>,
    offset: vec4<f32>,
    eye: vec4<f32>,    // xyz: the camera, w: the water's clock (s, wraps at 600)
    params: vec4<f32>, // x: the sky probe is there, y: the surface above the water level (m), z: wave strength
};

struct Frame {
    light_diffuse: vec4<f32>,
    light_ambient: vec4<f32>,
    light_direction: vec4<f32>,
    fog_colour: vec4<f32>,
    fog_distance: vec4<f32>,
    lights: array<Light, 90>,
    ring_counts: array<vec4<i32>, 3>,
    shading: vec4<f32>,
    fog_sun: vec4<f32>,             // xy: the sun's way along the ground (0: no ring), zw: the eye's x, z
    fog_ring: array<vec4<f32>, 12>, // the horizon's colour at each fog_ring_angle (fog_ring.hpp)
};

override experimental: bool = false;
override hdr_output: bool = false;

@group(0) @binding(0) var wave_texture: texture_2d<f32>;
@group(0) @binding(1) var sky_probe: texture_cube<f32>;
@group(0) @binding(2) var water_sampler: sampler;
@group(1) @binding(0) var<uniform> draw: Draw;
@group(1) @binding(1) var<uniform> frame: Frame;

struct SurfaceIn {
    @location(0) position: vec3<f32>,
    @location(1) depth: f32,
};

struct SurfaceOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) world: vec3<f32>,
    @location(1) depth: f32,
    @location(2) fog: vec4<f32>,
};

@vertex
fn vs_surface(input: SurfaceIn) -> SurfaceOut {
    var out: SurfaceOut;
    out.clip = draw.mvp * vec4<f32>(input.position, 1.0);
    out.world = input.position + draw.offset.xyz;
    out.depth = input.depth + draw.params.y;
    // The terrain's fog (its RTS-compatible path), so the water and the
    // ground under it fog alike.
    let fog_start = frame.fog_distance.x;
    let fog_end = frame.fog_distance.y;
    if (fog_end <= 0.0) {
        out.fog = vec4<f32>(0.0);
    } else if (fog_end > fog_start) {
        let amount = clamp(frame.fog_colour.w * ((out.clip.w - fog_start) / (fog_end - fog_start)), 0.0, 1.0);
        out.fog = vec4<f32>(fog_ring_colour(out.world.xyz, frame.fog_colour.xyz), amount);
    } else {
        out.fog = vec4<f32>(fog_ring_colour(out.world.xyz, frame.fog_colour.xyz), frame.fog_colour.w);
    }
    return out;
}

fn water_linear(c: vec3<f32>) -> vec3<f32> {
    return pow(max(c, vec3<f32>(0.0)), vec3<f32>(2.2));
}

fn water_display(c: vec3<f32>) -> vec3<f32> {
    return pow(max(c, vec3<f32>(0.0)), vec3<f32>(1.0 / 2.2));
}

// As the terrain's and the models' experimental path.
fn water_shoulder(c: vec3<f32>) -> vec3<f32> {
    let knee = 0.6;
    let room = 1.0 - knee;
    let over = max(c - vec3<f32>(knee), vec3<f32>(0.0));
    let rolled = vec3<f32>(knee) + room * (vec3<f32>(1.0) - exp(-over / room));
    return select(c, rolled, c > vec3<f32>(knee));
}

// A wave map's normal (z up), shorter where its mipmap averages ripples.
fn wave(uv: vec2<f32>) -> vec3<f32> {
    return textureSample(wave_texture, water_sampler, uv).xyz * 2.0 - 1.0;
}

// The sky towards a direction (linear): the probe, else a gradient from
// RTS's fog colour at the horizon to a deeper blue overhead.
fn sky_towards(direction: vec3<f32>) -> vec3<f32> {
    if (draw.params.x > 0.5) {
        return water_linear(textureSampleLevel(sky_probe, water_sampler, direction, 0.0).xyz);
    }
    let horizon = water_linear(frame.fog_colour.xyz);
    let zenith = horizon * vec3<f32>(0.45, 0.62, 1.0);
    return mix(horizon, zenith, sqrt(clamp(direction.y, 0.0, 1.0)));
}

@fragment
fn fs_surface(input: SurfaceOut) -> @location(0) vec4<f32> {
    let time = draw.eye.w;
    let p = input.world.xz;
    // Three octaves, each its own size, way and speed (map tiles a second,
    // whole sixhundredths, so the clock's wrap is not seen); the middle one
    // turned to hide the map's repeat, its slopes turned back.
    let turned = vec2<f32>(p.x * 0.8 - p.y * 0.6, p.x * 0.6 + p.y * 0.8);
    // A long swell under them (2026-11-17): its waves (about 9 m) stay
    // seen four times as far as the 37 m octave's.
    let d = wave(turned / 150.0 + vec2<f32>(0.005, -0.005) * time);
    let a = wave(p / 37.0 + vec2<f32>(0.010, 0.005) * time);
    let b = wave(turned / 13.0 + vec2<f32>(-0.015, 0.010) * time);
    let c = wave(p / 4.3 + vec2<f32>(0.025, -0.030) * time);
    let b_slope = vec2<f32>(b.x * 0.8 + b.y * 0.6, -b.x * 0.6 + b.y * 0.8);
    let d_slope = vec2<f32>(d.x * 0.8 + d.y * 0.6, -d.x * 0.6 + d.y * 0.8);
    // The water a pixel covers (m, the longer way: at a slant, along the
    // view). An octave whose ripples (about its map's size / 16) are under
    // a few pixels there only flickers: at a grazing angle the reflection
    // jumps between a cloud and the blue from frame to frame, the mipmaps
    // keeping the ripples across the view. It fades out, and what it took
    // away roughens the reflection instead (below): far off the water is the
    // sky blurred by ripples too small to see, not a mirror.
    let footprint = max(length(dpdx(p)), length(dpdy(p)));
    let keep_d = 1.0 - smoothstep(150.0 / 64.0, 150.0 / 16.0, footprint);
    let keep_a = 1.0 - smoothstep(37.0 / 64.0, 37.0 / 16.0, footprint);
    let keep_b = 1.0 - smoothstep(13.0 / 64.0, 13.0 / 16.0, footprint);
    let keep_c = 1.0 - smoothstep(4.3 / 64.0, 4.3 / 16.0, footprint);
    let strength = draw.params.z;
    let slope = (d_slope * 0.6 * keep_d + a.xy * keep_a + b_slope * keep_b + c.xy * 0.7 * keep_c) * strength;
    let n = normalize(vec3<f32>(slope.x, 1.0, slope.y));
    let toksvig = clamp((length(a) + length(b) + length(c) + length(d)) / 4.0, 0.05, 1.0);
    // The slopes faded out: each octave's (the map's are 0.35 root mean
    // square) as a spread of the reflection's way, a surface of this
    // roughness (GGX, perceptual) in the sky light's prefiltered cube.
    let lost = ((1.0 - keep_d) * 0.36 + (1.0 - keep_a) + (1.0 - keep_b) + (1.0 - keep_c) * 0.49) * 0.1225 * strength * strength;
    let ripple_roughness = sqrt(clamp(sqrt(lost), 0.0, 1.0));

    let to_eye = draw.eye.xyz - input.world;
    let v = to_eye / max(length(to_eye), 1e-3);
    let ndv = clamp(dot(n, v), 1e-3, 1.0);
    let fresnel = 0.02 + 0.98 * pow(1.0 - ndv, 5.0);
    var r = reflect(-v, n);
    r.y = max(r.y, 0.01); // a steep ripple turned below the horizon: the horizon
    var reflected = sky_towards(normalize(r));
    // With the sky's light (SkyLight: PBR, the sky's own light), the ripples
    // too small to see blur it: from the mirror near to the cube's mip for
    // their roughness (its reflections' strength setting left out: the
    // water's own).
    if (draw.params.x > 0.5 && effects.sky_light.x > 0.5) {
        let blurred = effects_sky_radiance(normalize(r), ripple_roughness) / max(effects.sky_tune.y, 1e-3);
        reflected = mix(reflected, blurred, smoothstep(0.05, 0.25, ripple_roughness));
    }

    let sun_colour = water_linear(frame.light_diffuse.xyz);
    let sky_light = water_linear(frame.light_ambient.xyz);
    let l = -normalize(frame.light_direction.xyz);
    let shadow = effects_shadow(input.world, vec3<f32>(0.0, 1.0, 0.0), input.clip) * effects_ray_sun_tint(input.clip);
    let h = normalize(l + v);
    let gloss = 1500.0;
    let power = toksvig * gloss / (toksvig + gloss * (1.0 - toksvig));
    let highlight = pow(max(dot(n, h), 0.0), power) * (power + 8.0) / 25.132741;
    let fresnel_h = 0.02 + 0.98 * pow(1.0 - clamp(dot(v, h), 0.0, 1.0), 5.0);
    let glint = sun_colour * highlight * fresnel_h * smoothstep(-0.02, 0.06, l.y) * shadow;

    // The water's own colour: light scattered back from inside it.
    let body = vec3<f32>(0.035, 0.060, 0.055) * (sky_light + sun_colour * max(l.y, 0.0) * shadow);
    // The ground under it, through the light left along the view's path.
    let path = max(input.depth, 0.0) / max(v.y, 0.1);
    let through = exp(-path * 0.8);

    let alpha = 1.0 - (1.0 - fresnel) * through;
    var colour = (reflected * fresnel + body * (1.0 - fresnel) * (1.0 - through) + glint) / max(alpha, 0.02);
    colour = min(colour, vec3<f32>(32.0));
    let aerial = experimental && effects.flags.z != 0u;
    if (aerial) {
        colour = effects_aerial(colour, input.world);
    }
    var display = water_display(select(water_shoulder(colour), max(colour, vec3<f32>(0.0)), hdr_output));
    if (!aerial && input.fog.w > 0.0) {
        display = mix(display, input.fog.xyz, input.fog.w);
    }
    // The linear HDR frame (frame_encoding()): the light as computed.
    if (frame.shading.z > 0.5) {
        var light = max(colour, vec3<f32>(0.0));
        if (!aerial && input.fog.w > 0.0) {
            light = mix(light, water_linear(input.fog.xyz), input.fog.w);
        }
        display = light;
    }
    // The shore: none of it where the ground meets the water.
    let shore = smoothstep(0.0, 0.35, input.depth);
    return vec4<f32>(display, clamp(alpha, 0.0, 1.0) * shore);
}
