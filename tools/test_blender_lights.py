# Blender round-trip test for the light export of blender_to_aether.
#
# Runs INSIDE headless Blender (the add-on needs bpy), standard library only
# otherwise, temporary output directory, exit code 1 on any failure:
#   blender --background --factory-startup --python-exit-code 1 --python tools/test_blender_lights.py
#
# The oracles are what Blender 5.3 defines, established from its source and
# measured on the installed build with Cycles on the CPU (tools/README.md,
# "Lights"); none of them calls the exporter's code:
#   power = energy * 2**exposure;  point, spot: I = P / 4pi
#   area: world size = data size x object world scale;
#         Normalize on  L = P / (pi A), flux P;  Normalize off L = P / pi, flux P A
#   sun: E = S;  spot: cos(inner) = 1 - (1 - blend) (1 - cos(outer))
# Layers:
#   1. numbers   - the conversion functions and the unit-scale law
#   2. world size - object scale folded into area lights, Normalize on and off
#   3. spot      - the inner / outer cone edges
#   4. rejection - zero, negative and non-finite power; unsupported lights
#   5. round trip- a built scene exported through the real operator and parsed
#   6. options   - the operator's options through the real operator
#   7. USD sphere - normalized SphereLight far-field intensity and radius reporting
#   8. no lights - a light-free scene is unchanged
# No GPU, no render.

import math
import sys
import tempfile
import tomllib
from pathlib import Path

import bpy
from mathutils import Matrix, Quaternion, Vector

TOOL_DIR = str(Path(__file__).resolve().parent)
sys.path.insert(0, TOOL_DIR)

CONV = Matrix.Rotation(
    math.radians(-90.0), 4, "X"
)  # Blender Z-up -> Aether Y-up (x, z, -y)
failures: list[str] = []


def check(condition: bool, message: str) -> None:
    if not condition:
        failures.append(message)
        print("FAIL:", message)


def near(a: float, b: float, tol: float = 1.0e-6) -> bool:
    return abs(a - b) <= tol * max(1.0, abs(a), abs(b))


def clear_scene() -> None:
    bpy.ops.wm.read_factory_settings(use_empty=True)


def add_light(
    name: str,
    kind: str,
    energy: float,
    color=(1.0, 1.0, 1.0),
    location=(0.0, 0.0, 0.0),
    rotation=(0.0, 0.0, 0.0),
    scale=(1.0, 1.0, 1.0),
    parent=None,
    exposure=0.0,
):
    data = bpy.data.lights.new(name, kind)
    data.energy = energy
    data.color = color
    data.exposure = exposure
    obj = bpy.data.objects.new(name, data)
    bpy.context.collection.objects.link(obj)
    obj.location = location
    obj.rotation_euler = rotation
    obj.scale = scale
    if parent is not None:
        obj.parent = parent
    return obj


def add_empty(name: str, location=(0.0, 0.0, 0.0), scale=(1.0, 1.0, 1.0), parent=None):
    obj = bpy.data.objects.new(name, None)
    bpy.context.collection.objects.link(obj)
    obj.location = location
    obj.scale = scale
    if parent is not None:
        obj.parent = parent
    return obj


_registered = False


def export(scene_path: Path, **ops) -> Path:
    global _registered
    import blender_to_aether

    if hasattr(blender_to_aether, "register") and not _registered:
        blender_to_aether.register()
        _registered = True  # register() raises when called twice in one session
    bpy.ops.export.aether_scene(filepath=str(scene_path), check_existing=False, **ops)
    return scene_path


def lights_of(scene_path: Path) -> list[dict]:
    with open(scene_path, "rb") as handle:  # tomllib reads binary
        return tomllib.load(handle).get("light", [])


def emission_axis(block: dict) -> Vector:
    # Aether: local -Z is the emission axis (slang-math order qx, qy, qz, qw).
    qx, qy, qz, qw = block["rotate"]
    return Quaternion((qw, qx, qy, qz)) @ Vector((0.0, 0.0, -1.0))


def block(obj, warnings, **options):
    import blender_to_aether as b2a

    # Objects created or transformed from a script carry their new transforms
    # only once the depsgraph has been evaluated; the exporter's operator does
    # this itself before it reads a matrix.
    bpy.context.view_layer.update()
    return b2a._light_block(
        obj,
        warnings,
        options.get("area_light_size", "world"),
        options.get("meters_per_unit", 1.0),
        options.get("efficacy", 1.0),
    )


# ---------------------------------------------------------------- layer 1 ----
def test_numbers() -> None:
    import blender_to_aether as b2a

    check(
        near(b2a.LUMINOUS_EFFICACY_DEFAULT, 1.0),
        "the default efficacy is the identity: no scale",
    )

    # Point, power P [W]: Blender's radiant intensity is P / 4pi. The exporter
    # writes P lumen at efficacy 1, so the candela a renderer holds is
    # (P / 4pi) - the same number, in candela.
    for power in (1.0, 10.0, 1000.0, 1256.6370849609375):
        lumen = b2a.point_power_to_lumen(power)
        check(
            near(lumen, power),
            f"point lumen is the power itself at efficacy 1 ({power} W)",
        )
        check(
            near(lumen / (4.0 * math.pi), power / (4.0 * math.pi)),
            f"point candela ({power} W)",
        )
        check(
            near(b2a.point_power_to_lumen(power, 683.0), 683.0 * power),
            "efficacy scales the flux",
        )
    # Inverse square at 1, 2, 10 m: cd / d^2 is the illuminance in lux.
    intensity = b2a.point_power_to_lumen(1000.0) / (4.0 * math.pi)
    for d in (1.0, 2.0, 10.0):
        check(
            near(intensity / (d * d), 1000.0 / (4.0 * math.pi * d * d)),
            f"inverse square at {d} m",
        )

    # Area, Normalize on, power P over width x height: radiance P / (pi A), flux P.
    for power, width, height in (
        (1500.0, 2.0, 1.0),
        (1500.0 * math.pi, 0.12317, 0.12317),
        (100.0, 0.5, 0.5),
    ):
        nit = b2a.area_power_to_nit(power, width, height)
        check(
            near(nit, power / (math.pi * width * height)),
            f"area nit, Normalize on ({power} W)",
        )
        check(
            near(math.pi * nit * width * height, power),
            f"area flux is the power ({power} W)",
        )
    # Area, Normalize off: radiance P / pi whatever the size; flux P A.
    for power, width, height in ((1500.0, 2.0, 1.0), (1500.0, 8.0, 2.0)):
        nit = b2a.area_exitance_to_nit(power)
        check(near(nit, power / math.pi), f"area nit, Normalize off ({power} W)")
        check(
            near(math.pi * nit * width * height, power * width * height),
            "Normalize-off flux is P A",
        )
    # Sun: strength S [W/m2] is an irradiance.
    for strength in (1000.0, 100.0, 0.001):
        check(
            near(b2a.sun_strength_to_lux(strength), strength), f"sun lux ({strength})"
        )

    # Unit-scale law. A scene rescaled by s (every length x s, light sizes
    # included) keeps its irradiance at corresponding points only if the
    # power of total-power lights scales with s^2. Under the exporter's
    # option: E = (lumen s^2 / 4pi) / (s d)^2 = E.
    point_power, area_power, area_exitance, sun, width, height, d = (
        1000.0,
        1500.0,
        1500.0,
        100.0,
        2.0,
        1.0,
        3.0,
    )
    base = dict(
        point=(point_power / (4.0 * math.pi)) / (d * d),
        area=b2a.area_power_to_nit(area_power, width, height)
        * width
        * height
        / (d * d),
        unnormalized=b2a.area_exitance_to_nit(area_exitance) * width * height / (d * d),
        sun=sun,
    )
    for s in (1.0, 0.01, 0.5, 2.0):
        scale = b2a.total_power_unit_scale(s)
        check(near(scale, s * s), f"total-power factor is s^2 (s = {s})")
        e = (b2a.point_power_to_lumen(point_power * scale) / (4.0 * math.pi)) / (
            (d * s) ** 2
        )
        luminance = b2a.area_power_to_nit(area_power * scale, width * s, height * s)
        e_area = luminance * (width * s) * (height * s) / ((d * s) ** 2)
        e_unnormalized = (
            b2a.area_exitance_to_nit(area_exitance)
            * (width * s)
            * (height * s)
            / ((d * s) ** 2)
        )
        check(
            near(e, base["point"]),
            f"point irradiance invariant under the unit conversion (s = {s})",
        )
        check(
            near(luminance, base["area"] * d * d / (width * height)),
            f"area radiance invariant (s = {s})",
        )
        check(near(e_area, base["area"]), f"area irradiance invariant (s = {s})")
        check(
            near(e_unnormalized, base["unnormalized"]),
            f"Normalize-off irradiance invariant, no factor (s = {s})",
        )
        check(
            near(b2a.sun_strength_to_lux(sun), base["sun"]),
            f"sun irradiance invariant, no factor (s = {s})",
        )
    # Negative control: WITHOUT the factor the rescaled scene is darker by
    # exactly 1 / s^2 - the over-lighting a unit conversion causes when the
    # geometry shrinks and the light powers do not follow.
    s = 0.01
    uncorrected = (b2a.point_power_to_lumen(point_power) / (4.0 * math.pi)) / (
        (d * s) ** 2
    )
    check(
        near(uncorrected / base["point"], 1.0 / (s * s)),
        "without the factor: 1e4 times the irradiance",
    )


# ---------------------------------------------------------------- layer 2 ----
def test_world_size() -> None:
    clear_scene()
    warnings: list[str] = []
    power = 1500.0
    lamp = add_light("Scaled", "AREA", power, scale=(4.0, 0.5, 2.0))
    lamp.data.shape = "RECTANGLE"
    lamp.data.size, lamp.data.size_y = 2.0, 1.0

    # Blender's renderer scales an area light by its object's world scale per
    # axis: 2 x 1 at scale (4, 0.5, ...) is 8 x 0.5.
    world = block(lamp, warnings)
    check(
        world is not None
        and all(near(a, b) for a, b in zip(world["half_extents"], (4.0, 0.25))),
        f"nonuniform object scale folded into the half extents, got {world and world['half_extents']}",
    )
    check(
        not any("Scaled" in w for w in warnings),
        f"a plain scaled light warns nothing, got {warnings}",
    )
    # Normalize on: the total flux is the power, whatever the area.
    area = 8.0 * 0.5
    check(
        near(world["intensity"], power / (math.pi * area)),
        "radiance = P / (pi A_world)",
    )
    check(
        near(math.pi * world["intensity"] * area, power),
        "total flux preserved: folding scale changes the radiance only",
    )
    # The unscaled size is an explicit option and leaves the flux untouched.
    data = block(lamp, warnings, area_light_size="data")
    check(
        all(near(a, b) for a, b in zip(data["half_extents"], (1.0, 0.5))),
        "data size is the size property",
    )
    check(
        near(math.pi * data["intensity"] * 2.0, power),
        "data size preserves the flux too",
    )
    check(
        data["intensity"] > world["intensity"],
        "the smaller rectangle is the brighter one",
    )

    # Normalize off: the radiance is given; the flux follows the area, so the
    # object scale multiplies the flux by the area ratio (8 x 0.5) / (2 x 1) = 2.
    lamp.data.normalize = False
    unnormalized = block(lamp, warnings)
    check(
        near(unnormalized["intensity"], power / math.pi),
        "Normalize off: radiance P / pi",
    )
    check(
        near(math.pi * unnormalized["intensity"] * area, power * area),
        "Normalize off: flux P A_world",
    )
    check(
        near(
            (math.pi * unnormalized["intensity"] * area)
            / (math.pi * (power / math.pi) * 2.0),
            2.0,
        ),
        "area-ratio rule: the flux grew by the area ratio",
    )
    lamp.data.normalize = True

    # The unit conversion applies to the power, not twice: with s the flux is
    # P s^2 over the same world area.
    converted = block(lamp, warnings, meters_per_unit=0.01)
    check(
        near(math.pi * converted["intensity"] * area, power * 1.0e-4),
        "Normalize on: flux P s^2",
    )
    lamp.data.normalize = False
    converted_unnormalized = block(lamp, warnings, meters_per_unit=0.01)
    check(
        near(converted_unnormalized["intensity"], power / math.pi),
        "Normalize off: no unit factor on the radiance",
    )
    lamp.data.normalize = True

    # A zero-scale axis leaves no orientation and no rectangle.
    lamp.scale = (0.0, 1.0, 1.0)
    check(
        block(lamp, warnings) is None and any("degenerate" in w for w in warnings),
        "zero scale on an axis",
    )
    collapsed_point = add_light("CollapsedPoint", "POINT", 10.0, scale=(0.0, 1.0, 1.0))
    check(
        block(collapsed_point, warnings) is None
        and any("CollapsedPoint" in w for w in warnings),
        "a point light with a zero-scale axis",
    )
    lamp.scale = (4.0, 0.5, 2.0)

    # A sheared light (a rotated light under a non-uniformly scaled parent) is a
    # parallelogram: reported, exported with the axis lengths.
    parent = add_empty("Shear", scale=(2.0, 1.0, 1.0))
    sheared = add_light(
        "Sheared", "AREA", 100.0, rotation=(0.0, 0.0, math.radians(45.0)), parent=parent
    )
    warnings.clear()
    result = block(sheared, warnings)
    check(
        result is not None and any("shears" in w for w in warnings),
        f"a sheared light is reported, got {warnings}",
    )

    # A mirrored light (negative scale) keeps pointing along its emission axis.
    plain = add_light("Plain", "AREA", 100.0)
    plain.data.shape = "RECTANGLE"
    plain.data.size, plain.data.size_y = 2.0, 1.0
    mirrored = add_light("Mirrored", "AREA", 100.0, scale=(-2.0, 1.0, 1.0))
    mirrored.data.shape = "RECTANGLE"
    mirrored.data.size, mirrored.data.size_y = 2.0, 1.0
    warnings.clear()
    a, b = block(plain, warnings), block(mirrored, warnings)
    axis_a = Quaternion((a["rotate"][3], *a["rotate"][:3])) @ Vector((0.0, 0.0, -1.0))
    axis_b = Quaternion((b["rotate"][3], *b["rotate"][:3])) @ Vector((0.0, 0.0, -1.0))
    check(
        all(near(x, y) for x, y in zip(axis_a, axis_b)),
        f"a mirror about the light plane keeps its normal: {axis_a} vs {axis_b}",
    )
    check(
        all(near(x, y) for x, y in zip(b["half_extents"], (2.0, 0.5))),
        "the mirrored size is 4 x 1",
    )

    # The USD-imported lamp mainLamp_spill_AL through its real parent chain:
    # root scale 0.01 (metersPerUnit), local scale 6.1585; energy 1500 pi at
    # exposure 2.6. USD's own rule for a Normalize-on RectLight is
    # L = intensity 2^exposure / world area, in the stage's units.
    root = add_empty("World", scale=(0.01, 0.01, 0.01))
    rig = add_empty("lightRig", parent=root)
    usd_lamp = add_light(
        "mainLamp_spill_AL",
        "AREA",
        1500.0 * math.pi,
        parent=rig,
        exposure=2.6,
        scale=(6.158496, 6.158496, 6.158496),
    )
    usd_lamp.data.shape = "RECTANGLE"
    usd_lamp.data.size, usd_lamp.data.size_y = 2.0, 2.0
    warnings.clear()
    usd_block = block(usd_lamp, warnings, meters_per_unit=0.01)
    world_extent = 2.0 * 6.158496 * 0.01
    check(
        all(
            near(x, y, 1.0e-5)
            for x, y in zip(usd_block["half_extents"], (world_extent * 0.5,) * 2)
        ),
        f"USD lamp world half extent {world_extent * 0.5}, got {usd_block['half_extents']}",
    )
    usd_luminance = 1500.0 * 2.0**2.6 / (2.0 * 6.158496) ** 2
    check(
        near(usd_block["intensity"], usd_luminance, 1.0e-5),
        f"USD lamp reproduces USD's own luminance {usd_luminance:.4f}, got {usd_block['intensity']:.4f}",
    )
    # Without the unit conversion the same lamp is over-lit by 1 / s^2.
    uncorrected = block(usd_lamp, warnings)
    check(
        near(uncorrected["intensity"] / usd_block["intensity"], 1.0e4),
        "without s^2: 1e4 times USD's luminance",
    )


# ---------------------------------------------------------------- layer 3 ----
def test_spot() -> None:
    clear_scene()
    warnings: list[str] = []
    spot = add_light("Cone", "SPOT", 400.0)
    spot.data.spot_size = math.radians(60.0)
    spot.data.spot_blend = 0.25
    result = block(spot, warnings)
    check(near(result["outer_angle"], 30.0), "outer half angle = spot_size / 2")
    expected_inner = math.degrees(
        math.acos(1.0 - 0.75 * (1.0 - math.cos(math.radians(30.0))))
    )
    check(
        near(result["inner_angle"], expected_inner),
        f"inner half angle {expected_inner:.3f} from the source formula",
    )
    # Edges of the Blend range.
    spot.data.spot_blend = 0.0
    hard = block(spot, warnings)
    check(
        near(hard["inner_angle"], hard["outer_angle"]),
        "Blend 0 is a hard edge: inner = outer",
    )
    spot.data.spot_blend = 1.0
    soft = block(spot, warnings)
    check(near(soft["inner_angle"], 0.0), "Blend 1: full strength on the axis only")
    spot.data.spot_blend = 0.25
    check(
        near(result["intensity"], 400.0),
        "a spot's lumen value is the flux without the cone",
    )


# ---------------------------------------------------------------- layer 4 ----
def test_rejection() -> None:
    clear_scene()
    warnings: list[str] = []
    for name, energy in {
        "Zero": 0.0,
        "Negative": -100.0,
        "Infinite": math.inf,
        "NaN": math.nan,
    }.items():
        warnings.clear()
        result = block(add_light(name, "POINT", energy), warnings)
        check(result is None, f"{name} power must not export")
        check(
            len(warnings) == 1 and name in warnings[0],
            f"{name} power must be reported, got {warnings}",
        )
    check(
        block(add_light("ZeroExp", "POINT", 0.0, exposure=5.0), warnings) is None,
        "0 W, +5 EV",
    )
    # Blender's RNA clamps Light.color to >= 0, so a negative component cannot
    # reach the exporter from Blender data (the Aether parser rejects it in the
    # file); a non-finite one can.
    check(
        block(
            add_light("NanColor", "POINT", 10.0, color=(1.0, math.nan, 0.0)), warnings
        )
        is None,
        "non-finite color component",
    )
    disk = add_light("Disk", "AREA", 100.0)
    disk.data.shape = "DISK"
    check(
        block(disk, warnings) is None and any("Disk" in w for w in warnings),
        "disk area light",
    )
    ellipse = add_light("Ellipse", "AREA", 100.0)
    ellipse.data.shape = "ELLIPSE"
    check(
        block(ellipse, warnings) is None and any("Ellipse" in w for w in warnings),
        "ellipse area light",
    )
    spread = add_light("Spread", "AREA", 100.0)
    spread.data.spread = math.radians(60.0)
    check(
        block(spread, warnings) is None and any("Spread" in w for w in warnings),
        "area spread",
    )
    # Exposure scales the power by 2**exposure (Cycles: energy * exp2f(exposure));
    # measured on the installed build: 500 W at +1 EV equals 1000 W.
    exposed = block(add_light("Exposed", "POINT", 100.0, exposure=2.0), warnings)
    check(
        exposed is not None and near(exposed["intensity"], 400.0),
        "2**exposure scales the power",
    )
    # Normalize off is a supported conversion now.
    unnormalized = add_light("Unnormalized", "AREA", 100.0)
    unnormalized.data.normalize = False
    check(
        block(unnormalized, warnings) is not None,
        "an area light with Normalize off exports",
    )


# ---------------------------------------------------------------- layer 5 ----
def test_round_trip(out_dir: Path) -> None:
    clear_scene()
    rig = add_empty("Rig", location=(10.0, 0.0, 0.0), scale=(2.0, 2.0, 2.0))
    add_light(
        "Bulb",
        "POINT",
        1000.0,
        color=(1.0, 0.5, 0.25),
        location=(1.0, 2.0, 3.0),
        parent=rig,
    )
    cone = add_light(
        "Cone",
        "SPOT",
        400.0,
        location=(-1.0, 4.0, 0.0),
        rotation=(math.radians(90.0), 0.0, 0.0),
    )
    cone.data.spot_size = math.radians(60.0)
    cone.data.spot_blend = 0.25
    panel = add_light("Panel", "AREA", 1500.0, location=(0.0, 3.0, 0.0))
    panel.data.shape = "RECTANGLE"
    panel.data.size, panel.data.size_y = 2.0, 1.0
    tile = add_light(
        "Tile", "AREA", 100.0, scale=(4.0, 0.5, 2.0)
    )  # square 0.5 -> world 2 x 0.25
    tile.data.shape = "SQUARE"
    tile.data.size = 0.5
    wide = add_light("Wide", "AREA", 1500.0)  # Normalize off
    wide.data.shape = "RECTANGLE"
    wide.data.size, wide.data.size_y = 2.0, 1.0
    wide.data.normalize = False
    add_light("Sun", "SUN", 100.0, rotation=(math.radians(45.0), 0.0, 0.0))
    add_light("Exposed", "POINT", 100.0, exposure=2.6)
    # Not exported: one of each rejection.
    add_light("Zero", "POINT", 0.0)
    add_light("Negative", "POINT", -5.0)
    add_light("Infinite", "POINT", math.inf)
    disk = add_light("Disk", "AREA", 100.0)
    disk.data.shape = "DISK"

    scene_path = export(out_dir / "lights.scene.toml", include_camera=False)
    lights = lights_of(scene_path)
    by_name = {b["name"]: b for b in lights}
    wanted = ["Cone", "Panel", "Tile", "Wide", "Sun", "Exposed", "Bulb"]
    check(
        len(lights) == 7,
        f"count: expected 7 exported lights, got {[b['name'] for b in lights]}",
    )
    check(
        [b["name"] for b in lights] == wanted,
        "declaration order is the hierarchy-sorted object order",
    )

    bulb = by_name.get("Bulb", {})
    check(bulb.get("type") == "point" and bulb.get("unit") == "lumen", "Bulb type/unit")
    check(
        near(bulb.get("intensity", -1.0), 1000.0), "Bulb lumen = 1000 W at efficacy 1"
    )
    check(
        bulb.get("color") == [1.0, 0.5, 0.25], "Bulb color is scene-linear, untouched"
    )
    check(bulb.get("color_space") == "lin_rec709_scene", "Bulb color space")
    # World position through the parent: Rig (10,0,0) scale 2 -> Blender (12, 4, 6) -> Aether (12, 6, -4).
    check(
        all(
            near(a, b, 1.0e-5)
            for a, b in zip(bulb.get("translate", []), (12.0, 6.0, -4.0))
        ),
        f"Bulb world position, got {bulb.get('translate')}",
    )

    cone_block = by_name.get("Cone", {})
    check(
        cone_block.get("type") == "spot" and cone_block.get("unit") == "lumen",
        "Cone type/unit",
    )
    check(
        near(cone_block.get("intensity", -1.0), 400.0),
        "Cone lumen = 400 W (flux without the cone)",
    )
    check(
        near(cone_block.get("outer_angle", 0.0), 30.0, 1.0e-5),
        "Cone outer half angle = spot_size / 2",
    )
    expected_inner = math.degrees(
        math.acos(1.0 - 0.75 * (1.0 - math.cos(math.radians(30.0))))
    )
    check(
        near(cone_block.get("inner_angle", 0.0), expected_inner, 1.0e-5),
        "Cone inner half angle",
    )
    check(
        all(
            near(a, b, 1.0e-5)
            for a, b in zip(cone_block.get("translate", []), (-1.0, 0.0, -4.0))
        ),
        f"Cone world position, got {cone_block.get('translate')}",
    )
    axis = emission_axis(cone_block)
    check(
        all(near(a, b, 1.0e-5) for a, b in zip(axis, (0.0, 0.0, -1.0))),
        f"Cone emission axis, got {tuple(axis)}",
    )

    panel_block = by_name.get("Panel", {})
    check(
        panel_block.get("type") == "rect" and panel_block.get("unit") == "nit",
        "Panel type/unit",
    )
    check(
        near(panel_block.get("intensity", -1.0), 1500.0 / (math.pi * 2.0)),
        "Panel nit = P / (pi A)",
    )
    check(
        all(
            near(a, b) for a, b in zip(panel_block.get("half_extents", []), (1.0, 0.5))
        ),
        "Panel half extents",
    )
    check(
        all(
            near(a, b, 1.0e-5)
            for a, b in zip(panel_block.get("translate", []), (0.0, 0.0, -3.0))
        ),
        f"Panel world position, got {panel_block.get('translate')}",
    )
    axis = emission_axis(panel_block)
    check(
        all(near(a, b, 1.0e-5) for a, b in zip(axis, (0.0, -1.0, 0.0))),
        f"Panel emits straight down, got {tuple(axis)}",
    )

    tile_block = by_name.get("Tile", {})
    check(tile_block.get("type") == "rect", "A SQUARE area light is a rect")
    check(
        all(
            near(a, b, 1.0e-5)
            for a, b in zip(tile_block.get("half_extents", []), (1.0, 0.125))
        ),
        f"Tile world half extents 2 x 0.25 / 2, got {tile_block.get('half_extents')}",
    )
    check(
        near(tile_block.get("intensity", -1.0), 100.0 / (math.pi * 0.5)),
        "Tile nit: flux 100 W preserved",
    )

    wide_block = by_name.get("Wide", {})
    check(
        wide_block.get("type") == "rect"
        and near(wide_block.get("intensity", -1.0), 1500.0 / math.pi),
        "Normalize off: nit = P / pi",
    )

    sun_block = by_name.get("Sun", {})
    check(
        sun_block.get("type") == "directional" and sun_block.get("unit") == "lux",
        "Sun -> directional/lux",
    )
    check(near(sun_block.get("intensity", -1.0), 100.0), "Sun lux = 100 W/m2")
    axis = emission_axis(sun_block)
    expected_axis = CONV.to_3x3() @ Vector(
        (0.0, 0.7071067811865476, -0.7071067811865476)
    )
    check(
        all(near(a, b, 1.0e-5) for a, b in zip(axis, expected_axis)),
        f"Sun travel direction, got {tuple(axis)}",
    )

    exposed_block = by_name.get("Exposed", {})
    check(
        near(exposed_block.get("intensity", -1.0), 100.0 * 2.0**2.6),
        "exposure 2.6 scales the lumen value",
    )


# ---------------------------------------------------------------- layer 6 ----
def test_options(out_dir: Path) -> None:
    clear_scene()
    lamp = add_light("Lamp", "AREA", 1500.0)
    lamp.data.shape = "RECTANGLE"
    lamp.data.size, lamp.data.size_y = 2.0, 1.0
    lamp.scale = (4.0, 4.0, 4.0)
    add_light("Bulb", "POINT", 1000.0)
    add_light("Sun", "SUN", 100.0)

    default = {
        b["name"]: b
        for b in lights_of(
            export(out_dir / "opt_default" / "s.scene.toml", include_camera=False)
        )
    }
    check(
        all(near(a, b) for a, b in zip(default["Lamp"]["half_extents"], (4.0, 2.0))),
        "default: world size 8 x 4",
    )
    check(
        near(default["Lamp"]["intensity"], 1500.0 / (math.pi * 32.0)),
        "default: Normalize-on radiance over the world area",
    )

    data = {
        b["name"]: b
        for b in lights_of(
            export(
                out_dir / "opt_data" / "s.scene.toml",
                include_camera=False,
                area_light_size="data",
            )
        )
    }
    check(
        all(near(a, b) for a, b in zip(data["Lamp"]["half_extents"], (1.0, 0.5))),
        "area_light_size = data: the size property",
    )
    check(
        near(data["Bulb"]["intensity"], 1000.0),
        "area_light_size leaves the other lights alone",
    )

    scaled = {
        b["name"]: b
        for b in lights_of(
            export(
                out_dir / "opt_unit" / "s.scene.toml",
                include_camera=False,
                source_meters_per_unit=0.01,
            )
        )
    }
    for name in ("Lamp", "Bulb"):
        check(
            near(scaled[name]["intensity"], default[name]["intensity"] * 1.0e-4),
            f"{name}: power x s^2",
        )
    check(
        near(scaled["Sun"]["intensity"], 100.0), "sun: an irradiance has no unit factor"
    )
    check(
        all(
            near(a, b)
            for a, b in zip(
                scaled["Lamp"]["half_extents"], default["Lamp"]["half_extents"]
            )
        ),
        "source_meters_per_unit changes power, never size",
    )

    bright = {
        b["name"]: b
        for b in lights_of(
            export(
                out_dir / "opt_efficacy" / "s.scene.toml",
                include_camera=False,
                light_luminous_efficacy=683.0,
            )
        )
    }
    for name in ("Lamp", "Bulb", "Sun"):
        check(
            near(bright[name]["intensity"], default[name]["intensity"] * 683.0),
            f"{name}: x the efficacy",
        )


# ---------------------------------------------------------------- layer 7 ----
def test_usd_sphere_light() -> None:
    """A normalized USD SphereLight and Blender's imported point agree in the far field."""
    clear_scene()
    intensity, exposure, radius, scale = 400.0, 2.6, 3.0, 0.01
    exposure_scale = 2.0**exposure

    # USD LightAPI: intensity is base luminance and Normalize divides it by the
    # sphere's surface area. A Lambertian sphere's far-field intensity is its
    # luminance times projected area pi r^2. Radius cancels independently of
    # anything in the exporter: the result is USD intensity / 4.
    for independent_radius in (0.1, 3.0, 20.0):
        usd_luminance = intensity * exposure_scale / (
            4.0 * math.pi * independent_radius**2
        )
        usd_far_candela = usd_luminance * math.pi * independent_radius**2
        blender_import_energy = intensity * math.pi
        blender_far_candela = blender_import_energy * exposure_scale / (4.0 * math.pi)
        check(
            near(usd_far_candela, intensity * exposure_scale / 4.0),
            "USD normalized sphere far intensity is intensity / 4",
        )
        check(
            near(blender_far_candela, usd_far_candela),
            "Blender's imported point must keep that intensity, not receive a x4 correction",
        )

    root = add_empty("World", scale=(scale, scale, scale))
    lamp = add_light(
        "LEDmeetMat",
        "POINT",
        intensity * math.pi,
        parent=root,
        exposure=exposure,
    )
    lamp.data.shadow_soft_size = radius
    warnings: list[str] = []
    result = block(lamp, warnings, meters_per_unit=scale)
    expected_lumen = intensity * math.pi * exposure_scale * scale**2
    check(near(result["intensity"], expected_lumen), "USD sphere export keeps Blender's flux")
    check(
        near(result["intensity"] / (4.0 * math.pi), intensity * exposure_scale * scale**2 / 4.0),
        "the exported point has the unit-converted far-field candela",
    )

    # Illuminance at a corresponding point is invariant under the unit conversion.
    stage_distance = 10.0
    usd_irradiance = intensity * exposure_scale / 4.0 / stage_distance**2
    exported_irradiance = result["intensity"] / (4.0 * math.pi) / (
        scale * stage_distance
    ) ** 2
    check(near(exported_irradiance, usd_irradiance), "corresponding-point irradiance is invariant")
    check(
        len(warnings) == 1
        and "LEDmeetMat" in warnings[0]
        and "0.03 m" in warnings[0]
        and "ideal point" in warnings[0],
        f"the source radius is converted and its ideal-point approximation reported: {warnings}",
    )


# ---------------------------------------------------------------- layer 8 ----
def test_no_light_compatibility(out_dir: Path) -> None:
    # A light-free scene writes no [[light]] and the same bytes whether the
    # option is on or off: nothing changes for existing scenes.
    def cube() -> None:
        clear_scene()
        bpy.ops.mesh.primitive_cube_add()
        bpy.context.active_object.name = "Cube"
        bpy.context.active_object.data.name = "Cube"

    cube()
    on = export(
        out_dir / "nolights_on" / "nolights.scene.toml",
        include_camera=False,
        include_lights=True,
    )
    cube()
    off = export(
        out_dir / "nolights_off" / "nolights.scene.toml",
        include_camera=False,
        include_lights=False,
    )
    check(
        on.read_bytes() == off.read_bytes(),
        "light-free export: include_lights on/off bytes differ",
    )
    check(
        b"[[light]]" not in on.read_bytes(),
        "light-free export must not contain a light block",
    )


def main() -> int:
    out_dir = Path(tempfile.mkdtemp(prefix="aether_lights_"))
    test_numbers()
    test_world_size()
    test_spot()
    test_rejection()
    test_round_trip(out_dir)
    test_options(out_dir)
    test_usd_sphere_light()
    test_no_light_compatibility(out_dir)
    print(
        f"blender_to_aether light tests: {'FAILED' if failures else 'passed'} "
        f"({len(failures)} failure(s)); output: {out_dir}"
    )
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
