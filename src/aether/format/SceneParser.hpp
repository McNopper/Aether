#pragma once

#include <filesystem>
#include <optional>

#include "aether/types/SceneDesc.hpp"

namespace aether {

/// Parses a `<name>.scene.toml` scene-definition file into an `aether::SceneDesc`.
///
/// FORMAT OVERVIEW
/// ───────────────
/// TOML document with full-word keys (chosen for human + agent clarity). Asset
/// paths (material_libraries, mesh `path`, environment_map) are recorded
/// **relative**, exactly as written; the consumer resolves them against its
/// asset directory.
///
///   material_libraries = ["name.materials.toml", ...]  # single string also ok
///
///   [render]        samples_per_pixel, max_depth, environment_map,
///                   environment_unit_nits,
///                   working_color_space ("lin_rec2020_scene" | "lin_rec709_scene")
///   [camera]        translate, rotate ([qx,qy,qz,qw]), rotate_x, rotate_y, rotate_z,
///                   scale, vertical_field_of_view, ev100   (rotate wins over Euler;
///                   Euler composed XYZ: q = qz * qy * qx). The camera's placement is
///                   a plain TRS transform — the *same* shared keys and semantics as an
///                   instance block (see below); SceneParser does not derive a look-at
///                   direction or view matrix, that is the consumer's job. `scale` is
///                   parsed for parity with instances but has no defined camera meaning.
///   [tonemap]       tonemapper   (raw keyword token, e.g. "agx")
///   [post_tonemap]   renderer     (raw keyword token, e.g. "green_screen")
///
/// REFERENCE + OVERRIDE
/// ────────────────────
/// The [render], [camera] and [tonemap] sections may each carry a
///   reference = "presets/<name>.<group>.toml"
/// key pointing at a standalone preset file (top-level keys, same shape as the
/// inline section). The referenced file is applied first as a base, then any
/// inline keys override it ("local wins"). Identical settings shared across
/// scenes therefore live in a single deduplicated preset file. References are
/// resolved relative to the scene file's directory; a missing/malformed
/// reference falls back to the inline keys only.
///
/// GEOMETRY: meshes + instances (instancing model)
/// ───────────────────────────────────────────────
/// Geometry is declared as reusable **meshes** (listed once, like
/// `material_libraries`) and placed as **instances** that reference a mesh by
/// name. A mesh carries only object-space geometry — no transform, no material.
/// An instance carries the placement TRS and the material assignment. This lets
/// the consumer import / upload / accelerate each unique mesh a single time and
/// instance it many times (true GPU instancing).
///
///   [[mesh]]   ordered array of unique mesh declarations
///     shared:    name = "..."   (unique; referenced by instances)
///     object:    path = "mesh.obj"          (default when `path` is given)
///     box:       type = "box", half_extents = [hx,hy,hz]
///     sphere:    type = "sphere", radius = r
///
///   [[instance]]   ordered array of placed instances
///     mesh = "name"                        (references a [[mesh]] name)
///     material = "Mat"                     (whole-mesh override)
///       or
///     materials = { Group = "Mat", ... }   (per-OBJ-group overrides)
///     translate, rotate ([qx,qy,qz,qw]), rotate_x, rotate_y, rotate_z,
///     scale (number or [x,y,z])   (rotate wins over Euler; XYZ order)
///
/// Transform composition follows the glTF convention: T × R × S.
/// Unknown keys / mesh types are silently ignored.
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
