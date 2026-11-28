
struct Settings {
    curve: u32,     // 0 none, 1 Khronos PBR Neutral, 2 AgX, 3 ACES, 4 day Khronos / night AgX, 5 day ACES / night AgX
    exposure: f32,
    night: f32,
    night_exposure: f32,
    saturation: f32,
    bloom: f32,     // the bloom's strength (0: none)
    shafts: f32,    // 1: the light shafts added
    linear_scene: f32, // 1: the scene frame holds linear light, 0: gamma 2.2 (frame_encoding())
    auto_exposure: f32, // 1: the exposure times the auto exposure's factor
    hdr_scale: f32,  // HDR output: the paper white over 80 nits (scRGB out); 0: SDR
    hdr_peak: f32,   // the display's peak over the paper white
    pad2: f32,
};
@group(0) @binding(0) var scene: texture_2d<f32>;
@group(0) @binding(1) var<uniform> settings: Settings;
@group(0) @binding(2) var bloom_image: texture_2d<f32>; // scene-linear (BloomRenderer), or 1 x 1 black
@group(0) @binding(3) var bloom_sampler: sampler;
@group(0) @binding(4) var shafts_image: texture_2d<f32>; // scene-linear (LightShaftRenderer), or 1 x 1 black
@group(0) @binding(5) var<storage, read> exposure_state: vec4<f32>; // AutoExposure: adapted, factor, measured, valid

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> @builtin(position) vec4<f32> {
    let uv = vec2<f32>(f32((index << 1u) & 2u), f32(index & 2u));
    return vec4<f32>(uv * 2.0 - 1.0, 0.0, 1.0);
}

// AgX (Troy Sobotka), the 6th-order fit by Benjamin Wrensch: scene-linear
// Rec.709 in, display-encoded out.
fn agx_contrast(x: vec3<f32>) -> vec3<f32> {
    let x2 = x * x;
    let x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

fn agx(linear: vec3<f32>) -> vec3<f32> {
    let inset = mat3x3<f32>(vec3<f32>(0.842479062253094, 0.0423282422610123, 0.0423756549057051),
                            vec3<f32>(0.0784335999999992, 0.878468636469772, 0.0784336),
                            vec3<f32>(0.0792237451477643, 0.0791661274605434, 0.879142973793104));
    let outset = mat3x3<f32>(vec3<f32>(1.19687900512017, -0.0528968517574562, -0.0529716355144438),
                             vec3<f32>(-0.0980208811401368, 1.15190312990417, -0.0980434501171241),
                             vec3<f32>(-0.0990297440797205, -0.0989611768448433, 1.15107367264116));
    let min_ev = -12.47393;
    let max_ev = 4.026069;
    var v = inset * max(linear, vec3<f32>(1e-10));
    v = clamp(log2(v), vec3<f32>(min_ev), vec3<f32>(max_ev));
    v = (v - min_ev) / (max_ev - min_ev);
    var display = agx_contrast(v);
    // The "punchy" look (Wrensch): more contrast and saturation than base AgX,
    // which reads washed out on content painted for a plain display.
    display = pow(max(display, vec3<f32>(0.0)), vec3<f32>(1.35));
    let luma = dot(display, vec3<f32>(0.2126, 0.7152, 0.0722));
    // x 1.2, not Wrensch's 1.4: by day it read oversaturated (testers 2026-11-15).
    display = luma + 1.2 * (display - luma);
    return clamp(outset * display, vec3<f32>(0.0), vec3<f32>(1.0));
}

// ACES, Stephen Hill's fit (RRT + sRGB ODT): scene-linear in, display-linear out.
fn aces(c: vec3<f32>) -> vec3<f32> {
    let input = mat3x3<f32>(vec3<f32>(0.59719, 0.07600, 0.02840),
                            vec3<f32>(0.35458, 0.90834, 0.13383),
                            vec3<f32>(0.04823, 0.01566, 0.83777));
    let output = mat3x3<f32>(vec3<f32>(1.60475, -0.10208, -0.00327),
                             vec3<f32>(-0.53108, 1.10813, -0.07276),
                             vec3<f32>(-0.07367, -0.00605, 1.07602));
    let v = input * c;
    let a = v * (v + 0.0245786) - 0.000090537;
    let b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return output * (a / b);
}

// Khronos PBR Neutral: scene-linear in, display-linear out. Its toe is kept
// for the brightest channel, but the colour is scaled down to it rather than
// the offset taken off each channel: subtracting nearly all of the darkest
// channel multiplied any tint of a dark colour. MSTS's DXT1 greys are never
// quite grey (5:6:5 endpoints leave green a 6-bit step off), so a shadowed
// grey went green: its 2 % surplus 18 % at a 0.4 shade, twice at 0.15. Greys
// come out as Khronos has them, colours keep their hue.
fn khronos_neutral(c_in: vec3<f32>) -> vec3<f32> {
    let start_compression = 0.8 - 0.04;
    let desaturation = 0.15;
    var c = c_in;
    let x = min(c.r, min(c.g, c.b));
    let offset = select(0.04, x - 6.25 * x * x, x < 0.08);
    let top = max(c.r, max(c.g, c.b));
    if (top > 0.0) {
        c = c * ((top - offset) / top);
    }
    let peak = max(c.r, max(c.g, c.b));
    if (peak < start_compression) {
        return c;
    }
    let d = 1.0 - start_compression;
    let new_peak = 1.0 - d * d / (peak + d - start_compression);
    c = c * (new_peak / peak);
    let g = 1.0 - 1.0 / (desaturation * (peak - new_peak) + 1.0);
    return mix(c, vec3<f32>(new_peak), g);
}

// The display-encoded picture through the chosen curve, the bloom's and
// the light shafts' light (scene-linear, already weighed) added before the
// exposure.
fn curved(stored: vec3<f32>, bloom: vec3<f32>) -> vec3<f32> {
    let linear_frame = settings.linear_scene > 0.5;
    if (settings.curve == 0u) {
        // The scene as drawn: the RTS look (a linear frame encoded back).
        if (linear_frame) {
            return pow(clamp(stored, vec3<f32>(0.0), vec3<f32>(1.0)), vec3<f32>(1.0 / 2.2));
        }
        return clamp(stored, vec3<f32>(0.0), vec3<f32>(1.0));
    }
    let scene_linear = select(pow(max(stored, vec3<f32>(0.0)), vec3<f32>(2.2)), max(stored, vec3<f32>(0.0)), linear_frame) + bloom;
    // The auto exposure (PBR stage 3): the set exposure is its correction.
    let adapted = select(1.0, exposure_state.y, settings.auto_exposure > 0.5 && exposure_state.w > 0.5);
    let linear = scene_linear * settings.exposure * adapted;
    if (settings.curve == 4u || settings.curve == 5u) {
        // Khronos keeps the day as authored (ACES: contrastier, its shade
        // deeper); AgX, brighter, holds the night.
        var day_display = vec3<f32>(0.0);
        if (settings.curve == 5u) {
            day_display = aces(linear * 1.3);
        } else {
            day_display = khronos_neutral(linear);
        }
        let day = pow(clamp(day_display, vec3<f32>(0.0), vec3<f32>(1.0)), vec3<f32>(1.0 / 2.2));
        if (settings.night <= 0.0) {
            return day;
        }
        let night = agx(scene_linear * settings.night_exposure * adapted);
        return mix(day, night, settings.night);
    }
    if (settings.curve == 1u) {
        let display = khronos_neutral(linear);
        return pow(clamp(display, vec3<f32>(0.0), vec3<f32>(1.0)), vec3<f32>(1.0 / 2.2));
    }
    if (settings.curve == 3u) {
        // Hill's fit put a mid grey 12 % darker than Khronos: x 1.3 brings
        // it level (testers 2026-11-15: "ACES is dark by itself").
        let display = aces(linear * 1.3);
        return pow(clamp(display, vec3<f32>(0.0), vec3<f32>(1.0)), vec3<f32>(1.0 / 2.2));
    }
    return agx(linear);
}

// Khronos PBR Neutral as above with its ceiling at `top` (the paper white
// 1): the same picture up to 0.76, the highlights rolled off to the
// display's peak, not to the paper white.
fn khronos_neutral_hdr(c_in: vec3<f32>, top_out: f32) -> vec3<f32> {
    let start_compression = 0.8 - 0.04;
    let desaturation = 0.15;
    var c = c_in;
    let x = min(c.r, min(c.g, c.b));
    let offset = select(0.04, x - 6.25 * x * x, x < 0.08);
    let top = max(c.r, max(c.g, c.b));
    if (top > 0.0) {
        c = c * ((top - offset) / top);
    }
    let peak = max(c.r, max(c.g, c.b));
    if (peak < start_compression) {
        return c;
    }
    let d = top_out - start_compression;
    let new_peak = top_out - d * d / (peak + d - start_compression);
    c = c * (new_peak / peak);
    let g = 1.0 - 1.0 / (desaturation * (peak - new_peak) + 1.0);
    return mix(c, vec3<f32>(new_peak), g);
}

// The HDR output's picture: display-linear, 1 the paper white, up to
// hdr_peak. Khronos (the day of every curve but AgX) rolls off to the peak;
// AgX (and the night) as the SDR picture, its white at the paper white.
fn hdr_curved(stored: vec3<f32>, bloom: vec3<f32>) -> vec3<f32> {
    let linear_frame = settings.linear_scene > 0.5;
    if (settings.curve == 0u) {
        let shown = clamp(stored, vec3<f32>(0.0), vec3<f32>(1.0));
        return select(pow(shown, vec3<f32>(2.2)), shown, linear_frame);
    }
    let scene_linear = select(pow(max(stored, vec3<f32>(0.0)), vec3<f32>(2.2)), max(stored, vec3<f32>(0.0)), linear_frame) + bloom;
    let adapted = select(1.0, exposure_state.y, settings.auto_exposure > 0.5 && exposure_state.w > 0.5);
    let linear = scene_linear * settings.exposure * adapted;
    if (settings.curve == 2u) {
        return pow(agx(linear), vec3<f32>(2.2));
    }
    let day = khronos_neutral_hdr(linear, settings.hdr_peak);
    if ((settings.curve == 4u || settings.curve == 5u) && settings.night > 0.0) {
        let night = pow(agx(scene_linear * settings.night_exposure * adapted), vec3<f32>(2.2));
        return mix(day, night, settings.night);
    }
    return day;
}

@fragment
fn fs_main(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> {
    let encoded = textureLoad(scene, vec2<i32>(position.xy), 0).rgb;
    var bloom = vec3<f32>(0.0);
    if (settings.bloom > 0.0) {
        let size = vec2<f32>(textureDimensions(scene));
        bloom = textureSampleLevel(bloom_image, bloom_sampler, position.xy / size, 0.0).rgb * settings.bloom;
    }
    if (settings.shafts > 0.0) {
        let size = vec2<f32>(textureDimensions(scene));
        bloom = bloom + textureSampleLevel(shafts_image, bloom_sampler, position.xy / size, 0.0).rgb;
    }
    if (settings.hdr_scale > 0.0) {
        var light = hdr_curved(encoded, bloom);
        if (settings.saturation != 1.0) {
            let luma = dot(light, vec3<f32>(0.2126, 0.7152, 0.0722));
            light = max(vec3<f32>(luma) + (light - vec3<f32>(luma)) * settings.saturation, vec3<f32>(0.0));
        }
        return vec4<f32>(min(light, vec3<f32>(settings.hdr_peak)) * settings.hdr_scale, 1.0);
    }
    var display = curved(encoded, bloom);
    if (settings.saturation != 1.0) {
        // Saturation around the pixel's own luma, as a display adjusts it.
        let luma = dot(display, vec3<f32>(0.2126, 0.7152, 0.0722));
        display = clamp(vec3<f32>(luma) + (display - vec3<f32>(luma)) * settings.saturation, vec3<f32>(0.0), vec3<f32>(1.0));
    }
    return vec4<f32>(display, 1.0);
}
