// Depth only (SceneEffects: the shadow cascades and the camera's pre-pass):
// the instance's placement as the shading paths have it, the view-projection
// of the pass by a dynamic offset; alpha-tested primitives cut out at 0.5.
// The camera's pass keeps the frame's portals as the main pass does
// (fs_portal: GTAO and the light clusters see the next car's salon through
// the doorway, not its end wall); the shadow passes draw every instance whole.
struct DepthFrame {
    view_projection: mat4x4<f32>,
    portals: array<mat4x4<f32>, 2>, // the camera's pass: as Frame.portals
    portal_doors: array<vec4<f32>, 2>,
};
struct Draw { // as the shading paths' (MaterialUniform)
    local: mat4x4<f32>,
    local_normal: mat4x4<f32>,
    light_material: i32,
    named_shader: i32,
    matrix_index: i32,
    lod_bias: f32,
    surface: vec4<f32>,
    base_color: vec4<f32>,
    emissive: vec4<f32>,
    maps: vec4<f32>,
    height: vec4<f32>,
    extent: vec4<f32>,
};
struct Instance {
    transform: mat4x4<f32>,
    normal: mat4x4<f32>,
    locals: i32,
    ring: i32,
    decal: i32,
    portal: i32,
};
struct Local {
    matrix: mat4x4<f32>,
    normal: mat4x4<f32>,
};
@group(0) @binding(0) var source_texture: texture_2d<f32>;
@group(0) @binding(1) var source_sampler: sampler;
@group(0) @binding(2) var<storage, read> draws: array<Draw>;
@group(1) @binding(0) var<uniform> depth_frame: DepthFrame;
@group(1) @binding(1) var<storage, read> instances: array<Instance>;
@group(1) @binding(2) var<storage, read> instance_ids: array<vec2<u32>>;
@group(1) @binding(3) var<storage, read> locals: array<Local>;

struct DepthOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) world: vec3<f32>,
    @location(2) @interpolate(flat) portal: i32,
};

fn through_portal(p: mat4x4<f32>, world: vec3<f32>) -> bool {
    let centre = p[0].xyz;
    let across = p[1].xyz;
    let up = p[2].xyz;
    let eye = p[3].xyz;
    let n = cross(across, up);
    let along = world - eye;
    let towards = dot(n, along);
    if (abs(towards) < 1e-6) {
        return false;
    }
    let t = dot(n, centre - eye) / towards;
    if (t <= 0.0 || t >= 1.0) {
        return false;
    }
    let at = eye + along * t - centre;
    return abs(dot(at, across)) <= p[1].w && abs(dot(at, up)) <= p[2].w;
}

fn portal_depth(p: mat4x4<f32>, world: vec3<f32>) -> f32 {
    return dot(world - p[0].xyz, normalize(cross(p[1].xyz, p[2].xyz)));
}

fn in_door_opening(p: mat4x4<f32>, d: vec4<f32>, world: vec3<f32>) -> bool {
    let at = world - p[0].xyz;
    return abs(dot(at, p[1].xyz)) <= d.z && abs(dot(at, p[2].xyz)) <= d.w;
}

fn portal_keeps(code: i32, world: vec3<f32>) -> bool {
    if (code == 0) {
        return true;
    }
    if (code > 8) {
        let k = min(u32(code - 9), 1u);
        let p = depth_frame.portals[k];
        let d = depth_frame.portal_doors[k];
        let depth = portal_depth(p, world);
        // No higher than the doors' opening: the top of his car's end
        // (the ED9M's blue roof edge) stood over the gangway.
        return through_portal(p, world) && depth >= p[3].w && depth < p[0].w
               && !(depth < d.y && in_door_opening(p, d, world)) && dot(world - p[0].xyz, p[2].xyz) <= d.w;
    }
    if (code > 0) {
        return through_portal(depth_frame.portals[min(u32(code - 1), 1u)], world);
    }
    let k = min(u32(-code - 1), 1u);
    let p = depth_frame.portals[k];
    let d = depth_frame.portal_doors[k];
    let depth = portal_depth(p, world);
    return !(through_portal(p, world) && (depth >= p[0].w || (depth >= d.x && in_door_opening(p, d, world))));
}

@vertex
fn vs_depth(@location(0) position: vec3<f32>, @location(2) uv: vec2<f32>, @builtin(instance_index) index: u32) -> DepthOut {
    let entry = instance_ids[index];
    let instance = instances[entry.x];
    let draw = draws[entry.y];
    var local = draw.local;
    if (instance.locals >= 0 && draw.matrix_index >= 0) {
        local = locals[u32(instance.locals + draw.matrix_index)].matrix;
    }
    var out: DepthOut;
    let world = instance.transform * local * vec4<f32>(position, 1.0);
    out.clip = depth_frame.view_projection * world;
    out.uv = uv;
    out.world = world.xyz;
    out.portal = instance.portal;
    return out;
}

@fragment
fn fs_cutout(input: DepthOut) {
    if (textureSample(source_texture, source_sampler, input.uv).a < 0.5) {
        discard;
    }
}

@fragment
fn fs_portal(input: DepthOut) {
    if (!portal_keeps(input.portal, input.world)) {
        discard;
    }
}

@fragment
fn fs_cutout_portal(input: DepthOut) {
    if (!portal_keeps(input.portal, input.world) || textureSample(source_texture, source_sampler, input.uv).a < 0.5) {
        discard;
    }
}
