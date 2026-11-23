
struct Params {
    size: vec2<u32>,   // the frame
    step: u32,         // a sample every step-th pixel each way
    linear: u32,       // 1: linear light, 0: gamma 2.2
    elapsed: f32,      // s since the last measure
    strength: f32,
    down: f32,
    up: f32,
    speed: f32,
    key: f32,
    reset: u32,
    pad: u32,
};
@group(0) @binding(0) var scene: texture_2d<f32>;
@group(0) @binding(1) var<uniform> params: Params;
@group(0) @binding(2) var<storage, read_write> histogram: array<atomic<u32>, 128>;
@group(0) @binding(3) var<storage, read_write> state: vec4<f32>; // adapted, factor, measured, valid

const bins = 128u;
const min_log = -14.0;
const max_log = 6.0;

var<workgroup> local: array<atomic<u32>, 128>;

// The frame's luminance as a histogram of its log2: bin 0 black, 1..127
// from 2^-14 to 2^6; the middle of the picture weighs three times its edges.
@compute @workgroup_size(16, 16)
fn build(@builtin(global_invocation_id) id: vec3<u32>, @builtin(local_invocation_index) index: u32) {
    if (index < bins) {
        atomicStore(&local[index], 0u);
    }
    workgroupBarrier();
    let p = id.xy * params.step + vec2<u32>(params.step / 2u);
    if (p.x < params.size.x && p.y < params.size.y) {
        var c = max(textureLoad(scene, vec2<i32>(p), 0).rgb, vec3<f32>(0.0));
        if (params.linear == 0u) {
            c = pow(c, vec3<f32>(2.2));
        }
        let l = dot(c, vec3<f32>(0.2126, 0.7152, 0.0722));
        var bin = 0u;
        if (l > exp2(min_log)) {
            bin = 1u + u32(clamp((log2(l) - min_log) / (max_log - min_log), 0.0, 0.9999) * f32(bins - 1u));
        }
        let uv = (vec2<f32>(p) + 0.5) / vec2<f32>(params.size) * 2.0 - 1.0;
        let weight = 1u + u32(round(2.0 * clamp(1.0 - dot(uv, uv) * 0.5, 0.0, 1.0)));
        atomicAdd(&local[bin], weight);
    }
    workgroupBarrier();
    if (index < bins) {
        let count = atomicLoad(&local[index]);
        if (count > 0u) {
            atomicAdd(&histogram[index], count);
        }
    }
}

var<workgroup> counts: array<u32, 128>;

// The mean log2 luminance without the darkest and the brightest tenth, the
// adapted level following it, and the exposure factor; the histogram
// cleared for the next frame.
@compute @workgroup_size(128)
fn adapt(@builtin(local_invocation_index) index: u32) {
    counts[index] = atomicLoad(&histogram[index]);
    atomicStore(&histogram[index], 0u);
    workgroupBarrier();
    if (index != 0u) {
        return;
    }
    var total = 0u;
    for (var i = 0u; i < bins; i++) {
        total += counts[i];
    }
    if (total == 0u) {
        return;
    }
    let low = f32(total) * 0.1;
    let high = f32(total) * 0.9;
    var below = 0.0;
    var sum = 0.0;
    var weight = 0.0;
    for (var i = 0u; i < bins; i++) {
        let n = f32(counts[i]);
        let from_here = max(below, low);
        let to_here = min(below + n, high);
        if (to_here > from_here) {
            let value = select(min_log, min_log + (f32(i) - 0.5) / f32(bins - 1u) * (max_log - min_log), i > 0u);
            sum += value * (to_here - from_here);
            weight += to_here - from_here;
        }
        below += n;
    }
    let measured = select(min_log, sum / weight, weight > 0.0);
    var s = state;
    if (params.reset != 0u || s.w < 0.5) {
        s.x = measured;
    } else {
        // Towards more light three times faster than towards the dark.
        let rate = select(1.0, 3.0, measured > s.x) * params.speed;
        s.x = s.x + (measured - s.x) * (1.0 - exp(-params.elapsed * rate));
    }
    s.y = exp2(clamp(params.strength * (params.key - s.x), -params.down, params.up));
    s.z = measured;
    s.w = 1.0;
    state = s;
}
