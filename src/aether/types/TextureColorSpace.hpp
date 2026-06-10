#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace aether {

/// Source color space of a texture asset.
/// Tokens are ASWF Color Interop Forum interop IDs (scene-referred set):
/// https://github.com/AcademySoftwareFoundation/ColorInterop
///
/// Only the Rec.709/Rec.2020 related spaces are supported. Aether records the
/// declared color space; the renderer / Harmonia converts to its (always
/// linear) working color space: decode transfer function, then primaries.
/// Non-color maps (normal, ORM, roughness) use Data — no conversion applies.
enum class TextureColorSpace : uint8_t {
    Data = 0,        ///< "data"              — uninterpreted; no conversion (normal, ORM, …)
    SrgbRec709Scene, ///< "srgb_rec709_scene" — sRGB OETF, Rec.709 primaries
    LinRec709Scene,  ///< "lin_rec709_scene"  — linear, Rec.709 primaries
    LinRec2020Scene, ///< "lin_rec2020_scene" — linear, Rec.2020 primaries
};

/// Parse a ColorInterop interop ID. Returns nullopt for unsupported tokens —
/// callers decide how to report the error.
[[nodiscard]] inline std::optional<TextureColorSpace> parseTextureColorSpace(std::string_view name) noexcept {
    if (name == "data")
        return TextureColorSpace::Data;
    if (name == "srgb_rec709_scene")
        return TextureColorSpace::SrgbRec709Scene;
    if (name == "lin_rec709_scene")
        return TextureColorSpace::LinRec709Scene;
    if (name == "lin_rec2020_scene")
        return TextureColorSpace::LinRec2020Scene;
    return std::nullopt;
}

} // namespace aether
