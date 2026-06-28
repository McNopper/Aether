#include "aether/format/MaterialLibrary.hpp"

#include <algorithm>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <toml++/toml.hpp>

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

[[nodiscard]] std::optional<Vec3> asVec3(const toml::node& n) {
    const toml::array* arr = n.as_array();
    if (arr == nullptr || arr->size() != 3) {
        return std::nullopt;
    }
    const auto x = (*arr)[0].value<double>();
    const auto y = (*arr)[1].value<double>();
    const auto z = (*arr)[2].value<double>();
    if (!x || !y || !z) {
        return std::nullopt;
    }
    return Vec3{static_cast<float>(*x), static_cast<float>(*y), static_cast<float>(*z)};
}

// ── Parameter dispatch ────────────────────────────────────────────────────

void applyKw(MaterialDesc& p, std::string_view rawKw, const toml::node& value) {
    const std::string_view kw = normalise(rawKw);

    // ── Texture map paths (string values) ────────────────────────────────
    if (kw == "map_base_color") {
        p.map_base_color.path = value.value_or<std::string>("");
        return;
    }
    if (kw == "map_normal") {
        p.map_normal.path = value.value_or<std::string>("");
        return;
    }
    if (kw == "map_orm") {
        p.map_orm.path = value.value_or<std::string>("");
        return;
    }
    if (kw == "map_roughness") {
        p.map_roughness.path = value.value_or<std::string>("");
        return;
    }
    if (kw == "map_metalness") {
        p.map_metalness.path = value.value_or<std::string>("");
        return;
    }
    if (kw == "map_emission_color") {
        p.map_emission_color.path = value.value_or<std::string>("");
        return;
    }
    if (kw == "map_coat_normal") {
        p.map_coat_normal.path = value.value_or<std::string>("");
        return;
    }
    if (kw == "map_tangent") {
        p.map_tangent.path = value.value_or<std::string>("");
        return;
    }
    if (kw == "map_coat_tangent") {
        p.map_coat_tangent.path = value.value_or<std::string>("");
        return;
    }

    // ── Texture map color spaces (ColorInterop interop IDs) ──────────────
    // Unknown tokens keep the slot's default and warn — silently treating a
    // wrong color space as data would render wrong colors.
    if (kw == "map_base_color_colorspace") {
        const std::string token = value.value_or<std::string>("");
        if (const auto cs = parseTextureColorSpace(token)) {
            p.map_base_color.colorSpace = *cs;
        } else {
            std::cerr << "[aether] unsupported texture colorspace \"" << token << "\" for map_base_color\n";
        }
        return;
    }
    if (kw == "map_normal_colorspace") {
        const std::string token = value.value_or<std::string>("");
        if (const auto cs = parseTextureColorSpace(token)) {
            p.map_normal.colorSpace = *cs;
        } else {
            std::cerr << "[aether] unsupported texture colorspace \"" << token << "\" for map_normal\n";
        }
        return;
    }
    if (kw == "map_orm_colorspace") {
        const std::string token = value.value_or<std::string>("");
        if (const auto cs = parseTextureColorSpace(token)) {
            p.map_orm.colorSpace = *cs;
        } else {
            std::cerr << "[aether] unsupported texture colorspace \"" << token << "\" for map_orm\n";
        }
        return;
    }
    if (kw == "map_roughness_colorspace") {
        const std::string token = value.value_or<std::string>("");
        if (const auto cs = parseTextureColorSpace(token)) {
            p.map_roughness.colorSpace = *cs;
        } else {
            std::cerr << "[aether] unsupported texture colorspace \"" << token << "\" for map_roughness\n";
        }
        return;
    }
    if (kw == "map_metalness_colorspace") {
        const std::string token = value.value_or<std::string>("");
        if (const auto cs = parseTextureColorSpace(token)) {
            p.map_metalness.colorSpace = *cs;
        } else {
            std::cerr << "[aether] unsupported texture colorspace \"" << token << "\" for map_metalness\n";
        }
        return;
    }
    if (kw == "map_emission_color_colorspace") {
        const std::string token = value.value_or<std::string>("");
        if (const auto cs = parseTextureColorSpace(token)) {
            p.map_emission_color.colorSpace = *cs;
        } else {
            std::cerr << "[aether] unsupported texture colorspace \"" << token << "\" for map_emission_color\n";
        }
        return;
    }
    if (kw == "map_coat_normal_colorspace") {
        const std::string token = value.value_or<std::string>("");
        if (const auto cs = parseTextureColorSpace(token)) {
            p.map_coat_normal.colorSpace = *cs;
        } else {
            std::cerr << "[aether] unsupported texture colorspace \"" << token << "\" for map_coat_normal\n";
        }
        return;
    }
    if (kw == "map_tangent_colorspace") {
        const std::string token = value.value_or<std::string>("");
        if (const auto cs = parseTextureColorSpace(token)) {
            p.map_tangent.colorSpace = *cs;
        } else {
            std::cerr << "[aether] unsupported texture colorspace \"" << token << "\" for map_tangent\n";
        }
        return;
    }
    if (kw == "map_coat_tangent_colorspace") {
        const std::string token = value.value_or<std::string>("");
        if (const auto cs = parseTextureColorSpace(token)) {
            p.map_coat_tangent.colorSpace = *cs;
        } else {
            std::cerr << "[aether] unsupported texture colorspace \"" << token << "\" for map_coat_tangent\n";
        }
        return;
    }

    // ── Colour keywords (3-element arrays) ────────────────────────────────
    // NOTE: Aether records colors verbatim in the declared input color space
    // (p.inputColorSpace); the consumer performs any conversion. Non-color data
    // (subsurface_radius_scale, transmission_scatter) is never color-converted.
    if (kw == "base_color" || kw == "specular_color" || kw == "transmission_color" || kw == "coat_color" ||
        kw == "fuzz_color" || kw == "emission_color" || kw == "subsurface_color" || kw == "subsurface_radius_scale" ||
        kw == "transmission_scatter") {
        const auto c = asVec3(value);
        if (!c) {
            return;
        }
        if (kw == "base_color") {
            p.base_color = *c;
        } else if (kw == "specular_color") {
            p.specular_color = *c;
        } else if (kw == "transmission_color") {
            p.transmission_color = *c;
        } else if (kw == "transmission_scatter") {
            p.transmission_scatter = *c;
        } else if (kw == "coat_color") {
            p.coat_color = *c;
        } else if (kw == "fuzz_color") {
            p.fuzz_color = *c;
        } else if (kw == "emission_color") {
            p.emission_color = *c;
        } else if (kw == "subsurface_color") {
            p.subsurface_color = *c;
        } else if (kw == "subsurface_radius_scale") {
            p.subsurface_radius_scale = *c;
        }
        return;
    }

    // ── Boolean keyword ───────────────────────────────────────────────────
    if (kw == "geometry_thin_walled") {
        if (const auto b = value.value<bool>()) {
            p.geometry_thin_walled = *b;
        } else if (const auto f = asFloat(value)) {
            p.geometry_thin_walled = (*f != 0.0F);
        }
        return;
    }
    if (kw == "emission_as_light_source") {
        if (const auto b = value.value<bool>()) {
            p.emission_as_light_source = *b;
        } else if (const auto f = asFloat(value)) {
            p.emission_as_light_source = (*f != 0.0F);
        }
        return;
    }

    // ── Scalar keywords ───────────────────────────────────────────────────
    const auto opt = asFloat(value);
    if (!opt) {
        return;
    }
    const float f = *opt;
    if (kw == "base_weight") {
        p.base_weight = f;
    } else if (kw == "base_metalness") {
        p.base_metalness = f;
    } else if (kw == "base_diffuse_roughness") {
        p.base_diffuse_roughness = f;
    } else if (kw == "specular_weight") {
        p.specular_weight = f;
    } else if (kw == "specular_roughness") {
        p.specular_roughness = f;
    } else if (kw == "specular_roughness_anisotropy") {
        p.specular_roughness_anisotropy = f;
    } else if (kw == "specular_ior") {
        p.specular_ior = f;
    } else if (kw == "transmission_weight") {
        p.transmission_weight = f;
    } else if (kw == "transmission_depth") {
        p.transmission_depth = f;
    } else if (kw == "transmission_scatter_anisotropy") {
        p.transmission_scatter_anisotropy = std::clamp(f, -1.0F, 1.0F);
    } else if (kw == "transmission_dispersion_scale") {
        p.transmission_dispersion_scale = std::max(f, 0.0F);
    } else if (kw == "transmission_dispersion_abbe_number") {
        p.transmission_dispersion_abbe_number = f;
    } else if (kw == "thin_film_weight") {
        p.thin_film_weight = f;
    } else if (kw == "thin_film_thickness") {
        p.thin_film_thickness = f;
    } else if (kw == "thin_film_ior") {
        p.thin_film_ior = f;
    } else if (kw == "coat_weight") {
        p.coat_weight = f;
    } else if (kw == "coat_roughness") {
        p.coat_roughness = f;
    } else if (kw == "coat_roughness_anisotropy") {
        p.coat_roughness_anisotropy = f;
    } else if (kw == "coat_ior") {
        p.coat_ior = f;
    } else if (kw == "coat_darkening") {
        p.coat_darkening = f;
    } else if (kw == "fuzz_weight") {
        p.fuzz_weight = f;
    } else if (kw == "fuzz_roughness") {
        p.fuzz_roughness = f;
    } else if (kw == "emission_luminance") {
        p.emission_luminance = f;
    } else if (kw == "subsurface_weight") {
        p.subsurface_weight = f;
    } else if (kw == "subsurface_radius") {
        p.subsurface_radius = f;
    } else if (kw == "subsurface_scatter_anisotropy") {
        p.subsurface_scatter_anisotropy = std::clamp(f, -1.0F, 1.0F);
    } else if (kw == "geometry_opacity") {
        p.geometry_opacity = f;
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
