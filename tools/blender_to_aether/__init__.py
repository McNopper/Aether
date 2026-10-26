bl_info = {
    "name": "Aether Scene Exporter",
    "author": "Aether Tools",
    "version": (1, 1, 0),
    "blender": (5, 2, 0),
    "location": "File > Export > Aether Scene (.scene.toml)",
    "description": "Export Blender scenes to Aether scene/camera/OBJ files",
    "category": "Import-Export",
    "support": "COMMUNITY",
}

import json
import math
import re
from collections.abc import Iterable, Sequence
from typing import ClassVar
from pathlib import Path

import bpy
from bpy.props import BoolProperty, FloatProperty, StringProperty
from bpy.types import Operator
from bpy_extras.io_utils import ExportHelper
from mathutils import Matrix, Quaternion, Vector

# Blender is Z-up / -Y-forward; Aether (like glTF) is Y-up / -Z-forward.
# CONV is the axis swizzle (x, y, z)_blender -> (x, z, -y)_aether, i.e. a -90°
# rotation about X. Mesh vertices are exported in this Y-up local frame (via the
# OBJ exporter's up/forward axes); object transforms are conjugated by CONV and
# camera world positions/directions are mapped by CONV. The conjugation
# distributes over composition — (CONV A CONV^-1)(CONV B CONV^-1) =
# CONV (AB) CONV^-1 — so conjugating every node's LOCAL matrix compounds down
# the Aether node graph to exactly the conjugated Blender world transforms.
CONV = Matrix.Rotation(math.radians(-90.0), 4, "X")


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


def _write_scene_toml(
    scene_path: Path,
    meshes: list[dict[str, object]],
    nodes: list[dict[str, object]],
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
            camera_reference,
            material_libraries,
            render_reference,
            tonemap_reference,
        )

        self.report({"INFO"}, f"Exported {len(meshes)} mesh(es) / {len(nodes)} node(s) to {scene_path.name}")
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
