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
///   colorspace = "lin_rec709"      # file-level input color space (optional)
///
///   [WhiteWall]
///   base_color = [0.725, 0.710, 0.680]
///   specular_roughness = 1.0
///
/// Color values default to linear Rec.709 input; the top-level `colorspace`
/// key may declare a different input space:
///
///   colorspace = "lin_rec709"    (default — flagged for conversion by the consumer)
///   colorspace = "lin_rec2020"   (already in the render working space)
///
/// Aether records the declared input color space on each MaterialDesc
/// (`inputColorSpace`) but performs **no** color conversion itself — that is the
/// consumer's responsibility.
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
