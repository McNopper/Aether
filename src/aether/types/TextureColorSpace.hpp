#pragma once

#include <cstdint>
#include <string_view>

namespace aether {

/// Source color space of a texture asset.
/// Names follow the OCIO / OpenEXR IIF color space registry.
///
/// Aether only records the declared color space; the renderer / Harmonia is
/// responsible for performing any conversion to its working color space.
/// Data maps (normal, ORM, roughness) use Raw — no color conversion applies.
enum class TextureColorSpace : uint8_t {
    Raw = 0,     ///< "raw"          — uninterpreted data; no conversion (normal, ORM, roughness)
    SrgbTexture, ///< "srgb_texture" — sRGB OETF + Rec.709 primaries
    LinSrgb,     ///< "lin_srgb"     — linear Rec.709 primaries
    LinRec2020,  ///< "lin_rec2020"  — linear Rec.2020 (render color space)
    AcesCg,      ///< "acescg"       — ACEScg / lin_ap1
};

/// Parse an OCIO/OpenEXR color space name string.
/// Returns Raw for unrecognised strings (safe fallback for data maps).
[[nodiscard]] inline TextureColorSpace parseTextureColorSpace(std::string_view name) noexcept {
    if (name == "srgb_texture")
        return TextureColorSpace::SrgbTexture;
    if (name == "lin_srgb")
        return TextureColorSpace::LinSrgb;
    if (name == "lin_rec2020")
        return TextureColorSpace::LinRec2020;
    if (name == "acescg" || name == "lin_ap1")
        return TextureColorSpace::AcesCg;
    return TextureColorSpace::Raw;
}

} // namespace aether
