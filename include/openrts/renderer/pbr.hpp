#pragma once

// openRTS's physically based light, stage 1 (docs/experimental-effects.md,
// "Physically based light"): what a surface is for the light. Until openRTS's
// own materials (stage 4) say more, it comes from RTS's data alone: the
// primitive's light material. Every surface is a dielectric (nothing in RTS's
// data says metal).

#include <cstdint>

namespace openrts::renderer {

// The parameters are glTF's (core metallic-roughness, KHR_materials_specular's
// specularFactor), so a material reads and exports as glTF would have it.
struct SurfaceMaterial {
    float roughness = 0.7f;    // perceptual roughness, 0 a mirror .. 1 matte
    float metallic = 0.0f;     // 0 a dielectric .. 1 a metal
    float reflectance = 0.04f; // a dielectric's F0 (4 %: IOR 1.5, paint, wood, stone, glass)
    // How much of a dielectric's reflection there is (KHR_materials_specular:
    // the Fresnel term times it, at grazing angles too; what is not reflected
    // is Lambert). 0: Lambert alone.
    float specular = 1.0f;
};

// RTS's light materials (the shape's vertex light material). The light
// material is what the content's author chose the shine by, so:
//  - LoShine (-6) and HiShine (-7): a full dielectric reflection, rough and
//    smooth, matched to the highlights the content was made for (the
//    experimental path's Blinn-Phong until stage 1, its peak alike);
//  - Normal (-5) and anything else: matte, fully rough with a tenth of the
//    reflection. Normal stands as much for gravel, sleepers, earth and brick
//    (their stones and pits drawn in the texture of a flat face, which
//    would show a smooth plane's sheen at grazing angles) as for paint. At
//    first a quarter at roughness 0.7: the ballast went silver towards a
//    low sun, and at night the sleepers under a lamp ahead looked glossy,
//    flat, their texture lost (the sheen near the lamp's own light against
//    the ballast's tenth of it). Now a grazing reflection is about 0.05 of
//    the light, the old faint highlight's;
//  - Circumference (-9: trees, bushes, grass tufts, their normals made to
//    point up, so a reflection would be of a surface that is not there) and
//    DarkShade (-12, lit by the sky alone): none.
// The unlit and special ones' light the shaders keep as it is.
[[nodiscard]] constexpr SurfaceMaterial rts_surface_material(const std::int32_t light_material) noexcept {
    if (light_material == -6) return {.roughness = 0.5f}; // LoShine
    if (light_material == -7) return {.roughness = 0.35f}; // HiShine
    if (light_material == -9 || light_material == -12) return {.specular = 0.0f};
    return {.roughness = 1.0f, .specular = 0.1f};
}

// The ground (RTS terrain): its textures show grass, earth and gravel over the
// coarse ground mesh, as Normal's (above) but nearly always; Lambert alone
// until a route's materials say more.
inline constexpr SurfaceMaterial terrain_surface_material{.roughness = 1.0f, .specular = 0.0f};

} // namespace openrts::renderer
