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
/// paths (material_libraries, mesh, environment_map) are recorded **relative**,
/// exactly as written; the consumer resolves them against its asset directory.
///
///   material_libraries = ["name.materials.toml", ...]  # single string also ok
///
///   [render]        samples_per_pixel, max_depth, environment_map,
///                   environment_unit_nits,
///                   working_color_space ("lin_rec2020_scene" | "lin_rec709_scene")
///   [camera]        translate, rotate ([qx,qy,qz,qw]), rotate_x, rotate_y, rotate_z,
///                   scale, vertical_field_of_view, ev100   (rotate wins over Euler;
///                   Euler composed XYZ: q = qz * qy * qx). The camera's placement is
///                   a plain TRS transform — the *same* shared keys and semantics as a
///                   geometry block (see below); SceneParser does not derive a look-at
///                   direction or view matrix, that is the consumer's job. `scale` is
///                   parsed for parity with geometry but has no defined camera meaning.
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
///   [[geometry]]   ordered array; type = "instance" | "box" | "sphere"
///     instance:  mesh = "...", materials = { Group = "Mat", ... }
///     box:       half_extents = [hx,hy,hz]
///     sphere:    radius = r
///     shared:    material, translate, rotate ([qx,qy,qz,qw]), rotate_x, rotate_y,
///                rotate_z, scale (number or [x,y,z])   (rotate wins over Euler; XYZ order)
///
/// Transform composition follows the glTF convention: T × R × S.
/// Unknown keys / geometry types are silently ignored.
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
