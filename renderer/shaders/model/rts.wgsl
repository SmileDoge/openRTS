
fn transformed_normal(normal_matrix: mat4x4<f32>, input_normal: vec3<f32>) -> vec3<f32> {
    if (draw.light_material == -9) { // Circumference
        return vec3<f32>(0.0, 1.0, 0.0);
    }
    return (normal_matrix * vec4<f32>(input_normal, 1.0)).xyz;
}

// Port of RTS shape.vert.
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
    out.normal = vec3<f32>(0.0);

    let shader = draw.named_shader;
    let material = draw.light_material;
    // RTS draws HalfBright (-11) and a night shape at night (-16) as
    // NamedShader 3 (rts1.exe 0x4B042C): unlit, Ambient 1.
    let forced_unlit = material == -11 || material == -16;
    let unlit_shader = shader == 1 || shader == 3 || shader == 6 || forced_unlit;
    var luminance = vec4<f32>(0.0, 0.0, 0.0, 1.0);
    var ambient = vec4<f32>(0.0);

    if (!unlit_shader && material != -8) {
        var diffuse = vec4<f32>(0.0);
        if (material == -11) { // HalfBright
            ambient = input.colour1;
        } else if (material == -1) { // vertex colour emission
            luminance = input.colour2;
        }
        out.normal = transformed_normal(place.normal_matrix, input.normal);

        let sun = frame.lights[0];
        let sun_factor = clamp(-dot(out.normal, sun.direction) * 2.0, 0.0, 1.0);
        var light_ambient = sun.ambient;
        var light_diffuse = sun.diffuse;
        if (material == -12) { // DarkShade
            light_ambient = vec4<f32>(0.0, 0.0, 0.0, 1.0);
            light_diffuse = sun.ambient;
        }
        diffuse = max(light_diffuse * sun_factor, diffuse);
        ambient = max(light_ambient, ambient);
        ambient = max(diffuse, ambient);
    } else {
        if (ring_count(out.ring) > 0) {
            out.normal = transformed_normal(place.normal_matrix, input.normal);
        }
        if (unlit_shader) {
            ambient = vec4<f32>(1.0);
        } else {
            ambient = input.colour1;
        }
    }
    out.colour = max(ambient, luminance);
    out.fog = fog_for(out.clip.w, out.world.xyz);
    return out;
}

// Port of RTS shape.frag.
@fragment
fn fs_main(input: VertexOut) -> @location(0) vec4<f32> {
    if (!portal_keeps(input.portal, input.world.xyz)) {
        discard;
    }
    draw = draws[input.draw_slot];
    let uv_in = keep_inside(input.uv);
    let texel = with_snow(textureSampleBias(source_texture, source_sampler, uv_in, draw.lod_bias), uv_in,
                          dpdx(uv_in) * exp2(draw.lod_bias), dpdy(uv_in) * exp2(draw.lod_bias));
    let alpha_width = fwidth(texel.w);
    let texels = uv_in * vec2<f32>(textureDimensions(source_texture));
    let texels_dx = dpdx(texels);
    let texels_dy = dpdy(texels);
    let alpha_mip = 0.5 * log2(max(max(dot(texels_dx, texels_dx), dot(texels_dy, texels_dy)), 1e-8));
    let shader = draw.named_shader;
    // Forced to NamedShader 3 (HalfBright, a night shape at night): an alpha shader.
    let alpha_shader = shader == 3 || shader == 4 || shader == 6 || shader == 7 || draw.light_material == -11
                       || draw.light_material == -16;

    var color = vec4<f32>(0.0);
    if (texel.w > 0.0 || !alpha_shader) {
        var specular_factor = 0.1;
        if (draw.light_material == -6) { // LoShine
            specular_factor = 0.3;
        } else if (draw.light_material == -7) { // HiShine
            specular_factor = 0.8;
        } else if (draw.light_material == -12) { // DarkShade
            specular_factor = 0.04;
        }

        let normal = normalize(input.normal);
        var light_colour = vec4<f32>(0.0);
        var specular = vec4<f32>(0.0);
        let extra_lights = ring_count(input.ring);
        for (var i = 0; i < extra_lights; i = i + 1) {
            let light = frame.rings[input.ring * 9u + u32(i)];
            let factor = light_factor(light, input.world, normal);
            if (factor < 0.0) { continue; }
            light_colour = max(light_colour, light.diffuse * factor);
            specular = max(specular, light.diffuse * factor * specular_factor);
        }

        // The lamps of the cab or salon (RTS shape.frag, LightsInt).
        let interior_lights = min(i32(frame.fog_distance.z), 64);
        for (var i = 0; i < interior_lights; i = i + 1) {
            let factor = interior_factor(frame.interior[i], input.world, normal);
            if (factor < 0.0) { continue; }
            light_colour = max(light_colour, frame.interior[i].diffuse * factor);
            specular = max(specular, frame.interior[i].diffuse * factor * specular_factor);
        }

        specular.w = 0.0;
        color = texel * max(input.colour, light_colour) + specular;
        if (input.fog.w > 0.0) {
            color = vec4<f32>(((1.0 - input.fog.w) * color + input.fog.w * input.fog).xyz, color.w);
        }
    }
    // A linear frame (not the RTS path's own, frame_encoding()): RTS's
    // display colour as linear light.
    if (frame.shading.z > 0.5) {
        color = vec4<f32>(pow(max(color.xyz, vec3<f32>(0.0)), vec3<f32>(2.2)), color.w);
    }

    // openRTS: raindrops on the cab's glass.
    color = rain_on_glass(input.world.xyz, input.normal, color, blended, select(draw.extent.x, 0.0, draw.extent.z > 0.5));

    // Fixed-function alpha test of RTS: GL_GREATER 0.3; with MSAA its edge
    // as coverage.
    if (alpha_test) {
        if (alpha_to_coverage) {
            color.w = coverage_alpha(color.w, 0.3, alpha_width, alpha_mip);
            if (color.w <= 0.0) {
                discard;
            }
        } else if (frame.temporal.x > 0.5) {
            if (!stochastic_keep(coverage_alpha(color.w, 0.3, alpha_width, alpha_mip), input.clip.xy)) {
                discard;
            }
        } else if (!(color.w > 0.3)) {
            discard;
        }
    }
    return color;
}
