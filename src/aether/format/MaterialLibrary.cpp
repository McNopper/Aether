#include "aether/format/MaterialLibrary.hpp"

#include <algorithm>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <toml++/toml.hpp>

#include "aether/format/toml_readers.hpp"

namespace aether {
namespace {

/// Map an OpenPBR Surface 1.1.1 keyword to the texture-slot struct field that
/// holds it. Scalar/color material parameters use the **verbatim OpenPBR field
/// name** (no translation, no abbreviation); only the `geometry_*` normal/tangent
/// *vector* inputs are realised as the renderer's texture-map slots, so just
/// those are mapped here. Foreign-format names (UsdPreviewSurface / MaterialX /
/// Wavefront MTL) are intentionally NOT accepted — files are authored in OpenPBR.
[[nodiscard]] std::string_view normalise(std::string_view kw) noexcept {
    // OpenPBR `geometry_*` normal/tangent vector inputs → texture-map slots.
    if (kw == "geometry_normal") {
        return "map_normal";
    }
    if (kw == "geometry_coat_normal") {
        return "map_coat_normal";
    }
    if (kw == "geometry_tangent") {
        return "map_tangent";
    }
    if (kw == "geometry_coat_tangent") {
        return "map_coat_tangent";
    }
    return kw;
}

// ── TOML value readers ─────────────────────────────────────────────────────

[[nodiscard]] std::optional<float> asFloat(const toml::node& n) {
    if (const auto v = n.value<double>()) {
        return static_cast<float>(*v);
    }
    return std::nullopt;
}

// ── Parameter dispatch ────────────────────────────────────────────────────

void applyKw(MaterialDesc& p, std::string_view rawKw, const toml::node& value) {
    const std::string_view kw = normalise(rawKw);

    using TextureSlot = TextureRef MaterialDesc::*;
    using ScalarSetter = void (*)(MaterialDesc&, float);
    using ColorSetter = void (*)(MaterialDesc&, const Vec3&);
    using BoolSetter = void (*)(MaterialDesc&, const toml::node&);

    // ── Texture map paths (string values) ────────────────────────────────
    static const std::unordered_map<std::string_view, TextureSlot> texturePathSetters = {
        {"map_base_color", &MaterialDesc::map_base_color},
        {"map_normal", &MaterialDesc::map_normal},
        {"map_orm", &MaterialDesc::map_orm},
        {"map_roughness", &MaterialDesc::map_roughness},
        {"map_metalness", &MaterialDesc::map_metalness},
        {"map_emission_color", &MaterialDesc::map_emission_color},
        {"map_coat_normal", &MaterialDesc::map_coat_normal},
        {"map_tangent", &MaterialDesc::map_tangent},
        {"map_coat_tangent", &MaterialDesc::map_coat_tangent},
    };
    if (const auto it = texturePathSetters.find(kw); it != texturePathSetters.end()) {
        (p.*(it->second)).path = value.value_or<std::string>("");
        return;
    }

    // ── Texture map color spaces (ColorInterop interop IDs) ──────────────
    // Unknown tokens keep the slot's default and warn — silently treating a
    // wrong color space as data would render wrong colors.
    struct ColorSpaceSetter {
        TextureSlot slot{};
        std::string_view label{};
    };
    static const std::unordered_map<std::string_view, ColorSpaceSetter> colorspaceSetters = {
        {"map_base_color_colorspace", {&MaterialDesc::map_base_color, "map_base_color"}},
        {"map_normal_colorspace", {&MaterialDesc::map_normal, "map_normal"}},
        {"map_orm_colorspace", {&MaterialDesc::map_orm, "map_orm"}},
        {"map_roughness_colorspace", {&MaterialDesc::map_roughness, "map_roughness"}},
        {"map_metalness_colorspace", {&MaterialDesc::map_metalness, "map_metalness"}},
        {"map_emission_color_colorspace", {&MaterialDesc::map_emission_color, "map_emission_color"}},
        {"map_coat_normal_colorspace", {&MaterialDesc::map_coat_normal, "map_coat_normal"}},
        {"map_tangent_colorspace", {&MaterialDesc::map_tangent, "map_tangent"}},
        {"map_coat_tangent_colorspace", {&MaterialDesc::map_coat_tangent, "map_coat_tangent"}},
    };
    if (const auto it = colorspaceSetters.find(kw); it != colorspaceSetters.end()) {
        const std::string token = value.value_or<std::string>("");
        if (const auto cs = parseTextureColorSpace(token)) {
            (p.*(it->second.slot)).colorSpace = *cs;
        } else {
            std::cerr << "[aether] unsupported texture colorspace \"" << token << "\" for " << it->second.label << "\n";
        }
        return;
    }

    // ── Colour keywords (3-element arrays) ────────────────────────────────
    // NOTE: Aether records colors verbatim in the declared input color space
    // (p.inputColorSpace); the consumer performs any conversion. Non-color data
    // (subsurface_radius_scale, transmission_scatter) is never color-converted.
    static const std::unordered_map<std::string_view, ColorSetter> colorSetters = {
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
    if (const auto it = colorSetters.find(kw); it != colorSetters.end()) {
        if (const auto c = asVec3(value)) {
            it->second(p, *c);
        }
        return;
    }

    // ── Boolean keywords ──────────────────────────────────────────────────
    static const std::unordered_map<std::string_view, BoolSetter> boolSetters = {
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
    if (const auto it = boolSetters.find(kw); it != boolSetters.end()) {
        it->second(p, value);
        return;
    }

    // ── Scalar keywords ───────────────────────────────────────────────────
    static const std::unordered_map<std::string_view, ScalarSetter> scalarSetters = {
        {"base_weight", [](MaterialDesc& d, float f) { d.base_weight = std::clamp(f, 0.0F, 1.0F); }},
        {"base_metalness", [](MaterialDesc& d, float f) { d.base_metalness = f; }},
        {"base_diffuse_roughness", [](MaterialDesc& d, float f) { d.base_diffuse_roughness = f; }},
        {"specular_weight", [](MaterialDesc& d, float f) { d.specular_weight = std::clamp(f, 0.0F, 1.0F); }},
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
    if (const auto it = scalarSetters.find(kw); it != scalarSetters.end()) {
        if (const auto f = asFloat(value)) {
            it->second(p, *f);
        }
        return;
    }
}

} // namespace

// ── Color space helpers ─────────────────────────────────────────────────────

namespace {

/// Parse a material color space token (ColorInterop interop ID:
/// "lin_rec709_scene" | "lin_rec2020_scene"). Unknown → nullopt.
[[nodiscard]] std::optional<MaterialColorSpace> parseMaterialColorSpace(std::string_view token) noexcept {
    if (token == "lin_rec709_scene") {
        return MaterialColorSpace::LinRec709;
    }
    if (token == "lin_rec2020_scene") {
        return MaterialColorSpace::LinRec2020;
    }
    return std::nullopt;
}

/// True when a color texture's declared space shares the primaries of the
/// material's declared color space. Encodings (sRGB OETF vs linear) may
/// differ — only the gamut must match. Data carries no primaries and is exempt.
[[nodiscard]] bool primariesMatch(TextureColorSpace tex, MaterialColorSpace mat) noexcept {
    switch (tex) {
    case TextureColorSpace::Data:
        return true;
    case TextureColorSpace::SrgbRec709Scene:
    case TextureColorSpace::LinRec709Scene:
        return mat == MaterialColorSpace::LinRec709;
    case TextureColorSpace::LinRec2020Scene:
        return mat == MaterialColorSpace::LinRec2020;
    }
    return false;
}

/// Enforce the one-color-space-per-material rule: every color texture must use
/// the primaries declared by the material's `colorspace`. Returns the name of
/// the first offending texture key, or nullopt when consistent.
[[nodiscard]] std::optional<std::string_view> findGamutMismatch(const MaterialDesc& p) noexcept {
    const struct {
        std::string_view key;
        const TextureRef& ref;
    } colorMaps[] = {
        {"map_base_color", p.map_base_color},
        {"map_emission_color", p.map_emission_color},
    };
    for (const auto& m : colorMaps) {
        if (!m.ref.empty() && !primariesMatch(m.ref.colorSpace, p.inputColorSpace)) {
            return m.key;
        }
    }
    return std::nullopt;
}

} // namespace

// ── MaterialLibrary ───────────────────────────────────────────────────────

bool MaterialLibrary::load(const std::filesystem::path& path) {
    toml::table root;
    try {
        root = toml::parse_file(path.string());
    } catch (const toml::parse_error&) {
        return false;
    }

    // File-level defaults: input color space (`colorspace`, default lin_rec709)
    // and material model (`model`, default openpbr). Both may be overridden
    // per material.
    MaterialColorSpace defaultColorSpace = MaterialColorSpace::LinRec709;
    if (const auto cs = root["colorspace"].value<std::string>()) {
        if (const auto parsed = parseMaterialColorSpace(*cs)) {
            defaultColorSpace = *parsed;
        } else {
            std::cerr << "[aether] " << path.filename().string() << ": unknown colorspace \"" << *cs
                      << "\", using lin_rec709\n";
        }
    }
    std::string defaultModel{"openpbr"};
    if (const auto m = root["model"].value<std::string>()) {
        defaultModel = *m;
    }

    // Every top-level table is a material named by its key.
    for (auto&& [key, node] : root) {
        const toml::table* matTbl = node.as_table();
        if (matTbl == nullptr) {
            continue; // skip scalar top-level keys (e.g. `colorspace`, `model`)
        }

        MaterialDesc params;
        params.inputColorSpace = defaultColorSpace;
        params.model = defaultModel;
        for (auto&& [pKey, pNode] : *matTbl) {
            const std::string_view pk{pKey.str()};
            // Per-material overrides of the file-level defaults.
            if (pk == "colorspace") {
                if (const auto cs = pNode.value<std::string>()) {
                    if (const auto parsed = parseMaterialColorSpace(*cs)) {
                        params.inputColorSpace = *parsed;
                    } else {
                        std::cerr << "[aether] " << path.filename().string() << ": material \"" << key.str()
                                  << "\": unknown colorspace \"" << *cs << "\", keeping default\n";
                    }
                }
                continue;
            }
            if (pk == "model") {
                if (const auto m = pNode.value<std::string>()) {
                    params.model = *m;
                }
                continue;
            }
            applyKw(params, pk, pNode);
        }

        // Unknown material model: warn and skip — a renderer cannot interpret
        // parameters of a model it does not know.
        if (params.model != "openpbr") {
            std::cerr << "[aether] " << path.filename().string() << ": material \"" << key.str()
                      << "\": unknown model \"" << params.model << "\", skipping\n";
            continue;
        }

        // One color space per material: values and color textures must share
        // the declared primaries.
        if (const auto offending = findGamutMismatch(params)) {
            std::cerr << "[aether] " << path.filename().string() << ": material \"" << key.str() << "\": " << *offending
                      << " color space mixes primaries with the material colorspace, skipping\n";
            continue;
        }

        m_materials.insert_or_assign(std::string{key.str()}, params);
    }

    return true;
}

std::optional<MaterialDesc> MaterialLibrary::get(const std::string& name) const {
    const auto it = m_materials.find(name);
    return it != m_materials.end() ? std::optional{it->second} : std::nullopt;
}

MaterialDesc MaterialLibrary::getOrDefault(const std::string& name) const {
    return get(name).value_or(MaterialDesc{});
}

} // namespace aether
