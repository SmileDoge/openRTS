// openRTS's own shading. Lighting is computed in linear light (textures and
// the RTS sun colours are display-encoded, gamma 2.2) and encoded back with a
// soft shoulder instead of a hard clip:
//  - ambient from the sky: full from above, darker towards the ground;
//  - Lambert sun diffuse;
//  - Blinn-Phong sun highlight, strength and sharpness by the MSTS light
//    material (normal faint, LoShine, HiShine); in a linear frame with PBR
//    (frame_pbr(), stage 1) the sun and the lamps by GGX, Smith and Schlick
//    instead (shaders/common/pbr.wgsl), the surface from the light material
//    (renderer/pbr.hpp);
//  - MSTS light materials: FullBright and the unlit shaders take the texture
//    as is, HalfBright uses the vertex colour as ambient, DarkShade gets only
//    the dimmed ambient as its light, vertex-colour emission adds the vertex
//    colour, Circumference lights as if facing up.
// Fog is applied in display space, as the fog colour is given (RTS).
@vertex
fn vs_main(input: VertexIn) -> VertexOut {
    var out: VertexOut;
    out.draw_slot = instance_ids[input.instance].y;
    draw = draws[out.draw_slot];
    let place = placement(input.instance);
    out.world = place.model * vec4<f32>(input.position, 1.0);
    out.clip = frame.view_projection * out.world;
    // openRTS: a transfer lies in the terrain's own triangles' planes
    // (transfer_shape.cpp); the two transforms round apart by far less than
    // a ten-thousandth of the distance, so it is drawn that much nearer
    // (reversed Z: a larger depth) and does not flicker against the ground.
    if (instances[instance_ids[input.instance].x].decal != 0) {
        out.clip.z = out.clip.z * 1.0001;
    }
    out.uv = input.uv;
    out.ring = u32(clamp(instances[instance_ids[input.instance].x].ring, 0, 9));
    out.portal = instances[instance_ids[input.instance].x].portal;
    out.normal = (place.normal_matrix * vec4<f32>(input.normal, 0.0)).xyz;
    if (draw.light_material == -9) { // Circumference
        out.normal = vec3<f32>(0.0, 1.0, 0.0);
    }
    /*TANGENT_OUT*/
    // HalfBright: ambient from the first vertex colour; emission: the second.
    out.colour = vec4<f32>(0.0);
    if (draw.light_material == -11) {
        out.colour = input.colour1;
    } else if (draw.light_material == -1) {
        out.colour = input.colour2;
    }
    out.fog = fog_for(out.clip.w, out.world.xyz);
    return out;
}

fn to_linear(c: vec3<f32>) -> vec3<f32> {
    return pow(max(c, vec3<f32>(0.0)), vec3<f32>(2.2));
}

fn to_display(c: vec3<f32>) -> vec3<f32> {
    return pow(max(c, vec3<f32>(0.0)), vec3<f32>(1.0 / 2.2));
}

// One further light's linear contribution: physical falloff, or RTS's shape
// factor taken in display space.
fn lamp_light(light: Light, world: vec4<f32>, n: vec3<f32>, physical: bool) -> vec3<f32> {
    if (physical) {
        let strength = physical_light(light, world, n);
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

// A debug view of the material (effects.views.x): the value shown as it is
// on the screen, in either frame encoding.
fn debug_view(value: vec3<f32>, coverage: f32) -> vec4<f32> {
    let shown = clamp(value, vec3<f32>(0.0), vec3<f32>(1.0));
    return vec4<f32>(select(shown, pow(shown, vec3<f32>(2.2)), frame.shading.z > 0.5), coverage);
}

// Linear up to the knee, then an exponential roll-off towards 1.
fn shoulder(c: vec3<f32>) -> vec3<f32> {
    let knee = 0.6;
    let room = 1.0 - knee;
    let over = max(c - vec3<f32>(knee), vec3<f32>(0.0));
    let rolled = vec3<f32>(knee) + room * (vec3<f32>(1.0) - exp(-over / room));
    return select(c, rolled, c > vec3<f32>(knee));
}

// The height map's relief (parallax occlusion mapping, docs/pbr-materials.md):
// where the eye's ray meets the relief (white its top, black `depth` metres
// under it; the model's surface at the map's level `reference`, so what is
// brighter stands out of it), as texture coordinates. A step on
// the surface is mapped to one in the image by the screen's derivatives
// (world_dx / world_dy against uv_step_x / uv_step_y), so it needs no
// tangents and a mirrored image shifts the right way. More steps the more
// edgewise the surface is seen (8 square on, `steps_most` edgewise).
fn parallax_uv(uv: vec2<f32>, view: vec3<f32>, n: vec3<f32>, depth: f32, reference: f32, world_dx: vec3<f32>, world_dy: vec3<f32>,
               uv_step_x: vec2<f32>, uv_step_y: vec2<f32>, uv_dx: vec2<f32>, uv_dy: vec2<f32>, steps_most: f32) -> vec2<f32> {
    let facing = dot(view, n);
    if (facing <= 0.0 || depth <= 0.0) {
        return uv;
    }
    // The ray's way along the surface down to the full depth (m), kept short
    // where the surface is seen nearly edgewise.
    let along = -(view - n * facing) * (depth / max(facing, 0.25));
    // In the image: along = a * world_dx + b * world_dy (least squares).
    let xx = dot(world_dx, world_dx);
    let xy = dot(world_dx, world_dy);
    let yy = dot(world_dy, world_dy);
    let det = xx * yy - xy * xy;
    if (det <= 1e-20) {
        return uv;
    }
    let px = dot(along, world_dx);
    let py = dot(along, world_dy);
    let shift = uv_step_x * ((px * yy - py * xy) / det) + uv_step_y * ((py * xx - px * xy) / det);
    let count = i32(clamp(mix(steps_most, 8.0, facing), 8.0, 64.0));
    let layer_step = 1.0 / f32(count);
    let uv_step = shift * layer_step;
    // From the relief's top: above the surface by (1 - reference) of the
    // depth, the ray that far back towards the eye.
    var layer = 0.0;
    var at = uv - shift * (1.0 - clamp(reference, 0.0, 1.0));
    var below = 1.0 - textureSampleGrad(height_texture, source_sampler, at, uv_dx, uv_dy).x;
    var previous_at = at;
    var previous_gap = below;
    for (var i = 0; i < count; i = i + 1) {
        if (layer >= below) {
            break;
        }
        previous_at = at;
        previous_gap = below - layer;
        at = at + uv_step;
        layer = layer + layer_step;
        below = 1.0 - textureSampleGrad(height_texture, source_sampler, at, uv_dx, uv_dy).x;
    }
    // Between the last step above the relief and the first under it.
    let gap = below - layer;
    let weight = clamp(gap / min(gap - previous_gap, -1e-6), 0.0, 1.0);
    return mix(at, previous_at, weight);
}

@fragment
fn fs_main(input: VertexOut) -> @location(0) vec4<f32> {
    if (!portal_keeps(input.portal, input.world.xyz)) {
        discard;
    }
    draw = draws[input.draw_slot];
    let uv_in = keep_inside(input.uv);
    // The texture as RTS reads it (its LOD bias); the material's maps with
    // these gradients (in any branch).
    var sampled = textureSampleBias(source_texture, source_sampler, uv_in, draw.lod_bias);
    let uv_step_x = dpdx(uv_in);
    let uv_step_y = dpdy(uv_in);
    let uv_dx = uv_step_x * exp2(draw.lod_bias);
    let uv_dy = uv_step_y * exp2(draw.lod_bias);
    let world_dx = dpdx(input.world.xyz);
    let world_dy = dpdy(input.world.xyz);
    // The height map's relief (effects.views.y its reach, faded out over its
    // last quarter): every map read where the eye's ray meets it, the
    // texture's colour again with the unshifted gradients (the shifted
    // coordinates' own would jump at the relief's steps). Its alpha stays
    // the unshifted one: a cut-out (a body's windows) is the model's edge,
    // as the depth pre-pass has it. Not for blended surfaces (glass).
    var uv = uv_in;
    if (!blended && draw.height.y > 0.5 && effects.views.y > 0.0) {
        let to_eye = frame.eye.xyz - input.world.xyz;
        let distance = length(to_eye);
        let fade = 1.0 - smoothstep(effects.views.y * 0.75, effects.views.y, distance);
        if (fade > 0.0) {
            uv = parallax_uv(uv_in, to_eye / max(distance, 1e-4), normalize(input.normal), draw.height.x * fade, draw.height.z, world_dx,
                             world_dy, uv_step_x, uv_step_y, uv_dx, uv_dy, effects.views.z);
            sampled = vec4<f32>(textureSampleGrad(source_texture, source_sampler, uv, uv_dx, uv_dy).xyz, sampled.w);
        }
    }
    // The material's base colour factor (openRTS's material; 1 without one).
    let texel = with_snow(vec4<f32>(sampled.xyz, sampled.w * draw.base_color.w), uv, uv_dx, uv_dy);
    let alpha_width = fwidth(texel.w);
    let texels = uv_in * vec2<f32>(textureDimensions(source_texture));
    let texels_dx = dpdx(texels);
    let texels_dy = dpdy(texels);
    let alpha_mip = 0.5 * log2(max(max(dot(texels_dx, texels_dx), dot(texels_dy, texels_dy)), 1e-8));
    var coverage = texel.w;
    if (alpha_test) {
        if (alpha_to_coverage) {
            coverage = coverage_alpha(texel.w, 0.5, alpha_width, alpha_mip);
            if (coverage <= 0.0) {
                discard;
            }
        } else if (texel.w < 0.5) {
            discard;
        }
    }

    let shader = draw.named_shader;
    let material = draw.light_material;
    // FullBright is unlit unless the panel lights it (frame.shading.y, light_fullbright()).
    // A night texture (-16) is self-lit whatever the panel says.
    let unlit = shader == 1 || shader == 3 || shader == 6 || (material == -8 && frame.shading.y < 0.5) || material == -16;
    let base = to_linear(texel.xyz) * draw.base_color.xyz;
    var colour = base;
    if (effects.views.x > 0.5 && effects.views.x < 1.5) {
        // Debug view: the base colour (the texture times the factor).
        return debug_view(base, coverage);
    }
    if (!unlit) {
        let sun = frame.lights[0];
        let sun_colour = to_linear(sun.diffuse.xyz);
        let sky_colour = to_linear(sun.ambient.xyz);
        let view = normalize(frame.eye.xyz - input.world.xyz);
        // The normal as the shape gives it, never turned towards the camera:
        // MSTS trees and many buildings point their normals up on purpose
        // (Circumference), and turning them would darken everything above
        // the camera's height.
        var n = normalize(input.normal);
        // openRTS's material: its normal map (tangents from MikkTSpace, the
        // bitangent up the image) and its ORM map's occlusion, roughness and
        // metallic, times the numbers.
        var material_surface = draw.surface;
        var material_ao = 1.0;
        if (draw.maps.z > 0.5) {
            let orm = textureSampleGrad(orm_texture, source_sampler, uv, uv_dx, uv_dy).xyz;
            material_surface.x = material_surface.x * orm.y;
            material_surface.y = material_surface.y * orm.z;
            material_ao = mix(1.0, orm.x, draw.maps.x);
        }
        if (draw.maps.w > 0.5 && material != -9 && dot(input.tangent.xyz, input.tangent.xyz) > 1e-10) {
            let mapped = textureSampleGrad(normal_texture, source_sampler, uv, uv_dx, uv_dy).xy * 2.0 - 1.0;
            let xy = mapped * draw.maps.y;
            let z = sqrt(max(1.0 - dot(mapped, mapped), 0.0));
            let t = normalize(input.tangent.xyz - n * dot(n, input.tangent.xyz));
            let b = cross(n, t) * input.tangent.w;
            n = normalize(t * xy.x + b * xy.y + n * z);
        }
        if (effects.views.x > 1.5) {
            // Debug views: roughness, metallic, the normal, the occlusion.
            if (effects.views.x < 2.5) {
                return debug_view(vec3<f32>(material_surface.x), coverage);
            } else if (effects.views.x < 3.5) {
                return debug_view(vec3<f32>(material_surface.y), coverage);
            } else if (effects.views.x < 4.5) {
                return debug_view(n * 0.5 + 0.5, coverage);
            } else if (effects.views.x < 5.5) {
                return debug_view(vec3<f32>(min(select(effects_ao(input.clip), 1.0, blended), material_ao)), coverage);
            }
        }
        let to_sun = -normalize(sun.direction);
        let facing = max(dot(n, to_sun), 0.0);

        // Sky light. Environments give the sun an ambient colour as bright as
        // its direct one at noon (SummerClear: both ffffffff), which RTS
        // shows flat (max of the two). Here the stronger the direct sun, the
        // less of that ambient is taken, down to 35 %, so the sun's
        // direction always shows; at night (no direct sun) it stays as given.
        // Full from above, 55 % from below.
        let sun_strength = clamp(dot(sun_colour, vec3<f32>(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
        let hemisphere = mix(0.55, 1.0, n.y * 0.5 + 0.5);
        var ambient = sky_colour * (1.0 - 0.65 * sun_strength) * hemisphere * mix(1.0, frame.shading.x, 1.0 - sun_strength);
        // Physically based light (frame.shading.w, frame_pbr(); PBR stage 1):
        // the sun and the lamps by the BRDF (shaders/common/pbr.wgsl), the surface as
        // its light material makes it (draw.surface, SurfaceMaterial: some
        // reflect nothing); the special materials as below.
        let pbr = frame.shading.w > 0.5;
        let surface = pbr_surface(base, n, view, material_surface);
        let reflects = material_surface.w > 0.0;
        // The sky's own light (PBR stage 2, effects.sky_light.x: SkyLight):
        // the sky and the ground under it from the sky's probe, Lambert by
        // its SH9 (the ground's bounce in it), the reflection by the split
        // sum below; near the eye as open as the sky is round it. RTS's
        // environment keeps its say at night: its sky light where its sun is
        // gone (as the old path's at night, the night sky light's factor
        // with it), where that is more than the sky's own (the moon and the
        // night's glow openRTS's sky lacks), never added to it.
        let sky_light = pbr && effects.sky_light.x > 0.5;
        let openness = select(1.0, effects_sky_openness(input.world.xyz), sky_light);
        let night_light = sky_colour * frame.shading.x * (1.0 - sun_strength);
        if (sky_light) {
            ambient = max(effects_sky_light(n), night_light * hemisphere) * openness;
        }
        // The sun's shadow and the ambient occlusion (SceneEffects).
        let shadow = effects_shadow(input.world.xyz, n, input.clip);
        // Glass (blended) takes none: the occlusion at its pixel is of the
        // surface behind it (a sticker on a door's glass showed the seats'
        // shading through it).
        // The material's occlusion map with it, the darker of the two (not
        // both: the same corners darkened twice); indirect light only.
        let occlusion = min(select(effects_ao(input.clip), 1.0, blended), material_ao);
        ambient = ambient * occlusion;
        // Indoors (the cab or salon the eye is in): the sun come in through
        // the windows and the sky's light, thrown about the room
        // (SceneEffectSettings::indoor_light, effects.lamps.w); the shadows
        // alone left it far darker than outside. The occlusion's corners
        // kept. With the sky's own light, its light facing up, and none in a
        // tunnel.
        var in_room = false;
        if (frame.room_lower.w > 0.5) {
            let at = (frame.room * vec4<f32>(input.world.xyz, 1.0)).xyz;
            in_room = all(at >= frame.room_lower.xyz) && all(at <= frame.room_upper.xyz);
        }
        if (in_room && effects.lamps.w > 0.0) {
            let room_sky = select(sky_colour, effects_sky_up(), sky_light);
            ambient = ambient + effects.lamps.w * (sun_colour * max(to_sun.y, 0.0) * 0.25 + room_sky * 0.35) * sqrt(occlusion) * openness;
        }
        var diffuse = sun_colour * facing * shadow;
        // Light the ground throws back (SceneEffectSettings::ground_bounce):
        // the ground lit by the sun and the sky, onto surfaces turned down
        // (all of it) or sideways (half); the occlusion keeps gaps dark. The
        // sky's own light has it as its lower hemisphere.
        var bounce = effects.lamps.y * (sun_colour * max(to_sun.y, 0.0) + sky_colour * (1.0 - 0.65 * sun_strength))
            * (0.5 - 0.5 * n.y) * occlusion;
        if (sky_light) {
            bounce = vec3<f32>(0.0);
        }
        var strength = 0.04;
        var sharpness = 16.0;
        if (material == -6) { // LoShine
            strength = 0.25;
            sharpness = 32.0;
        } else if (material == -7) { // HiShine
            strength = 0.6;
            sharpness = 64.0;
        } else if (material == -11) { // HalfBright
            ambient = max(ambient, to_linear(input.colour.xyz));
        } else if (material == -12) { // DarkShade
            ambient = vec3<f32>(0.0);
            bounce = vec3<f32>(0.0);
            diffuse = sky_colour * facing;
            strength = 0.0;
        }
        let halfway = normalize(to_sun + view);
        let highlight = sun_colour * strength * pow(max(dot(n, halfway), 0.0), sharpness) * step(0.0, dot(n, to_sun)) * shadow;

        // Headlights, searchlights and the lamps of the cab or salon: the
        // RTS light shapes, added in linear light. A light's falloff is taken
        // in display space, as RTS's (its colour times the factor, then
        // linear): a bright middle fading out, not a flat disc.
        var lamps = vec3<f32>(0.0);
        var lamps_out = vec3<f32>(0.0); // PBR: what the lamps send to the eye
        let physical = frame.fog_distance.w > 0.5; // physical_light_falloff()
        if (effects.flags.x != 0u) {
            // Every lamp of the fragment's cluster (SceneEffects).
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
            // The RTS rings; the lamps throwing shadows found by their key.
            let extra_lights = ring_count(input.ring);
            for (var i = 0; i < extra_lights; i = i + 1) {
                let ring_light = frame.rings[input.ring * 9u + u32(i)];
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
        // Weaker by day (SceneEffectSettings::lamps_in_daylight): the
        // headlights and the scenery's lamps out in the sun.
        lamps = lamps * (1.0 - effects.lamps.x);
        lamps_out = lamps_out * (1.0 - effects.lamps.x);
        // The cab's and salon's lamps by day at least half as strong as at
        // night (effects.lamps.z): they shine indoors, out of the sun (until
        // 2026-11-15 faded with the others: with the setting at 0 a cab's
        // lights did nothing by day).
        var cab_lamps = vec3<f32>(0.0);
        var cab_out = vec3<f32>(0.0);
        let interior_lights = min(i32(frame.fog_distance.z), 64);
        for (var i = 0; i < interior_lights; i = i + 1) {
            let factor = interior_factor(frame.interior[i], input.world, n);
            if (factor > 0.0) {
                let lit = to_linear(frame.interior[i].diffuse.xyz * factor);
                cab_lamps = cab_lamps + lit;
                if (pbr) {
                    cab_out = cab_out + pbr_lit(surface, pbr_lamp_way(frame.interior[i], input.world), lit, reflects);
                }
            }
        }
        lamps = lamps + cab_lamps * (1.0 - effects.lamps.z);
        lamps_out = lamps_out + cab_out * (1.0 - effects.lamps.z);
        if (pbr) {
            // The sun's light (DarkShade's: the sky's, Lambert alone) by the
            // BRDF; the sky's light Lambert on the albedo, less what its
            // reflection takes. Indoors a surface reflects mostly the room:
            // the sky's reflection there a third (about the windows' share).
            var sky = PbrSkyReflection(vec3<f32>(0.0), vec3<f32>(0.0));
            if (sky_light && reflects) {
                sky = pbr_sky_reflection(surface, occlusion, night_light);
                sky.light = sky.light * openness * select(1.0, effects.sky_tune.z, in_room);
            }
            colour = surface.diffuse * (ambient + bounce) * (vec3<f32>(1.0) - sky.taken) + sky.light
                + pbr_lit(surface, to_sun, diffuse, reflects) + lamps_out;
        } else {
            colour = base * (ambient + diffuse + lamps + bounce) + highlight;
        }
        if (material == -1) { // vertex colour emission
            colour = max(colour, base * to_linear(input.colour.xyz));
        }
    }
    // openRTS's material: its emission, lit or not.
    if (any(draw.emissive.xyz > vec3<f32>(0.0))) {
        var glow = draw.emissive.xyz;
        if (draw.emissive.w > 0.5) {
            glow = glow * to_linear(textureSampleGrad(emissive_texture, source_sampler, uv, uv_dx, uv_dy).xyz);
        }
        colour = colour + glow;
    }
    if (effects.views.x > 5.5) {
        // Debug view: the emission.
        return debug_view(colour * select(0.0, 1.0, any(draw.emissive.xyz > vec3<f32>(0.0))), coverage);
    }

    if (effects.flags.z != 0u) {
        // Aerial perspective in linear light instead of RTS's fog.
        colour = effects_aerial(colour, input.world.xyz);
    }
    if (effects.flags.y == 2u) {
        // The occlusion itself (--effects show-ao), shown alike in either encoding.
        let ao = vec3<f32>(effects_ao(input.clip));
        return vec4<f32>(select(ao, pow(ao, vec3<f32>(2.2)), frame.shading.z > 0.5), coverage);
    }
    // The linear HDR frame (frame_encoding()): the light as computed, RTS's
    // fog (aerial perspective off) mixed in linear light.
    if (frame.shading.z > 0.5) {
        var light = max(colour, vec3<f32>(0.0));
        if (effects.flags.z == 0u && input.fog.w > 0.0) {
            light = mix(light, to_linear(input.fog.xyz), input.fog.w);
        }
        return rain_on_glass(input.world.xyz, input.normal, vec4<f32>(light, coverage), blended, draw.extent.x);
    }
    var display = to_display(select(shoulder(colour), max(colour, vec3<f32>(0.0)), hdr_output));
    if (effects.flags.z == 0u && input.fog.w > 0.0) {
        display = mix(display, input.fog.xyz, input.fog.w);
    }
    return rain_on_glass(input.world.xyz, input.normal, vec4<f32>(display, coverage), blended, draw.extent.x);
}
