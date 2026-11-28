// The HDR calibration's patterns (hdr_output.hpp, HdrCalibration), scRGB
// out (1 = 80 nits), over the whole frame.
// mode 1, the peak: on black a field at 10 000 nits (the display clips it
// at its own peak), a square in its middle at the peak set: once that is
// the display's, the square is gone.
// mode 2, the paper white: black, a row of 11 greys of the SDR picture
// (0 .. 1, sRGB) at the paper white, a white bar at the paper white over
// it and one at the peak under it (the headroom the highlights get).
struct Settings {
    mode: f32,
    peak: f32,  // nits
    paper: f32, // nits
    pad: f32,
    size: vec4<f32>, // xy the frame's size, pixels
};
@group(0) @binding(0) var<uniform> settings: Settings;

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> @builtin(position) vec4<f32> {
    let uv = vec2<f32>(f32((index << 1u) & 2u), f32(index & 2u));
    return vec4<f32>(uv * 2.0 - 1.0, 0.0, 1.0);
}

fn to_linear(c: f32) -> f32 {
    return select(pow((c + 0.055) / 1.055, 2.4), c / 12.92, c <= 0.04045);
}

@fragment
fn fs_main(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> {
    let size = max(settings.size.xy, vec2<f32>(1.0));
    let p = position.xy / size;
    if (settings.mode < 1.5) {
        // Black round a field at 10 000 nits (the display clips it at its
        // own peak), in the field's middle a square at the peak set.
        let x = abs(p.x - 0.5) * size.x / size.y;
        let y = abs(p.y - 0.42);
        var nits = 0.0;
        if (x < 0.22 && y < 0.22) { nits = 10000.0; }
        if (x < 0.07 && y < 0.07) { nits = settings.peak; }
        return vec4<f32>(vec3<f32>(nits / 80.0), 1.0);
    }
    var nits = 0.0;
    if (p.y > 0.3 && p.y < 0.42 && p.x > 0.1 && p.x < 0.9) {
        nits = settings.paper;
    } else if (p.y > 0.45 && p.y < 0.6 && p.x > 0.1 && p.x < 0.9) {
        let step = floor((p.x - 0.1) / 0.8 * 11.0) / 10.0;
        nits = to_linear(clamp(step, 0.0, 1.0)) * settings.paper;
    } else if (p.y > 0.63 && p.y < 0.75 && p.x > 0.1 && p.x < 0.9) {
        nits = settings.peak;
    }
    return vec4<f32>(vec3<f32>(nits / 80.0), 1.0);
}
