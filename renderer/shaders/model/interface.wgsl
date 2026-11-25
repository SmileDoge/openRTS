// Bindings and types shared by both shading paths.
struct Frame {
    lights: array<Light, 1>, // the sun
    rings: array<Light, 90>, // the further lights, nine a ring
    ring_counts: array<vec4<i32>, 3>,
    interior: array<Light, 64>,
    fog_colour: vec4<f32>,
    fog_distance: vec4<f32>,
    view_projection: mat4x4<f32>,
    eye: vec4<f32>,
    shading: vec4<f32>, // x: the experimental path's night ambient factor, y: FullBright lit, z: 1 the frame is linear, w: 1 PBR
    fog_sun: vec4<f32>,             // xy: the sun's way along the ground (0: no ring), zw: the eye's x, z
    fog_ring: array<vec4<f32>, 12>, // the horizon's colour at each fog_ring_angle (fog_ring.hpp)
    portals: array<mat4x4<f32>, 2>, // centre, (across, half), (up, half), eye
    portal_doors: array<vec4<f32>, 2>, // far_door, near_door, the opening's half sizes
    room: mat4x4<f32>,      // world -> the room the eye is in (ModelRoom)
    room_lower: vec4<f32>,  // its box; w 1: there is one
    room_upper: vec4<f32>,
    rain: vec4<f32>,        // ModelRainGlass: amount, time, speed, since
    rain_grid: vec4<f32>,   // the wipers' glass: x0, y0, x1, y1 (room)
    rain_front: vec4<f32>,  // x the front's sign, y 1 a grid
    rain_side: vec4<f32>,   // xy the side drops' run (m, z y), zw their way now
    snow_cover: vec4<f32>,  // x how much of the snow maps' snow lies (0..1)
    temporal: vec4<f32>,    // x 1: an alpha test's edge by noise; y the frame's number
    rain_wiped: array<vec4<f32>, 2048>, // each cell's last wipe (128 x 64, four a vec4)
};

// openRTS: raindrops on the cab's glass (ModelRainGlass). The blended
// surfaces at the room box's walls facing out; the drops in cells of 6 to 27 mm
// on the glass's plane, as many as the time since it was wiped and the
// rain give, each cell's drop drawn anew by the wipe's time; on the move
// they run up the front glass and back along the side windows, longer the
// faster, standing they slide down.
fn rain_hash(p: vec2<f32>, seed: f32) -> vec4<f32> {
    var q = vec4<f32>(dot(p, vec2<f32>(127.1, 311.7)), dot(p, vec2<f32>(269.5, 183.3)), dot(p, vec2<f32>(419.2, 371.9)), dot(p, vec2<f32>(91.7, 233.1)));
    q = q + seed * vec4<f32>(17.13, 31.71, 7.37, 13.97);
    return fract(sin(q) * 43758.5453);
}
// Snow on the glass: flakes that stick where they hit and stay (no flow),
// gathering with the time since wiped; on the move the front glass takes
// them faster, the side windows slower. Two sizes: fine flakes and clumps;
// a flake's edge ragged, its middle denser.
// How light the sky is (the sun's ambient and diffuse): the drops' and the
// film's highlights follow it, dark at night.
fn rain_sky() -> f32 {
    let sun = frame.lights[0];
    return clamp(dot(sun.ambient.xyz + sun.diffuse.xyz * 0.5, vec3<f32>(0.333)), 0.03, 1.0);
}
// A layer of flakes: cells of `size` (m) turned by `turn` (rad), a flake in
// some of them anywhere inside (not on a lattice: the layers are turned
// against each other and each flake set off up to 0.3 of its cell).
// On the move the flakes are pushed along the glass (`run`, m) and drawn
// out along the airflow (`along`, unit, by `stretch`).
fn snow_flake(uv: vec2<f32>, size: f32, turn: f32, seed: f32, density: f32, run: vec2<f32>, along: vec2<f32>, stretch: f32) -> f32 {
    let rot = mat2x2<f32>(cos(turn), sin(turn), -sin(turn), cos(turn));
    let q = rot * (uv - run) / size;
    let cell = floor(q);
    let h = rain_hash(cell, seed + 3.0);
    if (h.x > density) {
        return 0.0;
    }
    let c = cell + vec2<f32>(0.2, 0.2) + h.yz * 0.6;
    let way = rot * along;
    let off0 = q - c;
    let off = vec2<f32>(dot(off0, vec2<f32>(way.y, -way.x)), dot(off0, way) / stretch);
    // A wet lump, no star: its outline only a little uneven (two slow
    // waves round it, each flake its own), its edge soft (testers
    // 2026-11-23: five-pointed stars on the glass).
    let angle = atan2(off.y, off.x);
    let uneven = 0.16 * sin(angle * 2.0 + h.y * 6.2831853) * h.z + 0.1 * sin(angle * 3.0 + h.w * 6.2831853);
    let r = (0.07 + 0.13 * h.w) * (1.0 + uneven);
    let d = length(off);
    return (1.0 - smoothstep(r * 0.2, r, d)) * (0.45 + 0.45 * h.w);
}
// Smooth value noise (0..1) on cells of `size` (m): where the snow sticks
// more and where less, patches of some 10 cm.
fn snow_patches(uv: vec2<f32>, size: f32, seed: f32) -> f32 {
    let q = uv / size;
    let i = floor(q);
    let f = q - i;
    let s = f * f * (vec2<f32>(3.0) - 2.0 * f);
    let a = rain_hash(i, seed).x;
    let b = rain_hash(i + vec2<f32>(1.0, 0.0), seed).x;
    let c = rain_hash(i + vec2<f32>(0.0, 1.0), seed).x;
    let d = rain_hash(i + vec2<f32>(1.0, 1.0), seed).x;
    return mix(mix(a, b, s.x), mix(c, d, s.x), s.y);
}
fn snow_on_glass(uv: vec2<f32>, seed: f32, age: f32, side: bool, colour: vec4<f32>) -> vec4<f32> {
    let speed = frame.rain.z;
    // On the move the windscreen meets the falling snow head on: at speed it
    // gets plastered (testers 2026-11-23: it should stick up the glass when
    // it falls), the side windows less.
    let hit = select(1.0 + min(speed * 0.18, 6.0), 1.0 - 0.5 * clamp(speed / 15.0, 0.0, 1.0), side);
    let gathered = clamp(age * frame.rain.x * 0.03 * hit, 0.0, 0.97);
    if (gathered <= 0.0) {
        return colour;
    }
    // Uneven: patches denser and thinner (0.35 .. 1.45 of it).
    let patchy = 0.35 + 1.1 * (0.65 * snow_patches(uv, 0.11, seed + 41.0) + 0.35 * snow_patches(uv, 0.037, seed + 43.0));
    let density = clamp(gathered * patchy, 0.0, 0.97);
    // On the move pushed up the windscreen and back along the side windows
    // (frame.snow_cover.yzw: the run TrainView integrates while it moves;
    // standing it stays put), the clumps the most, drawn out in streaks.
    let moving = clamp((speed - 3.0) / 12.0, 0.0, 1.0);
    let run = select(vec2<f32>(0.0, frame.snow_cover.y), frame.snow_cover.zw, side);
    let along = select(vec2<f32>(0.0, 1.0), normalize(vec2<f32>(-1.0, -0.25)), side);
    let stretch = 1.0 + moving * min(speed * 0.04, 1.5);
    let fine = snow_flake(uv, 0.009, 0.45, seed, density, run * 0.6, along, stretch);
    let middle = snow_flake(uv + vec2<f32>(0.13, 0.29), 0.016, 1.7, seed + 7.0, density * 0.7, run * 0.8, along, stretch);
    let clump = snow_flake(uv + vec2<f32>(0.37, 0.71), 0.038, 2.9, seed + 11.0, density * 0.4, run, along, stretch);
    let cover = clamp(max(max(fine, middle * 1.1), clump * 1.25), 0.0, 1.0);
    if (cover <= 0.0) {
        return colour;
    }
    // Lit by the sky and by what light reaches the glass (the lamps, the
    // cab's light in the glass's own colour), no glow of its own: at night
    // dark unless a lamp is on it (testers 2026-11-23: it shone white).
    let light = select(0.85, 0.5, frame.shading.z > 0.5) * rain_sky();
    let white = vec3<f32>(light) + colour.xyz * 0.6;
    return vec4<f32>(mix(colour.xyz, white, cover * 0.9), max(colour.w, cover * 0.85));
}
// The last wipe at a point of the wipers' glass (else the rain's start):
// of the four nearest cells the newest wipe that holds at least half their
// weight there. Between a wiped cell and one not its edge is a smooth line
// half way (no squares of the grid), and a drop and its pixels ask alike:
// a drop is cut where that line crosses it.
// One cell's last wipe (else the rain's start).
fn rain_wiped_cell(c: vec2<i32>) -> f32 {
    let at = clamp(c, vec2<i32>(0), vec2<i32>(127, 63));
    let index = at.y * 128 + at.x;
    return max(frame.rain_wiped[index / 4][index % 4], frame.rain.w);
}
// A corner's wipe if the corners wiped at that time or later hold at least
// half the weight, else the rain's start.
fn rain_wipe_held(times: vec4<f32>, weights: vec4<f32>, time: f32) -> f32 {
    let held = dot(weights, select(vec4<f32>(0.0), vec4<f32>(1.0), times >= vec4<f32>(time)));
    return select(frame.rain.w, time, held >= 0.5);
}
// Without loops or local arrays: D3D12's shader compiler unrolled the loops
// over arrays (called five times a pixel) for minutes or for ever on some
// drivers (a Radeon RX 570 hung at "starting the graphics", v0.38.3).
fn rain_wipe_at(uv: vec2<f32>) -> f32 {
    let g = (uv - frame.rain_grid.xy) / max(frame.rain_grid.zw - frame.rain_grid.xy, vec2<f32>(0.01));
    if (any(g < vec2<f32>(0.0)) || any(g >= vec2<f32>(1.0))) {
        return frame.rain.w;
    }
    // The edge of what was wiped follows the cells (some 2 x 4 cm): read
    // a little off, by a smooth noise of some 3 cells, it is a wavy line,
    // not the grid's steps (testers 2026-11-23). The same for a drop and
    // its pixels.
    let wobble = vec2<f32>(snow_patches(uv, 0.07, 61.0), snow_patches(uv, 0.07, 67.0)) - vec2<f32>(0.5);
    let f = g * vec2<f32>(128.0, 64.0) - vec2<f32>(0.5) + wobble * 1.6;
    let base = floor(f);
    let t = f - base;
    let b = vec2<i32>(base);
    let times = vec4<f32>(rain_wiped_cell(b), rain_wiped_cell(b + vec2<i32>(1, 0)), rain_wiped_cell(b + vec2<i32>(0, 1)),
                          rain_wiped_cell(b + vec2<i32>(1, 1)));
    let weights = vec4<f32>((1.0 - t.x) * (1.0 - t.y), t.x * (1.0 - t.y), (1.0 - t.x) * t.y, t.x * t.y);
    return max(max(rain_wipe_held(times, weights, times.x), rain_wipe_held(times, weights, times.y)),
               max(rain_wipe_held(times, weights, times.z), rain_wipe_held(times, weights, times.w)));
}
fn rain_on_glass(world: vec3<f32>, n_world: vec3<f32>, colour: vec4<f32>, is_blended: bool, extent: f32) -> vec4<f32> {
    // A window's glass is large; a gauge's glass and its needle small.
    if (!is_blended || extent < 0.25 || frame.rain.x <= 0.0 || frame.room_lower.w < 0.5) {
        return colour;
    }
    let p = (frame.room * vec4<f32>(world, 1.0)).xyz;
    let lo = frame.room_lower.xyz;
    let hi = frame.room_upper.xyz;
    if (any(p < lo - vec3<f32>(0.3)) || any(p > hi + vec3<f32>(0.3))) {
        return colour;
    }
    let n = normalize((frame.room * vec4<f32>(n_world, 0.0)).xyz);
    let centre = (lo + hi) * 0.5;
    let rel = (p - centre) / max((hi - lo) * 0.5, vec3<f32>(0.01));
    if (abs(n.y) > 0.75) {
        return colour; // a roof light
    }
    var uv = vec2<f32>(p.x, p.y);
    var side = false;
    // A window: at the room's wall (within 0.4 m of the box's front or back,
    // 0.25 m of its side); deeper in, a gauge's glass stays dry.
    let wall = min(p - lo, hi - p);
    if (abs(n.z) >= abs(n.x)) {
        if (wall.z > 0.6) {
            return colour;
        }
    } else {
        if (wall.x > 0.25) {
            return colour;
        }
        uv = vec2<f32>(p.z, p.y);
        side = true;
    }
    // The time since wiped: the rain's, the wipers' glass by its point.
    let gridded = !side && frame.rain_front.y > 0.5 && sign(rel.z) == frame.rain_front.x;
    let wiped_here = select(frame.rain.w, rain_wipe_at(uv), gridded);
    let age = frame.rain.y - wiped_here;
    if (frame.rain_front.z > 0.5) {
        return snow_on_glass(uv, floor(wiped_here * 4.0), age, side, colour);
    }
    // On the move the windscreen meets more of them, the side windows fewer.
    let speed = frame.rain.z;
    let hit = select(1.0 + min(speed * 0.1, 3.0), 1.0 - 0.4 * clamp(speed / 15.0, 0.0, 1.0), side);
    // Gathering slower the more there are: in a rain standing most of the
    // glass is taken in half a minute, in a downpour in some fifteen seconds,
    // faster on the move.
    let gather = frame.rain.x * 0.06 * hit;
    if (frame.rain.y - frame.rain.w <= 0.0 || gather <= 0.0) {
        return colour;
    }
    // The flow: up (front) or back (side) on the move, down standing.
    let moving = clamp((speed - 3.0) / 12.0, 0.0, 1.0);
    // How far they have run (m, integrated by the viewer: a change of speed
    // moves them on from where they are, not the whole film at once).
    let flow = frame.rain_front.w;
    let stretch = 1.0 + moving * min(speed * 0.08, 3.0);
    // Four layers of sizes, from fine drops (one and a half to two and a
    // half millimetres) to a few of up to six, shifted and turned against each
    // other: no grid to be seen. The small ones the most, the big ones run
    // the most.
    var best = 0.0;
    var best_off = vec2<f32>(0.0);
    var best_up = vec2<f32>(0.0, 1.0);
    for (var layer = 0; layer < 4; layer = layer + 1) {
        let fl = f32(layer);
        let s = 0.009 * pow(1.6, fl);
        let turn = 0.6 + 1.1 * fl;
        let rot = mat2x2<f32>(cos(turn), sin(turn), -sin(turn), cos(turn));
        let shift = vec2<f32>(0.37, 0.11) * fl;
        // Each layer at its own pace: drops of a size run alike, not all.
        // The front glass's drops run along its up; the side windows' their
        // own way (back and down on the move).
        let run = select(vec2<f32>(0.0, flow), frame.rain_side.xy, side) * (0.35 + 0.3 * fl);
        let q = rot * (uv + shift - run) / s;
        let cell = floor(q);
        // The drops of a cell are drawn anew by the wipe at its middle (the
        // glass there wiped: new ones gather); a drop is there once the
        // time since gives enough of them.
        let middle = transpose(rot) * ((cell + vec2<f32>(0.5)) * s) - shift + run;
        let wiped_middle = select(frame.rain.w, rain_wipe_at(middle), gridded);
        let density = 0.9 * (1.0 - exp(-max(frame.rain.y - wiped_middle, 0.0) * gather));
        let h = rain_hash(cell, floor(wiped_middle * 4.0) + fl * 5.0);
        // The biggest layer the rarest (testers 2026-11-23: some drops
        // stood out too big).
        if (h.x > density * (0.62 - 0.13 * fl) * select(1.0, 0.35, layer == 3)) {
            continue;
        }
        // A blade that went over a part of it took that part only: the
        // rest stays, cut along the blade's way.
        if (wiped_here > wiped_middle + 0.01) {
            continue;
        }
        let c = cell + vec2<f32>(0.25, 0.25) + h.yz * 0.5;
        // Its size: 0.6 to all of its layer's largest, 2.5, 3.7, 4.9, 6.1 mm
        // across (in the cell's units). A drop on upright glass holds up to
        // some 6 mm before it runs; they were up to 26 mm (testers
        // 2026-11-23: some drops too big).
        let r = (0.6 + 0.4 * h.w) * (0.00125 + 0.0006 * fl) / s;
        // Drawn out along the flow (the glass's up, turned into the layer).
        let along = rot * select(vec2<f32>(0.0, 1.0), frame.rain_side.zw, side);
        let across = vec2<f32>(along.y, -along.x);
        let o = q - c;
        let o2 = vec2<f32>(dot(o, across), dot(o, along) / stretch) / r;
        let d = length(o2);
        // Its edge soft, the smallest the most (a pixel or two across).
        let soft = 0.3 - 0.06 * fl;
        let drop = 1.0 - smoothstep(1.0 - soft, 1.0, d);
        if (drop > best) {
            best = drop;
            best_off = o2;
            // The glass's up in the drop's frame: the lens's sky and ground.
            let up = rot * vec2<f32>(0.0, 1.0);
            best_up = normalize(vec2<f32>(dot(up, across), dot(up, along) * stretch) + vec2<f32>(0.0, 1e-4));
        }
    }
    // A thin film of water left between the drops: the view behind a little
    // hazy, unevenly (wiped: clear again).
    let film_amount = 0.22 * (1.0 - exp(-max(age, 0.0) * frame.rain.x * 0.03 * hit));
    let film_seed = floor(wiped_here * 4.0);
    let film_noise = rain_hash(floor(uv / 0.06), film_seed + 23.0).x * 0.5 + rain_hash(floor(uv / 0.023), film_seed + 29.0).y * 0.5;
    // Behind a blade a thin streak of water, gone in a second.
    let streak = select(0.0, 0.14 * exp(-max(age, 0.0) / 0.6), gridded && wiped_here > frame.rain.w);
    let film = film_amount * (0.55 + 0.45 * film_noise) + streak * (0.4 + 0.6 * film_noise);
    let film_light = select(0.7, 0.45, frame.shading.z > 0.5) * rain_sky();
    let base = vec4<f32>(mix(colour.xyz, vec3<f32>(film_light), film), max(colour.w, film));
    if (best <= 0.0) {
        return base;
    }
    // A drop is a lens: the world behind it seen small and upside down. Its
    // lower part shows the sky (the fog's grey, the horizon's colour), its
    // upper the ground, dark; at its rim almost no light comes through (a
    // dark ring), and the sky shines on its top as a spot of light.
    let lin = frame.shading.z > 0.5;
    var horizon = frame.fog_colour.xyz;
    if (lin) {
        horizon = pow(max(horizon, vec3<f32>(0.0)), vec3<f32>(2.2));
    }
    let bright = rain_sky();
    // Seen through it about as bright as what lies behind, the sky below
    // a little lighter, the ground above darker. The horizon's part as light
    // as the sky (at night the fog's grey is no light: the drops shone white
    // on dark glass, testers 2026-11-23): at night a drop shows what lies
    // behind it, lit by the lamps.
    // `base` is the glass as lit, the cab's lamps on it too: a drop is no
    // brighter than the light from outside lets it be (testers 2026-11-23:
    // with the cab's light on the drops went opaque white, like snow or
    // mud); the lamps give it a glint only.
    let outside = horizon * (0.25 + 0.75 * bright) + vec3<f32>(0.02);
    let through = min(base.xyz, outside * 1.3);
    let sky = mix(through, horizon * bright, 0.5) * 1.1;
    let ground = through * 0.5 + horizon * 0.08 * bright;
    let d = length(best_off);
    let height = dot(best_off, best_up); // -1 its bottom .. 1 its top
    var lens = mix(ground, sky, 1.0 - smoothstep(-0.55, 0.45, height));
    // The rim: darker towards the edge, a thin bright line of refracted sky
    // right at its bottom.
    let rim = smoothstep(0.5, 0.97, d);
    lens = lens * (1.0 - 0.6 * rim);
    lens = lens + sky * 0.35 * smoothstep(0.7, 0.92, d) * (1.0 - smoothstep(0.92, 1.0, d)) * (1.0 - smoothstep(-0.9, -0.3, height));
    // The highlight: the sky's light off its dome, up and to one side.
    let spot = best_off - best_up * 0.45 - vec2<f32>(best_up.y, -best_up.x) * 0.25;
    // The glint: the sky's, and the lamps' that light the glass.
    let lamp = max(dot(base.xyz - through, vec3<f32>(0.333)), 0.0);
    let glint = exp(-dot(spot, spot) * 40.0) * select(0.7, 0.5, lin) * min(bright + lamp * 2.0, 1.0);
    lens = lens + vec3<f32>(glint);
    return vec4<f32>(mix(base.xyz, lens, best * 0.92), max(base.w, best * 0.85));
}

// One primitive of the shape (static per model generation).
struct Draw {
    local: mat4x4<f32>,
    local_normal: mat4x4<f32>,
    light_material: i32,
    named_shader: i32,
    matrix_index: i32,
    lod_bias: f32,
    surface: vec4<f32>, // roughness, metallic, a dielectric's F0, specular factor (SurfaceMaterial)
    base_color: vec4<f32>, // openRTS's material: the base colour's factor (linear)
    emissive: vec4<f32>,   // rgb the emission (linear), w 1: an emissive map
    maps: vec4<f32>,       // x the occlusion map's strength, y the normal map's, z 1: an ORM map, w 1: a normal map
    height: vec4<f32>,     // x the height map's depth (m), y 1: a height map, z the map's level the surface lies at
    extent: vec4<f32>,     // x the primitive's second largest extent (m), y 1: a snow map
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

// Alpha test is a pipeline variant: RTS toggles it between primitives.
override alpha_test: bool = false;
// MSAA with alpha-to-coverage: the alpha test's edge becomes coverage.
override alpha_to_coverage: bool = false;
// HDR frame: the experimental path keeps its highlights for the tone curve.
override hdr_output: bool = false;
// Blended (glass): not in the camera's depth pre-pass, so the screen's
// ambient occlusion there belongs to what lies behind it.
override blended: bool = false;

// Alpha-to-coverage for an alpha test at `cutoff` (Golus, "Anti-aliased
// Alpha Test"): the edge spread over one pixel (`width`: fwidth of the
// alpha) and the alpha raised in the far mips (`mip`) so foliage and fences
// do not thin out with distance. Zero coverage drops the fragment.
// An alpha test's edge by noise (ModelRenderer::set_stochastic_alpha): the
// texel kept as often as its coverage says, the noise (interleaved gradient
// noise) moved every frame; the temporal AA or the upscaler averages it.
fn stochastic_keep(coverage: f32, pixel: vec2<f32>) -> bool {
    let p = pixel + vec2<f32>(5.588238 * frame.temporal.y);
    let noise = fract(52.9829189 * fract(dot(p, vec2<f32>(0.06711056, 0.00583715))));
    return coverage > noise;
}

fn coverage_alpha(alpha: f32, cutoff: f32, width: f32, mip: f32) -> f32 {
    let kept = alpha * (1.0 + max(mip, 0.0) * 0.25);
    return clamp((kept - cutoff) / max(width, 0.0001) + 0.5, 0.0, 1.0);
}

@group(0) @binding(0) var source_texture: texture_2d<f32>;
@group(0) @binding(1) var source_sampler: sampler;
// The model's primitives, by slot; each id says which one it draws.
@group(0) @binding(2) var<storage, read> draws: array<Draw>;
// openRTS's material maps (PBR stage 4): occlusion, roughness, metallic
// (ORM); the normal map (green up); the emission (gamma 2.2). Without one a
// 1 x 1 stand-in (white, flat, black), read only where the draw says so.
@group(0) @binding(3) var orm_texture: texture_2d<f32>;
@group(0) @binding(4) var normal_texture: texture_2d<f32>;
@group(0) @binding(5) var emissive_texture: texture_2d<f32>;
// Its height map (white the surface; without one a white stand-in).
@group(0) @binding(6) var height_texture: texture_2d<f32>;
// openRTS's snow map (the material's `snow`): the snow's colour, its alpha
// where it lies; laid over the texture as much as frame.snow_cover says.
@group(0) @binding(7) var snow_texture: texture_2d<f32>;
fn with_snow(texel: vec4<f32>, uv: vec2<f32>, uv_dx: vec2<f32>, uv_dy: vec2<f32>) -> vec4<f32> {
    let snow = textureSampleGrad(snow_texture, source_sampler, uv, uv_dx, uv_dy);
    let cover = select(0.0, frame.snow_cover.x, draw.extent.y > 0.5);
    // The cover grows from where the snow is thickest (its alpha highest).
    // Not on what shows through (glass, a fake shadow under a car: the
    // ВЛ60ПК's SDW plane turned a white veil over its body).
    let solid = smoothstep(0.85, 0.98, texel.w);
    let lies = clamp((snow.w - (1.0 - cover)) / max(cover, 0.001), 0.0, 1.0) * select(0.0, 1.0, cover > 0.0) * solid;
    return vec4<f32>(mix(texel.xyz, snow.xyz, lies), texel.w);
}
@group(1) @binding(0) var<uniform> frame: Frame;
@group(1) @binding(1) var<storage, read> instances: array<Instance>;
// Per draw call a contiguous run of (instance, primitive) entries, starting at
// the draw's first instance; a multi-draw call has a run per primitive.
@group(1) @binding(2) var<storage, read> instance_ids: array<vec2<u32>>;
@group(1) @binding(3) var<storage, read> locals: array<Local>;
// The primitive being drawn: set first thing in each entry point.
var<private> draw: Draw;

// A draw whose UVs stay within 0..1 on an axis does not repeat its image
// there: the sample is kept half a texel inside, so linear filtering does
// not take the other edge's texels (the Kuban backdrops' tree walls: their
// image's opaque bottom row showed as a thin line along their top, whose
// row is clear). draw.height.w: 1 U, 2 V.
fn keep_inside(uv: vec2<f32>) -> vec2<f32> {
    let axes = u32(draw.height.w + 0.5);
    if (axes == 0u) {
        return uv;
    }
    let half_texel = 0.5 / vec2<f32>(textureDimensions(source_texture));
    var kept = uv;
    if ((axes & 1u) != 0u) {
        kept.x = clamp(uv.x, half_texel.x, 1.0 - half_texel.x);
    }
    if ((axes & 2u) != 0u) {
        kept.y = clamp(uv.y, half_texel.y, 1.0 - half_texel.y);
    }
    return kept;
}

struct Placement {
    model: mat4x4<f32>,
    normal_matrix: mat4x4<f32>,
};

// World and normal matrix of this primitive for one instance:
// instance transform times the primitive's (possibly animated) local matrix.
fn placement(instance_index: u32) -> Placement {
    let instance = instances[instance_ids[instance_index].x];
    var local = draw.local;
    var local_normal = draw.local_normal;
    if (instance.locals >= 0 && draw.matrix_index >= 0) {
        let animated = locals[u32(instance.locals + draw.matrix_index)];
        local = animated.matrix;
        local_normal = animated.normal;
    }
    return Placement(instance.transform * local, instance.normal * local_normal);
}

struct VertexIn {
    @location(0) position: vec3<f32>,
    @location(1) normal: vec3<f32>,
    @location(2) uv: vec2<f32>,
    @location(3) colour1: vec4<f32>,
    @location(4) colour2: vec4<f32>,
    /*TANGENT_IN*/
    @builtin(instance_index) instance: u32,
};

struct VertexOut {
    @builtin(position) clip: vec4<f32>,
    @location(0) world: vec4<f32>,
    @location(1) normal: vec3<f32>,
    @location(2) uv: vec2<f32>,
    @location(3) colour: vec4<f32>,
    @location(4) fog: vec4<f32>,
    @location(5) @interpolate(flat) ring: u32, // the instance's ring of further lights
    @location(6) @interpolate(flat) draw_slot: u32, // the primitive (draws[])
    @location(7) @interpolate(flat) portal: i32,    // the instance's portal (set_portal)
    @location(8) tangent: vec4<f32>,                // a normal map's (xyz world, w the bitangent's sign); 0: none
};

// openRTS: does the line from the eye to `world` pass through portal `p`
// (a doorway, ModelPortal) before it gets there?
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

// How far beyond portal `p` `world` lies, into the next car.
fn portal_depth(p: mat4x4<f32>, world: vec3<f32>) -> f32 {
    return dot(world - p[0].xyz, normalize(cross(p[1].xyz, p[2].xyz)));
}

// Inside the doors' opening (`d`: far_door, near_door, its half sizes).
fn in_door_opening(p: mat4x4<f32>, d: vec4<f32>, world: vec3<f32>) -> bool {
    let at = world - p[0].xyz;
    return abs(dot(at, p[1].xyz)) <= d.z && abs(dot(at, p[2].xyz)) <= d.w;
}

// An instance's fragment kept by its portal code (ModelPortal): +k only
// through portal k - 1 (the next car's salon), -k all but through it from
// its far cut on and in the opening from its far door on (that car's
// outside: its insides and its end door), 8 + k only through it between its
// cuts, not in the opening short of its near door (his own car's outside:
// its half of the gangway, not its end door).
fn portal_keeps(code: i32, world: vec3<f32>) -> bool {
    if (code == 0) {
        return true;
    }
    if (code > 8) {
        let k = min(u32(code - 9), 1u);
        let p = frame.portals[k];
        let d = frame.portal_doors[k];
        let depth = portal_depth(p, world);
        // No higher than the doors' opening: the top of his car's end
        // (the ED9M's blue roof edge) stood over the gangway.
        return through_portal(p, world) && depth >= p[3].w && depth < p[0].w
               && !(depth < d.y && in_door_opening(p, d, world)) && dot(world - p[0].xyz, p[2].xyz) <= d.w;
    }
    if (code > 0) {
        return through_portal(frame.portals[min(u32(code - 1), 1u)], world);
    }
    let k = min(u32(-code - 1), 1u);
    let p = frame.portals[k];
    let d = frame.portal_doors[k];
    let depth = portal_depth(p, world);
    return !(through_portal(p, world) && (depth >= p[0].w || (depth >= d.x && in_door_opening(p, d, world))));
}

fn ring_count(ring: u32) -> i32 {
    return min(frame.ring_counts[ring / 4u][ring % 4u], 9);
}

// RTS fog: linear in the eye's depth (clip w: under reversed Z clip z is
// not a distance, and the fog had stopped), scaled by the fog colour alpha;
// its colour by direction under openRTS's sky (fog_ring_colour).
fn fog_for(depth: f32, world: vec3<f32>) -> vec4<f32> {
    let fog_start = frame.fog_distance.x;
    let fog_end = frame.fog_distance.y;
    if (fog_end <= 0.0) {
        return vec4<f32>(0.0);
    }
    let colour = fog_ring_colour(world, frame.fog_colour.xyz);
    if (fog_end > fog_start) {
        let amount = clamp(frame.fog_colour.w * ((depth - fog_start) / (fog_end - fog_start)), 0.0, 1.0);
        return vec4<f32>(colour, amount);
    }
    return vec4<f32>(colour, frame.fog_colour.w);
}
