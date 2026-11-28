"""Headless Blender checks for the camera projection conversion in blender_to_aether."""
import math
import sys
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


def exported_vfov(camera, aspect):
    lines = b2a._camera_block(camera, 0.0, frame_aspect=aspect)
    prefix = "vertical_field_of_view = "
    return float(next(line[len(prefix):] for line in lines if line.startswith(prefix)))


bpy.ops.wm.read_factory_settings(use_empty=True)
data = bpy.data.cameras.new("Projection")
camera = bpy.data.objects.new("Projection", data)
bpy.context.collection.objects.link(camera)
data.lens = 35.0
data.sensor_width = 51.428672790527344
data.sensor_height = 38.571502685546875

for fit in ("HORIZONTAL", "AUTO"):
    data.sensor_fit = fit
    for aspect in (16.0 / 9.0, 4.0 / 3.0, 1.0):
        expected = math.degrees(2.0 * math.atan(data.sensor_width / (2.0 * data.lens * aspect)))
        actual = exported_vfov(camera, aspect)
        check(near(actual, expected), f"{fit} aspect {aspect}: expected {expected}, got {actual}")

# A vertical-fit camera owns its vertical angle directly; frame aspect does not change it.
data.sensor_fit = "VERTICAL"
expected_vertical = math.degrees(data.angle_y)
for aspect in (16.0 / 9.0, 1.0, 9.0 / 16.0):
    actual = exported_vfov(camera, aspect)
    check(near(actual, expected_vertical), f"VERTICAL aspect {aspect}: {actual} != {expected_vertical}")

# AUTO chooses vertical fit for a portrait frame.
data.sensor_fit = "AUTO"
expected_portrait = math.degrees(data.angle_y)
actual_portrait = exported_vfov(camera, 9.0 / 16.0)
check(near(actual_portrait, expected_portrait), "AUTO portrait must preserve Blender's vertical FOV")

# The OpenPBR title camera's sourced values, independently derived from lens/sensor/aspect.
data.sensor_fit = "HORIZONTAL"
actual_title = exported_vfov(camera, 16.0 / 9.0)
check(near(actual_title, 44.9072952706, 1.0e-5), f"OpenPBR title projection: {actual_title}")
check(abs(actual_title - math.degrees(data.angle_y)) > 10.0, "the test must distinguish angle_y from horizontal fit")

for invalid in (0.0, -1.0, math.inf, math.nan):
    try:
        b2a.camera_vertical_fov(data, invalid)
    except ValueError:
        pass
    else:
        failures.append(f"invalid aspect {invalid} was accepted")

print(f"blender_to_aether camera tests: {'FAILED' if failures else 'passed'} ({checks} checks, {len(failures)} failures)")
raise SystemExit(1 if failures else 0)
