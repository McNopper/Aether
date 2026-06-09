#include "aether/format/MaterialLibrary.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

namespace aether {
namespace {

/// Map OpenPBR Surface parameter names to this library's internal storage field
/// names (which abbreviate a few of the longer spec names). This is **not**
/// alias resolution — every accepted keyword is a verbatim OpenPBR Surface
/// keyword; we only translate the canonical spec name to the struct field that
/// holds it. Foreign-format names (UsdPreviewSurface / MaterialX / Wavefront
/// MTL) are intentionally NOT accepted — material files are authored in OpenPBR.
[[nodiscard]] std::string_view normalise(std::string_view kw) noexcept {
    // OpenPBR `geometry_*` parameters → internal fields.
    if (kw == "geometry_opacity") {
        return "opacity";
    }
    if (kw == "geometry_thin_walled") {
        return "thin_walled";
    }
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
    // OpenPBR `subsurface_radius_scale` → internal `subsurface_scale` field.
    if (kw == "subsurface_radius_scale") {
        return "subsurface_scale";
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

    // ── Texture map color spaces (OCIO / OpenEXR IIF names) ──────────────
    if (kw == "map_base_color_colorspace") {
        p.map_base_color.colorSpace = parseTextureColorSpace(value.value_or<std::string>(""));
        return;
    }
    if (kw == "map_normal_colorspace") {
        p.map_normal.colorSpace = parseTextureColorSpace(value.value_or<std::string>(""));
        return;
    }
    if (kw == "map_orm_colorspace") {
        p.map_orm.colorSpace = parseTextureColorSpace(value.value_or<std::string>(""));
        return;
    }
    if (kw == "map_roughness_colorspace") {
        p.map_roughness.colorSpace = parseTextureColorSpace(value.value_or<std::string>(""));
        return;
    }
    if (kw == "map_metalness_colorspace") {
        p.map_metalness.colorSpace = parseTextureColorSpace(value.value_or<std::string>(""));
        return;
    }
    if (kw == "map_emission_color_colorspace") {
        p.map_emission_color.colorSpace = parseTextureColorSpace(value.value_or<std::string>(""));
        return;
    }
    if (kw == "map_coat_normal_colorspace") {
        p.map_coat_normal.colorSpace = parseTextureColorSpace(value.value_or<std::string>(""));
        return;
    }
    if (kw == "map_tangent_colorspace") {
        p.map_tangent.colorSpace = parseTextureColorSpace(value.value_or<std::string>(""));
        return;
    }
    if (kw == "map_coat_tangent_colorspace") {
        p.map_coat_tangent.colorSpace = parseTextureColorSpace(value.value_or<std::string>(""));
        return;
    }

    // ── Colour keywords (3-element arrays) ────────────────────────────────
    // NOTE: Aether records colors verbatim in the declared input color space
    // (p.inputColorSpace); the consumer performs any conversion. Non-color data
    // (subsurface_radius, transmission_scatter) is never color-converted.
    if (kw == "base_color" || kw == "specular_color" || kw == "transmission_color" || kw == "coat_color" ||
        kw == "fuzz_color" || kw == "emission_color" || kw == "subsurface_color" || kw == "subsurface_radius" ||
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
        } else if (kw == "subsurface_radius") {
            p.subsurface_radius = *c;
        }
        return;
    }

    // ── Boolean keyword ───────────────────────────────────────────────────
    if (kw == "thin_walled") {
        if (const auto b = value.value<bool>()) {
            p.thin_walled = *b;
        } else if (const auto f = asFloat(value)) {
            p.thin_walled = (*f != 0.0F);
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
    } else if (kw == "subsurface_scale") {
        p.subsurface_scale = f;
    } else if (kw == "subsurface_scatter_anisotropy") {
        p.subsurface_scatter_anisotropy = std::clamp(f, -1.0F, 1.0F);
    } else if (kw == "opacity") {
        p.opacity = f;
    }
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

    // File-level input color space (top-level `colorspace` key); default Rec.709.
    MaterialColorSpace inputColorSpace = MaterialColorSpace::LinRec709;
    if (const auto cs = root["colorspace"].value<std::string>()) {
        inputColorSpace = (*cs == "lin_rec2020" || *cs == "rec2020") ? MaterialColorSpace::LinRec2020
                                                                     : MaterialColorSpace::LinRec709;
    }

    // Every top-level table is a material named by its key.
    for (auto&& [key, node] : root) {
        const toml::table* matTbl = node.as_table();
        if (matTbl == nullptr) {
            continue; // skip scalar top-level keys (e.g. `colorspace`)
        }

        MaterialDesc params;
        params.inputColorSpace = inputColorSpace;
        for (auto&& [pKey, pNode] : *matTbl) {
            applyKw(params, std::string_view{pKey.str()}, pNode);
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
