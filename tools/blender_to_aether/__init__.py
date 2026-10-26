bl_info = {
    "name": "Aether Scene Exporter",
    "author": "Aether Tools",
    "version": (1, 2, 0),
    "blender": (5, 2, 0),
    "location": "File > Export > Aether Scene (.scene.toml)",
    "description": "Export Blender scenes to Aether scene/animation/camera/OBJ files",
    "category": "Import-Export",
    "support": "COMMUNITY",
}

import json
import math
import re
from bisect import bisect_right
from collections.abc import Iterable, Sequence
from typing import ClassVar
from pathlib import Path

import bpy
from bpy.props import BoolProperty, FloatProperty, StringProperty
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


def _quote(value: str) -> str:
    return json.dumps(value)


def _format_value(value) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int) and not isinstance(value, bool):
        return str(value)
    if isinstance(value, float):
        return repr(float(value))
    if isinstance(value, str):
        return _quote(value)
    if isinstance(value, Path):
        return _quote(str(value))
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


def _export_obj_file(obj: bpy.types.Object, obj_path: Path) -> None:
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
                export_materials=False,
                apply_modifiers=True,
            )
        elif hasattr(bpy.ops.export_scene, "obj"):
            bpy.ops.export_scene.obj(
                filepath=str(obj_path),
                use_selection=True,
                axis_forward="-Z",
                axis_up="Y",
                path_mode="AUTO",
                use_materials=False,
                use_mesh_modifiers=True,
            )
        else:
            raise RuntimeError("No OBJ exporter is available in this Blender build")
    finally:
        obj.matrix_world = saved_matrix
        bpy.context.view_layer.update()
        bpy.ops.object.select_all(action="DESELECT")
        for selected in prev_selected:
            if bpy.data.objects.get(selected.name) is not None:
                selected.select_set(True)
        if prev_active and bpy.data.objects.get(prev_active.name) is not None:
            bpy.context.view_layer.objects.active = prev_active


def _camera_block(camera: bpy.types.Object, ev100: float) -> list[str]:
    # Camera world transform mapped Blender Z-up -> Aether Y-up. A camera's
    # placement is a plain TRS (identical keys to a mesh instance): translate +
    # rotate. The scene format accepts rotate (quat [qx,qy,qz,qw]) or
    # rotate_x/y/z (Euler); the quaternion is emitted for a lossless export.
    world = CONV @ camera.matrix_world
    location = world.translation
    rotation = world.to_quaternion()
    vfov = math.degrees(getattr(camera.data, "angle_y", camera.data.angle))
    lines = [
        f"translate = [{location.x}, {location.y}, {location.z}]",
        f"rotate = [{rotation.x}, {rotation.y}, {rotation.z}, {rotation.w}]",
        f"vertical_field_of_view = {vfov}",
        f"ev100 = {ev100}",
    ]
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
) -> dict[str, object]:
    # Local transform relative to the exported parent: matrix_local is
    # Blender's parent-relative matrix (it includes the parent inverse, so
    # parent.matrix_world @ matrix_local reproduces matrix_world exactly).
    # A node whose Blender parent is not exported becomes a root and bakes the
    # world matrix instead, which keeps placement correct for subtree exports.
    # Conjugating every local matrix by CONV distributes over composition, so
    # the Aether parser's per-level T*R*S compounding reproduces the conjugated
    # Blender world transforms against the Y-up local mesh geometry (see CONV).
    local = obj.matrix_local if parent_id is not None else obj.matrix_world
    local = CONV @ local @ CONV.inverted()
    location, rotation, scale = _decompose_checked(local, obj.name, warnings)
    block: dict[str, object] = {"id": obj.name}
    if parent_id is not None:
        block["parent"] = parent_id
    block["translate"] = [location.x, location.y, location.z]
    block["rotate"] = [rotation.x, rotation.y, rotation.z, rotation.w]
    block["scale"] = [scale.x, scale.y, scale.z]
    if mesh_name is not None:
        block["mesh"] = mesh_name
        if obj.active_material:
            block["material"] = obj.active_material.name
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
# `fps / fps_base` and time = frame · fps_base / fps. The schema has no fps
# field (none is invented); the rate used is emitted as a TOML comment and the
# conversion is documented in tools/README.md.
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
# Registry limit: the ANI1 target registry has only the three vector parameters
# (translation | rotation | scale) and a target parameter may not contain '/'
# (splitAnimationTarget), so per-component scalar tracks do not parse. A
# property is therefore exported as ONE vector track per mode segment only when
# its component F-curves share identical key frames (the block-keyed common
# case); when component key frames differ the property cannot be represented
# without resampling (forbidden) and is NOT exported — a warning names it and
# the node's static TRS stands. Scalar component targets in the registry are a
# tracked follow-up.
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
    arity = _ANIM_ARITY[target.rsplit("/", 1)[1]]
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


def _quaternion_value(components: Sequence[float]) -> list[float]:
    # Blender rotation_quaternion channels are (w, x, y, z); the schema wants
    # (x, y, z, w). Component lerp can leave the key slightly off unit length,
    # so the key is normalized before conjugation (the parser normalizes too;
    # a degenerate zero quaternion maps to identity, matching sm::normalize).
    w, x, y, z = components
    norm = math.sqrt(w * w + x * x + y * y + z * z)
    if norm == 0.0:
        return _conjugate_rotation(0.0, 0.0, 0.0, 1.0)
    return _conjugate_rotation(x / norm, y / norm, z / norm, w / norm)


def _rotation_tangent(components: Sequence[float], tangent: Sequence[float]) -> list[float]:
    # Tangent components (dw, dx, dy, dz) in Blender channel order (w, x, y, z).
    return _conjugate_rotation(tangent[1], tangent[2], tangent[3], tangent[0])


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
    # comment for the split rule and the declaration-order precedence).
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
    # tracks only when every present component curve keys the SAME frames
    # (block-keyed); a missing component contributes its static value.
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
    for index, keys in keys_by_index.items():
        if [key["frame"] for key in keys] != frames:
            warnings.append(
                f"{obj.name}: {parameter} component F-curves key different frames — the "
                "ANI1 registry has no per-component scalar targets (only the vector "
                f"'nodes/<id>/{parameter}'), block-keyed components are the supported "
                "case, and resampling is not allowed: this property's animation is NOT "
                "exported and its static TRS stands"
            )
            return []
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

    if is_quaternion:

        def to_value(components: Sequence[float]) -> list[float]:
            return _quaternion_value(components)

        to_tangent = _rotation_tangent

    else:

        def quat_parts(angles: Sequence[float]) -> tuple[float, float, float, float]:
            quat = Euler((angles[0], angles[1], angles[2]), obj.rotation_mode).to_quaternion()
            return (quat.x, quat.y, quat.z, quat.w)

        def to_value(components: Sequence[float]) -> list[float]:
            # Blender applies the euler angles in the object's rotation order.
            return _conjugate_rotation(*quat_parts(components))

        def to_tangent(components: Sequence[float], tangent: Sequence[float]) -> list[float]:
            # dq/dt of the euler→quaternion conversion for the per-channel
            # euler rates: central difference per angle (exact to float
            # precision), conjugated like the value.
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
            return _conjugate_rotation(*derivative)

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
) -> None:
    lines: list[str] = []
    if material_libraries:
        lines.append(f'material_libraries = [{", ".join(_quote(item) for item in material_libraries)}]')
        lines.append("")
    if render_reference:
        lines.extend(["[render]", f"reference = {_quote(render_reference)}", ""])
    if camera_reference:
        lines.extend(["[camera]", f"reference = {_quote(camera_reference)}", ""])
    if tonemap_reference:
        lines.extend(["[tonemap]", f"reference = {_quote(tonemap_reference)}", ""])
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
    if animations:
        # The schema has no fps field; times are seconds, converted from
        # Blender frames at the scene's effective rate (render.fps /
        # render.fps_base). The comment records the rate for provenance.
        lines.append(f"# animation times are seconds: frame / {animation_fps:g} fps (render.fps / render.fps_base)")
        lines.append("")
        for block in animations:
            lines.append("[[animation]]")
            for key, value in block.items():
                lines.append(f"{key} = {_format_value(value)}")
            lines.append("")
    _write_simple_toml(scene_path, lines)


class AetherSceneExporter(Operator, ExportHelper):
    bl_idname = "export.aether_scene"
    bl_label = "Export Aether Scene"
    bl_options: ClassVar[set[str]] = {"PRESET", "UNDO"}

    filename_ext = ".scene.toml"
    filter_glob: StringProperty(default="*.scene.toml", options={"HIDDEN"}, maxlen=255)

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

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "selected_only")
        layout.prop(self, "include_camera")
        layout.prop(self, "material_libraries")
        layout.prop(self, "render_reference")
        layout.prop(self, "tonemap_reference")
        layout.prop(self, "camera_ev100")

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
        meshes: list[dict[str, object]] = []
        nodes: list[dict[str, object]] = []
        mesh_by_data: dict[str, str] = {}
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
                    _export_obj_file(obj, obj_path)
                    mesh_name = obj_path.stem
                    meshes.append({"name": mesh_name, "path": obj_path.name})
                    if data_key is not None:
                        mesh_by_data[data_key] = mesh_name
            nodes.append(_node_block(obj, mesh_name, parent_id, warnings))

        # NH4b: one [[animation]] entry per same-mode F-curve segment run,
        # 1:1 (never sampled). Objects without an active action contribute
        # nothing.
        scene_fps = _effective_fps(context.scene, warnings)
        animations: list[dict[str, object]] = []
        for obj in objects:
            animations.extend(_animation_tracks(obj, scene_fps, warnings))

        for warning in warnings:
            print(f"Aether export: {warning}")
            self.report({"WARNING"}, warning)

        camera_reference = None
        if self.include_camera and context.scene.camera:
            camera_path = _camera_path(scene_path)
            _write_simple_toml(camera_path, _camera_block(context.scene.camera, self.camera_ev100))
            camera_reference = camera_path.name

        material_libraries = [item.strip() for item in self.material_libraries.split(",") if item.strip()]
        render_reference = self.render_reference.strip() or None
        tonemap_reference = self.tonemap_reference.strip() or None

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
        )

        self.report(
            {"INFO"},
            f"Exported {len(meshes)} mesh(es) / {len(nodes)} node(s) / {len(animations)} animation "
            f"track(s) to {scene_path.name}",
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
