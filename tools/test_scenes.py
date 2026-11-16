# Test-scene generator for the blender_to_aether exporter (NH4 round-trip
# verification). Builds named deterministic scenes in headless Blender,
# exports them via the addon, and writes an expectations file (JSON) with the
# expected world matrices per meshed node - the oracle for the round-trip
# verification against the Aether parser.
#
# Usage (headless):
#   blender --background --python tools/test_scenes.py -- --scene <name> --out <dir>
#   (scene names: hierarchy | animated | simple | instancing; repeatable runs
#   skinned | skinned_dqs | camera_orbit | skinned_orbit | camera_static |
#   feature_matrix |
#   skinned_pose;
#   repeatable runs
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
import mathutils
from mathutils import Matrix, Vector

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


def build_skinned():
    """ANI4/ANI5 verification matrix: a two-bone armature (root + child), a
    triangle-quad mesh with blocky vertex-group weights, and a 'Smile' shape
    key animated 0 -> 1 over frames 1..30 (the morph weight track). The same
    builder backs the 'skinned' (LBS) and 'skinned_dqs' (DQS) scenes - only the
    operator's skin_blend differs."""
    _clear_scene()
    bpy.context.scene.render.fps = 30

    # Armature: two bones along +Z (Bone.Root -> Bone.Child).
    bpy.ops.object.armature_add(location=(0.0, 0.0, 0.0))
    arm = bpy.context.active_object
    arm.name = "Rig"
    arm.data.name = "RigBones"
    bpy.ops.object.mode_set(mode="EDIT")
    root = arm.data.edit_bones["Bone"]
    root.name = "Bone.Root"
    root.head = (0.0, 0.0, 0.0)
    root.tail = (0.0, 0.0, 1.0)
    child = arm.data.edit_bones.new("Bone.Child")
    child.head = (0.0, 0.0, 1.0)
    child.tail = (0.0, 0.0, 2.0)
    child.parent = root
    child.use_connect = True
    bpy.ops.object.mode_set(mode="OBJECT")

    # Mesh: two triangles (import-parity: triangles only), blocky weights.
    mesh = bpy.data.meshes.new("SkinnedQuad")
    verts = [(0.0, 0.0, 0.0), (0.0, 1.0, 0.0), (1.0, 1.0, 0.0), (1.0, 0.0, 0.0)]
    mesh.from_pydata(verts, [], [(0, 1, 2), (0, 2, 3)])
    mesh.update()
    obj = bpy.data.objects.new("SkinnedQuad", mesh)
    bpy.context.collection.objects.link(obj)
    lower = obj.vertex_groups.new(name="Bone.Root")
    lower.add([0, 1], 1.0, "REPLACE")
    upper = obj.vertex_groups.new(name="Bone.Child")
    upper.add([2, 3], 1.0, "REPLACE")
    modifier = obj.modifiers.new("Armature", "ARMATURE")
    modifier.object = arm

    # Shape key: 'Smile' lifts the upper row +z; animated as the weight track.
    obj.shape_key_add(name="Basis", from_mix=False)
    smile = obj.shape_key_add(name="Smile", from_mix=False)
    for index, co in enumerate(smile.data):
        base = verts[index]
        smile.data[index].co = (base[0], base[1], base[2] + (0.25 if index >= 2 else 0.0))
    smile.value = 0.0
    smile.keyframe_insert(data_path="value", frame=1)
    smile.value = 1.0
    smile.keyframe_insert(data_path="value", frame=30)
    smile.value = 0.0  # export-time default weight stays 0; the track drives it

    return {
        "meshed": ["SkinnedQuad"],
        "deformation": {
            "armature": "Rig",
            "joints": ["Rig::Bone.Root", "Rig::Bone.Child"],
            "morphs": ["Smile"],
            "weight_track": None,  # filled by the test driver from the export
        },
    }


def _orbit_rig():
    """ANI8 orbit rig: an `OrbitPivot` empty at the origin with a camera child
    at (0, -6, 2) Blender units looking at the origin (to_track_quat '-Z',
    'Y'), and the pivot's whole-parameter Z rotation keyed 0 -> 90 deg over
    frames 1..60 - the camera orbits the scene while keeping its aim, so every
    tick renders a different angle. Registers the camera as the scene camera
    (the exporter's include_camera path picks it up)."""
    pivot = _add_empty("OrbitPivot")
    cam_data = bpy.data.cameras.new("OrbitCam")
    cam = bpy.data.objects.new("OrbitCam", cam_data)
    bpy.context.collection.objects.link(cam)
    cam.parent = pivot
    cam.location = (0.0, -6.0, 2.0)
    # Local (pivot-space) aim: -Z toward the pivot origin, Y up. Constant for
    # the whole orbit - only the pivot's rotation animates.
    cam.rotation_euler = (Vector((0.0, 0.0, 0.0)) - cam.location).to_track_quat("-Z", "Y").to_euler()

    # All three rotation components keyed at the same frames -> the exporter's
    # agreeing-component path emits ONE whole-parameter rotation track.
    pivot.rotation_euler = (0.0, 0.0, 0.0)
    pivot.keyframe_insert(data_path="rotation_euler", frame=1)
    pivot.rotation_euler = (0.0, 0.0, math.radians(90.0))
    pivot.keyframe_insert(data_path="rotation_euler", frame=60)

    bpy.context.scene.camera = cam
    return cam


def build_camera_orbit():
    """ANI8 matrix: a static cube under an orbiting camera (see _orbit_rig).
    The [camera] section binds to the OrbitCam NODE, so the renderers must
    follow the animated transform every tick (angles differ per frame)."""
    _clear_scene()
    bpy.context.scene.render.fps = 30
    _add_cube("Subject")
    _orbit_rig()
    return {"meshed": ["Subject"]}


def build_skinned_orbit():
    """ANI8 example: the skinned/morphed fixture (build_skinned: two-bone rig,
    blocky weights, animated Smile shape key) PLUS the orbiting camera - one
    scene exercising deformation and the animated camera together."""
    out = build_skinned()
    _orbit_rig()
    return out

def build_feature_matrix():
    """Exporter feature matrix (blender_to_aether 1.6.0): every format key
    beyond the core node/animation path - META2 node description + tags (Blender
    custom properties), a two-slot material object (per-group `materials` name
    map + usemtl groups in the OBJ - NAME bindings only, Blender materials are
    never converted), the computed mesh bounds (BV1 AABB), and the scene-level
    [render]/[tonemap]/[post_tonemap]/[meta] inline options passed via
    SCENE_OPS. Materials here are bare NAMES (no parameters) on purpose."""
    _clear_scene()
    bpy.context.scene.render.fps = 30

    cube = _add_cube("FeatureCube")
    cube["description"] = "Feature-matrix cube (META2 node description)"
    cube["tags"] = " showcase, feature-matrix "
    cube.data.materials.append(bpy.data.materials.new("MatAlpha"))
    cube.data.materials.append(bpy.data.materials.new("MatBeta"))
    for polygon in cube.data.polygons:
        polygon.material_index = polygon.index % 2

    return {"meshed": ["FeatureCube"]}

def build_skinned_pose():
    """Skeletal pose-bone animation fixture (the Blender side of the pose
    export): the two-bone rig + blocky-weighted quad (NO shape keys, so the
    expected deformation is exact), with POSE-bone channels keyed - the root's
    location (0 -> 0.5 along the bone over frames 1..30) and the child's euler
    rotation (0 -> 90 degrees about local X over frames 1..30). The pose is
    reset to rest after keying so the exported OBJ stays at the bind pose
    (Blender evaluates the armature modifier with the CURRENT pose). The
    expectations carry Blender's own depsgraph-evaluated vertices at frames 0
    and 30 (in the Aether frame) - the ground truth the C++ round-trip compares
    the exported pipeline against."""
    _clear_scene()
    bpy.context.scene.render.fps = 30

    bpy.ops.object.armature_add(location=(0.0, 0.0, 0.0))
    arm = bpy.context.active_object
    arm.name = "Rig"
    arm.data.name = "RigBones"
    bpy.ops.object.mode_set(mode="EDIT")
    root = arm.data.edit_bones["Bone"]
    root.name = "Bone.Root"
    root.head = (0.0, 0.0, 0.0)
    root.tail = (0.0, 0.0, 1.0)
    child = arm.data.edit_bones.new("Bone.Child")
    child.head = (0.0, 0.0, 1.0)
    child.tail = (0.0, 0.0, 2.0)
    child.parent = root
    child.use_connect = True
    bpy.ops.object.mode_set(mode="OBJECT")

    mesh = bpy.data.meshes.new("SkinnedQuad")
    verts = [(0.0, 0.0, 0.0), (0.0, 1.0, 0.0), (1.0, 1.0, 0.0), (1.0, 0.0, 0.0)]
    mesh.from_pydata(verts, [], [(0, 1, 2), (0, 2, 3)])
    mesh.update()
    obj = bpy.data.objects.new("SkinnedQuad", mesh)
    bpy.context.collection.objects.link(obj)
    lower = obj.vertex_groups.new(name="Bone.Root")
    lower.add([0, 1], 1.0, "REPLACE")
    upper = obj.vertex_groups.new(name="Bone.Child")
    upper.add([2, 3], 1.0, "REPLACE")
    modifier = obj.modifiers.new("Armature", "ARMATURE")
    modifier.object = arm

    # POSE-bone channels (data_path pose.bones["<name>"].<channel>).
    pose_root = arm.pose.bones["Bone.Root"]
    pose_root.location = (0.0, 0.0, 0.0)
    pose_root.keyframe_insert(data_path="location", frame=1)
    pose_root.location = (0.0, 0.5, 0.0)  # along the bone (Blender local Y)
    pose_root.keyframe_insert(data_path="location", frame=30)
    pose_root.location = (0.0, 0.0, 0.0)

    pose_child = arm.pose.bones["Bone.Child"]
    pose_child.rotation_mode = "XYZ"
    pose_child.rotation_euler = (0.0, 0.0, 0.0)
    pose_child.keyframe_insert(data_path="rotation_euler", frame=1)
    pose_child.rotation_euler = (math.radians(90.0), 0.0, 0.0)
    pose_child.keyframe_insert(data_path="rotation_euler", frame=30)
    pose_child.rotation_euler = (0.0, 0.0, 0.0)

    return {"meshed": ["SkinnedQuad"], "pose_frames": [0, 30]}

def build_camera_static():
    """ANI8 static-node camera fixture: a cube under a camera bound through
    `[camera] node` with NO animation anywhere. The merged-guard edge case:
    the scripted camera must re-place idempotently WITHOUT tripping the
    animation block's accumulation reset - an offline classic capture of this
    scene must still converge (verified against an identical sequence capture
    in the render tests)."""
    _clear_scene()
    bpy.context.scene.render.fps = 30

    _add_cube("StaticSubject")
    cam_data = bpy.data.cameras.new("StaticCam")
    cam = bpy.data.objects.new("StaticCam", cam_data)
    bpy.context.collection.objects.link(cam)
    cam.location = (0.0, -6.0, 2.0)
    cam.rotation_euler = (Vector((0.0, 0.0, 0.0)) - cam.location).to_track_quat("-Z", "Y").to_euler()
    bpy.context.scene.camera = cam

    return {"meshed": ["StaticSubject"]}


SCENES = {
    "hierarchy": build_hierarchy,
    "animated": build_animated,
    "simple": build_simple,
    "instancing": build_instancing,
    "skinned": build_skinned,
    "skinned_dqs": build_skinned,
    "camera_orbit": build_camera_orbit,
    "skinned_orbit": build_skinned_orbit,
    "feature_matrix": build_feature_matrix,
    "skinned_pose": build_skinned_pose,
    "camera_static": build_camera_static,
}

# Extra operator kwargs per scene (skin_blend rides the exporter option so the
# same builder covers both blend modes; the orbit scenes turn on the camera
# export for the ANI8 [camera] node binding and take the meadow IBL preset -
# factory-empty scenes carry no lights, so a direct-light-only preset would
# render black).
SCENE_OPS = {
    "skinned_dqs": {"skin_blend": "dqs"},
    "camera_orbit": {
        "include_camera": True,
        "render_reference": "presets/meadow_64spp_8bounce.render.toml",
    },
    "skinned_orbit": {
        "include_camera": True,
        "render_reference": "presets/meadow_64spp_8bounce.render.toml",
    },
    # Scene-level inline options: they OVERRIDE the preset reference (local
    # wins), which the round-trip test asserts key by key.
    "camera_static": {
        "include_camera": True,
        "render_reference": "presets/meadow_64spp_8bounce.render.toml",
    },
    "feature_matrix": {
        "samples_per_pixel": 128,
        "max_depth": 5,
        "working_color_space": "lin_rec709_scene",
        "environment_map": "meadow_2_4k.exr",
        "environment_unit_nits": 10000.0,
        "tonemapper": "agx",
        "post_tonemap_renderer": "green_screen",
        "meta_title": "Feature Matrix",
        "meta_description": "Exporter feature-matrix fixture",
        # A real library reference - also exercises the material-library link.
        "material_libraries": "openpbr_mtlx.materials.toml",
    },
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
    ops = {"include_camera": False, **SCENE_OPS.get(scene_name, {})}
    bpy.ops.export.aether_scene(filepath=str(scene_path), check_existing=False, **ops)
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
    if "pose_frames" in expected:
        # Blender ground truth: evaluate the armature modifier at each frame
        # (depsgraph) and write the vertex positions CONV-ed into the Aether
        # frame - the oracle the C++ round-trip compares against.
        scene = bpy.context.scene
        saved_frame = scene.frame_current
        mesh_obj = bpy.data.objects[expected["meshed"][0]]
        truth: dict[str, list[list[float]]] = {}
        for frame in expected["pose_frames"]:
            scene.frame_set(frame)
            depsgraph = bpy.context.evaluated_depsgraph_get()
            evaluated = mesh_obj.evaluated_get(depsgraph)
            evaluated_mesh = evaluated.to_mesh()
            truth[str(frame)] = [
                list(CONV @ mathutils.Vector(vertex.co)) for vertex in evaluated_mesh.vertices
            ]
            evaluated.to_mesh_clear()
        scene.frame_set(saved_frame)
        expectations["_pose_truth"] = truth

    if "deformation" in expected:
        deformation = dict(expected["deformation"])
        with open(scene_path, encoding="utf-8") as handle:
            text = handle.read()
        # Provenance straight from the emitted file: the blend key and the
        # morph weight track target actually present in the export.
        deformation["blend_dqs_emitted"] = 'blend = "dqs"' in text
        deformation["weight_track_present"] = 'target = "morphs/SkinnedQuad/Smile"' in text
        expectations["_deformation"] = deformation
    with open(Path(args.out) / f"{args.scene}.expected.json", "w",
             encoding="utf-8") as f:
        json.dump(expectations, f, indent=1)

    print(f"test-scene '{args.scene}' exported: {scene_path}")
    print(f"expectations: {Path(args.out) / f'{args.scene}.expected.json'}")
    return 0



if __name__ == "__main__":
    sys.exit(main())
