#ifndef AETHER_FORMAT_OMMIMPORTER_HPP
#define AETHER_FORMAT_OMMIMPORTER_HPP

#include <filesystem>
#include <optional>

#include "aether/types/OpacityMicromap.hpp"

namespace aether {

/// Parses an Aether opacity-micromap asset (a `.micromap.toml` descriptor plus
/// its OBJ-style `.omm` text sidecar produced by `tools/omm_bake.py`) into the
/// CPU buffers a Vulkan consumer builds a `VkMicromapEXT` from.
///
/// The descriptor carries parameters, the `[data]` sidecar reference and one
/// `[[group]]` table (name + triangle count + usage histogram) per baked group.
/// The sidecar's `G`/`T`/`S` records are packed, per group, into the group's
/// `dataBits` (LSB-first), `triangles` (one `VkMicromapTriangleEXT`-compatible
/// record per non-uniform base triangle) and `micromapIndices` (record ordinal
/// or special, per base triangle). The histogram declared in the descriptor is
/// cross-checked against the records actually parsed.
class OmmImporter {
  public:
    /// Parse @p micromapToml (a `.micromap.toml`) and its referenced `.omm`.
    /// Paths in the descriptor are resolved relative to the descriptor file.
    /// Returns std::nullopt if either file cannot be opened/parsed or the
    /// records are inconsistent with the declared histogram / triangle count.
    [[nodiscard]] static std::optional<OpacityMicromapData> parse(const std::filesystem::path& micromapToml);
};

} // namespace aether
#endif // AETHER_FORMAT_OMMIMPORTER_HPP
