// Laid out as EffectsUniform in scene_effects.cpp.
struct Effects {
    cascades: array<mat4x4<f32>, 4>, // world -> shadow clip, per cascade
    splits: vec4<f32>,         // each cascade's far view depth
    shadow: vec4<f32>,         // x: cascades (0: no shadows), y: strength, z: softness (texels), w: map size
    eye: vec4<f32>,            // xyz: the camera, w: near plane
    forward: vec4<f32>,        // xyz: the camera's forward, w: far plane
    clusters: vec4<u32>,       // grid x, y, z; w: lights a cluster holds at most
    cluster_params: vec4<f32>, // x, y: tile size in pixels; z, w: slice = log(depth) * z + w
    screen: vec4<f32>,         // width, height, 1 / width, 1 / height
    flags: vec4<u32>,          // x: clustered lights, y: ambient occlusion, z: aerial perspective, w: lights
    sun: vec4<f32>,            // xyz: towards the sun
    aerial: vec4<f32>,         // x: extinction per metre at the eye, y: its fall-off with height (1/m), z: Mie g, w: sun glow
    haze: vec4<f32>,           // xyz: linear in-scattered colour (the horizon's), w: RTS's fog strength (its fog colour's alpha)
    sun_colour: vec4<f32>,     // linear
    texel: vec4<f32>,          // a shadow texel's size in metres, per cascade
    view: mat4x4<f32>,         // world -> view
    inv_projection: mat4x4<f32>,
    ao: vec4<f32>,             // x: radius (m), y: strength (power), z: tan(vertical fov / 2), w: aspect
    horizon: vec4<f32>,        // xyz: RTS's fog colour (linear), w: where its fog ends (m)
    focus: mat4x4<f32>,        // world -> the focus map's clip (the cab)
    focus_params: vec4<f32>,   // x: its layer + 1 (0: none), y: its texel (m), z: margin (clip units), w: least filter radius (m)
    lamps: vec4<f32>,          // x: how much daylight dims the lamps (0: not at all), y: the ground's reflectance for the bounce, z: x for the cab's and salon's lamps
    lamp_shadows: array<mat4x4<f32>, 8>, // world -> each shadowed lamp's clip
    lamp_shadow_ids: array<vec4<u32>, 2>, // each one's index in scene_lights + 1 (lamp s: [s / 4][s % 4])
    lamp_shadow_texel: array<vec4<f32>, 2>, // a texel a metre from the lamp (parallel: in metres)
    lamp_shadow_params: vec4<f32>,       // x: lamps, y: map size, z: filter radius (texels)
    lamp_shadow_keys: array<vec4<f32>, 8>, // each one's to_light translation (the lamp in the RTS rings)
    lamp_focus: vec4<f32>,               // in flight: the sphere they cover (w: radius; 0 none), they fade out at its edge
    fog_sun: vec4<f32>,                  // xy: the sun's way along the ground (0: no ring, fog_ring.hpp)
    fog_ring: array<vec4<f32>, 12>,      // the horizon's colour at each fog_ring_angle (linear)
    cloud_area: vec4<f32>,               // the clouds' shadow map: xy its middle from the eye, z its half width, w the layer's middle above the eye (m)
    cloud_sun: vec4<f32>,                // xyz: towards the sun as the clouds are marched, w: strength (0: none)
    cloud_edge: vec4<f32>,               // xy: the light read taken through smoothstep(x, y) (the edge band); z: the light where the map's shadow is not (a low sun's, CloudShadow::low_sun)
    sky_light: vec4<f32>,                // the sky's light (SkyLight): x 1 on, y the prefiltered cube's last mip, z the sky's openness round the eye
    sky_tune: vec4<f32>,                 // its tuning: x the diffuse part, y the reflections, z the reflections indoors (factors)
    views: vec4<f32>,                    // x: the material debug view (0 none, 1 base colour .. 6 emission), y: the relief's reach (m, 0 off), z: its steps at most
};
