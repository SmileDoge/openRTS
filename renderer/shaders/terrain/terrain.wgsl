// Port of RTS terrain.vert/frag, the additional light sources included.
struct Draw {
    mvp: mat4x4<f32>,
    offset: vec4<f32>,
    uv_rect: vec4<f32>,
    uv_mode: vec4<f32>,
};

struct Frame {
    light_diffuse: vec4<f32>,
    light_ambient: vec4<f32>,
    light_direction: vec4<f32>,
    fog_colour: vec4<f32>,
    fog_distance: vec4<f32>,
    lights: array<Light, 90>, // nine a ring
    ring_counts: array<vec4<i32>, 3>,
    shading: vec4<f32>, // x: the experimental path's night ambient factor, z: 1 the frame is linear, w: 1 PBR
    fog_sun: vec4<f32>,             // xy: the sun's way along the ground (0: no ring), zw: the eye's x, z
    fog_ring: array<vec4<f32>, 12>, // the horizon's colour at each fog_ring_angle (fog_ring.hpp)
    surface: vec4<f32>,             // the ground: roughness, metallic, a dielectric's F0, specular factor (SurfaceMaterial)
};

// The experimental path (openRTS's shading, as the models': linear light,
// lamps added) and the HDR frame (no soft clip, the tone pass does it).
override experimental: bool = false;
override hdr_output: bool = false;

@group(0) @binding(0) var base_texture: texture_2d<f32>;
@group(0) @binding(1) var micro_texture: texture_2d<f32>;
@group(0) @binding(2) var terrain_sampler: sampler;
// The ground texture's material (openRTS, docs/pbr-materials.md): its
// normal map, ORM map and numbers; a flat map, a white one and no file
// without one.
struct TerrainMaterial {
    factors: vec4<f32>, // roughness, metallic, a dielectric's F0, specular
    extra: vec4<f32>,   // normal strength, occlusion strength, 1 a normal map, 1 a material file
};
@group(0) @binding(3) var normal_texture: texture_2d<f32>;
@group(0) @binding(4) var orm_texture: texture_2d<f32>;
@group(0) @binding(5) var<uniform> ground: TerrainMaterial;
@group(1) @binding(0) var<uniform> draw: Draw;
@group(1) @binding(1) var<uniform> frame: Frame;

struct VertexIn {
    @location(0) position: vec3<f32>,
    @location(1) normal: vec3<f32>,
    @location(2) uv: vec2<f32>,
    // The patch's ring of further lights, passed as the draw's first instance.
    @builtin(instance_index) ring: u32,
};

struct VertexOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) colour: vec4<f32>,
    @location(2) fog: vec4<f32>,
    @location(3) world: vec4<f32>,
    @location(4) normal: vec3<f32>,
    @location(5) @interpolate(flat) ring: u32,
};

fn ring_count(ring: u32) -> i32 {
    return min(frame.ring_counts[ring / 4u][ring % 4u], 9);
}

@vertex
fn vs_main(input: VertexIn) -> VertexOut {
    var out: VertexOut;
    out.ring = min(input.ring, 9u);
    out.clip = draw.mvp * vec4<f32>(input.position, 1.0);
    out.uv = draw.uv_rect.xy + input.uv * draw.uv_rect.zw;
    out.world = vec4<f32>(input.position + draw.offset.xyz, 1.0);
    out.normal = input.normal;

    let factor = clamp(-dot(input.normal, frame.light_direction.xyz) * 2.0, 0.0, 1.0);
    let diffuse = max(frame.light_diffuse * factor, vec4<f32>(0.0));
    let ambient = max(frame.light_ambient, vec4<f32>(0.0));
    out.colour = max(diffuse, ambient);

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

// Water layers (RTS water.vert): lit with the normal up; UVs per patch, or
// following the camera (world_water_layer_sky_reflection, 0x4A2144).
struct WaterIn {
    @location(0) position: vec3<f32>,
    @location(1) patch_corner: vec2<f32>,
    @location(2) corner: vec2<f32>,
    @builtin(instance_index) ring: u32,
};

@vertex
fn vs_water(input: WaterIn) -> VertexOut {
    var out: VertexOut;
    out.ring = min(input.ring, 9u);
    out.clip = draw.mvp * vec4<f32>(input.position, 1.0);
    if (draw.uv_mode.x > 0.5) {
        let size = draw.uv_mode.w;
        let start = vec2<f32>((input.patch_corner.x + draw.offset.x - draw.uv_mode.y) / size,
                              (draw.uv_mode.z - input.patch_corner.y - draw.offset.z) / size);
        out.uv = start + draw.uv_rect.xy + input.corner * draw.uv_rect.zw;
    } else {
        out.uv = draw.uv_rect.xy + input.corner * draw.uv_rect.zw;
    }
    out.world = vec4<f32>(input.position + draw.offset.xyz, 1.0);
    out.normal = vec3<f32>(0.0, 1.0, 0.0);
    let factor = clamp(-dot(out.normal, frame.light_direction.xyz) * 2.0, 0.0, 1.0);
    out.colour = max(max(frame.light_diffuse * factor, vec4<f32>(0.0)), max(frame.light_ambient, vec4<f32>(0.0)));
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

fn to_linear(c: vec3<f32>) -> vec3<f32> {
    return pow(max(c, vec3<f32>(0.0)), vec3<f32>(2.2));
}

fn to_display(c: vec3<f32>) -> vec3<f32> {
    return pow(max(c, vec3<f32>(0.0)), vec3<f32>(1.0 / 2.2));
}

// Linear up to the knee, then an exponential roll-off towards 1 (as the models').
fn shoulder(c: vec3<f32>) -> vec3<f32> {
    let knee = 0.6;
    let room = 1.0 - knee;
    let over = max(c - vec3<f32>(knee), vec3<f32>(0.0));
    let rolled = vec3<f32>(knee) + room * (vec3<f32>(1.0) - exp(-over / room));
    return select(c, rolled, c > vec3<f32>(knee));
}

// The experimental path, as the models' (model_renderer.cpp): sky ambient
// (less of it the stronger the sun, darker towards the ground), Lambert sun,
// the further lights added, all in linear light (each light's falloff taken
// in display space, as RTS's).
fn lamp_light(light: Light, world: vec4<f32>, n: vec3<f32>, physical: bool) -> vec3<f32> {
    // A photometric lamp (ambient.w, photometric.hpp) is physical always.
    if (physical || light.ambient.w > 0.5) {
        let strength = physical_light(light, world, n);
        if (strength > 0.0 && light.ambient.w > 0.5) {
            return light.ambient.xyz * strength; // photometric: linear already
        }
        if (strength > 0.0) {
            return to_linear(light.diffuse.xyz) * strength;
        }
        return vec3<f32>(0.0);
    }
    let factor = light_factor(light, world, n);
    if (factor > 0.0) {
        return to_linear(light.diffuse.xyz * factor);
    }
    return vec3<f32>(0.0);
}

// The ground's normal bent by its map: the frame from the screen's
// derivatives of the place and the texture coordinates (the terrain has no
// tangents); the map's green up the image (v down in RTS's coordinates).
fn ground_normal(normal: vec3<f32>, world: vec3<f32>, uv: vec2<f32>, mapped: vec3<f32>) -> vec3<f32> {
    let n = normalize(normal);
    let dp1 = dpdx(world);
    let dp2 = dpdy(world);
    let duv1 = dpdx(uv);
    let duv2 = dpdy(uv);
    if (ground.extra.z < 0.5) {
        return n;
    }
    let dp2perp = cross(dp2, n);
    let dp1perp = cross(n, dp1);
    let t = dp2perp * duv1.x + dp1perp * duv2.x;
    let b = dp2perp * duv1.y + dp1perp * duv2.y;
    let scale = max(dot(t, t), dot(b, b));
    if (scale <= 1e-20) {
        return n;
    }
    let inverse = inverseSqrt(scale);
    var m = mapped * 2.0 - vec3<f32>(1.0);
    m = vec3<f32>(m.xy * ground.extra.x, m.z);
    return normalize(t * inverse * m.x - b * inverse * m.y + n * max(m.z, 0.0));
}

fn shade_experimental(input: VertexOut, texel: vec4<f32>, bent: vec3<f32>, orm: vec3<f32>) -> vec4<f32> {
    let base = to_linear(texel.xyz);
    let sun_colour = to_linear(frame.light_diffuse.xyz);
    let sky_colour = to_linear(frame.light_ambient.xyz);
    let n = bent;
    // The material's own surface, else the ground's (frame.surface).
    var surface_material = frame.surface;
    if (ground.extra.w > 0.5) {
        surface_material = vec4<f32>(clamp(ground.factors.x * orm.y, 0.03, 1.0), clamp(ground.factors.y * orm.z, 0.0, 1.0),
                                     ground.factors.z, ground.factors.w);
    }
    let occluded = mix(1.0, orm.x, ground.extra.y);
    let to_sun = -normalize(frame.light_direction.xyz);
    let facing = max(dot(n, to_sun), 0.0);
    let sun_strength = clamp(dot(sun_colour, vec3<f32>(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
    // The sun's shadow and the ambient occlusion (SceneEffects).
    let shadow = effects_shadow(input.world.xyz, n, input.clip);
    let ambient = sky_colour * (1.0 - 0.65 * sun_strength) * mix(0.55, 1.0, n.y * 0.5 + 0.5)
        * mix(1.0, frame.shading.x, 1.0 - sun_strength) * effects_ao(input.clip) * occluded;
    // Physically based light (frame.shading.w, frame_pbr(); PBR stage 1):
    // the sun and the lamps by the BRDF (shaders/common/pbr.wgsl), the ground as
    // frame.surface makes it (terrain_surface_material: as yet Lambert
    // alone); the sky light Lambert on the albedo.
    let pbr = frame.shading.w > 0.5;
    let surface = pbr_surface(base, n, normalize(effects.eye.xyz - input.world.xyz), surface_material);
    let reflects = surface_material.w > 0.0;
    // The sky's own light (PBR stage 2, as the models'): Lambert by the
    // sky's SH9, at least the environment's at night, near the eye as open
    // as the sky round it; the reflection by the split sum.
    let sky_light = pbr && effects.sky_light.x > 0.5;
    let night_light = sky_colour * frame.shading.x * (1.0 - sun_strength);
    let openness = effects_sky_openness(input.world.xyz);
    var lamps = vec3<f32>(0.0);
    var lamps_out = vec3<f32>(0.0); // PBR: what the lamps send to the eye
    let physical = frame.fog_distance.w > 0.5; // physical_light_falloff()
    if (effects.flags.x != 0u) {
        let cluster = effects_cluster(input.clip, input.world.xyz);
        let count = min(cluster_counts[cluster], effects.clusters.w);
        let first = cluster * effects.clusters.w;
        for (var i = 0u; i < count; i = i + 1u) {
            let index = cluster_lights[first + i];
            let light = lamp_light(scene_lights[index], input.world, n, physical);
            if (any(light > vec3<f32>(0.0))) {
                let lit = light * effects_lamp_shadow(index, input.world.xyz, n, input.clip);
                lamps = lamps + lit;
                if (pbr) {
                    lamps_out = lamps_out + pbr_lit(surface, pbr_lamp_way(scene_lights[index], input.world), lit, reflects);
                }
            }
        }
    } else {
        let lights = ring_count(input.ring);
        for (var i = 0; i < lights; i = i + 1) {
            let ring_light = frame.lights[input.ring * 9u + u32(i)];
            let light = lamp_light(ring_light, input.world, n, physical);
            if (any(light > vec3<f32>(0.0))) {
                let lit = light * effects_lamp_shadow_of(ring_light, input.world.xyz, n, input.clip);
                lamps = lamps + lit;
                if (pbr) {
                    lamps_out = lamps_out + pbr_lit(surface, pbr_lamp_way(ring_light, input.world), lit, reflects);
                }
            }
        }
    }
    lamps = lamps * (1.0 - effects.lamps.x); // weaker by day
    lamps_out = lamps_out * (1.0 - effects.lamps.x);
    var colour = base * (ambient + sun_colour * facing * shadow + lamps);
    if (pbr) {
        var sky_ambient = ambient;
        var sky = PbrSkyReflection(vec3<f32>(0.0), vec3<f32>(0.0));
        if (sky_light) {
            let occlusion = effects_ao(input.clip) * occluded;
            sky_ambient = max(effects_sky_light(n), night_light * mix(0.55, 1.0, n.y * 0.5 + 0.5)) * openness * occlusion;
            if (reflects) {
                sky = pbr_sky_reflection(surface, occlusion, night_light);
                sky.light = sky.light * openness;
            }
        }
        colour = surface.diffuse * sky_ambient * (vec3<f32>(1.0) - sky.taken) + sky.light
            + pbr_lit(surface, to_sun, sun_colour * facing * shadow, reflects) + lamps_out;
    }
    if (effects.flags.z != 0u) {
        colour = effects_aerial(colour, input.world.xyz);
    }
    if (effects.flags.y == 2u) {
        // The occlusion itself (--effects show-ao), shown alike in either encoding.
        let ao = vec3<f32>(effects_ao(input.clip));
        return vec4<f32>(select(ao, pow(ao, vec3<f32>(2.2)), frame.shading.z > 0.5), texel.w);
    }
    // The linear HDR frame (frame_encoding()): the light as computed.
    if (frame.shading.z > 0.5) {
        var light = max(colour, vec3<f32>(0.0));
        if (effects.flags.z == 0u && input.fog.w > 0.0) {
            light = mix(light, to_linear(input.fog.xyz), input.fog.w);
        }
        return vec4<f32>(light, texel.w);
    }
    var display = to_display(select(shoulder(colour), max(colour, vec3<f32>(0.0)), hdr_output));
    if (effects.flags.z == 0u && input.fog.w > 0.0) {
        display = mix(display, input.fog.xyz, input.fog.w);
    }
    return vec4<f32>(display, texel.w);
}

@fragment
fn fs_main(input: VertexOut) -> @location(0) vec4<f32> {
    let texel = textureSample(base_texture, terrain_sampler, input.uv)
        * textureSample(micro_texture, terrain_sampler, input.uv * 16.0) * 2.0;
    // The material's maps (sampled here, in uniform control flow).
    let mapped = textureSample(normal_texture, terrain_sampler, input.uv).xyz;
    let orm = textureSample(orm_texture, terrain_sampler, input.uv).xyz;
    let bent = ground_normal(input.normal, input.world.xyz, input.uv, mapped);
    if (experimental) {
        if (texel.w <= 0.0) {
            return vec4<f32>(0.0);
        }
        return shade_experimental(input, texel, bent, orm);
    }
    var color = vec4<f32>(0.0);
    if (texel.w > 0.0) {
        // The further lights as RTS adds them to models: the strongest wins.
        var light_colour = vec4<f32>(0.0);
        let normal = normalize(input.normal);
        let lights = ring_count(input.ring);
        for (var i = 0; i < lights; i = i + 1) {
            let light = frame.lights[input.ring * 9u + u32(i)];
            let factor = light_factor(light, input.world, normal);
            if (factor < 0.0) { continue; }
            light_colour = max(light_colour, light.diffuse * factor);
        }
        color = texel * max(input.colour, light_colour);
        if (input.fog.w > 0.0) {
            color = vec4<f32>(((1.0 - input.fog.w) * color + input.fog.w * input.fog).xyz, color.w);
        }
    }
    // A linear frame: RTS's display colour as linear light.
    if (frame.shading.z > 0.5) {
        color = vec4<f32>(pow(max(color.xyz, vec3<f32>(0.0)), vec3<f32>(2.2)), color.w);
    }
    return color;
}
