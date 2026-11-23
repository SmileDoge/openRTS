
// The fog's colour towards `world` (openRTS's sky: the horizon behind it,
// by its angle from the sun's way); `fallback`, RTS's one colour, while the
// ring is off.
fn fog_ring_colour(world: vec3<f32>, fallback: vec3<f32>) -> vec3<f32> {
    let way = frame.fog_sun.xy;
    let flat = world.xz - frame.fog_sun.zw;
    let size = length(flat);
    if (dot(way, way) < 0.5 || size < 1e-3) {
        return fallback;
    }
    let s = sqrt(acos(clamp(dot(flat / size, way), -1.0, 1.0)) / 3.14159265) * 11.0;
    let i = min(u32(s), 10u);
    return mix(frame.fog_ring[i].xyz, frame.fog_ring[i + 1u].xyz, clamp(s - f32(i), 0.0, 1.0));
}
