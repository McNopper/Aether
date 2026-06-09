#include "aether/format/MaterialLibrary.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace aether {
namespace {

// ── Helpers ───────────────────────────────────────────────────────────────

[[nodiscard]] std::string_view trimSv(std::string_view sv) noexcept {
    const auto first = sv.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = sv.find_last_not_of(" \t\r\n");
    return sv.substr(first, last - first + 1);
}

[[nodiscard]] std::string_view stripComment(std::string_view line) noexcept {
    const auto pos = line.find('#');
    return trimSv(pos == std::string_view::npos ? line : line.substr(0, pos));
}

/// Map UsdPreviewSurface / MaterialX aliases → OpenPBR keyword names.
/// Classic OBJ/MTL keywords (Kd, Ks, Ni, Tr, Ke, map_Kd, map_Ns, …) are
/// intentionally NOT listed here — they must be silently ignored.
[[nodiscard]] std::string_view normalise(std::string_view kw) noexcept {
    if (kw == "diffuseColor") {
        return "base_color";
    }
    if (kw == "metallic") {
        return "base_metalness";
    }
    if (kw == "roughness") {
        return "specular_roughness";
    }
    if (kw == "base_roughness") {
        return "base_diffuse_roughness";
    }
    if (kw == "ior") {
        return "specular_ior";
    }
    if (kw == "emissiveColor") {
        return "emission_color";
    }
    if (kw == "emissiveLuminance") {
        return "emission_luminance";
    }
    if (kw == "clearcoat") {
        return "coat_weight";
    }
    if (kw == "clearcoatRoughness") {
        return "coat_roughness";
    }
    if (kw == "transmissionAmount") {
        return "transmission_weight";
    }
    if (kw == "specularColor") {
        return "specular_color";
    }
    // OpenPBR canonical geometry opacity name (stored as `opacity`).
    if (kw == "geometry_opacity") {
        return "opacity";
    }
    if (kw == "geometry_thin_walled") {
        return "thin_walled";
    }
    // OpenPBR canonical subsurface radius-scale name (stored as `subsurface_scale`).
    if (kw == "subsurface_radius_scale") {
        return "subsurface_scale";
    }
    // OpenPBR canonical geometry normal/tangent map names.
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

[[nodiscard]] bool parseVec3(std::string_view text, Vec3& out) {
    std::istringstream ss{std::string(text)};
    return static_cast<bool>(ss >> out.x >> out.y >> out.z);
}

[[nodiscard]] bool parseFloat(std::string_view text, float& out) {
    std::istringstream ss{std::string(text)};
    return static_cast<bool>(ss >> out);
}

// ── Parameter dispatch ────────────────────────────────────────────────────

void applyKw(MaterialDesc& p, std::string_view rawKw, std::string_view rest) {
    const std::string_view kw = normalise(rawKw);

    // ── Texture map paths (string values) ────────────────────────────────
    if (kw == "map_base_color") {
        p.map_base_color.path = std::string(rest);
        return;
    }
    if (kw == "map_normal") {
        p.map_normal.path = std::string(rest);
        return;
    }
    if (kw == "map_orm") {
        p.map_orm.path = std::string(rest);
        return;
    }
    if (kw == "map_roughness") {
        p.map_roughness.path = std::string(rest);
        return;
    }
    if (kw == "map_metalness") {
        p.map_metalness.path = std::string(rest);
        return;
    }
    if (kw == "map_emission_color") {
        p.map_emission_color.path = std::string(rest);
        return;
    }
    if (kw == "map_coat_normal") {
        p.map_coat_normal.path = std::string(rest);
        return;
    }
    if (kw == "map_tangent") {
        p.map_tangent.path = std::string(rest);
        return;
    }
    if (kw == "map_coat_tangent") {
        p.map_coat_tangent.path = std::string(rest);
        return;
    }

    // ── Texture map color spaces (OCIO / OpenEXR IIF names) ──────────────
    if (kw == "map_base_color_colorspace") {
        p.map_base_color.colorSpace = parseTextureColorSpace(rest);
        return;
    }
    if (kw == "map_normal_colorspace") {
        p.map_normal.colorSpace = parseTextureColorSpace(rest);
        return;
    }
    if (kw == "map_orm_colorspace") {
        p.map_orm.colorSpace = parseTextureColorSpace(rest);
        return;
    }
    if (kw == "map_roughness_colorspace") {
        p.map_roughness.colorSpace = parseTextureColorSpace(rest);
        return;
    }
    if (kw == "map_metalness_colorspace") {
        p.map_metalness.colorSpace = parseTextureColorSpace(rest);
        return;
    }
    if (kw == "map_emission_color_colorspace") {
        p.map_emission_color.colorSpace = parseTextureColorSpace(rest);
        return;
    }
    if (kw == "map_coat_normal_colorspace") {
        p.map_coat_normal.colorSpace = parseTextureColorSpace(rest);
        return;
    }
    if (kw == "map_tangent_colorspace") {
        p.map_tangent.colorSpace = parseTextureColorSpace(rest);
        return;
    }
    if (kw == "map_coat_tangent_colorspace") {
        p.map_coat_tangent.colorSpace = parseTextureColorSpace(rest);
        return;
    }

    // ── Colour keywords ───────────────────────────────────────────────────
    // NOTE: Aether records colors verbatim in the declared input color space
    // (p.inputColorSpace); the consumer performs any conversion to its working
    // color space. Non-color data (subsurface_radius, transmission_scatter) is
    // never color-converted.
    Vec3 c{};
    if (kw == "base_color" || kw == "specular_color" || kw == "transmission_color" || kw == "coat_color" ||
        kw == "fuzz_color" || kw == "emission_color" || kw == "subsurface_color" || kw == "subsurface_radius" ||
        kw == "transmission_scatter") {
        if (!parseVec3(rest, c)) {
            return;
        }
        if (kw == "base_color") {
            p.base_color = c;
        } else if (kw == "specular_color") {
            p.specular_color = c;
        } else if (kw == "transmission_color") {
            p.transmission_color = c;
        } else if (kw == "transmission_scatter") {
            p.transmission_scatter = c;
        } else if (kw == "coat_color") {
            p.coat_color = c;
        } else if (kw == "fuzz_color") {
            p.fuzz_color = c;
        } else if (kw == "emission_color") {
            p.emission_color = c;
        } else if (kw == "subsurface_color") {
            p.subsurface_color = c;
        } else if (kw == "subsurface_radius") {
            p.subsurface_radius = c;
        }
        return;
    }

    // ── Scalar keywords ───────────────────────────────────────────────────
    float f = 0.0F;
    if (!parseFloat(rest, f)) {
        return;
    }
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
    } else if (kw == "thin_walled") {
        p.thin_walled = (f != 0.0F);
    }
}

} // namespace

// ── MaterialLibrary ───────────────────────────────────────────────────────

bool MaterialLibrary::load(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        return false;
    }

    std::string currentName;
    MaterialDesc currentParams;
    bool hasCurrent = false;
    MaterialColorSpace inputColorSpace = MaterialColorSpace::LinRec709;

    auto flush = [&] {
        if (hasCurrent && !currentName.empty()) {
            currentParams.inputColorSpace = inputColorSpace;
            m_materials.insert_or_assign(currentName, currentParams);
        }
    };

    std::string line;
    while (std::getline(file, line)) {
        const std::string_view trimmed = stripComment(line);
        if (trimmed.empty()) {
            continue;
        }

        std::istringstream ss{std::string(trimmed)};
        std::string kw;
        ss >> kw;

        if (kw == "colorspace") {
            // File-level color space declaration.
            std::string cs;
            std::getline(ss, cs);
            const std::string_view csv = trimSv(cs);
            inputColorSpace = (csv == "lin_rec2020" || csv == "rec2020") ? MaterialColorSpace::LinRec2020
                                                                         : MaterialColorSpace::LinRec709;
        } else if (kw == "newmtl") {
            flush();
            currentName = {};
            currentParams = {};
            hasCurrent = true;
            std::getline(ss, currentName);
            currentName = std::string(trimSv(currentName));
        } else if (hasCurrent) {
            // Rest of line after the keyword.
            std::string rest;
            std::getline(ss, rest);
            applyKw(currentParams, kw, trimSv(rest));
        }
    }
    flush();

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
