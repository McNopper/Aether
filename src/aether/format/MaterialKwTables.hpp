#ifndef AETHER_FORMAT_MATERIALKWTABLES_HPP
#define AETHER_FORMAT_MATERIALKWTABLES_HPP

#include <algorithm>
#include <optional>
#include <string_view>
#include <toml++/toml.hpp>
#include <unordered_map>

#include "aether/types/MaterialDesc.hpp"

namespace aether::format {

// ── TOML value readers ─────────────────────────────────────────────────────

/// Read a TOML node as float (double → float narrowing), or nullopt.
[[nodiscard]] inline std::optional<float> asFloat(const toml::node& n) {
    if (const auto v = n.value<double>()) {
        return static_cast<float>(*v);
    }
    return std::nullopt;
}

// ── applyKw dispatch types ────────────────────────────────────────────────

/// The texture-slot struct field that holds a texture map reference.
using TextureSlot = TextureRef MaterialDesc::*;
using ScalarSetter = void (*)(MaterialDesc&, float);
using ColorSetter = void (*)(MaterialDesc&, const Vec3&);
using BoolSetter = void (*)(MaterialDesc&, const toml::node&);

/// Texture-slot target of a `map_X_colorspace` key: the slot whose colorSpace
/// is set, plus the warning label (the slot's `map_X` key, without the
/// `_colorspace` suffix).
struct ColorSpaceSetter {
    TextureSlot slot{};
    std::string_view label{};
};

// ── Keyword dispatch tables (consumed by MaterialLibrary.cpp applyKw) ─────
// File scope: hoisted from applyKw's function-local statics so the pairing
// invariant between the texture-path and colorspace tables can be enforced
// structurally by the MaterialLibrary.KwTablePairingInvariant test (which
// includes this header directly). Internal to the material-library parser —
// not public API.

/// Texture map paths (string values): `map_X` → the texture-slot field. Keys
/// use the **verbatim OpenPBR field name** (no translation, no abbreviation);
/// only the `geometry_*` normal/tangent *vector* inputs are realised as the
/// renderer's texture-map slots (normalise() in MaterialLibrary.cpp maps them
/// to `map_*` keys). Foreign-format names (UsdPreviewSurface / MaterialX /
/// Wavefront MTL) are intentionally NOT accepted — files are authored in
/// OpenPBR.
inline const std::unordered_map<std::string_view, TextureSlot> texturePathSetters = {
    {"map_base_color", &MaterialDesc::map_base_color},
    {"map_normal", &MaterialDesc::map_normal},
    {"map_orm", &MaterialDesc::map_orm},
    {"map_roughness", &MaterialDesc::map_roughness},
    {"map_metalness", &MaterialDesc::map_metalness},
    {"map_emission_color", &MaterialDesc::map_emission_color},
    {"map_coat_normal", &MaterialDesc::map_coat_normal},
    {"map_tangent", &MaterialDesc::map_tangent},
    {"map_coat_tangent", &MaterialDesc::map_coat_tangent},
    {"map_opacity", &MaterialDesc::map_opacity},
};

/// Texture map color spaces (ColorInterop interop IDs): `map_X_colorspace` →
/// the same slot + its warning label. Unknown tokens keep the slot's default
/// and warn (in applyKw) — silently treating a wrong color space as data
/// would render wrong colors.
inline const std::unordered_map<std::string_view, ColorSpaceSetter> colorspaceSetters = {
    {"map_base_color_colorspace", {&MaterialDesc::map_base_color, "map_base_color"}},
    {"map_normal_colorspace", {&MaterialDesc::map_normal, "map_normal"}},
    {"map_orm_colorspace", {&MaterialDesc::map_orm, "map_orm"}},
    {"map_roughness_colorspace", {&MaterialDesc::map_roughness, "map_roughness"}},
    {"map_metalness_colorspace", {&MaterialDesc::map_metalness, "map_metalness"}},
    {"map_emission_color_colorspace", {&MaterialDesc::map_emission_color, "map_emission_color"}},
    {"map_coat_normal_colorspace", {&MaterialDesc::map_coat_normal, "map_coat_normal"}},
    {"map_tangent_colorspace", {&MaterialDesc::map_tangent, "map_tangent"}},
    {"map_coat_tangent_colorspace", {&MaterialDesc::map_coat_tangent, "map_coat_tangent"}},
    {"map_opacity_colorspace", {&MaterialDesc::map_opacity, "map_opacity"}},
};

/// Colour keywords (3-element arrays). Aether records colors verbatim in the
/// declared input color space (MaterialDesc::inputColorSpace); the consumer
/// performs any conversion. Non-color data (subsurface_radius_scale,
/// transmission_scatter) is never color-converted.
inline const std::unordered_map<std::string_view, ColorSetter> colorSetters = {
    {"base_color", [](MaterialDesc& d, const Vec3& v) { d.base_color = v; }},
    {"specular_color", [](MaterialDesc& d, const Vec3& v) { d.specular_color = v; }},
    {"transmission_color", [](MaterialDesc& d, const Vec3& v) { d.transmission_color = v; }},
    {"transmission_scatter", [](MaterialDesc& d, const Vec3& v) { d.transmission_scatter = v; }},
    {"coat_color", [](MaterialDesc& d, const Vec3& v) { d.coat_color = v; }},
    {"fuzz_color", [](MaterialDesc& d, const Vec3& v) { d.fuzz_color = v; }},
    {"emission_color", [](MaterialDesc& d, const Vec3& v) { d.emission_color = v; }},
    {"subsurface_color", [](MaterialDesc& d, const Vec3& v) { d.subsurface_color = v; }},
    {"subsurface_radius_scale", [](MaterialDesc& d, const Vec3& v) { d.subsurface_radius_scale = v; }},
};

/// Boolean keywords (bool, or a numeric 0/1 — see applyKw).
inline const std::unordered_map<std::string_view, BoolSetter> boolSetters = {
    {"geometry_thin_walled",
     [](MaterialDesc& d, const toml::node& v) {
         if (const auto b = v.value<bool>()) {
             d.geometry_thin_walled = *b;
         } else if (const auto f = asFloat(v)) {
             d.geometry_thin_walled = (*f != 0.0F);
         }
     }},
    {"emission_as_light_source",
     [](MaterialDesc& d, const toml::node& v) {
         if (const auto b = v.value<bool>()) {
             d.emission_as_light_source = *b;
         } else if (const auto f = asFloat(v)) {
             d.emission_as_light_source = (*f != 0.0F);
         }
     }},
};

/// Scalar keywords (floats; per-field clamping lives in the setters).
inline const std::unordered_map<std::string_view, ScalarSetter> scalarSetters = {
    {"base_weight", [](MaterialDesc& d, float f) { d.base_weight = std::clamp(f, 0.0F, 1.0F); }},
    {"base_metalness", [](MaterialDesc& d, float f) { d.base_metalness = f; }},
    {"base_diffuse_roughness", [](MaterialDesc& d, float f) { d.base_diffuse_roughness = f; }},
    {"specular_weight", [](MaterialDesc& d, float f) { d.specular_weight = std::max(f, 0.0F); }},
    {"specular_roughness", [](MaterialDesc& d, float f) { d.specular_roughness = std::clamp(f, 0.0F, 1.0F); }},
    {"specular_roughness_anisotropy",
     [](MaterialDesc& d, float f) { d.specular_roughness_anisotropy = std::clamp(f, 0.0F, 1.0F); }},
    {"specular_ior", [](MaterialDesc& d, float f) { d.specular_ior = std::max(f, 1.0F); }},
    {"transmission_weight", [](MaterialDesc& d, float f) { d.transmission_weight = std::clamp(f, 0.0F, 1.0F); }},
    {"transmission_depth", [](MaterialDesc& d, float f) { d.transmission_depth = f; }},
    {"transmission_scatter_anisotropy",
     [](MaterialDesc& d, float f) { d.transmission_scatter_anisotropy = std::clamp(f, -1.0F, 1.0F); }},
    {"transmission_dispersion_scale",
     [](MaterialDesc& d, float f) { d.transmission_dispersion_scale = std::max(f, 0.0F); }},
    {"transmission_dispersion_abbe_number",
     [](MaterialDesc& d, float f) { d.transmission_dispersion_abbe_number = f; }},
    {"thin_film_weight", [](MaterialDesc& d, float f) { d.thin_film_weight = std::clamp(f, 0.0F, 1.0F); }},
    {"thin_film_thickness", [](MaterialDesc& d, float f) { d.thin_film_thickness = std::max(f, 0.0F); }},
    {"thin_film_ior", [](MaterialDesc& d, float f) { d.thin_film_ior = std::max(f, 1.0F); }},
    {"coat_weight", [](MaterialDesc& d, float f) { d.coat_weight = std::clamp(f, 0.0F, 1.0F); }},
    {"coat_roughness", [](MaterialDesc& d, float f) { d.coat_roughness = std::clamp(f, 0.0F, 1.0F); }},
    {"coat_roughness_anisotropy",
     [](MaterialDesc& d, float f) { d.coat_roughness_anisotropy = std::clamp(f, 0.0F, 1.0F); }},
    {"coat_ior", [](MaterialDesc& d, float f) { d.coat_ior = std::max(f, 1.0F); }},
    {"coat_darkening", [](MaterialDesc& d, float f) { d.coat_darkening = f; }},
    {"fuzz_weight", [](MaterialDesc& d, float f) { d.fuzz_weight = std::clamp(f, 0.0F, 1.0F); }},
    {"fuzz_roughness", [](MaterialDesc& d, float f) { d.fuzz_roughness = f; }},
    {"emission_luminance", [](MaterialDesc& d, float f) { d.emission_luminance = f; }},
    {"subsurface_weight", [](MaterialDesc& d, float f) { d.subsurface_weight = std::clamp(f, 0.0F, 1.0F); }},
    {"subsurface_radius", [](MaterialDesc& d, float f) { d.subsurface_radius = f; }},
    {"subsurface_scatter_anisotropy",
     [](MaterialDesc& d, float f) { d.subsurface_scatter_anisotropy = std::clamp(f, -1.0F, 1.0F); }},
    {"geometry_opacity", [](MaterialDesc& d, float f) { d.geometry_opacity = std::clamp(f, 0.0F, 1.0F); }},
};

} // namespace aether::format
#endif // AETHER_FORMAT_MATERIALKWTABLES_HPP
