#pragma once

#include <string>

#include "aether/types/Math.hpp"
#include "aether/types/TextureColorSpace.hpp"

namespace aether {

/// Declared input color space for a material's color-valued parameters and
/// color textures. Tokens are ColorInterop interop IDs; values are always
/// linear (only textures may additionally be sRGB-encoded):
///   colorspace = "lin_rec709_scene" | "lin_rec2020_scene"
/// Aether records this but does **not** convert; the consumer (Harmonia /
/// renderer) converts to its working color space on load.
///
/// One material = one color space: every color value and every color texture
/// of a material share the primaries declared here. Mixing gamuts within a
/// material is invalid (texture encodings like sRGB OETF are fine — only the
/// primaries must match). Data maps carry no primaries and are exempt.
enum class MaterialColorSpace : uint8_t {
    LinRec709 = 0, ///< "lin_rec709_scene"  — default, convert on load
    LinRec2020,    ///< "lin_rec2020_scene"
};

/// A texture map reference: file path (relative to the material-library directory) plus the
/// source color space (ColorInterop interop ID) the consumer must decode from.
struct TextureRef {
    std::string path;
    TextureColorSpace colorSpace = TextureColorSpace::SrgbRec709Scene;

    [[nodiscard]] bool empty() const noexcept { return path.empty(); }
};

/// Parsed OpenPBR Surface v1.1 material — pure CPU data, no GPU types.
///
/// Color-valued fields (base_color, specular_color, …) are stored in the space
/// declared by `inputColorSpace`; non-color data (subsurface_radius,
/// transmission_scatter) is never color-converted by the consumer.
///
/// Default values follow the OpenPBR Surface specification, matching a
/// diffuse-gray material when default-constructed.
struct MaterialDesc {
    // ── base ───────────────────────────────────────────────────────────────
    Vec3 base_color{0.8F, 0.8F, 0.8F};
    float base_weight = 1.0F;
    float base_metalness = 0.0F;
    float base_diffuse_roughness = 0.0F; ///< Oren-Nayar σ for the diffuse lobe

    // ── specular (defaults per OpenPBR Surface spec) ───────────────────────
    Vec3 specular_color{1.0F, 1.0F, 1.0F};
    float specular_weight = 1.0F;
    float specular_roughness = 0.3F;
    float specular_roughness_anisotropy = 0.0F; ///< 0 = isotropic, 1 = fully anisotropic
    float specular_ior = 1.5F;

    // ── transmission ───────────────────────────────────────────────────────
    float transmission_weight = 0.0F;
    Vec3 transmission_color{1.0F, 1.0F, 1.0F};
    float transmission_depth = 0.0F; ///< Beer-law depth (world units); 0 = no absorption
    Vec3 transmission_scatter{0.0F, 0.0F, 0.0F};
    float transmission_scatter_anisotropy = 0.0F;      ///< HG mean cosine g ∈ [-1,1]
    float transmission_dispersion_scale = 0.0F;        ///< chromatic dispersion strength; 0 = off
    float transmission_dispersion_abbe_number = 20.0F; ///< Abbe number Vd (lower = more dispersion)

    // ── coat (defaults per OpenPBR Surface spec) ───────────────────────────
    float coat_weight = 0.0F;
    Vec3 coat_color{1.0F, 1.0F, 1.0F};
    float coat_roughness = 0.3F;
    float coat_roughness_anisotropy = 0.0F;
    float coat_ior = 1.5F;
    float coat_darkening = 1.0F; ///< energy darkening at base/coat interface

    // ── fuzz / sheen ───────────────────────────────────────────────────────
    float fuzz_weight = 0.0F;
    Vec3 fuzz_color{1.0F, 1.0F, 1.0F};
    float fuzz_roughness = 0.5F;

    // ── thin film ──────────────────────────────────────────────────────────
    float thin_film_weight = 0.0F;
    float thin_film_thickness = 0.0F; ///< nm; 0–2000 typical range
    float thin_film_ior = 1.5F;

    // ── emission ───────────────────────────────────────────────────────────
    Vec3 emission_color{1.0F, 1.0F, 1.0F};
    float emission_luminance = 0.0F;
    bool emission_as_light_source = true; ///< false = visible emission only (skip NEE/light synthesis)

    // ── subsurface (defaults per OpenPBR Surface spec) ─────────────────────
    float subsurface_weight = 0.0F;
    Vec3 subsurface_color{0.8F, 0.8F, 0.8F};
    Vec3 subsurface_radius{1.0F, 0.5F, 0.25F};
    float subsurface_scale = 1.0F;
    float subsurface_scatter_anisotropy = 0.0F; ///< HG mean cosine g ∈ [-1,1]

    // ── geometry ───────────────────────────────────────────────────────────
    float opacity = 1.0F;
    bool thin_walled = false; ///< geometry_thin_walled: double-sided thin sheet

    // ── material model tag ─────────────────────────────────────────────────
    /// Material model this description follows. Only "openpbr" exists today;
    /// the tag is recorded so future material models can coexist in one file.
    std::string model{"openpbr"};

    // ── input color space of the color values and color textures ───────────
    /// Applies to all color-valued fields above and all color texture maps
    /// below — a single material never mixes color spaces.
    MaterialColorSpace inputColorSpace = MaterialColorSpace::LinRec709;

    // ── texture maps (path + source color space) ───────────────────────────
    // Slot order matches the renderer's bindless layout:
    //   [0] base_color [1] normal [2] ORM [3] emission
    //   [4] coat_normal [5] tangent [6] coat_tangent
    TextureRef map_base_color{"", TextureColorSpace::SrgbRec709Scene};
    TextureRef map_normal{"", TextureColorSpace::Data};
    TextureRef map_orm{"", TextureColorSpace::Data};
    TextureRef map_roughness{"", TextureColorSpace::Data};
    TextureRef map_metalness{"", TextureColorSpace::Data};
    TextureRef map_emission_color{"", TextureColorSpace::SrgbRec709Scene};
    TextureRef map_coat_normal{"", TextureColorSpace::Data};
    TextureRef map_tangent{"", TextureColorSpace::Data};
    TextureRef map_coat_tangent{"", TextureColorSpace::Data};
};

} // namespace aether
