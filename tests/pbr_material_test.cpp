// PBR stage 1's surfaces from RTS's data (renderer/pbr.hpp): every light
// material a dielectric; LoShine and HiShine reflect fully, their highlight's
// peak at normal incidence (GGX, Smith, Schlick, as shaders/common/pbr.wgsl) near the
// Blinn-Phong one the experimental path had before (strength 0.25 and 0.6);
// Normal matte, its reflection at a grazing mirror angle (a lamp ahead over
// the ballast) under a tenth of the light; Circumference, DarkShade and the
// ground none.
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

#include <openrts/renderer/pbr.hpp>

namespace {

using namespace openrts::renderer;

int failures = 0;

void check(const bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

// The reflection's peak, light and eye along the normal, in the scene's
// light scale (pbr_lit: specular BRDF times pi): D(1) V(1, 1) F(1) pi, with
// D(1) = 1 / (pi alpha^2), V(1, 1) = 1/4, F(1) = specular F0.
[[nodiscard]] double peak(const SurfaceMaterial& m) {
    const double alpha = static_cast<double>(m.roughness) * m.roughness;
    return m.specular * m.reflectance * 0.25 / (alpha * alpha);
}

} // namespace

int main() {
    constexpr std::int32_t normal = -5, lo_shine = -6, hi_shine = -7, circumference = -9, dark_shade = -12;
    for (const std::int32_t material : {-1, normal, lo_shine, hi_shine, -8, circumference, -11, dark_shade, -16, 0}) {
        const auto m = rts_surface_material(material);
        check(m.metallic == 0.0f, "a dielectric: " + std::to_string(material));
        check(m.reflectance == 0.04f, "F0 4 %: " + std::to_string(material));
        check(m.roughness > 0.0f && m.roughness <= 1.0f && m.specular >= 0.0f && m.specular <= 1.0f, "in range: " + std::to_string(material));
    }
    const auto lo = rts_surface_material(lo_shine);
    const auto hi = rts_surface_material(hi_shine);
    const auto plain = rts_surface_material(normal);
    check(lo.specular == 1.0f && hi.specular == 1.0f, "LoShine and HiShine reflect fully");
    check(hi.roughness < lo.roughness && lo.roughness < plain.roughness, "HiShine smoother than LoShine, LoShine than Normal");
    const double hi_ratio = peak(hi) / 0.6;
    const double lo_ratio = peak(lo) / 0.25;
    std::cout << "highlight peaks against the Blinn-Phong ones: HiShine " << hi_ratio << ", LoShine " << lo_ratio
              << ", Normal " << peak(plain) / 0.04 << '\n';
    check(hi_ratio > 0.5 && hi_ratio < 2.0, "HiShine's peak near its Blinn-Phong one");
    check(lo_ratio > 0.5 && lo_ratio < 2.0, "LoShine's peak near its Blinn-Phong one");
    check(plain.specular > 0.0f && plain.specular < 1.0f && peak(plain) < 0.04, "Normal reflects, weaker than before");
    {
        // The light and the eye 11.5 and 8.6 degrees over a surface, mirrored
        // about its normal (a lamp ahead, the camera over the track): GGX,
        // the height-correlated Smith visibility, Schlick, times pi and the
        // specular factor, against the light as pbr_lit has it (n.l in it).
        const double n_dot_l = 0.2;
        const double n_dot_v = 0.15;
        const double alpha = static_cast<double>(plain.roughness) * plain.roughness;
        const double a2 = alpha * alpha;
        const double d = 1.0 / (3.14159265 * a2); // n.h 1
        const double gv = n_dot_l * std::sqrt(n_dot_v * n_dot_v * (1.0 - a2) + a2);
        const double gl = n_dot_v * std::sqrt(n_dot_l * n_dot_l * (1.0 - a2) + a2);
        const double visibility = 0.5 / (gv + gl);
        // v.h: the cosine of half the angle between the two ways.
        const double between = n_dot_l * n_dot_v - std::sqrt((1.0 - n_dot_l * n_dot_l) * (1.0 - n_dot_v * n_dot_v));
        const double v_dot_h = std::sqrt(0.5 * (1.0 + between));
        const double fresnel = plain.reflectance + (1.0 - plain.reflectance) * std::pow(1.0 - v_dot_h, 5.0);
        const double sheen = d * visibility * fresnel * 3.14159265 * plain.specular;
        std::cout << "Normal's reflection at a grazing mirror angle: " << sheen << " of the light\n";
        check(sheen < 0.1, "Normal's grazing reflection under a tenth of the light (about the ballast's own share of it)");
    }
    check(rts_surface_material(circumference).specular == 0.0f, "Circumference (normals made to point up): no reflection");
    check(rts_surface_material(dark_shade).specular == 0.0f, "DarkShade: no reflection");
    check(terrain_surface_material.specular == 0.0f && terrain_surface_material.metallic == 0.0f, "the ground: Lambert alone");

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "pbr material: ok\n";
    return 0;
}
