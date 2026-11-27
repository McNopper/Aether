bl_info = {
    "name": "Aether Scene Exporter",
    "author": "Aether Tools",
    "version": (1, 6, 0),
    "blender": (5, 2, 0),
    "location": "File > Export > Aether Scene (.scene.toml)",
    "description": "Export Blender scenes to Aether scene/animation/camera/OBJ files",
    "category": "Import-Export",
    "support": "COMMUNITY",
}

import json
import math
import re
import struct
from bisect import bisect_right
from collections.abc import Iterable, Sequence
from typing import ClassVar
from pathlib import Path

import bpy
from bpy.props import BoolProperty, EnumProperty, FloatProperty, IntProperty, StringProperty
from bpy.types import Operator
from bpy_extras.io_utils import ExportHelper
from mathutils import Euler, Matrix, Quaternion, Vector

# Blender is Z-up / -Y-forward; Aether (like glTF) is Y-up / -Z-forward.
# CONV is the axis swizzle (x, y, z)_blender -> (x, z, -y)_aether, i.e. a -90°
# rotation about X. Mesh vertices are exported in this Y-up local frame (via the
# OBJ exporter's up/forward axes); object transforms are conjugated by CONV and
# camera world positions/directions are mapped by CONV. The conjugation
# distributes over composition — (CONV A CONV^-1)(CONV B CONV^-1) =
# CONV (AB) CONV^-1 — so conjugating every node's LOCAL matrix compounds down
# the Aether node graph to exactly the conjugated Blender world transforms.
CONV = Matrix.Rotation(math.radians(-90.0), 4, "X")
# The same axis swizzle as a quaternion, as (w, x, y, z) parts — used to
# conjugate animated rotation keys (and their tangent 4-vectors) into the
# Aether frame exactly like the static node matrices above. The quaternion
# product is spelled out in plain Python doubles on purpose: mathutils'
# Quaternion products are float32 in Blender 5.x, and animated keys conjugate
# exactly, not to float32 precision. CONV is a -90° X rotation, so the parts
# are cos/sin of half that angle.
_CONV_HALF_ANGLE = math.radians(-90.0) / 2.0
Q_CONV_PARTS = (math.cos(_CONV_HALF_ANGLE), math.sin(_CONV_HALF_ANGLE), 0.0, 0.0)
Q_CONV_INV_PARTS = (Q_CONV_PARTS[0], -Q_CONV_PARTS[1], 0.0, 0.0)


def _quat_product(left, right):
    # Hamilton product, double precision.
    lw, lx, ly, lz = left
    rw, rx, ry, rz = right
    return (
        lw * rw - lx * rx - ly * ry - lz * rz,
        lw * rx + lx * rw + ly * rz - lz * ry,
        lw * ry - lx * rz + ly * rw + lz * rx,
        lw * rz + lx * ry - ly * rx + lz * rw,
    )


def _conjugate_rotation(x, y, z, w):
    # Q_CONV ⊗ q ⊗ Q_CONV⁻¹ (Blender → Aether). The product is bilinear in the
    # four components, so non-unit tangent 4-vectors conjugate exactly too.
    quat = _quat_product(_quat_product(Q_CONV_PARTS, (w, x, y, z)), Q_CONV_INV_PARTS)
    return [quat[1], quat[2], quat[3], quat[0]]


def _premultiply_rotation(x, y, z, w):
    # Q_CONV ⊗ q - the rotation part of CONV @ R(q) (Blender → Aether CAMERA
    # form; see the camera note in _node_block). Also bilinear, so tangent
    # 4-vectors premultiply exactly too.
    quat = _quat_product(Q_CONV_PARTS, (w, x, y, z))
    return [quat[1], quat[2], quat[3], quat[0]]


def _quote(value: str) -> str:
    return json.dumps(value)


class _TomlFloat:
    """A float that is already reduced to its shortest TOML text."""

    def __init__(self, text: str) -> None:
        self.text = text


def _single_precision_text(value: float) -> _TomlFloat:
    """The shortest decimal that parses back to the single-precision value
    Blender stores (a float property is 32 bit), written as a float literal:
    0.0012 stays 0.0012 instead of the 0.0012000000569969416 its widened form
    prints as, and 220 stays 220.0."""
    packed = struct.pack("<f", value)
    # A whole number reads best in plain notation (220.0, not 2.2e+02).
    candidates = [f"{value:.1f}"] if float(value).is_integer() and abs(value) < 1.0e15 else []
    candidates += [f"{value:.{digits}g}" for digits in range(1, 10)]
    for text in candidates:
        if struct.pack("<f", float(text)) == packed:
            if "e" not in text and "." not in text:
                text += ".0"
            return _TomlFloat(text)
    return _TomlFloat(repr(value))


def _format_value(value) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int) and not isinstance(value, bool):
        return str(value)
    if isinstance(value, _TomlFloat):
        return value.text
    if isinstance(value, float):
        return repr(float(value))
    if isinstance(value, str):
        return _quote(value)
    if isinstance(value, Path):
        return _quote(str(value))
    if isinstance(value, dict):
        # TOML inline table: { k = v, ... } (the [[mesh]] bounds block).
        return "{ " + ", ".join(f"{key} = {_format_value(v)}" for key, v in value.items()) + " }"
    if isinstance(value, Sequence):
        return "[" + ", ".join(_format_value(item) for item in value) + "]"
    raise TypeError(f"Unsupported TOML value: {type(value)!r}")


def _sanitize_filename(name: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9_.-]+", "_", name).strip("._")
    return cleaned or "object"


def _scene_stem(path: Path) -> str:
    name = path.name
    if name.endswith(".scene.toml"):
        return name[: -len(".scene.toml")]
    return path.stem


def _camera_path(scene_path: Path) -> Path:
    return scene_path.with_name(f"{_scene_stem(scene_path)}.camera.toml")


def _obj_export_path(export_dir: Path, obj_name: str, used: set[str]) -> Path:
    base = _sanitize_filename(obj_name)
    candidate = base
    suffix = 1
    while candidate in used:
        suffix += 1
        candidate = f"{base}_{suffix}"
    used.add(candidate)
    return export_dir / f"{candidate}.obj"


def _select_only(objects: Iterable[bpy.types.Object]) -> None:
    bpy.ops.object.select_all(action="DESELECT")
    for obj in objects:
        obj.select_set(True)


def _obj_stats(obj_path: Path) -> dict[str, object] | None:
    """What the exported OBJ IS, read back from the FILE (post axis-conversion,
    post modifiers): the BV1 object-space AABB over its `v` lines plus the
    [meta.link] counts - triangle count (`f` lines; the export triangulates,
    so every face is a triangle) and group count (usemtl splits, 1 when the
    file carries none) - and the ``usemtl`` names in first-occurrence order
    (the sub-mesh labels the node's ``materials`` map is keyed by). None when
    the file has no vertices."""
    lo = [math.inf, math.inf, math.inf]
    hi = [-math.inf, -math.inf, -math.inf]
    vertices = 0
    triangles = 0
    groups = 0
    usemtl: list[str] = []
    try:
        with open(obj_path, encoding="utf-8", errors="replace") as handle:
            for line in handle:
                if line.startswith("v "):
                    parts = line.split()
                    if len(parts) < 4:
                        continue
                    try:
                        xyz = [float(parts[1]), float(parts[2]), float(parts[3])]
                    except ValueError:
                        continue
                    vertices += 1
                    for axis in range(3):
                        lo[axis] = min(lo[axis], xyz[axis])
                        hi[axis] = max(hi[axis], xyz[axis])
                elif line.startswith("f "):
                    triangles += 1
                elif line.startswith("usemtl "):
                    groups += 1
                    name = line[len("usemtl ") :].strip()
                    if name and name not in usemtl:
                        usemtl.append(name)
    except OSError:
        return None
    if vertices == 0:
        return None
    return {"min": lo, "max": hi, "triangles": triangles, "groups": max(1, groups), "usemtl": usemtl}


def _export_obj_file(obj: bpy.types.Object, obj_path: Path, export_materials: bool = False) -> None:
    prev_active = bpy.context.view_layer.objects.active
    prev_selected = [o for o in bpy.context.selected_objects]
    # Export the mesh in LOCAL space (not baked): temporarily clear the world
    # transform so the OBJ holds local geometry. World placement (scale, rotate,
    # translate) is exported separately in the scene TRS. The OBJ exporter's
    # up/forward axes put the local mesh into the Y-up frame.
    saved_matrix = obj.matrix_world.copy()

    try:
        obj.matrix_world = Matrix.Identity(4)
        bpy.context.view_layer.update()

        _select_only([obj])
        bpy.context.view_layer.objects.active = obj

        if hasattr(bpy.ops.wm, "obj_export"):
            bpy.ops.wm.obj_export(
                filepath=str(obj_path),
                export_selected_objects=True,
                forward_axis="NEGATIVE_Z",
                up_axis="Y",
                path_mode="AUTO",
                # Multi-slot objects need the usemtl group lines - they are the
                # keys of the [[node]] materials map (single-slot objects keep
                # the plain node-level material binding instead).
                export_materials=export_materials,
                apply_modifiers=True,
                # Parity with aether::ObjImporter (triangles only) - the
                # deformation row expansion re-reads this file's faces.
                export_triangulated_mesh=True,
            )
        elif hasattr(bpy.ops.export_scene, "obj"):
            bpy.ops.export_scene.obj(
                filepath=str(obj_path),
                use_selection=True,
                axis_forward="-Z",
                axis_up="Y",
                path_mode="AUTO",
                use_materials=export_materials,
                use_mesh_modifiers=True,
                use_triangulation=True,
            )
        else:
            raise RuntimeError("No OBJ exporter is available in this Blender build")
        if export_materials:
            # The usemtl lines are all Aether needs: ObjImporter splits the OBJ
            # into sub-meshes at each one and keeps its name as the label the
            # scene's material map addresses; the material DEFINITIONS are
            # ignored by design. The sidecar .mtl is Blender's non-OpenPBR dump -
            # materials are authored in TOML, so the file is dropped and its
            # dangling mtllib line removed.
            mtl_path = obj_path.with_suffix(".mtl")
            if mtl_path.exists():
                mtl_path.unlink()
            text = obj_path.read_text(encoding="utf-8", errors="replace")
            filtered = "".join(
                line for line in text.splitlines(keepends=True) if not line.startswith("mtllib ")
            )
            obj_path.write_text(filtered, encoding="utf-8", newline="")
    finally:
        obj.matrix_world = saved_matrix
        bpy.context.view_layer.update()
        bpy.ops.object.select_all(action="DESELECT")
        for selected in prev_selected:
            if bpy.data.objects.get(selected.name) is not None:
                selected.select_set(True)
        if prev_active and bpy.data.objects.get(prev_active.name) is not None:
            bpy.context.view_layer.objects.active = prev_active


def _camera_block(camera: bpy.types.Object, ev100: float, node_id: str | None = None) -> list[str]:
    # Camera world transform mapped Blender Z-up -> Aether Y-up. A camera's
    # placement is a plain TRS (identical keys to a mesh instance): translate +
    # rotate. The scene format accepts rotate (quat [qx,qy,qz,qw]) or
    # rotate_x/y/z (Euler); the quaternion is emitted for a lossless export.
    # ANI8: when the camera object is itself part of the exported node graph,
    # `node = "<id>"` binds the section to it - the renderers re-place the
    # camera from this node's evaluated world every animation tick (animated
    # camera), while the static TRS below stays the load-time fallback/preview.
    world = CONV @ camera.matrix_world
    location = world.translation
    rotation = world.to_quaternion()
    vfov = math.degrees(getattr(camera.data, "angle_y", camera.data.angle))
    lines = []
    if node_id is not None:
        lines.append(f"node = {_quote(node_id)}")
    lines.extend(
        [
            f"translate = [{location.x}, {location.y}, {location.z}]",
            f"rotate = [{rotation.x}, {rotation.y}, {rotation.z}, {rotation.w}]",
            f"vertical_field_of_view = {vfov}",
            f"ev100 = {ev100}",
        ]
    )
    return lines


def _write_simple_toml(path: Path, lines: list[str]) -> None:
    path.write_text("\n".join(lines).rstrip() + "\n", encoding="utf-8")


def _hierarchy_depth(obj: bpy.types.Object) -> int:
    # Number of ancestors of an object. Used to sort the export list into a
    # stable parent-before-child node order (a parent always has fewer hops).
    depth = 0
    parent = obj.parent
    while parent is not None:
        depth += 1
        parent = parent.parent
    return depth


# Relative tolerance for the TRS round-trip check below. Blender matrices are
# single precision, so the epsilon must scale with the matrix magnitude.
_TRS_EPSILON = 1e-4


def _decompose_checked(
    matrix: Matrix, name: str, warnings: list[str]
) -> tuple[Vector, Quaternion, Vector]:
    # Decompose a (conjugated) local matrix into TRS and prove the round-trip:
    # recomposing the parts must reproduce the matrix within an epsilon scaled
    # by its magnitude. Shear — or any component a quaternion TRS cannot
    # express, including negative scale — is not representable in the node
    # format; the decomposed TRS is exported anyway and a warning is emitted.
    location, rotation, scale = matrix.decompose()
    recomposed = Matrix.LocRotScale(location, rotation, scale)
    magnitude = max(1.0, max(abs(value) for row in matrix for value in row))
    delta = max(
        abs(left - right)
        for matrix_row, recomposed_row in zip(matrix, recomposed)
        for left, right in zip(matrix_row, recomposed_row)
    )
    if delta > _TRS_EPSILON * magnitude:
        warnings.append(
            f"{name}: transform has shear or a non-TRS component (recomposed "
            f"delta {delta:.3g}); the decomposed TRS is exported — shear is "
            "not representable in the node format"
        )
    if min(scale) < 0.0:
        warnings.append(
            f"{name}: negative scale (mirrored transform) — the decomposed "
            "TRS preserves it, but it flips triangle winding downstream; "
            "verify the mesh renders as intended"
        )
    return location, rotation, scale


def _node_block(
    obj: bpy.types.Object,
    mesh_name: str | None,
    parent_id: str | None,
    warnings: list[str],
    used_materials: list[str],
) -> dict[str, object]:
    # Local transform relative to the exported parent: matrix_local is
    # Blender's parent-relative matrix (it includes the parent inverse, so
    # parent.matrix_world @ matrix_local reproduces matrix_world exactly).
    # A node whose Blender parent is not exported becomes a root and bakes the
    # world matrix instead, which keeps placement correct for subtree exports.
    # Conjugating every local matrix by CONV distributes over composition, so
    # the Aether parser's per-level T*R*S compounding reproduces the conjugated
    # Blender world transforms against the Y-up local mesh geometry (see CONV).
    # ANI8 CAMERA exception: a camera node's payload is its OPTICAL AXIS, and
    # conjugation swizzles the local axes too (local -Z lands on -Y), while the
    # consumers map `forward = rotation * (0, 0, -1)` - exactly what the static
    # [camera] block's PRE-multiplied CONV @ R form satisfies. So camera nodes
    # pre-multiply instead of conjugating: the compounded world then comes out
    # as CONV @ R_chain (position identical either way - only the rotation
    # differs) and the aim is the true view direction. A camera parented to
    # ANOTHER camera breaks the telescoping product and is not supported.
    local = obj.matrix_local if parent_id is not None else obj.matrix_world
    # (ternary keeps the camera form one line - see the CAMERA note above)
    local = CONV @ local if obj.type == "CAMERA" else CONV @ local @ CONV.inverted()
    location, rotation, scale = _decompose_checked(local, obj.name, warnings)
    block: dict[str, object] = {"id": obj.name}
    if parent_id is not None:
        block["parent"] = parent_id
    block["translate"] = [location.x, location.y, location.z]
    block["rotate"] = [rotation.x, rotation.y, rotation.z, rotation.w]
    block["scale"] = [scale.x, scale.y, scale.z]
    if mesh_name is not None:
        block["mesh"] = mesh_name
        slots = [m for m in obj.data.materials if m is not None]
        if len(slots) > 1:
            # NAME-only bindings (NEVER parameters - Blender materials are not
            # OpenPBR and are deliberately not converted, see tools/README).
            # The keys are the OBJ's `usemtl` names - the labels Aether's
            # ObjImporter gives the sub-meshes. They come from the exported
            # FILE, not from the object's slot list: Blender writes a usemtl
            # line only for a slot that owns faces, and a binding for an unused
            # slot would name no sub-mesh (the scene loader diagnoses and
            # ignores it).
            block["material"] = (
                obj.active_material.name if obj.active_material else slots[0].name
            )
            if used_materials:
                block["materials"] = {name: name for name in used_materials}
        elif obj.active_material:
            block["material"] = obj.active_material.name
    # META2 (agent-legible context, never affects rendering): custom property
    # `description` (string) and `tags` (comma-separated string or list).
    raw_description = obj.get("description")
    if isinstance(raw_description, str) and raw_description.strip():
        block["description"] = raw_description.strip()
    raw_tags = obj.get("tags")
    if raw_tags:
        tag_list = (
            [tag.strip() for tag in raw_tags.split(",") if tag.strip()]
            if isinstance(raw_tags, str)
            else [str(tag) for tag in raw_tags]
        )
        if tag_list:
            block["tags"] = tag_list
    return block


# ── Animation export (NH4b): F-curves → [[animation]] tracks, 1:1 ───────────
# Blender F-curves are exported KEY FOR KEY — the exporter never samples or
# bakes. A scalar Blender F-curve is ONE channel of a vector ANI1 track; per-key
# interpolation variance splits it into consecutive same-mode [[animation]]
# entries on the same target (the Aether evaluator gives a later-declared track
# precedence from its first key onwards, and split boundaries share their
# boundary key, so concatenated tracks reproduce the F-curve exactly).
#
# Frames → seconds: ANI1 times are SECONDS. Blender's frame-rate model is
# `render.fps` frames per `render.fps_base` seconds, so the effective rate is
# `fps / fps_base` and time = frame · fps_base / fps. The rate is emitted as
# the scene's ANI2 tick clock ([animation_clock] fps), so Aether tick N at the
# exported clock is exactly Blender frame N; the conversion is documented in
# tools/README.md.
#
# Frames → axes: animated values are exported in the Aether frame exactly like
# the static node TRS (CONV conjugation): a translation key (x, y, z)_blender
# becomes (x, z, -y)_aether, a scale key (x, y, z) becomes (x, z, y), and a
# rotation key q becomes Q_CONV ⊗ q ⊗ Q_CONV⁻¹. CONV is a fixed rotation, so
# the conjugation distributes over the animation exactly as it does over the
# static node graph.
#
# Tangents: a Blender BEZIER segment is the cubic Bézier over
# (co, handle_right | handle_left, co); as a Hermite over the segment duration
# dt that is  m_out = 3·(handle_right.y - co.y) / dt  and
# m_in = 3·(co.y - handle_left.y) / dt  (value per SECOND) — the derivative
# form the ANI1 cubic_spline evaluator multiplies by dt. Key values are always
# exact; the segment SHAPE is the glTF-style uniform-parameterization Hermite,
# which is exact when the handle x-deltas are proportional to the segment
# (auto/clamped handles) and the same approximation every glTF exporter makes
# against Blender ≥ 3.0's x-parameterized Bézier evaluation.
#
# Component key times: a property whose component F-curves share identical
# key frames exports as ONE vector track per mode segment (the block-keyed
# common case; a missing component contributes its static value, which is
# exact — Blender evaluates a channel without an F-curve as its static
# value). When component key frames differ, the property exports as
# PER-COMPONENT scalar tracks on the registry's component targets
# (nodes/<id>/<parameter>/<component> — translation/scale: x|y|z, rotation:
# x|y|z|w): one track per same-mode run of each component's own curve,
# keyed 1:1 (no resampling). A property is never MIXED — either vector
# tracks or component tracks, because a whole-parameter track and component
# tracks on one parameter would interact through declaration-order
# precedence. A quaternion component cannot be conjugated in isolation, so
# its keys are the conjugated component of the normalized Blender pose at
# the key frame (the other channels contribute what Blender shows there)
# and its tangents are one-sided differences of that scalar; translation/
# scale components swizzle/negate exactly like their vector values.
# Aether-side policy: a rotation-component write renormalizes the whole
# quaternion after the scalar write.
#
# Rotation: only the curves matching the object's rotation_mode drive the pose
# (QUATERNION → rotation_quaternion channels (w, x, y, z); an Euler mode →
# rotation_euler channels in that Euler order). Euler keys are converted to
# quaternion keys at the UNION of the three curves' key frames — key count is
# preserved (no resampling): at a foreign frame a channel contributes the value
# Blender itself shows there (its F-curve evaluated and clamped, or its static
# value when the channel has no curve). In-between, Blender blends the euler
# channels component-wise before converting, while ANI1 slerps quaternions —
# key poses are exact, in-between motion differs slightly (schema property).

_ANIM_INTERPOLATION = {"CONSTANT": "step", "LINEAR": "linear", "BEZIER": "cubic_spline"}
_ANIM_ARITY = {"translation": 3, "rotation": 4, "scale": 3}


def _target_arity(target: str) -> int:
    # Whole-parameter targets end in the parameter name; component targets
    # (nodes/<id>/<parameter>/<component>) are scalar — the registry's
    # component rows are arity 1.
    return _ANIM_ARITY.get(target.rsplit("/", 1)[1], 1)


def _effective_fps(scene: bpy.types.Scene, warnings: list[str]) -> float:
    # Blender frame-rate model: one frame lasts `fps_base / fps` seconds.
    fps = float(scene.render.fps)
    fps_base = float(scene.render.fps_base)
    if fps <= 0.0:
        warnings.append(f"{scene.name}: non-positive render fps — frame→second conversion falls back to 24 fps")
        return 24.0
    return fps / max(fps_base, 1e-9)


def _action_fcurves(animation_data: bpy.types.AnimData, obj_name: str, warnings: list[str]) -> list[bpy.types.FCurve]:
    # Blender 4.4+ slotted actions (Blender 5.2: the only kind) keep their
    # F-curves per slot in a channelbag owned by a layer's strip; legacy
    # (pre-4.4) unslotted actions keep action.fcurves.
    action = animation_data.action
    layers = getattr(action, "layers", None)
    if layers:
        if len(layers) > 1:
            warnings.append(f"{obj_name}: layered action — only the first layer's F-curves are exported")
        slot = animation_data.action_slot
        for strip in layers[0].strips:
            bag = strip.channelbag(slot) if slot is not None and hasattr(strip, "channelbag") else None
            if bag is None and hasattr(strip, "channelbags") and len(strip.channelbags):
                bag = strip.channelbags[0]
            if bag is not None:
                return list(bag.fcurves)
        return []
    fcurves = getattr(action, "fcurves", None)
    return list(fcurves) if fcurves else []


def _fcurve_keys(fcurve: bpy.types.FCurve) -> list[dict[str, float]] | None:
    """Ordered per-key frame / value / handle values of an F-curve; None when
    the curve is keyless (a keyless F-curve drives its channel to 0 in Blender
    — never exported) or violates the ANI1 time rules (duplicate or non-finite
    keys)."""
    if not fcurve.keyframe_points:
        return None
    keys: list[dict[str, float]] = []
    previous: float | None = None
    for key in fcurve.keyframe_points:
        frame = float(key.co.x)
        value = float(key.co.y)
        if not math.isfinite(frame) or not math.isfinite(value):
            return None
        if previous is not None and frame <= previous:
            return None
        keys.append(
            {
                "frame": frame,
                "value": value,
                "in_y": float(key.handle_left.y),
                "out_y": float(key.handle_right.y),
            }
        )
        previous = frame
    return keys


def _mode_runs(segment_modes: Sequence[str]) -> list[tuple[int, int]]:
    # Runs of consecutive segments sharing one interpolation mode; run (a, b)
    # covers segments a..b, i.e. keys a..b+1. Splitting at every mode change
    # keeps each emitted track single-mode (one ANI1 interpolation token).
    runs: list[tuple[int, int]] = []
    start = 0
    for index in range(1, len(segment_modes)):
        if segment_modes[index] != segment_modes[index - 1]:
            runs.append((start, index - 1))
            start = index
    runs.append((start, len(segment_modes) - 1))
    return runs


def _track_interpolation(
    mode: str, warned_modes: set[str], obj_name: str, target: str, warnings: list[str]
) -> str:
    known = _ANIM_INTERPOLATION.get(mode)
    if known is not None:
        return known
    # Easing-curve modes (SINE .. BOUNCE) have no ANI1 equivalent: their keys
    # are exact, the in-between shape is approximated by the handle tangents.
    if mode not in warned_modes:
        warned_modes.add(mode)
        warnings.append(
            f"{obj_name}: key interpolation '{mode}' has no ANI1 equivalent "
            f"on {target} — exported as cubic_spline (keys exact, in-between approximated)"
        )
    return "cubic_spline"


def _animation_block(
    target: str, interpolation: str, times: list[float], values: list[float], warnings: list[str]
) -> dict[str, object] | None:
    # Emit-time self-check against the ANI1 parse rules: the exporter must
    # never write a track the Aether parser would reject.
    arity = _target_arity(target)
    stride = arity * (3 if interpolation == "cubic_spline" else 1)
    if (
        not times
        or len(values) != len(times) * stride
        or any(not math.isfinite(t) for t in times)
        or any(not math.isfinite(v) for v in values)
        or any(times[i - 1] >= times[i] for i in range(1, len(times)))
    ):
        warnings.append(f"{target}: track failed its ANI1 self-check and was dropped")
        return None
    return {"target": target, "interpolation": interpolation, "times": times, "values": values}


def _swizzle_translation(values: Sequence[float]) -> list[float]:
    return [values[0], values[2], -values[1]]


def _swizzle_scale(values: Sequence[float]) -> list[float]:
    return [values[0], values[2], values[1]]


def _passthrough_value(components: Sequence[float]) -> list[float]:
    return list(components)


def _passthrough_tangent(components: Sequence[float], tangent: Sequence[float]) -> list[float]:
    return list(tangent)


# Per-component emission (mismatched component key times): the Aether
# component letter and value sign for each Blender array index, matching the
# vector-path swizzles exactly — translation (x, y, z)_blender ->
# (x, z, -y)_aether (Blender Y negates into Aether Z), scale -> (x, z, y).
_COMPONENT_OF_INDEX = {
    "translation": (("x", 1.0), ("z", -1.0), ("y", 1.0)),
    "scale": (("x", 1.0), ("z", 1.0), ("y", 1.0)),
}


def _quaternion_value(components: Sequence[float], map_rot=_conjugate_rotation) -> list[float]:
    # Blender rotation_quaternion channels are (w, x, y, z); the schema wants
    # (x, y, z, w). Component lerp can leave the key slightly off unit length,
    # so the key is normalized before the rotation map (the parser normalizes
    # too; a degenerate zero quaternion maps to identity, matching sm::normalize).
    w, x, y, z = components
    norm = math.sqrt(w * w + x * x + y * y + z * z)
    if norm == 0.0:
        return map_rot(0.0, 0.0, 0.0, 1.0)
    return map_rot(x / norm, y / norm, z / norm, w / norm)


def _rotation_tangent(
    components: Sequence[float], tangent: Sequence[float], map_rot=_conjugate_rotation
) -> list[float]:
    # Tangent components (dw, dx, dy, dz) in Blender channel order (w, x, y, z).
    del components  # both maps are linear in the four components
    return map_rot(tangent[1], tangent[2], tangent[3], tangent[0])


def _run_tracks(
    target: str,
    rows: list[dict[str, object]],
    segment_modes: list[str],
    fps: float,
    to_value,
    to_tangent,
    arity: int,
    obj_name: str,
    warnings: list[str],
    warned_modes: set[str],
) -> list[dict[str, object]]:
    # One [[animation]] entry per same-mode run of segments (see the section
    # comment for the split rule and the declaration-order precedence). A row
    # carries the key's frame, per-component values and handle y-values
    # ("in_y"/"out_y", None where the channel has no key here); a row MAY
    # carry "rates" — per-component (in, out) tangents in value-per-second —
    # instead, which replaces the handle derivation (used by the
    # per-component quaternion path, whose scalar tangents are not
    # handle-derivable).
    if len(rows) == 1:
        # A single key is a constant track (ANI1: one key holds everywhere —
        # the same constant Blender's clamped evaluation shows).
        block = _animation_block(target, "linear", [rows[0]["frame"] / fps], to_value(rows[0]["values"]), warnings)
        return [block] if block else []
    tracks: list[dict[str, object]] = []
    for first_segment, last_segment in _mode_runs(segment_modes):
        first_key = first_segment
        last_key = last_segment + 1
        interpolation = _track_interpolation(
            segment_modes[first_segment], warned_modes, obj_name, target, warnings
        )
        run_rows = rows[first_key : last_key + 1]
        times = [row["frame"] / fps for row in run_rows]
        values: list[float] = []
        if interpolation == "cubic_spline":
            for local, row in enumerate(run_rows):
                # Hermite reconstruction of Blender's bezier handles: the
                # tangent is the value-per-second derivative over the run's
                # segment duration (a run-boundary key only uses the segment
                # inside its own track — the neighbour track re-derives it).
                dt_out = (
                    times[local + 1] - times[local]
                    if local + 1 < len(times)
                    else times[local] - times[local - 1]
                )
                dt_in = times[local] - times[local - 1] if local > 0 else dt_out
                in_tangent = []
                out_tangent = []
                for component in range(arity):
                    rates = row.get("rates")
                    if rates is not None:
                        # Direct (in, out) rates per second, already in the
                        # emitted value space.
                        in_rate, out_rate = rates[component]
                        in_tangent.append(in_rate)
                        out_tangent.append(out_rate)
                        continue
                    value = row["values"][component]
                    # Components without a key at this frame (static or
                    # union-evaluated) contribute zero tangents: constants.
                    out_tangent.append(
                        0.0 if row["out_y"][component] is None else 3.0 * (row["out_y"][component] - value) / dt_out
                    )
                    in_tangent.append(
                        0.0 if row["in_y"][component] is None else 3.0 * (value - row["in_y"][component]) / dt_in
                    )
                values.extend(to_tangent(row["values"], in_tangent))
                values.extend(to_value(row["values"]))
                values.extend(to_tangent(row["values"], out_tangent))
        else:
            for row in run_rows:
                values.extend(to_value(row["values"]))
        block = _animation_block(target, interpolation, times, values, warnings)
        if block is not None:
            tracks.append(block)
    return tracks


def _scalar_property_tracks(
    obj: bpy.types.Object,
    curves: dict[int, bpy.types.FCurve],
    parameter: str,
    static,
    swizzle,
    fps: float,
    warnings: list[str],
    warned_modes: set[str],
) -> list[dict[str, object]]:
    # location / scale: components in array-index order. Exported as vector
    # tracks when every present component curve keys the SAME frames
    # (block-keyed; a missing component contributes its static value);
    # mismatched component key frames export per-component scalar tracks
    # instead (see the section comment — a property is never mixed).
    target = f"nodes/{obj.name}/{parameter}"
    keys_by_index: dict[int, list[dict[str, float]]] = {}
    for index, fcurve in sorted(curves.items()):
        keys = _fcurve_keys(fcurve)
        if keys is None:
            warnings.append(f"{obj.name}.{fcurve.data_path}[{index}]: keyless or invalid F-curve is not exported")
            continue
        keys_by_index[index] = keys
    if not keys_by_index:
        return []
    reference_index = min(keys_by_index)
    reference_curve = curves[reference_index]
    frames = [key["frame"] for key in keys_by_index[reference_index]]
    if any([key["frame"] for key in keys] != frames for keys in keys_by_index.values()):
        return _component_property_tracks(obj, curves, keys_by_index, parameter, fps, warnings, warned_modes)
    modes = [key.interpolation for key in reference_curve.keyframe_points]
    for index, curve in curves.items():
        if index in keys_by_index and [key.interpolation for key in curve.keyframe_points] != modes:
            warnings.append(
                f"{obj.name}: {parameter} components disagree on per-key interpolation — "
                "the mode segments follow the lowest-index channel"
            )
            break
    rows: list[dict[str, object]] = []
    for position, frame in enumerate(frames):
        values = []
        in_y = []
        out_y = []
        for index in range(len(static)):
            key = keys_by_index[index][position] if index in keys_by_index else None
            values.append(key["value"] if key is not None else float(static[index]))
            in_y.append(key["in_y"] if key is not None else None)
            out_y.append(key["out_y"] if key is not None else None)
        rows.append({"frame": frame, "values": values, "in_y": in_y, "out_y": out_y})

    def to_value(components: Sequence[float]) -> list[float]:
        return swizzle(components)

    def to_tangent(components: Sequence[float], tangent: Sequence[float]) -> list[float]:
        return swizzle(tangent)

    return _run_tracks(
        target,
        rows,
        modes[:-1],
        fps,
        to_value,
        to_tangent,
        len(static),
        obj.name,
        warnings,
        warned_modes,
    )


def _component_property_tracks(
    obj: bpy.types.Object,
    curves: dict[int, bpy.types.FCurve],
    keys_by_index: dict[int, list[dict[str, float]]],
    parameter: str,
    fps: float,
    warnings: list[str],
    warned_modes: set[str],
) -> list[dict[str, object]]:
    # Mismatched component key times (location / scale): each component
    # F-curve exports as its own scalar tracks on the registry's component
    # target, keyed 1:1 against that curve — key frames, values, handles and
    # per-key interpolation modes are the component curve's own (no
    # resampling, no cross-component agreement needed). The swizzle and the
    # translation's Y negation are applied to the values AND the handle
    # y-values up front, so the tangent derivation matches the vector path
    # exactly. Components without an F-curve emit no track — the node's
    # static TRS stands for them (exact: Blender evaluates a channel without
    # an F-curve at its static value). Components are emitted in Blender
    # channel-index order; component tracks of DIFFERENT components never
    # override each other (distinct registry targets).
    tracks: list[dict[str, object]] = []
    for index, keys in sorted(keys_by_index.items()):
        letter, sign = _COMPONENT_OF_INDEX[parameter][index]
        target = f"nodes/{obj.name}/{parameter}/{letter}"
        modes = [key.interpolation for key in curves[index].keyframe_points]
        rows = [
            {
                "frame": key["frame"],
                "values": [sign * key["value"]],
                "in_y": [sign * key["in_y"]],
                "out_y": [sign * key["out_y"]],
            }
            for key in keys
        ]
        tracks.extend(
            _run_tracks(
                target,
                rows,
                modes[:-1],
                fps,
                _passthrough_value,
                _passthrough_tangent,
                1,
                obj.name,
                warnings,
                warned_modes,
            )
        )
    return tracks


def _rotation_tracks(
    obj: bpy.types.Object,
    curves: dict[int, bpy.types.FCurve],
    is_quaternion: bool,
    fps: float,
    warnings: list[str],
    warned_modes: set[str],
) -> list[dict[str, object]]:
    # The node rotation binding is a quaternion, so rotation exports as one
    # (x, y, z, w) quaternion track per mode segment. Quaternion-mode objects
    # use the rotation_quaternion channels (w, x, y, z); Euler-mode objects
    # convert their euler keys at the UNION of the curves' key frames.
    # Quaternion components keyed at DIFFERENT frames export per-component
    # scalar tracks instead (below) — the euler union stays the euler path's
    # contract unchanged.
    target = f"nodes/{obj.name}/rotation"
    arity = 4 if is_quaternion else 3
    keys_by_index: dict[int, list[dict[str, float]]] = {}
    for index, fcurve in sorted(curves.items()):
        keys = _fcurve_keys(fcurve)
        if keys is None:
            warnings.append(f"{obj.name}.{fcurve.data_path}[{index}]: keyless or invalid F-curve is not exported")
            continue
        keys_by_index[index] = keys
    if not keys_by_index:
        return []
    if is_quaternion:
        reference_frames = [key["frame"] for key in keys_by_index[min(keys_by_index)]]
        if any([key["frame"] for key in keys] != reference_frames for keys in keys_by_index.values()):
            return _quaternion_component_tracks(obj, curves, keys_by_index, fps, warnings, warned_modes)
    union_frames = sorted({key["frame"] for keys in keys_by_index.values() for key in keys})
    keyed = {index: {key["frame"]: key for key in keys} for index, keys in keys_by_index.items()}
    static = obj.rotation_quaternion if is_quaternion else obj.rotation_euler
    rows: list[dict[str, object]] = []
    for frame in union_frames:
        values = []
        in_y = []
        out_y = []
        for index in range(arity):
            key = keyed.get(index, {}).get(frame)
            if key is not None:
                values.append(key["value"])
                in_y.append(key["in_y"])
                out_y.append(key["out_y"])
            else:
                # No key here: the channel contributes exactly what Blender
                # shows at this frame — its F-curve evaluated (clamped outside
                # its key range), or the static channel value without a curve.
                fcurve = curves.get(index)
                values.append(float(fcurve.evaluate(frame)) if fcurve is not None else float(static[index]))
                in_y.append(None)
                out_y.append(None)
        rows.append({"frame": frame, "values": values, "in_y": in_y, "out_y": out_y})
    # Segment modes follow the lowest-index present curve, at the segment in
    # force at each union frame (a curve is not required to key every union
    # frame; block-keyed curves — the common case — align exactly).
    reference_index = min(keys_by_index)
    reference_frames = [key["frame"] for key in keys_by_index[reference_index]]
    reference_modes = [key.interpolation for key in curves[reference_index].keyframe_points]
    segment_modes = []
    for frame in union_frames[:-1]:
        position = max(bisect_right(reference_frames, frame) - 1, 0)
        segment_modes.append(reference_modes[position] if reference_modes else "LINEAR")

    # ANI8: camera nodes carry the PRE-multiplied camera form (see _node_block),
    # so their rotation keys/tangents map through the same form instead of the
    # node-graph conjugation. Both maps are linear in the four components.
    map_rot = _premultiply_rotation if obj.type == "CAMERA" else _conjugate_rotation

    if is_quaternion:

        def to_value(components: Sequence[float]) -> list[float]:
            return _quaternion_value(components, map_rot)

        def to_tangent(components: Sequence[float], tangent: Sequence[float]) -> list[float]:
            return _rotation_tangent(components, tangent, map_rot)

    else:

        def quat_parts(angles: Sequence[float]) -> tuple[float, float, float, float]:
            quat = Euler((angles[0], angles[1], angles[2]), obj.rotation_mode).to_quaternion()
            return (quat.x, quat.y, quat.z, quat.w)

        def to_value(components: Sequence[float]) -> list[float]:
            # Blender applies the euler angles in the object's rotation order.
            return map_rot(*quat_parts(components))

        def to_tangent(components: Sequence[float], tangent: Sequence[float]) -> list[float]:
            # dq/dt of the euler→quaternion conversion for the per-channel
            # euler rates: central difference per angle (exact to float
            # precision), mapped like the value.
            h = 1e-4
            derivative = [0.0, 0.0, 0.0, 0.0]
            for index in range(3):
                plus = list(components)
                minus = list(components)
                plus[index] += h
                minus[index] -= h
                upper = quat_parts(plus)
                lower = quat_parts(minus)
                scale = 0.5 / h * float(tangent[index])
                derivative = [derivative[c] + (upper[c] - lower[c]) * scale for c in range(4)]
            return map_rot(*derivative)

    return _run_tracks(
        target,
        rows,
        segment_modes,
        fps,
        to_value,
        to_tangent,
        arity,
        obj.name,
        warnings,
        warned_modes,
    )


def _quaternion_component_tracks(
    obj: bpy.types.Object,
    curves: dict[int, bpy.types.FCurve],
    keys_by_index: dict[int, list[dict[str, float]]],
    fps: float,
    warnings: list[str],
    warned_modes: set[str],
) -> list[dict[str, object]]:
    # rotation_quaternion components keyed at different frames: each channel
    # exports as its own scalar tracks on the registry's rotation component
    # targets, keyed 1:1 against its curve (no resampling — no
    # union-of-times rewriting; the union stays the EULER path's contract).
    # A quaternion component cannot be conjugated in isolation (the
    # conjugation mixes all four components), so a key's emitted scalar is
    # the Aether component of the normalized conjugated Blender pose at the
    # key frame: the keyed channel contributes its key value, every other
    # channel what Blender itself shows there (its F-curve evaluated and
    # clamped, or the static channel value without a curve — the same
    # contribution rule as the euler union). Tangents are one-sided
    # differences of that emitted scalar, value per second — not
    # handle-derivable for the same mixing reason. Aether renormalizes the
    # whole quaternion after a component write (registry policy), so the
    # emitted keys reconstruct the normalized pose exactly whenever the
    # sibling components stand at their Blender values. Channels without a
    # curve emit no track — the node's static TRS stands for them.
    keyed = {index: {key["frame"]: key for key in keys} for index, keys in keys_by_index.items()}
    static = obj.rotation_quaternion

    def emitted_component(frame: float, channel: int) -> float:
        parts = []
        for index in range(4):
            key = keyed.get(index, {}).get(frame)
            if key is not None:
                parts.append(key["value"])
                continue
            fcurve = curves.get(index)
            parts.append(float(fcurve.evaluate(frame)) if fcurve is not None else float(static[index]))
        # _quaternion_value conjugates the (w, x, y, z) pose into schema
        # order (x, y, z, w); map the channel to its slot in that order.
        return _quaternion_value(parts)[(3, 0, 1, 2)[channel]]

    seconds_per_frame = 1.0 / fps
    frame_epsilon = 1.0e-2  # frames; one-sided difference of the scalar
    tracks: list[dict[str, object]] = []
    for channel, keys in sorted(keys_by_index.items()):
        target = f"nodes/{obj.name}/rotation/{'wxyz'[channel]}"
        modes = [key.interpolation for key in curves[channel].keyframe_points]
        rows: list[dict[str, object]] = []
        for key in keys:
            frame = key["frame"]
            value = emitted_component(frame, channel)
            in_rate = (value - emitted_component(frame - frame_epsilon, channel)) / (frame_epsilon * seconds_per_frame)
            out_rate = (emitted_component(frame + frame_epsilon, channel) - value) / (frame_epsilon * seconds_per_frame)
            rows.append({"frame": frame, "values": [value], "rates": [(in_rate, out_rate)]})
        tracks.extend(
            _run_tracks(
                target,
                rows,
                modes[:-1],
                fps,
                _passthrough_value,
                _passthrough_tangent,
                1,
                obj.name,
                warnings,
                warned_modes,
            )
        )
    return tracks


def _animation_tracks(obj: bpy.types.Object, fps: float, warnings: list[str]) -> list[dict[str, object]]:
    # All [[animation]] entries of one exported object, in emission order
    # translation → rotation → scale (segment runs of one target stay
    # consecutive, which fixes the declaration-order precedence).
    animation_data = obj.animation_data
    if animation_data is None or animation_data.action is None:
        return []
    if len(animation_data.nla_tracks):
        warnings.append(f"{obj.name}: NLA tracks present — only the active action's F-curves are exported")
    quaternion_mode = obj.rotation_mode == "QUATERNION"
    driven_path = "rotation_quaternion" if quaternion_mode else "rotation_euler"
    ignored_path = "rotation_euler" if quaternion_mode else "rotation_quaternion"
    by_path: dict[str, dict[int, bpy.types.FCurve]] = {}
    for fcurve in _action_fcurves(animation_data, obj.name, warnings):
        if fcurve.data_path in ("location", "rotation_euler", "rotation_quaternion", "scale"):
            by_path.setdefault(fcurve.data_path, {})[fcurve.array_index] = fcurve
        # Other data paths (constraints, deltas, shape keys) are not node TRS.
    if not by_path.get(driven_path) and by_path.get(ignored_path):
        warnings.append(
            f"{obj.name}: rotation is F-curve animated as '{ignored_path}' but rotation_mode is "
            f"'{obj.rotation_mode}' — those curves do not drive the pose and are not exported"
        )
    warned_modes: set[str] = set()
    tracks: list[dict[str, object]] = []
    tracks.extend(
        _scalar_property_tracks(
            obj, by_path.get("location", {}), "translation", obj.location, _swizzle_translation, fps, warnings, warned_modes
        )
    )
    tracks.extend(
        _rotation_tracks(obj, by_path.get(driven_path, {}), quaternion_mode, fps, warnings, warned_modes)
    )
    tracks.extend(
        _scalar_property_tracks(
            obj, by_path.get("scale", {}), "scale", obj.scale, _swizzle_scale, fps, warnings, warned_modes
        )
    )
    return tracks


def _generator_field() -> str:
    # The [meta] generator token: "blender_to_aether/<bl_info version>",
    # formatted "x.y.z".
    return "blender_to_aether/" + ".".join(str(part) for part in bl_info["version"])


def _write_scene_toml(
    scene_path: Path,
    meshes: list[dict[str, object]],
    nodes: list[dict[str, object]],
    animations: list[dict[str, object]],
    animation_fps: float,
    camera_reference: str | None,
    material_libraries: list[str],
    render_reference: str | None,
    tonemap_reference: str | None,
    skins: list[dict[str, object]] | None = None,
    morphs: list[dict[str, object]] | None = None,
    render_keys: dict[str, object] | None = None,
    tonemap_keys: dict[str, object] | None = None,
    post_tonemap_reference: str | None = None,
    post_tonemap_keys: dict[str, object] | None = None,
    haze_keys: dict[str, object] | None = None,
    meta_keys: dict[str, object] | None = None,
    meta_links: list[tuple[str, dict[str, object]]] | None = None,
) -> None:
    lines: list[str] = []
    if material_libraries:
        lines.append(f'material_libraries = [{", ".join(_quote(item) for item in material_libraries)}]')
        lines.append("")
    # Inline keys OVERRIDE the referenced preset (the parser applies the
    # reference first, then the local keys - "local wins").
    if render_reference or render_keys:
        lines.append("[render]")
        if render_reference:
            lines.append(f"reference = {_quote(render_reference)}")
        for key, value in (render_keys or {}).items():
            lines.append(f"{key} = {_format_value(value)}")
        lines.append("")
    if camera_reference:
        lines.extend(["[camera]", f"reference = {_quote(camera_reference)}", ""])
    if tonemap_reference or tonemap_keys:
        lines.append("[tonemap]")
        if tonemap_reference:
            lines.append(f"reference = {_quote(tonemap_reference)}")
        for key, value in (tonemap_keys or {}).items():
            lines.append(f"{key} = {_format_value(value)}")
        lines.append("")
    if post_tonemap_reference or post_tonemap_keys:
        lines.append("[post_tonemap]")
        if post_tonemap_reference:
            lines.append(f"reference = {_quote(post_tonemap_reference)}")
        for key, value in (post_tonemap_keys or {}).items():
            lines.append(f"{key} = {_format_value(value)}")
        lines.append("")
    if haze_keys:
        lines.append("[haze]")
        for key, value in haze_keys.items():
            lines.append(f"{key} = {_format_value(value)}")
        lines.append("")
    if meshes or nodes:
        # Provenance for agents reading the file: which tool wrote it. Only
        # when the scene has content (an empty export stays headerless).
        lines.append("[meta]")
        lines.append(f"generator = {_quote(_generator_field())}")
        for key, value in (meta_keys or {}).items():
            lines.append(f"{key} = {_format_value(value)}")
        lines.append("")
        # [meta.link."..."] per linked file: agent summaries the exporter
        # actually knows (mesh stats from the written OBJ, explicitly-provided
        # env, referenced material libraries) - never data it would have to
        # guess.
        for path, body in meta_links or []:
            lines.append(f"[meta.link.{_quote(path)}]")
            for key, value in body.items():
                lines.append(f"{key} = {_format_value(value)}")
            lines.append("")
    for mesh in meshes:
        lines.append("[[mesh]]")
        for key, value in mesh.items():
            lines.append(f"{key} = {_format_value(value)}")
        lines.append("")
    for block in nodes:
        lines.append("[[node]]")
        for key, value in block.items():
            lines.append(f"{key} = {_format_value(value)}")
        lines.append("")
    # ANI4/ANI5: deformation blocks (the parser binds skins/morphs against the
    # completed node + mesh lists regardless of section order).
    for block in skins or []:
        lines.append("[[skin]]")
        for key, value in block.items():
            lines.append(f"{key} = {_format_value(value)}")
        lines.append("")
    for block in morphs or []:
        lines.append("[[morph]]")
        for key, value in block.items():
            lines.append(f"{key} = {_format_value(value)}")
        lines.append("")
    if animations:
        # ANI2: the animation tick clock ([animation_clock] fps) — required
        # (positive) whenever [[animation]] tracks exist, and it is exactly
        # the rate the times were converted with: times are seconds, computed
        # as frame / fps from Blender's render.fps / render.fps_base, so
        # Aether tick N at this clock is Blender frame N.
        lines.extend(["[animation_clock]", f"fps = {_format_value(animation_fps)}", ""])
        for block in animations:
            lines.append("[[animation]]")
            for key, value in block.items():
                lines.append(f"{key} = {_format_value(value)}")
            lines.append("")
    _write_simple_toml(scene_path, lines)


# ════════════════════════════════════════════════════════════════════════════
# Skeletal pose-bone animation export (blender_to_aether 1.6.0):
#   Blender `pose.bones["<bone>"].<channel>` F-curves (the skeletal animation
#   an animator authors) become whole-parameter ANI1 tracks on the exported
#   bone nodes. A bone node's animated local is rest_local @ matrix_basis(t)
#   (glTF-style pose composition), which is EXACTLY expressible per key:
#     translation = t_rest + R_rest * swizzle(t_basis)
#     rotation    = q_rest (x) conjugate(q_basis)
#     scale       = s_rest * swizzle(s_basis)
#   because the maps are linear (affine) in each channel group - and left
#   quaternion multiplication commutes with BOTH slerp and the Hermite basis,
#   so Aether's interpolation of the composed keys reproduces
#   rest (x) interpolate(basis) exactly (tangents map through the same linear
#   map; the euler path central-differences the composed map like the object
#   path does). Per parameter group all KEYED components must share key frames
#   and interpolation modes (unkeyed components take their saved pose value);
#   otherwise the group is skipped with a warning - the NH4b never-sample
#   contract holds: keys stay 1:1 with Blender's.
# ════════════════════════════════════════════════════════════════════════════

_POSE_PATH = re.compile(r'pose\.bones\["([^"]+)"\]\.(location|rotation_euler|rotation_quaternion|scale)$')


def _all_action_fcurves(animation_data: bpy.types.AnimData, obj_name: str, warnings: list[str]):
    # Blender 4.4+ slotted actions keep pose channels in a DIFFERENT slot than
    # the object transform, so every channelbag of the first layer is scanned
    # (the object-TRS path keeps using its own slot via _action_fcurves).
    action = animation_data.action
    layers = getattr(action, "layers", None)
    if layers:
        if len(layers) > 1:
            warnings.append(f"{obj_name}: layered action - only the first layer's pose F-curves are exported")
        curves: list[bpy.types.FCurve] = []
        for strip in layers[0].strips:
            channelbags = getattr(strip, "channelbags", None)
            if channelbags:
                for bag in channelbags:
                    curves.extend(bag.fcurves)
        return curves
    fcurves = getattr(action, "fcurves", None)
    return list(fcurves) if fcurves else []


def _left_compose_rotation(rest_xyzw, quat_xyzw):
    """q_rest (x) q - Hamilton product with both quaternions in (x, y, z, w)."""
    left = (rest_xyzw[3], rest_xyzw[0], rest_xyzw[1], rest_xyzw[2])
    right = (quat_xyzw[3], quat_xyzw[0], quat_xyzw[1], quat_xyzw[2])
    out = _quat_product(left, right)
    return [out[1], out[2], out[3], out[0]]


def _pose_tracks(
    arm: "bpy.types.Object",
    arm_exported: bool,
    fps: float,
    warnings: list[str],
    warned_modes: set[str],
) -> list[dict[str, object]]:
    """Skeletal pose-bone animation: compose `pose.bones[...]` channels onto
    each bone node's rest local (see the section comment) and emit the result
    as whole-parameter ANI1 tracks through the shared _run_tracks machinery."""
    animation_data = arm.animation_data
    if animation_data is None or animation_data.action is None:
        return []
    if len(animation_data.nla_tracks):
        warnings.append(f"{arm.name}: NLA tracks present - only the active action's pose F-curves are exported")

    grouped: dict[tuple[str, str], dict[int, bpy.types.FCurve]] = {}
    for fcurve in _all_action_fcurves(animation_data, arm.name, warnings):
        match = _POSE_PATH.match(fcurve.data_path)
        if match is None:
            if fcurve.data_path.startswith("pose."):
                warnings.append(f"{arm.name}: unsupported pose F-curve path '{fcurve.data_path}' (skipped)")
            continue
        grouped.setdefault((match.group(1), match.group(2)), {})[fcurve.array_index] = fcurve

    tracks: list[dict[str, object]] = []
    rotation_exported: set[str] = set()
    for (bone_name, param), components in grouped.items():
        bone = arm.data.bones.get(bone_name)
        pose_bone = arm.pose.bones.get(bone_name)
        if bone is None or pose_bone is None:
            warnings.append(f"{arm.name}: pose channel for unknown bone '{bone_name}' skipped")
            continue

        if param in ("rotation_euler", "rotation_quaternion"):
            # The channel name carries the mode (the regex captures it whole).
            quat_mode = param == "rotation_quaternion"
            static = list(pose_bone.rotation_quaternion) if quat_mode else list(pose_bone.rotation_euler)
            arity = 4 if quat_mode else 3
            target_param = "rotation"
            if bone_name in rotation_exported:
                warnings.append(
                    f"{arm.name}: pose.{bone_name} has both euler and quaternion rotation curves - "
                    "only the first mode is exported"
                )
                continue
            rotation_exported.add(bone_name)
        elif param == "location":
            quat_mode = False
            static = list(pose_bone.location)
            arity = 3
            target_param = "translation"
        else:  # scale
            quat_mode = False
            static = list(pose_bone.scale)
            arity = 3
            target_param = "scale"

        keys_by_index: dict[int, list[dict[str, float]]] = {}
        for index, fcurve in sorted(components.items()):
            keys = _fcurve_keys(fcurve)
            if keys is None:
                warnings.append(
                    f"{arm.name}.pose.{bone_name}.{param}[{index}]: keyless or invalid F-curve skipped"
                )
                continue
            keys_by_index[index] = keys
        if not keys_by_index:
            continue
        reference = min(keys_by_index)
        frames = [key["frame"] for key in keys_by_index[reference]]
        if any([key["frame"] for key in keys] != frames for keys in keys_by_index.values()):
            warnings.append(
                f"{arm.name}: pose.{bone_name}.{param} components have mismatched key times - "
                "group skipped (composition needs aligned keys, no sampling)"
            )
            continue
        modes = [point.interpolation for point in components[reference].keyframe_points]
        if any(
            [point.interpolation for point in components[index].keyframe_points] != modes
            for index in keys_by_index
        ):
            warnings.append(
                f"{arm.name}: pose.{bone_name}.{param} components disagree on per-key interpolation - "
                "group skipped"
            )
            continue

        # Rows carry the RAW Blender values (unkeyed components take their
        # saved-pose static, handles None -> zero tangent), exactly like the
        # object whole-parameter path; the mappers below compose.
        rows: list[dict[str, object]] = []
        for position, frame in enumerate(frames):
            values: list[float] = []
            in_y: list[float | None] = []
            out_y: list[float | None] = []
            for index in range(arity):
                key = keys_by_index[index][position] if index in keys_by_index else None
                values.append(key["value"] if key is not None else float(static[index]))
                in_y.append(key["in_y"] if key is not None else None)
                out_y.append(key["out_y"] if key is not None else None)
            rows.append({"frame": frame, "values": values, "in_y": in_y, "out_y": out_y})

        rest = _bone_rest_local(arm, bone, arm_exported)
        rest_t = rest.translation
        rest_q = rest.to_quaternion()
        rest_parts = [rest_q.x, rest_q.y, rest_q.z, rest_q.w]
        rest_scale = rest.to_scale()
        rot3 = rest.to_3x3()

        if param == "location":

            def to_value(components: Sequence[float]) -> list[float]:
                swizzled = _swizzle_translation(list(components))
                moved = rot3 @ Vector(swizzled)
                return [rest_t.x + moved.x, rest_t.y + moved.y, rest_t.z + moved.z]

            def to_tangent(components: Sequence[float], tangent: Sequence[float]) -> list[float]:
                del components  # the map is affine: tangents see the linear part only
                moved = rot3 @ Vector(_swizzle_translation(list(tangent)))
                return [moved.x, moved.y, moved.z]

        elif param == "scale":

            def to_value(components: Sequence[float]) -> list[float]:
                swizzled = _swizzle_scale(list(components))
                return [rest_scale.x * swizzled[0], rest_scale.y * swizzled[1], rest_scale.z * swizzled[2]]

            def to_tangent(components: Sequence[float], tangent: Sequence[float]) -> list[float]:
                del components
                swizzled = _swizzle_scale(list(tangent))
                return [rest_scale.x * swizzled[0], rest_scale.y * swizzled[1], rest_scale.z * swizzled[2]]

        elif quat_mode:

            def to_value(components: Sequence[float]) -> list[float]:
                w, x, y, z = components
                norm = math.sqrt(w * w + x * x + y * y + z * z)
                if norm == 0.0:
                    conj = _conjugate_rotation(0.0, 0.0, 0.0, 1.0)
                else:
                    conj = _conjugate_rotation(x / norm, y / norm, z / norm, w / norm)
                return _left_compose_rotation(rest_parts, conj)

            def to_tangent(components: Sequence[float], tangent: Sequence[float]) -> list[float]:
                del components  # linear map: no normalization on tangents
                conj = _conjugate_rotation(tangent[1], tangent[2], tangent[3], tangent[0])
                return _left_compose_rotation(rest_parts, conj)

        else:  # euler rotation (Blender applies the angles in the pose mode's order)
            euler_mode = pose_bone.rotation_mode

            def quat_parts(angles: Sequence[float]) -> tuple[float, float, float, float]:
                quat = Euler((angles[0], angles[1], angles[2]), euler_mode).to_quaternion()
                return (quat.x, quat.y, quat.z, quat.w)

            def compose(angles: Sequence[float]) -> list[float]:
                conj = _conjugate_rotation(*quat_parts(angles))
                return _left_compose_rotation(rest_parts, conj)

            def to_value(components: Sequence[float]) -> list[float]:
                return compose(list(components))

            def to_tangent(components: Sequence[float], tangent: Sequence[float]) -> list[float]:
                # Central difference of the COMPOSED map (same pattern as the
                # object euler path - the composition is smooth in the angles).
                h = 1e-4
                derivative = [0.0, 0.0, 0.0, 0.0]
                for index in range(3):
                    plus = list(components)
                    minus = list(components)
                    plus[index] += h
                    minus[index] -= h
                    upper = compose(plus)
                    lower = compose(minus)
                    scale = 0.5 / h * float(tangent[index])
                    derivative = [derivative[c] + (upper[c] - lower[c]) * scale for c in range(4)]
                return derivative

        target = f"nodes/{_bone_id(arm.name, bone_name)}/{target_param}"
        tracks.extend(
            _run_tracks(
                target,
                rows,
                modes[:-1],
                fps,
                to_value,
                to_tangent,
                arity,
                arm.name,
                warnings,
                warned_modes,
            )
        )
    return tracks


# ════════════════════════════════════════════════════════════════════════════
# ANI4/ANI5 deformation export (blender_to_aether 1.4.0):
#   * armature bones -> NH1 nodes (ids "<arm>::<bone>"; root bones parent to
#     the armature object node, child bones to their parent bone; locals are
#     the armature-space rest chains conjugated by CONV, so the compounded
#     Aether worlds equal CONV(armature_world @ bone.matrix_local)) and a
#     [[skin]] block (joints in arm.data.bones order, inverse_bind = inverse
#     of that world in the Aether frame, per-imported-vertex top-4 vertex-group
#     weights), with blend = "lbs" | "dqs" from the operator.
#   * shape keys -> [[morph]] blocks (basis deltas in the OBJ/Y-up frame,
#     expanded per Aether-imported vertex corner; the exported default weight
#     is the key's value at export time) + ANI1 weight tracks
#     (morphs/<mesh>/<name>, 1:1 from the shape-key action's value F-curves).
# The per-imported-vertex expansion mirrors aether::ObjImporter: face order,
# TRIANGLES only - it re-reads the OBJ the exporter just wrote.
# ════════════════════════════════════════════════════════════════════════════

_BONE_SEP = "::"


def _bone_id(arm_name: str, bone_name: str) -> str:
    return f"{arm_name}{_BONE_SEP}{bone_name}"


def _bone_parent_depth(bone: "bpy.types.Bone") -> int:
    depth = 0
    parent = bone.parent
    while parent is not None:
        depth += 1
        parent = parent.parent
    return depth


def _bone_rest_local(arm: "bpy.types.Object", bone: "bpy.types.Bone", arm_exported: bool) -> Matrix:
    """The exported (CONV-conjugated) REST local of one bone - the bone node's
    static TRS and the base every pose track composes onto. Root bones frame to
    the armature object node when it is exported (its Aether world is always
    CONV(arm.matrix_world), so the compound reproduces the armature-space rest
    exactly); otherwise the armature world is baked like _node_block roots."""
    if bone.parent is None:
        raw = bone.matrix_local.copy() if arm_exported else arm.matrix_world @ bone.matrix_local
    else:
        raw = bone.parent.matrix_local.inverted() @ bone.matrix_local
    return CONV @ raw @ CONV.inverted()


def _armature_deform_setup(
    arm: "bpy.types.Object",
    arm_exported: bool,
    skin_blend: str,
    warnings: list[str],
) -> tuple[list[dict[str, object]], list[str], list[list[float]]]:
    """Bone nodes (parents before children), the joint-id list in arm.data.bones
    order, and that order's inverse_bind rows (16 row-major floats each)."""
    bones = list(arm.data.bones)
    nodes: list[dict[str, object]] = []
    for bone in sorted(bones, key=_bone_parent_depth):
        parent_bone = bone.parent
        if parent_bone is None:
            parent_id = arm.name if arm_exported else None
        else:
            parent_id = _bone_id(arm.name, parent_bone.name)
        local = _bone_rest_local(arm, bone, arm_exported)
        location, rotation, scale = _decompose_checked(local, f"{arm.name}::{bone.name}", warnings)
        block: dict[str, object] = {"id": _bone_id(arm.name, bone.name)}
        if parent_id is not None:
            block["parent"] = parent_id
        block["translate"] = [location.x, location.y, location.z]
        block["rotate"] = [rotation.x, rotation.y, rotation.z, rotation.w]
        block["scale"] = [scale.x, scale.y, scale.z]
        nodes.append(block)

    joints = [_bone_id(arm.name, bone.name) for bone in bones]
    # inverse_bind = inverse(joint world in the Aether frame). The conjugation
    # distributes, so CONV(arm.matrix_world @ bone.matrix_local) is exactly the
    # compounded Aether world of the emitted bone chain (either parent rule).
    inverse_bind: list[list[float]] = []
    arm_world = arm.matrix_world
    for bone in bones:
        joint_world = CONV @ (arm_world @ bone.matrix_local) @ CONV.inverted()
        inverse = joint_world.inverted()
        inverse_bind.append([inverse[row][col] for row in range(4) for col in range(4)])
    return nodes, joints, inverse_bind


def _round_f32(value: float) -> float:
    """Round a float64 value to binary32 (round-to-nearest-even) - one IEEE
    -754 single-precision operation, bit-exact with C++ float arithmetic."""
    return struct.unpack("<f", struct.pack("<f", value))[0]


def _f32(text: str) -> float:
    """Parse a decimal component exactly like C++'s float extraction: the text
    is exact in float64 (OBJ components carry <=17 significant digits), so the
    final rounding to binary32 is the correctly-rounded from_chars/>>
    result - the value dedup below must see the bits ObjImporter sees."""
    return _round_f32(float(text))


def _normalized(values: Sequence[float]) -> bytes:
    """sm::normalize over binary32 components - every op rounded like float
    arithmetic (products/sums of binary32 values are exact in float64, so a
    single rounding per op reproduces the C++ result); zero-length input
    yields (0,0,0), the len > 0 guard of slang-math's normalize. Packed to
    bytes so -0.0f stays distinct from +0.0f exactly like VertexEqual's
    memcmp (Python's == would merge them)."""
    x, y, z = values
    sx = _round_f32(x * x)
    sy = _round_f32(y * y)
    sz = _round_f32(z * z)
    total = _round_f32(_round_f32(sx + sy) + sz)
    length = _round_f32(math.sqrt(total))
    if length > 0.0:
        return struct.pack("<3f", x / length, y / length, z / length)
    return struct.pack("<3f", 0.0, 0.0, 0.0)


def _imported_source_vertices(obj_path: Path, label: str, warnings: list[str]) -> list[int] | None:
    """Blender source-vertex index per AETHER-imported vertex - a faithful
    mirror of aether::ObjImporter (parse + flushMesh). The [[skin]]/[[morph]]
    row space must equal what the importer creates, or validateSkinVertexCounts
    / validateMorphVertexCounts reject the scene at load:

      * faces in file order, TRIANGLES ONLY - the importer drops every other
        arity (a 4th token, e.g. a trailing comment, drops the face too),
      * groups break on o / g / usemtl: dedup is PER GROUP with a fresh map,
        first occurrence wins, first-occurrence order,
      * the dedup key is the RESOLVED vertex VALUES in VertexEqual's memcmp
        domain: binary32 components packed to bytes, normals normalized at
        parse time like sm::normalize (zero-length -> (0,0,0)), missing
        normal -> (0,1,0), missing uv -> (0,0),
      * indices resolve like resolveIndex (>0 = 1-based, <0 = relative to the
        current count, 0 = invalid); an out-of-range POSITION aborts the
        parse (None), an out-of-range normal/uv falls back to the default.

    Returns one 0-based Blender source-vertex index per imported vertex,
    groups concatenated in file order. None on parse failure or no geometry.
    """
    positions: list[bytes] = []
    normals: list[bytes] = []
    texcoords: list[bytes] = []
    out: list[int] = []
    pending: list[tuple[int, bytes]] = []
    skipped = 0
    int32 = re.compile(r"-?[0-9]+$")

    def flush() -> None:
        seen: dict[bytes, int] = {}
        for source, key in pending:
            if key not in seen:
                seen[key] = source  # first occurrence wins; dict order = import order
        out.extend(seen.values())
        pending.clear()

    def resolve(idx: int, count: int) -> int:
        if idx > 0:
            return idx - 1
        if idx < 0:
            return count + idx
        return -1

    def field(fields: list[str], position: int) -> int | None:
        # Absent or empty -> 0 (resolve -> -1 -> the default value); present
        # but malformed -> None (parseInt nullopt aborts the whole import).
        if position >= len(fields) or fields[position] == "":
            return 0
        text = fields[position]
        if not int32.match(text):
            return None
        value = int(text)
        return value if -(2**31) < value < 2**31 else None

    def corner(token: str) -> tuple[int, bytes] | None:
        # parseFaceVertex: 'p', 'p/t', 'p//n', 'p/t/n' (more than 3 fields =
        # parseInt on a field containing '/' fails -> nullopt).
        fields = token.split("/")
        if len(fields) > 3:
            return None
        pos_raw = field(fields, 0)
        tex_raw = field(fields, 1)
        nrm_raw = field(fields, 2)
        if pos_raw is None or tex_raw is None or nrm_raw is None:
            return None
        p = resolve(pos_raw, len(positions))
        if p < 0 or p >= len(positions):
            return None  # flushMesh returns false -> parse failure
        t = resolve(tex_raw, len(texcoords))
        n = resolve(nrm_raw, len(normals))
        uv = texcoords[t] if 0 <= t < len(texcoords) else struct.pack("<2f", 0.0, 0.0)
        normal = normals[n] if 0 <= n < len(normals) else struct.pack("<3f", 0.0, 1.0, 0.0)
        return p, positions[p] + normal + uv

    try:
        with open(obj_path, encoding="utf-8", errors="replace") as handle:
            for line in handle:
                stripped = line.strip()
                if not stripped or stripped.startswith("#"):
                    continue
                fields = stripped.split()
                kw = fields[0]
                if kw in ("v", "vn", "vt"):
                    need = 2 if kw == "vt" else 3
                    if len(fields) < need + 1:
                        continue  # parseVec* fails -> the line is skipped
                    try:
                        values = [_f32(text) for text in fields[1 : need + 1]]
                    except ValueError:
                        continue  # non-numeric component -> the line is skipped
                    if kw == "v":
                        positions.append(struct.pack("<3f", *values))
                    elif kw == "vn":
                        normals.append(_normalized(values))
                    else:
                        texcoords.append(struct.pack("<2f", *values))
                elif kw in ("o", "g", "usemtl"):
                    flush()  # group boundary: dedup restarts per group
                elif kw == "f":
                    corners = fields[1:]
                    if len(corners) != 3:
                        skipped += 1  # non-triangle faces are dropped by the importer
                        continue
                    parsed: list[tuple[int, bytes]] = []
                    for token in corners:
                        item = corner(token)
                        if item is None:
                            warnings.append(
                                f"{label}: OBJ face the Aether parser would reject - deformation skipped"
                            )
                            return None
                        parsed.append(item)
                    pending.extend(parsed)
    except OSError as exc:
        warnings.append(f"{label}: cannot re-read the exported OBJ ({exc})")
        return None
    flush()
    if skipped:
        warnings.append(f"{label}: {skipped} non-triangle face(s) skipped (Aether imports triangles only)")
    if not out:
        warnings.append(f"{label}: no triangle faces in the exported OBJ - deformation skipped")
        return None
    return out


def _vertex_group_joints(
    obj: bpy.types.Object, arm: bpy.types.Object, joints: list[str], warnings: list[str]
) -> dict[int, int]:
    """vertex-group index -> joint index (by matching the group name to the
    armature bone of the same name - the Blender skinning convention). Unknown
    groups are dropped with a warning (their weights simply never bind)."""
    bone_index = {bone.name: index for index, bone in enumerate(arm.data.bones)}
    mapping: dict[int, int] = {}
    dropped: set[str] = set()
    for group in obj.vertex_groups:
        joint = bone_index.get(group.name)
        if joint is not None:
            mapping[group.index] = joint
        else:
            dropped.add(group.name)
    if dropped:
        warnings.append(
            f"{obj.name}: vertex group(s) without a same-named bone skipped: {', '.join(sorted(dropped))}"
        )
    return mapping


def _deformation_blocks(
    obj: bpy.types.Object,
    mesh_name: str,
    obj_path: Path | None,
    arm_exported: bool,
    skin_blend: str,
    fps: float,
    warned_modes: set[str],
    warnings: list[str],
) -> tuple[list[dict[str, object]], list[dict[str, object]], list[dict[str, object]], list[dict[str, object]]]:
    """(extra bone nodes, [[skin]] blocks, [[morph]] blocks, morph weight
    tracks) for one exported mesh object; every list is empty when the object
    carries neither an armature modifier nor shape keys."""
    arm = next(
        (m.object for m in obj.modifiers if m.type == "ARMATURE" and m.object is not None), None
    )
    shape_keys = getattr(obj.data, "shape_keys", None)
    has_keys = shape_keys is not None and len(shape_keys.key_blocks) > 1
    if arm is None and not has_keys:
        return [], [], [], []
    if obj_path is None:
        warnings.append(f"{obj.name}: no exported OBJ of its own - deformation skipped")
        return [], [], [], []
    source = _imported_source_vertices(obj_path, obj.name, warnings)
    if source is None:
        return [], [], [], []

    extra_nodes: list[dict[str, object]] = []
    skins: list[dict[str, object]] = []
    if arm is not None:
        if not arm_exported:
            warnings.append(
                f"{obj.name}: armature '{arm.name}' is not in the export - its bones are "
                "exported as world-rooted nodes"
            )
        extra_nodes, joints, inverse_bind = _armature_deform_setup(arm, arm_exported, skin_blend, warnings)
        group_map = _vertex_group_joints(obj, arm, joints, warnings)
        bone_indices: list[list[int]] = []
        bone_weights: list[list[float]] = []
        for source_vertex in source:
            vertex = obj.data.vertices[source_vertex]
            candidates = [
                (group.group, group.weight)
                for group in vertex.groups
                if group.weight > 0.0 and group.group in group_map
            ]
            candidates.sort(key=lambda item: -item[1])
            top = candidates[:4]
            indices = [group_map[group_index] for group_index, _ in top]
            weights = [weight for _, weight in top]
            bone_indices.append(indices + [0] * (4 - len(indices)))
            bone_weights.append(weights + [0.0] * (4 - len(weights)))
        skins.append(
            {
                "node": obj.name,
                "joints": joints,
                "blend": skin_blend,
                "inverse_bind": inverse_bind,
                "bone_indices": bone_indices,
                "bone_weights": bone_weights,
            }
        )

    morphs: list[dict[str, object]] = []
    morph_tracks: list[dict[str, object]] = []
    if has_keys:
        basis = shape_keys.key_blocks[0]  # Blender's basis key is always first
        conv3 = CONV.to_3x3()
        name_map: dict[str, str] = {}
        for key in list(shape_keys.key_blocks)[1:]:
            emitted = key.name.replace("/", "_")
            if emitted != key.name:
                warnings.append(f"{obj.name}: shape key '{key.name}' exported as '{emitted}' ('/' is not allowed)")
            name_map[key.name] = emitted
            deltas: list[list[float]] = []
            for source_vertex in source:
                delta = conv3 @ (key.data[source_vertex].co - basis.data[source_vertex].co)
                deltas.append([delta.x, delta.y, delta.z])
            morphs.append(
                {
                    "mesh": mesh_name,
                    "name": emitted,
                    "weight": float(key.value),
                    "deltas": deltas,
                }
            )
        morph_tracks = _morph_weight_tracks(obj, mesh_name, name_map, shape_keys, fps, warnings, warned_modes)
    return extra_nodes, skins, morphs, morph_tracks


def _morph_weight_tracks(
    obj: bpy.types.Object,
    mesh_name: str,
    name_map: dict[str, str],
    shape_keys: "bpy.types.ShapeKey",
    fps: float,
    warnings: list[str],
    warned_modes: set[str],
) -> list[dict[str, object]]:
    """ANI1 weight tracks for shape-key value F-curves: 1:1 through the shared
    scalar machinery (same-mode segment runs, cubic handles via the existing
    derivative conversion) onto morphs/<mesh>/<emitted-key> targets."""
    animation_data = shape_keys.animation_data
    if animation_data is None or animation_data.action is None:
        return []
    if len(animation_data.nla_tracks):
        warnings.append(f"{obj.name}: shape-key NLA tracks present - only the active action's F-curves are exported")
    tracks: list[dict[str, object]] = []
    for fcurve in _action_fcurves(animation_data, f"{obj.name}.shape_keys", warnings):
        match = re.match(r'key_blocks\["([^"]+)"\]\.value$', fcurve.data_path)
        if match is None:
            warnings.append(
                f"{obj.name}.shape_keys: unsupported F-curve path '{fcurve.data_path}' (only key value curves export)"
            )
            continue
        key_name = match.group(1)
        if key_name not in name_map:
            continue  # basis keys and dropped targets carry no track
        keys = _fcurve_keys(fcurve)
        if keys is None:
            warnings.append(f"{obj.name}.shape_keys.{key_name}: keyless or invalid F-curve is not exported")
            continue
        target = f"morphs/{mesh_name}/{name_map[key_name]}"
        rows = [
            {"frame": key["frame"], "values": [key["value"]], "in_y": [key["in_y"]], "out_y": [key["out_y"]]}
            for key in keys
        ]
        modes = [point.interpolation for point in fcurve.keyframe_points]
        tracks.extend(
            _run_tracks(
                target, rows, modes, fps, _passthrough_value, _passthrough_tangent, 1, obj.name, warnings, warned_modes
            )
        )
    return tracks


class AetherSceneExporter(Operator, ExportHelper):
    bl_idname = "export.aether_scene"
    bl_label = "Export Aether Scene"
    bl_options: ClassVar[set[str]] = {"PRESET", "UNDO"}

    filename_ext = ".scene.toml"
    filter_glob: StringProperty(default="*.scene.toml", options={"HIDDEN"}, maxlen=255)

    skin_blend: EnumProperty(
        name="Skin Blend",
        description="Blend mode written to [[skin]] blend (Aether ANI4: DQS is "
        "rigid-by-design and drops scale/shear; LBS keeps it)",
        items=(
            ("lbs", "Linear (LBS)", "Matrix blend - preserves scale, collapses volume under rotation blends"),
            ("dqs", "Dual-Quaternion (DQS)", "Rigid blend - correct rotation blends, drops scale/shear"),
        ),
        default="lbs",
    )

    selected_only: BoolProperty(
        name="Selected Only",
        description="Export only the selected objects (parents outside the "
        "selection are not exported; their children are re-rooted with the "
        "world transform)",
        default=False,
    )

    include_camera: BoolProperty(
        name="Include Camera",
        description="Write a matching camera preset file",
        default=True,
    )

    material_libraries: StringProperty(
        name="Material Libraries",
        description="Comma-separated .materials.toml files to reference",
        default="",
    )

    render_reference: StringProperty(
        name="Render Reference",
        description="Optional render preset reference",
        default="presets/preview.render.toml",
    )

    tonemap_reference: StringProperty(
        name="Tonemap Reference",
        description="Optional tonemap preset reference",
        default="presets/aces.tonemap.toml",
    )

    camera_ev100: FloatProperty(
        name="Camera EV100",
        description="EV100 value written to the camera preset",
        default=12.0,
    )

    # Scene-level format keys (config, not materials - Blender materials are
    # never converted). Inline keys OVERRIDE the preset references ("local
    # wins" in the Aether parser); 0 / empty / "omit" values are left out so
    # the referenced preset decides.

    samples_per_pixel: IntProperty(
        name="Samples Per Pixel",
        description="[render] samples_per_pixel (0 = omit, the preset decides)",
        default=0,
        min=0,
    )

    max_depth: IntProperty(
        name="Max Depth",
        description="[render] max_depth (0 = omit, the preset decides)",
        default=0,
        min=0,
    )

    working_color_space: EnumProperty(
        name="Working Color Space",
        description="[render] working_color_space (omit = the preset decides)",
        items=(
            ("omit", "From preset", "Do not write the key"),
            ("lin_rec709_scene", "Linear Rec.709 scene", ""),
            ("lin_rec2020_scene", "Linear Rec.2020 scene", ""),
        ),
        default="omit",
    )

    environment_map: StringProperty(
        name="Environment Map",
        description="[render] environment_map - equirect EXR filename, written "
        "as-is and resolved against the assets dir (empty = omit)",
        default="",
    )

    environment_unit_nits: FloatProperty(
        name="Environment Nits",
        description="[render] environment_unit_nits (0 = omit)",
        default=0.0,
        min=0.0,
    )

    tonemapper: EnumProperty(
        name="Tonemapper",
        description="[tonemap] tonemapper (omit = the preset decides)",
        items=(
            ("omit", "From preset", "Do not write the key"),
            ("aces", "ACES", ""),
            ("agx", "AgX", ""),
            ("reinhard", "Reinhard", ""),
            ("hable", "Hable", ""),
        ),
        default="omit",
    )

    post_tonemap_renderer: EnumProperty(
        name="Post Tonemap Renderer",
        description="[post_tonemap] renderer (omit = no post-tonemap stage)",
        items=(
            ("omit", "None", "Do not write the key"),
            ("green_screen", "Green screen", ""),
        ),
        default="omit",
    )

    post_tonemap_reference: StringProperty(
        name="Post Tonemap Reference",
        description="[post_tonemap] reference preset (empty = omit)",
        default="",
    )

    haze_density: FloatProperty(
        name="Haze Density",
        description="[haze] density: extinction per metre at height zero; needs Haze Height "
        "(0 = no haze)",
        default=0.0,
        min=0.0,
    )

    haze_height: FloatProperty(
        name="Haze Height",
        description="[haze] height in metres above height zero at which the extinction has "
        "fallen to 1/e; needs Haze Density (0 = no haze)",
        default=0.0,
        min=0.0,
    )

    meta_title: StringProperty(
        name="Meta Title",
        description="[meta] title (empty = the scene filename stem)",
        default="",
    )

    meta_description: StringProperty(
        name="Meta Description",
        description="[meta] description (empty = omit)",
        default="",
    )

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "skin_blend")
        layout.prop(self, "selected_only")
        layout.prop(self, "include_camera")
        layout.prop(self, "material_libraries")
        layout.prop(self, "render_reference")
        layout.prop(self, "tonemap_reference")
        layout.prop(self, "camera_ev100")
        col = layout.column(align=True)
        col.prop(self, "samples_per_pixel")
        col.prop(self, "max_depth")
        col.prop(self, "working_color_space")
        col.prop(self, "environment_map")
        col.prop(self, "environment_unit_nits")
        col.prop(self, "haze_density")
        col.prop(self, "haze_height")
        col = layout.column(align=True)
        col.prop(self, "tonemapper")
        col.prop(self, "post_tonemap_renderer")
        col.prop(self, "post_tonemap_reference")
        col = layout.column(align=True)
        col.prop(self, "meta_title")
        col.prop(self, "meta_description")

    def execute(self, context):
        scene_path = Path(self.filepath)
        export_dir = scene_path.parent
        export_dir.mkdir(parents=True, exist_ok=True)

        # Single code path: EVERY object becomes a [[node]]. Mesh objects
        # additionally get an OBJ + a mesh binding; everything else (empties,
        # collection instances, lights, cameras) is a meshless grouping node.
        # No flat [[instance]] blocks are emitted — the Aether parser compounds
        # the node graph into its flat instance list itself.
        objects = (
            list(context.selected_objects) if self.selected_only else list(context.scene.objects)
        )
        exported_ids = {obj.name for obj in objects}
        # Stable parent-before-child order so the file reads top-down; the
        # parser resolves parents by id regardless of file order.
        objects.sort(key=_hierarchy_depth)

        used_names: set[str] = set()
        meta_links: list[tuple[str, dict[str, object]]] = []
        meshes: list[dict[str, object]] = []
        nodes: list[dict[str, object]] = []
        mesh_by_data: dict[str, str] = {}
        obj_paths: dict[str, Path] = {}
        data_paths: dict[str, Path] = {}
        # mesh name -> `usemtl` names of its OBJ (the node `materials` keys).
        mesh_usemtl: dict[str, list[str]] = {}
        warnings: list[str] = []

        for obj in objects:
            parent_id = (
                obj.parent.name
                if obj.parent is not None and obj.parent.name in exported_ids
                else None
            )
            mesh_name = None
            if obj.type == "MESH":
                # Shared mesh data (Blender instancing) is exported once and
                # referenced by every node using it. Objects carrying their own
                # modifiers bake object-specific geometry, so they always get
                # their own OBJ instead of joining the data-sharing group.
                shareable = not any(obj.modifiers)
                data_key = obj.data.name if shareable else None
                if data_key is not None and data_key in mesh_by_data:
                    mesh_name = mesh_by_data[data_key]
                else:
                    obj_path = _obj_export_path(export_dir, obj.name, used_names)
                    slots = [m for m in obj.data.materials if m is not None]
                    _export_obj_file(obj, obj_path, export_materials=len(slots) > 1)
                    mesh_name = obj_path.stem
                    mesh_block: dict[str, object] = {"name": mesh_name, "path": obj_path.name}
                    # BV1 + meta links: everything below is read back from
                    # the exported OBJ (the renderers cull against the AABB;
                    # agents get the [meta.link] summary without opening it).
                    stats = _obj_stats(obj_path)
                    if stats is not None:
                        mesh_block["bounds"] = {"min": stats["min"], "max": stats["max"]}
                        link: dict[str, object] = {
                            "kind": "mesh",
                            "triangles": stats["triangles"],
                            "groups": stats["groups"],
                            "bounds": {"min": stats["min"], "max": stats["max"], "frame": "object"},
                        }
                        raw_description = obj.get("description")
                        if isinstance(raw_description, str) and raw_description.strip():
                            link["description"] = raw_description.strip()
                        meta_links.append((obj_path.name, link))
                        mesh_usemtl[mesh_name] = list(stats["usemtl"])
                    meshes.append(mesh_block)
                    obj_paths[obj.name] = obj_path
                    if data_key is not None:
                        mesh_by_data[data_key] = mesh_name
                        data_paths[data_key] = obj_path
            nodes.append(
                _node_block(obj, mesh_name, parent_id, warnings, mesh_usemtl.get(mesh_name, []))
            )

        # NH4b: one [[animation]] entry per same-mode F-curve segment run,
        # 1:1 (never sampled). Objects without an active action contribute
        # nothing.
        scene_fps = _effective_fps(context.scene, warnings)
        animations: list[dict[str, object]] = []

        # ANI4/ANI5: deformation (armature skins + shape-key morphs) for the
        # exported mesh objects that carry either. Bone nodes join the node
        # list; morph weight tracks join the animation list after the node
        # tracks (distinct targets - precedence cannot collide).
        skins: list[dict[str, object]] = []
        morphs: list[dict[str, object]] = []
        morph_animations: list[dict[str, object]] = []
        armatures_with_bones: set[str] = set()
        exported_ids_for_bones = {obj.name for obj in objects}
        warned_modes: set[str] = set()
        for obj in objects:
            if obj.type != "MESH":
                continue
            obj_path = obj_paths.get(obj.name) or data_paths.get(obj.data.name)
            extra_nodes, skin_blocks, morph_blocks, tracks = _deformation_blocks(
                obj,
                obj_path.stem if obj_path else "",
                obj_path,
                obj.name in exported_ids_for_bones,
                self.skin_blend,
                scene_fps,
                warned_modes,
                warnings,
            )
            nodes.extend(extra_nodes)
            skins.extend(skin_blocks)
            morphs.extend(morph_blocks)
            morph_animations.extend(tracks)
            if skin_blocks:
                # The armature's bone NODES exist only when a skinned mesh
                # referenced it - pose tracks must not target absent nodes.
                for modifier in obj.modifiers:
                    if modifier.type == "ARMATURE" and modifier.object is not None:
                        armatures_with_bones.add(modifier.object.name)

        for obj in objects:
            animations.extend(_animation_tracks(obj, scene_fps, warnings))
            if obj.type == "ARMATURE" and obj.name in armatures_with_bones:
                animations.extend(
                    _pose_tracks(obj, obj.name in exported_ids_for_bones, scene_fps, warnings, warned_modes)
                )
        animations.extend(morph_animations)

        for warning in warnings:
            print(f"Aether export: {warning}")
            self.report({"WARNING"}, warning)

        camera_reference = None
        if self.include_camera and context.scene.camera:
            camera_path = _camera_path(scene_path)
            # ANI8: bind the [camera] section to the camera OBJECT's node when
            # that node is part of this export - the renderers then follow the
            # camera's animated transform per tick. selected_only exports that
            # exclude the camera keep the static TRS only.
            camera_node = (
                context.scene.camera.name if context.scene.camera.name in exported_ids else None
            )
            _write_simple_toml(
                camera_path, _camera_block(context.scene.camera, self.camera_ev100, camera_node)
            )
            camera_reference = camera_path.name

        material_libraries = [item.strip() for item in self.material_libraries.split(",") if item.strip()]
        render_reference = self.render_reference.strip() or None
        tonemap_reference = self.tonemap_reference.strip() or None

        # Scene-level inline keys (config only - material parameters are never
        # exported). Unset values stay out of the file entirely.
        render_keys: dict[str, object] = {}
        if self.samples_per_pixel > 0:
            render_keys["samples_per_pixel"] = int(self.samples_per_pixel)
        if self.max_depth > 0:
            render_keys["max_depth"] = int(self.max_depth)
        if self.working_color_space != "omit":
            render_keys["working_color_space"] = self.working_color_space
        if self.environment_map.strip():
            render_keys["environment_map"] = self.environment_map.strip()
        if self.environment_unit_nits > 0.0:
            render_keys["environment_unit_nits"] = float(self.environment_unit_nits)
        tonemap_keys: dict[str, object] = {}
        if self.tonemapper != "omit":
            tonemap_keys["tonemapper"] = self.tonemapper
        post_tonemap_keys: dict[str, object] = {}
        if self.post_tonemap_renderer != "omit":
            post_tonemap_keys["renderer"] = self.post_tonemap_renderer
        post_tonemap_reference = self.post_tonemap_reference.strip() or None
        # [haze] takes both keys or neither: one of them set is a mistake the
        # format rejects, so it is reported here and left out.
        haze_keys: dict[str, object] = {}
        if self.haze_density > 0.0 and self.haze_height > 0.0:
            haze_keys["density"] = _single_precision_text(self.haze_density)
            haze_keys["height"] = _single_precision_text(self.haze_height)
        elif self.haze_density > 0.0 or self.haze_height > 0.0:
            warnings.append(
                "[haze] needs both density and height; only one is set, so no haze is exported"
            )
        meta_keys: dict[str, object] = {"title": self.meta_title.strip() or scene_path.stem}
        if self.meta_description.strip():
            meta_keys["description"] = self.meta_description.strip()
        # Link tables cover only what the EXPORTER knows: the explicitly given
        # environment map and the referenced material libraries (a preset-
        # derived env is deliberately not linked - the link must not depend on
        # which directory the scene gets exported into). Mesh links were
        # appended per export above.
        if render_keys.get("environment_map"):
            meta_links.append((str(render_keys["environment_map"]), {"kind": "env"}))
        for library in material_libraries:
            meta_links.append((library, {"kind": "material-library"}))

        # Honesty guard for [meta] units: geometry exports UNSCALED, so an
        # Aether scene must be meter-based. Blender's own unit bookkeeping is
        # checked and warned about (imperial or a scale_length != 1).
        unit_settings = context.scene.unit_settings
        if unit_settings.system == "IMPERIAL" or abs(float(unit_settings.scale_length) - 1.0) > 1.0e-9:
            warnings.append(
                "scene units are not meters (geometry exports unscaled; Aether assumes meters - "
                "[meta] units stays the default 'm')"
            )

        _write_scene_toml(
            scene_path,
            meshes,
            nodes,
            animations,
            scene_fps,
            camera_reference,
            material_libraries,
            render_reference,
            tonemap_reference,
            skins,
            morphs,
            render_keys=render_keys,
            tonemap_keys=tonemap_keys,
            post_tonemap_reference=post_tonemap_reference,
            post_tonemap_keys=post_tonemap_keys,
            haze_keys=haze_keys,
            meta_keys=meta_keys,
            meta_links=meta_links,
        )

        self.report(
            {"INFO"},
            f"Exported {len(meshes)} mesh(es) / {len(nodes)} node(s) / {len(animations)} animation "
            f"track(s) / {len(skins)} skin(s) / {len(morphs)} morph(s) [blend={self.skin_blend}] "
            f"to {scene_path.name}",
        )
        return {"FINISHED"}


def menu_func_export(self, context):
    self.layout.operator(AetherSceneExporter.bl_idname, text="Aether Scene (.scene.toml)")


def register():
    bpy.utils.register_class(AetherSceneExporter)
    bpy.types.TOPBAR_MT_file_export.append(menu_func_export)


def unregister():
    bpy.types.TOPBAR_MT_file_export.remove(menu_func_export)
    bpy.utils.unregister_class(AetherSceneExporter)


if __name__ == "__main__":
    register()
