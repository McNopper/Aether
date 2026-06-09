#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "aether/types/MeshData.hpp"

namespace aether {

/// Parses a Wavefront OBJ file into geometry-only mesh groups.
///
/// OBJ files are treated as **pure geometry**: their own `mtllib` / `usemtl`
/// material directives are intentionally NOT imported (material assignment is
/// the scene file's job). `usemtl`, `o` and `g` still act as mesh boundaries so
/// a multi-material OBJ splits into separately-assignable sub-meshes.
///
/// Vertices are emitted in **object/local space** (no world transform) and are
/// deduplicated. Only triangles are supported; non-triangle faces are skipped.
class ObjImporter {
  public:
    /// Parse @p path into one or more named mesh groups.
    /// Returns std::nullopt only if the file cannot be opened or a face
    /// references an invalid vertex index.
    [[nodiscard]] static std::optional<std::vector<MeshGroup>> parse(const std::filesystem::path& path);
};

} // namespace aether
