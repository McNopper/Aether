#include "aether/format/MaterialLibrary.hpp"

#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <toml++/toml.hpp>

#include "aether/format/MaterialKwTables.hpp"
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

// ── Parameter dispatch ────────────────────────────────────────────────────

void applyKw(MaterialDesc& p, std::string_view rawKw, const toml::node& value) {
    const std::string_view kw = normalise(rawKw);

    // Dispatch tables live in MaterialKwTables.hpp (file scope, shared with
    // the KwTablePairingInvariant test); the order below is the parser's
    // contract: texture paths, color spaces, colors, bools, scalars —
    // unmatched keys are ignored.
    //
    // ── Texture map paths (string values) ────────────────────────────────
    if (const auto it = format::texturePathSetters.find(kw); it != format::texturePathSetters.end()) {
        (p.*(it->second)).path = value.value_or<std::string>("");
        return;
    }

    // ── Texture map color spaces (ColorInterop interop IDs) ──────────────
    // Unknown tokens keep the slot's default and warn — silently treating a
    // wrong color space as data would render wrong colors.
    if (const auto it = format::colorspaceSetters.find(kw); it != format::colorspaceSetters.end()) {
        const std::string token = value.value_or<std::string>("");
        if (const auto cs = parseTextureColorSpace(token)) {
            (p.*(it->second.slot)).colorSpace = *cs;
        } else {
            std::cerr << "[aether] unsupported texture colorspace \"" << token << "\" for " << it->second.label << "\n";
        }
        return;
    }

    // ── Colour keywords (3-element arrays) ────────────────────────────────
    if (const auto it = format::colorSetters.find(kw); it != format::colorSetters.end()) {
        if (const auto c = asVec3(value)) {
            it->second(p, *c);
        }
        return;
    }

    // ── Boolean keywords ──────────────────────────────────────────────────
    if (const auto it = format::boolSetters.find(kw); it != format::boolSetters.end()) {
        it->second(p, value);
        return;
    }

    // ── Scalar keywords ───────────────────────────────────────────────────
    if (const auto it = format::scalarSetters.find(kw); it != format::scalarSetters.end()) {
        if (const auto f = format::asFloat(value)) {
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
