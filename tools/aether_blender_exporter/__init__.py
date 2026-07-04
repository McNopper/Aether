bl_info = {
    "name": "Aether Scene Exporter",
    "author": "Aether Tools",
    "version": (1, 0, 0),
    "blender": (4, 0, 0),
    "location": "File > Export > Aether Scene (.scene.toml)",
    "description": "Export Blender scenes to Aether scene/camera/OBJ files",
    "category": "Import-Export",
    "support": "COMMUNITY",
}

import json
import math
import os
import re
from collections.abc import Sequence
from pathlib import Path
from typing import Iterable

import bpy
from bpy.props import BoolProperty, FloatProperty, StringProperty
from bpy.types import Operator
from bpy_extras.io_utils import ExportHelper
from mathutils import Matrix, Vector

# Blender is Z-up / -Y-forward; Aether (like glTF) is Y-up / -Z-forward.
# CONV is the axis swizzle (x, y, z)_blender -> (x, z, -y)_aether, i.e. a -90°
# rotation about X. Mesh vertices are exported in this Y-up local frame (via the
# OBJ exporter's up/forward axes); object transforms are conjugated by CONV and
# camera world positions/directions are mapped by CONV.
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
    # Camera world position/direction mapped Blender Z-up -> Aether Y-up.
    world = CONV @ camera.matrix_world
    location = world.translation
    forward = world.to_quaternion() @ Vector((0.0, 0.0, -1.0))
    look_at = location + forward
    vfov = math.degrees(getattr(camera.data, "angle_y", camera.data.angle))
    lines = [
        f"translate = [{location.x}, {location.y}, {location.z}]",
        f"look_at = [{look_at.x}, {look_at.y}, {look_at.z}]",
        f"vertical_field_of_view = {vfov}",
        f"ev100 = {ev100}",
    ]
    return lines


def _write_simple_toml(path: Path, lines: list[str]) -> None:
    path.write_text("\n".join(lines).rstrip() + "\n", encoding="utf-8")


def _scene_geometry_block(obj: bpy.types.Object, mesh_name: str) -> dict[str, object]:
    # Conjugate the world transform by the axis swizzle so the exported TRS
    # places the (Y-up local) mesh correctly: T_aether = CONV @ T_blender @ CONV^-1.
    world = CONV @ obj.matrix_world @ CONV.inverted()
    location, rotation, scale = world.decompose()
    block: dict[str, object] = {
        "type": "instance",
        "mesh": mesh_name,
        "translate": [location.x, location.y, location.z],
        "rotate": [rotation.x, rotation.y, rotation.z, rotation.w],
        "scale": [scale.x, scale.y, scale.z],
    }
    if obj.active_material:
        block["material"] = obj.active_material.name
    return block


def _write_scene_toml(
    scene_path: Path,
    geometry: list[dict[str, object]],
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
    for block in geometry:
        lines.append("[[geometry]]")
        for key, value in block.items():
            lines.append(f"{key} = {_format_value(value)}")
        lines.append("")
    _write_simple_toml(scene_path, lines)


class AetherSceneExporter(Operator, ExportHelper):
    bl_idname = "export.aether_scene"
    bl_label = "Export Aether Scene"
    bl_options = {"PRESET", "UNDO"}

    filename_ext = ".scene.toml"
    filter_glob: StringProperty(default="*.scene.toml", options={"HIDDEN"}, maxlen=255)

    selected_only: BoolProperty(
        name="Selected Only",
        description="Export only selected mesh objects",
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

        objects = (
            [obj for obj in context.selected_objects if obj.type == "MESH"]
            if self.selected_only
            else [obj for obj in context.scene.objects if obj.type == "MESH"]
        )

        used_names: set[str] = set()
        geometry: list[dict[str, object]] = []

        for obj in objects:
            obj_path = _obj_export_path(export_dir, obj.name, used_names)
            _export_obj_file(obj, obj_path)
            geometry.append(_scene_geometry_block(obj, obj_path.name))

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
            geometry,
            camera_reference,
            material_libraries,
            render_reference,
            tonemap_reference,
        )

        self.report({"INFO"}, f"Exported {len(geometry)} object(s) to {scene_path.name}")
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
