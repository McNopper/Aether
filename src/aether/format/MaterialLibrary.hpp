#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

#include "aether/types/MaterialDesc.hpp"

namespace aether {

/// Loads a `.mtlx` OpenPBR material library into renderer-agnostic CPU structs.
///
/// This format is NOT Wavefront MTL and NOT MaterialX XML: it is a line-based
/// text format that reuses the `newmtl` block delimiter but expresses every
/// material property with OpenPBR Surface v1.1 keyword names. Any classic
/// Wavefront MTL keyword (Kd, Ks, Ni, Tr, Ke, map_Kd, …) is silently ignored.
///
/// Color values default to linear Rec.709 input; the file-level `colorspace`
/// keyword may declare a different input space:
///
///   colorspace lin_rec709    (default — flagged for conversion by the consumer)
///   colorspace lin_rec2020   (already in the render working space)
///
/// Aether records the declared input color space on each MaterialDesc
/// (`inputColorSpace`) but performs **no** color conversion itself — that is the
/// consumer's responsibility.
///
/// UsdPreviewSurface / MaterialX aliases (diffuseColor, metallic, roughness, …)
/// are mapped to OpenPBR names; geometry_* aliases map to map_* / opacity.
class MaterialLibrary {
  public:
    /// Load material definitions from a .mtlx file.
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
