#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

#include "aether/types/MaterialDesc.hpp"

namespace aether {

/// Loads a `<name>.materials.toml` OpenPBR material library into renderer-agnostic
/// CPU structs.
///
/// FORMAT
/// ──────
/// A TOML document. Every top-level table is a material named by its key; its
/// keys are OpenPBR Surface v1.1 parameter names. Colors are 3-element float
/// arrays, scalars are numbers, texture maps are string paths, `thin_walled` is
/// a bool. Any unrecognised key is silently ignored.
///
///   model = "openpbr"               # file-level material model (optional)
///   colorspace = "lin_rec709_scene" # file-level input color space (optional)
///
///   [WhiteWall]
///   base_color = [0.725, 0.710, 0.680]
///   specular_roughness = 1.0
///
/// COLOR MANAGEMENT
/// ────────────────
/// Color space tokens are ASWF ColorInterop interop IDs; only the
/// Rec.709/Rec.2020 related scene-referred spaces are supported. Material
/// color values are always linear:
///
///   colorspace = "lin_rec709_scene"   (default — converted by the consumer)
///   colorspace = "lin_rec2020_scene"
///
/// Texture maps may additionally be sRGB-encoded ("srgb_rec709_scene") or
/// non-color ("data"), declared per map via `map_*_colorspace` keys. One
/// material uses one set of primaries: a color texture whose primaries differ
/// from the material's `colorspace` is an error (the material is skipped).
/// Both `model` and `colorspace` may be overridden per material.
///
/// Aether records the declared input color space on each MaterialDesc
/// (`inputColorSpace`) but performs **no** color conversion itself — that is the
/// consumer's responsibility.
///
/// MATERIAL MODEL
/// ──────────────
/// `model` tags the material model of the parameters; only "openpbr" is
/// understood today (default). Materials with an unknown model are skipped
/// with a warning so future models can coexist in one file.
///
/// Keywords are OpenPBR Surface v1.1 parameter names, used verbatim
/// (https://academysoftwarefoundation.github.io/OpenPBR/). A few longer spec
/// names map to abbreviated internal fields (e.g. geometry_opacity → opacity,
/// geometry_thin_walled → thin_walled, geometry_normal → map_normal,
/// subsurface_radius_scale → subsurface_scale). Foreign-format names
/// (UsdPreviewSurface / MaterialX / Wavefront MTL) are not accepted.
class MaterialLibrary {
  public:
    /// Load material definitions from a .materials.toml file.
    /// Returns false only if the file cannot be opened.
    bool load(const std::filesystem::path& path);

    /// Look up a material by name. Returns std::nullopt if not found.
    [[nodiscard]] std::optional<MaterialDesc> get(const std::string& name) const;

    /// Look up a material by name, or return a default diffuse-gray material.
    [[nodiscard]] MaterialDesc getOrDefault(const std::string& name) const;

    [[nodiscard]] bool empty() const noexcept { return m_materials.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return m_materials.size(); }

    [[nodiscard]] const std::unordered_map<std::string, MaterialDesc>& materials() const noexcept {
        return m_materials;
    }

  private:
    std::unordered_map<std::string, MaterialDesc> m_materials;
};

} // namespace aether
