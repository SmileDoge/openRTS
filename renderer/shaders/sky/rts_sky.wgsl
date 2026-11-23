// Ports of RTS sky.vert/frag and satellite.vert/frag.
struct Draw {
    mvp: mat4x4<f32>,
    shift: vec4<f32>,
    colour: vec4<f32>,
    azimuth: vec4<f32>,
    half_size: f32,
    named_shader: i32,
    draw_flags: i32,
    padding: f32,
};

struct Frame {
    light_diffuse: vec4<f32>,
    light_ambient: vec4<f32>,
    fog_colour: vec4<f32>,
    fog_distance: vec4<f32>,
};

@group(0) @binding(0) var source_texture: texture_2d<f32>;
@group(0) @binding(1) var source_sampler: sampler;
@group(1) @binding(0) var<uniform> draw: Draw;
@group(1) @binding(1) var<uniform> frame: Frame;

struct VertexIn {
    @location(0) position: vec3<f32>,
    @location(1) uv: vec2<f32>,
};

struct VertexOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) colour: vec4<f32>,
    @location(2) fog: vec4<f32>,
};

fn fog_for(distance: f32) -> vec4<f32> {
    let fog_start = frame.fog_distance.x;
    let fog_end = frame.fog_distance.y;
    if (fog_end <= 0.0) {
        return vec4<f32>(0.0);
    }
    if (fog_end > fog_start) {
        let amount = clamp(frame.fog_colour.w * ((distance - fog_start) / (fog_end - fog_start)), 0.0, 1.0);
        return vec4<f32>(frame.fog_colour.xyz, amount);
    }
    return frame.fog_colour;
}

fn unlit(shader: i32) -> bool {
    return shader == 1 || shader == 3 || shader == 6;
}

@vertex
fn vs_sky(input: VertexIn) -> VertexOut {
    var out: VertexOut;
    out.clip = draw.mvp * vec4<f32>(input.position, 1.0);
    out.uv = input.uv + draw.shift.xy;
    if (!unlit(draw.named_shader)) {
        let diffuse = max(frame.light_diffuse, vec4<f32>(0.0));
        let ambient = max(frame.light_ambient, vec4<f32>(0.0, 0.0, 0.0, 1.0));
        out.colour = max(diffuse, ambient);
    } else {
        out.colour = vec4<f32>(1.0);
    }
    out.colour.w = draw.shift.z;
    out.fog = fog_for(length(input.position));
    return out;
}

@vertex
fn vs_satellite(input: VertexIn) -> VertexOut {
    var out: VertexOut;
    let vertex = vec3<f32>(input.position.xy * draw.half_size, input.position.z);
    let angles = vec4<f32>(cos(-draw.azimuth.x), sin(-draw.azimuth.x), cos(draw.azimuth.y), sin(draw.azimuth.y));
    var scale = 1.0;
    out.fog = vec4<f32>(0.0);

    // The sun grows towards the screen edges and is fogged.
    if ((draw.draw_flags & 1) != 0) {
        let center = vec3<f32>(vertex.z * angles.y * angles.z, vertex.z * angles.w, vertex.z * angles.x * angles.z);
        let length_on_screen = length((draw.mvp * vec4<f32>(center, 1.0)).xy);
        if (length_on_screen > 400.0) {
            scale = min(scale + (length_on_screen - 400.0) * 0.0008, 2.7);
        }
        out.fog = fog_for(length(vertex));
    }

    let position = vec3<f32>(
        (vertex.x * scale * angles.x + vertex.z * angles.y) * angles.z,
        vertex.y * scale * angles.z + vertex.z * angles.w,
        (-vertex.x * scale * angles.y + vertex.z * angles.x) * angles.z);
    out.clip = draw.mvp * vec4<f32>(position, 1.0);
    // openRTS's own sky drawn: the satellite on the far plane (reversed Z),
    // where only the sky is (its pipeline tests the depth for equality).
    if ((draw.draw_flags & 2) != 0) {
        out.clip.z = 0.0;
    }
    out.uv = input.uv;
    if (!unlit(draw.named_shader)) {
        out.colour = draw.colour;
    } else {
        out.colour = vec4<f32>(1.0);
    }
    return out;
}

@fragment
fn fs_main(input: VertexOut) -> @location(0) vec4<f32> {
    var color = textureSample(source_texture, source_sampler, input.uv) * input.colour;
    if (input.fog.w > 0.0) {
        color = vec4<f32>(((1.0 - input.fog.w) * color + input.fog.w * input.fog).xyz, color.w);
    }
    // Drawn into a linear frame (draw.padding 1, frame_encoding()): RTS's
    // display colour as linear light.
    if (draw.padding > 0.5) {
        color = vec4<f32>(pow(max(color.xyz, vec3<f32>(0.0)), vec3<f32>(2.2)), color.w);
    }
    return color;
}
