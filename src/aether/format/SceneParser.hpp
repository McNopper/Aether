#ifndef AETHER_FORMAT_SCENEPARSER_HPP
#define AETHER_FORMAT_SCENEPARSER_HPP

#include <filesystem>
#include <optional>
#include <string>

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
/// NODE GRAPH (optional hierarchy, NH1)
/// ────────────────────────────────────
///   [[node]]   optional ordered array of scene-graph nodes
///     id = "..."                           (required, unique; child `parent`
///                                          refs address nodes by it)
///     parent = "..."                       (optional; absent/empty = root)
///     translate, rotate, rotate_x/y/z, scale   (the same shared TRS keys as
///                                          an instance block; same semantics)
///     mesh = "name"                        (optional; absent = pure grouping
///                                          node that places nothing itself)
///     material / materials                 (optional, exactly the instance
///                                          material bindings)
///
/// Resolution: the parser validates the graph — duplicate node id, unknown
/// parent id, or a parent cycle are parse ERRORS naming the offending id
/// (reported through the std::string* overload below; parse() then returns
/// nullopt) — and compounds parent * child TRS depth-first, following the
/// instance convention (T × R × S; world = parent_world * local, so the child
/// translation is rotated and scaled into the parent frame). Every node with
/// a mesh becomes one flat instance with its world-space TRS; several nodes
/// may reference the same mesh (instanced subtrees). COEXISTENCE: flat
/// [[instance]] blocks always parse exactly as before and remain first in
/// SceneDesc::instances (declaration order); node-derived instances are
/// appended after them, depth-first with children in declaration order. A
/// node-free scene resolves to the identical flat instance list as before.
///
/// ANIMATION (optional keyframed channels, ANI1 + tick clock ANI2)
/// ────────────────────────────────────────────────────────────────────────
///   [animation_clock]  optional tick-clock table (REQUIRED when any
///                      [[animation]] track exists — a scene with tracks and
///                      no clock, or a non-positive clock, is a parse error)
///     fps = 30          (positive ticks-per-second; the deterministic
///                       animation time base, independent of the render rate)
///
///   [[animation]]   optional ordered array of keyframed tracks
///     target = "nodes/<id>/<parameter>"  (required; `parameter` is
///                translation | rotation | scale, optionally extended to a
///                scalar COMPONENT target "nodes/<id>/<parameter>/<component>"
///                — translation/scale components x|y|z, rotation components
///                x|y|z|w (x, y, z, w order); an unknown node, parameter or
///                component is a parse error, and the node id must name a
///                [[node]] entry)
///     interpolation = "step" | "linear" | "cubic_spline"
///                                     (optional, default "linear"; glTF 2.0
///                                     sampler semantics, 1:1)
///     times  = [ ... ]               (finite, strictly ascending)
///     values = [ ... ]               (flat floats; `arity` per key — 3 for
///                translation and scale, 4 for rotation (x,y,z,w), 1 for a
///                component target; a cubic_spline track carries 3x that per
///                key: inTangent, value, outTangent)
///
/// The parsed track binds to its `[[node]]` by id (stored as a node index —
/// unknown node, unknown parameter or component, unknown interpolation token,
/// non-finite values, non-ascending times, wrong value arity, or an empty
/// track is a parse ERROR naming the target). Component tracks drive ONE TRS
/// scalar with the same policies as their parent parameter; a rotation
/// component write renormalizes the whole quaternion. Validity at evaluation:
/// samples clamp to [times.front, times.back] (loop/wrap reserved); rotation
/// keys are normalized at load and every sampled quaternion renormalized
/// (linear rotation blends as shortest-arc slerp); scale components floor
/// away from zero. EVALUATION IS AETHER-SIDE — the renderers advance the
/// clock and call aether::SceneAnimator::evaluate(scene, t) (seconds), or
/// evaluateAtTick(scene, tick) over the [animation_clock] fps clock
/// (seconds = tick / fps — the deterministic tick-based time base), which
/// samples every track, writes the node-local TRS and re-runs the node-graph
/// compounding, and returns the fully resolved flat instance list; they never
/// see keyframes. Scenes without [[animation]] parse and evaluate exactly as
/// before.
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

    /// As parse(), additionally reporting the reason for a structural rejection
    /// in @p errorMessage (node-graph errors: duplicate node id, unknown parent,
    /// cycle — each naming the offending node id; animation errors: malformed
    /// target, unknown node / parameter, unknown interpolation, non-finite
    /// values, non-ascending times, wrong value arity — each naming the
    /// track's target). File-open failures leave @p errorMessage untouched.
    [[nodiscard]] static std::optional<SceneDesc> parse(const std::filesystem::path& sceneFile,
                                                        std::string* errorMessage);
};

} // namespace aether
#endif // AETHER_FORMAT_SCENEPARSER_HPP
