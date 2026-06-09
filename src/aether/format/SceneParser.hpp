#pragma once

#include <filesystem>
#include <optional>

#include "aether/types/SceneDesc.hpp"

namespace aether {

/// Parses a `.scene` scene-definition file into an `aether::SceneDesc`.
///
/// FORMAT OVERVIEW
/// ───────────────
/// Line-based text format inspired by — but distinct from — Wavefront OBJ/MTL.
/// Lines starting with '#' are comments. Asset paths (mtllib, instance, env_map)
/// are recorded **relative**, exactly as written; the consumer resolves them
/// against its asset directory.
///
/// Global keywords:  mtllib, spp, max_depth, env_map, env_unit_nits, ev100,
///                   tonemapper
/// Block keywords:   camera, instance <obj>, sphere <r>, box <hx> <hy> <hz>
/// Block modifiers:  translate, rotate (qx qy qz qw), rotate_y <deg>,
///                   usemtl, material <group> <name>, scale,
///                   look_at, up, vfov
///
/// Transform composition follows the glTF convention: T × R × S.
/// Unrecognised keywords are silently ignored, matching OBJ/MTL behaviour.
///
/// SceneParser is renderer-agnostic: it neither loads referenced material
/// libraries / OBJ files nor uploads anything to the GPU. It only records what
/// the file declares.
class SceneParser {
  public:
    /// Parse @p sceneFile into a SceneDesc.
    /// Returns std::nullopt only if the file cannot be opened.
    [[nodiscard]] static std::optional<SceneDesc> parse(const std::filesystem::path& sceneFile);
};

} // namespace aether
