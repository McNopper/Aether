"""Headless Blender checks for mirrored-mesh winding in blender_to_aether."""
import sys
import tempfile
import tomllib
from pathlib import Path

import bpy

sys.path.insert(0, str(Path(__file__).resolve().parent))
import blender_to_aether as b2a

failures = []
checks = 0


def check(condition, message):
    global checks
    checks += 1
    if not condition:
        failures.append(message)
        print("FAIL:", message)


def near(a, b, tolerance=1.0e-6):
    return abs(a - b) <= tolerance * max(1.0, abs(a), abs(b))


def signed_volume(path):
    vertices = []
    volume = 0.0
    triangles = 0
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        if line.startswith("v "):
            vertices.append(tuple(float(value) for value in line.split()[1:4]))
        elif line.startswith("f "):
            indices = [int(token.split("/")[0]) - 1 for token in line.split()[1:]]
            if len(indices) != 3:
                raise AssertionError(f"non-triangle face in {path}: {line}")
            a, b, c = (vertices[index] for index in indices)
            cross = (b[1] * c[2] - b[2] * c[1],
                     b[2] * c[0] - b[0] * c[2],
                     b[0] * c[1] - b[1] * c[0])
            volume += (a[0] * cross[0] + a[1] * cross[1] + a[2] * cross[2]) / 6.0
            triangles += 1
    return volume, triangles


def face_uvs(path):
    coordinates = []
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        if line.startswith("vt "):
            fields = line.split()
            coordinates.append((round(float(fields[1]), 6), round(float(fields[2]), 6)))
    return set(coordinates)


bpy.ops.wm.read_factory_settings(use_empty=True)
b2a.register()
bpy.ops.mesh.primitive_cube_add(size=2.0)
positive = bpy.context.object
positive.name = "positive"
mirrored = bpy.data.objects.new("mirrored", positive.data)
bpy.context.collection.objects.link(mirrored)
mirrored.location.x = 3.0
mirrored.scale.x = -1.0
bpy.context.view_layer.update()

with tempfile.TemporaryDirectory(prefix="aether_blender_mirror_test_") as temporary:
    scene_path = Path(temporary) / "mirrored.scene.toml"
    result = bpy.ops.export.aether_scene(filepath=str(scene_path), include_camera=False, include_lights=False)
    check(result == {"FINISHED"}, f"export operator returned {result}")
    with scene_path.open("rb") as handle:
        scene = tomllib.load(handle)
    meshes = {entry["name"]: entry for entry in scene["mesh"]}
    nodes = {entry["id"]: entry for entry in scene["node"] if "mesh" in entry}
    positive_name = nodes["positive"]["mesh"]
    mirrored_name = nodes["mirrored"]["mesh"]
    check(positive_name != mirrored_name,
          "opposite winding signs must not share one OBJ")
    positive_volume, positive_triangles = signed_volume(scene_path.parent / meshes[positive_name]["path"])
    mirrored_volume, mirrored_triangles = signed_volume(scene_path.parent / meshes[mirrored_name]["path"])
    check(positive_triangles == 12 and mirrored_triangles == 12,
          f"expected two 12-triangle cubes, got {positive_triangles}/{mirrored_triangles}")
    check(near(abs(positive_volume), abs(mirrored_volume)),
          f"mirror changed volume magnitude: {positive_volume} vs {mirrored_volume}")
    check(positive_volume * mirrored_volume < 0.0,
          f"mirrored OBJ must carry reversed faces: {positive_volume} vs {mirrored_volume}")
    determinant_sign = -1.0 if mirrored.matrix_world.to_3x3().determinant() < 0.0 else 1.0
    corrected_volume = mirrored_volume * determinant_sign
    check(near(corrected_volume, positive_volume),
          f"world-space winding differs after the mirrored transform: {positive_volume} vs {corrected_volume}")

# Blender/USD UVs use bottom-left image origin; Aether samples the first image row at v=0.
# A non-symmetric square catches both an unchanged V and an accidental U/V swizzle.
uv_mesh = bpy.data.meshes.new("UV square")
uv_mesh.from_pydata([(-1.0, -1.0, 0.0), (1.0, -1.0, 0.0), (1.0, 1.0, 0.0), (-1.0, 1.0, 0.0)], [], [(0, 1, 2, 3)])
uv_object = bpy.data.objects.new("UV square", uv_mesh)
bpy.context.collection.objects.link(uv_object)
uv_layer = uv_mesh.uv_layers.new(name="st")
source_uv = [(0.2, 0.1), (0.4, 0.2), (0.45, 0.8), (0.15, 0.9)]
for loop in uv_mesh.loops:
    uv_layer.data[loop.index].uv = source_uv[loop.vertex_index]
bpy.context.view_layer.update()
with tempfile.TemporaryDirectory(prefix="aether_blender_uv_test_") as temporary:
    obj_path = Path(temporary) / "uv.obj"
    b2a._export_obj_file(uv_object, obj_path)
    actual_uv = face_uvs(obj_path)
    expected_uv = {(round(u, 6), round(1.0 - v, 6)) for u, v in source_uv}
    check({u for u, _ in actual_uv} == {u for u, _ in expected_uv},
          f"U must survive export unchanged: {actual_uv}")
    check(actual_uv == expected_uv,
          f"Blender V must convert to the renderer's first-image-row origin: {actual_uv} != {expected_uv}")
    legacy_path = Path(temporary) / "uv_legacy.obj"
    b2a._export_obj_file(uv_object, legacy_path, flip_uv_v=False)
    source_uv_set = {(round(u, 6), round(v, 6)) for u, v in source_uv}
    check(face_uvs(legacy_path) == source_uv_set,
          "flip_uv_v=False must preserve the legacy Blender OBJ coordinates")

print(f"blender_to_aether mesh/UV tests: {'FAILED' if failures else 'passed'} "
      f"({checks} checks, {len(failures)} failures)")
raise SystemExit(1 if failures else 0)
