# Test-scene generator for the blender_to_aether exporter (NH4 round-trip
# verification). Builds named deterministic scenes in headless Blender,
# exports them via the addon, and writes an expectations file (JSON) with the
# expected world matrices per meshed node - the oracle for the round-trip
# verification against the Aether parser.
#
# Usage (headless):
#   blender --background --python tools/test_scenes.py -- --scene <name> --out <dir>
#   (scene names: hierarchy | animated | simple | instancing; repeatable runs
#    are deterministic - same scene name, same output bytes)
#
# The addon (tools/blender_to_aether) is imported from this script's directory
# and registered explicitly; the export runs via its operator.

import argparse
import json
import math
import sys
from pathlib import Path

import bpy
from mathutils import Matrix

TOOL_DIR = str(Path(__file__).resolve().parent)

# Z-up (Blender) -> Y-up (Aether) handedness convention, identical to the
# exporter's CONV.
CONV = Matrix.Rotation(math.radians(-90.0), 4, "X")


def _clear_scene() -> None:
    bpy.ops.wm.read_factory_settings(use_empty=True)


def _add_cube(name: str, mesh_data=None):
    if mesh_data is None:
        bpy.ops.mesh.primitive_cube_add()
        obj = bpy.context.active_object
        obj.name = name
        obj.data.name = name
    else:
        obj = bpy.data.objects.new(name, mesh_data)
        bpy.context.collection.objects.link(obj)
    return obj


def _add_empty(name: str):
    empty = bpy.data.objects.new(name, None)
    bpy.context.collection.objects.link(empty)
    return empty


def _set_local(obj, matrix_local):
    """Set the parent-relative transform (includes Blender's parent inverse
    handling exactly like the interactive workflow)."""
    obj.matrix_local = matrix_local


def _world_of(obj):
    return CONV @ obj.matrix_world @ CONV.inverted()


# ---------------------------------------------------------------- scenes ----
def build_hierarchy():
    """The NH4a verification matrix: 3-level parent chain (empties + mesh),
    shared-mesh instancing under different parents, negative-scale grouping
    node, parentless mesh, meshless grouping empties."""
    _clear_scene()
    root = _add_empty("RootEmpty")
    root.matrix_local = CONV @ Matrix.Translation((1.0, 3.0, -2.0)) @ CONV.inverted() \
        @ Matrix.Rotation(math.radians(30.0), 4, "Z") @ Matrix.Diagonal((2.0, 2.0, 2.0, 1.0))

    mid = _add_empty("MidEmpty")
    mid.parent = root
    mid.matrix_local = Matrix.Translation((0.5, 2.6e-15, 0.0)) \
        @ Matrix.Rotation(math.radians(15.0), 4, "X") @ Matrix.Diagonal((1.5, 1.25, 1.0, 1.0))

    leaf = _add_cube("LeafCube")
    leaf.parent = mid
    _set_local(leaf, Matrix.Translation((0.25, -0.5, 0.0)) \
        @ Matrix.Rotation(math.radians(17.0), 4, "Y") @ Matrix.Diagonal((1.0, 0.5, 2.0, 1.0)))

    twin = _add_cube("TwinCube")
    twin.parent = root
    twin.matrix_local = Matrix.Translation((1.0, 1.0, -0.5)) \
        @ Matrix.Rotation(math.radians(90.0), 4, "Y") @ Matrix.Diagonal((0.5, 0.5, 0.5, 1.0))
    twin.data = leaf.data  # shared mesh data -> Aether instancing

    neg = _add_empty("NegGroup")
    neg.parent = root
    neg.matrix_local = Matrix.Scale(-1.0, 4)

    lone = _add_cube("LoneCube")
    lone.matrix_local = CONV @ Matrix.Translation((5.0, 0.0, 0.0)) @ CONV.inverted()

    _add_empty("MarkerEmpty")
    return {"meshed": ["LeafCube", "TwinCube", "LoneCube"]}


def build_animated():
    """NH4b matrix: location keyed as a CONSTANT block, location with BEZIER
    handles, rotation_quaternion with BEZIER handles, and a mixed-interpolation
    location curve (CONSTANT + LINEAR run) exercising the segment-split rule."""
    _clear_scene()
    bpy.context.scene.render.fps = 30

    cube = _add_cube("AnimCube")

    # location: CONSTANT block (frames 1-10) then BEZIER (frames 10-40).
    cube.location = (0.0, 0.0, 0.0)
    cube.keyframe_insert(data_path="location", frame=1)
    cube.keyframe_insert(data_path="location", frame=10)
    cube.location = (2.0, 1.0, 0.5)
    cube.keyframe_insert(data_path="location", frame=40)

    # rotation_quaternion: BEZIER with handles, 90-deg swing around Z.
    cube.rotation_mode = "QUATERNION"
    cube.rotation_quaternion = (1.0, 0.0, 0.0, 0.0)
    cube.keyframe_insert(data_path="rotation_quaternion", frame=1)
    cube.rotation_quaternion = (0.7071068, 0.0, 0.0, 0.7071068)
    cube.keyframe_insert(data_path="rotation_quaternion", frame=60)

    # scale: LINEAR ramp.
    cube.scale = (1.0, 1.0, 1.0)
    cube.keyframe_insert(data_path="scale", frame=1)
    cube.scale = (3.0, 3.0, 3.0)
    cube.keyframe_insert(data_path="scale", frame=30)

    # Blender 4.4+/5.x slotted actions: legacy action.fcurves is gone in 5.2.
    # F-curve access runs through slot -> layer -> strip -> channelbag.
    def keyed_fcurves(obj, data_path):
        action = obj.animation_data.action
        slot = obj.animation_data.action_slot
        for layer in action.layers:
            for strip in layer.strips:
                bag = strip.channelbag(slot)
                for fcurve in bag.fcurves:
                    if fcurve.data_path == data_path:
                        yield fcurve

    for fcurve in keyed_fcurves(cube, "location"):
        for kp in fcurve.keyframe_points:
            if kp.co.x == 10.0:
                kp.interpolation = "CONSTANT"

    rot = list(keyed_fcurves(cube, "rotation_quaternion"))
    assert len(rot) == 4, f"expected 4 rotation fcurves, got {len(rot)}"
    kp = rot[0].keyframe_points[0]
    kp.handle_right = (kp.co.x + 5.0, kp.co.y + 0.1)

    return {"meshed": ["AnimCube"]}


def build_simple():
    _clear_scene()
    cube = _add_cube("OnlyCube")
    cube.matrix_local = CONV @ Matrix.Translation((0.0, 0.0, 1.0)) @ CONV.inverted()
    return {"meshed": ["OnlyCube"]}


def build_instancing():
    _clear_scene()
    proto = _add_cube("ProtoMesh")
    proto.matrix_local = Matrix.Identity(4)
    a = bpy.data.objects.new("InstA", proto.data)
    b = bpy.data.objects.new("InstB", proto.data)
    bpy.context.collection.objects.link(a)
    bpy.context.collection.objects.link(b)
    a.matrix_local = Matrix.Translation((3.0, 0.0, 0.0))
    b.matrix_local = Matrix.Translation((-3.0, 0.0, 0.0))
    return {"meshed": ["InstA", "InstB"]}


SCENES = {
    "hierarchy": build_hierarchy,
    "animated": build_animated,
    "simple": build_simple,
    "instancing": build_instancing,
}


# ---------------------------------------------------------------- export ----
def export_scene(out_dir: Path, scene_name: str) -> Path:
    sys.path.insert(0, TOOL_DIR)
    import blender_to_aether

    if hasattr(blender_to_aether, "register"):
        blender_to_aether.register()
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    scene_path = out / f"{scene_name}.scene.toml"
    bpy.ops.export.aether_scene(filepath=str(scene_path), include_camera=False,
                                check_existing=False)
    return scene_path


def main() -> int:
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(prog="test_scenes")
    parser.add_argument("--scene", required=True, choices=sorted(SCENES))
    parser.add_argument("--out", required=True)
    args = parser.parse_args(argv)

    expected = SCENES[args.scene]()
    scene_path = export_scene(args.out, args.scene)

    expectations = {}
    for name in expected["meshed"]:
        obj = bpy.data.objects[name]
        expectations[name] = {
            "world": [list(row) for row in _world_of(obj)],
            "parent": obj.parent.name if obj.parent else "",
        }
    with open(Path(args.out) / f"{args.scene}.expected.json", "w",
             encoding="utf-8") as f:
        json.dump(expectations, f, indent=1)

    print(f"test-scene '{args.scene}' exported: {scene_path}")
    print(f"expectations: {Path(args.out) / f'{args.scene}.expected.json'}")
    return 0



if __name__ == "__main__":
    sys.exit(main())
