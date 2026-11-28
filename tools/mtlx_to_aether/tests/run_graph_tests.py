"""Self-contained tests for the MaterialX node-graph resolver and texture baker.

    python tests/run_graph_tests.py

Every operator is checked against pixel values calculated in this file from the
MaterialX definition, independently of the implementation (the expected values
never call ``mtlx_graph``). Also covered: color spaces of source and derived
textures, the UDIM normalization report, the duplicate-input policy, the
unsupported-node error, content-hash deduplication, the per-tile manifest and
the command line. Three negative controls run deliberately broken
implementations and must be caught. Exit code 0 = all pass.
"""

from __future__ import annotations

import json
import math
import os
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT))

import mtlx_graph
import mtlx_to_toml as conv
import package_textures

FAILURES: list[str] = []
CHECKS = 0


# ── assertion helpers and the independent reference math ───────────────────


def check(condition: bool, message: str) -> None:
    global CHECKS
    CHECKS += 1
    if not condition:
        FAILURES.append(message)


def raises(function, error_type, *contains: str) -> str:
    """Call `function`; it must raise `error_type` whose message contains every `contains`."""
    try:
        function()
    except error_type as exc:
        text = str(exc)
        for needle in contains:
            check(needle in text, f"{error_type.__name__} {text!r} lacks {needle!r}")
        return text
    except Exception as exc:
        check(False, f"expected {error_type.__name__}, got {type(exc).__name__}: {exc}")
        return ""
    check(False, f"expected {error_type.__name__}, nothing was raised")
    return ""


def same_path(left: str, right: Path) -> bool:
    return os.path.normcase(os.path.normpath(left)) == os.path.normcase(
        os.path.normpath(right)
    )


def q(value: float) -> int:
    """[0, 1] -> 8 bit: the quantization a PNG can hold."""
    return math.floor(max(0.0, min(1.0, value)) * 255.0 + 0.5)


def srgb_decode(code: float) -> float:
    return code / 12.92 if code < 0.04045 else ((code + 0.055) / 1.055) ** 2.4


def srgb_encode(value: float) -> float:
    return value * 12.92 if value <= 0.0031308 else 1.055 * value ** (1.0 / 2.4) - 0.055


# ── workspace: documents live in materials/, textures in tex/ ───────────────


DOC = """<?xml version="1.0"?>
<materialx version="1.39" colorspace="lin_rec709">
  <geompropvalue name="st" type="vector2">
    <input name="geomprop" type="string" value="st" />
  </geompropvalue>
{nodes}
  <open_pbr_surface name="srf" type="surfaceshader">
{inputs}
  </open_pbr_surface>
  <surfacematerial name="{name}" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="srf" />
  </surfacematerial>
{extra}
</materialx>
"""

ST1 = (
    '  <geompropvalue name="st1" type="vector2">\n'
    '    <input name="geomprop" type="string" value="st1" />\n  </geompropvalue>\n'
)


def image(
    name: str, kind: str, file: str, colorspace: str | None = None, texcoord: str = "st"
) -> str:
    """An image node; `file` is relative to tex/."""
    space = f' colorspace="{colorspace}"' if colorspace else ""
    coord = (
        f'\n    <input name="texcoord" type="vector2" nodename="{texcoord}" />'
        if texcoord
        else ""
    )
    return (
        f'  <image name="{name}" type="{kind}">\n'
        f'    <input name="file" type="filename"{space} value="../tex/{file}" />{coord}\n'
        f"  </image>\n"
    )


def node(category: str, name: str, kind: str, *inputs: tuple[str, str, str]) -> str:
    body = "".join(
        f'\n    <input name="{i}" type="{t}" {rest} />' for i, t, rest in inputs
    )
    return f'  <{category} name="{name}" type="{kind}">{body}\n  </{category}>\n'


def connected(name: str, kind: str, target: str) -> str:
    return f'    <input name="{name}" type="{kind}" nodename="{target}" />'


def literal(name: str, kind: str, value: str) -> str:
    return f'    <input name="{name}" type="{kind}" value="{value}" />'


def extract(name: str, source: str, kind: str, index: int) -> str:
    return node(
        "extract",
        name,
        "float",
        ("in", kind, f'nodename="{source}"'),
        ("index", "integer", f'value="{index}"'),
    )


def extra_surface(shader: str, material: str, inputs: list[str]) -> str:
    body = "\n".join("    " + line.strip() for line in inputs)
    return (
        f'  <open_pbr_surface name="{shader}" type="surfaceshader">\n{body}\n  </open_pbr_surface>\n'
        f'  <surfacematerial name="{material}" type="material">\n'
        f'    <input name="surfaceshader" type="surfaceshader" nodename="{shader}" />\n'
        f"  </surfacematerial>\n"
    )


class Workspace:
    def __init__(self, root: Path) -> None:
        self.root = root
        (root / "materials").mkdir(parents=True)

    def png(self, file: str, pixels: list[list[tuple[int, ...]]]) -> None:
        """Write an 8-bit PNG under tex/; `pixels` is a list of rows."""
        import numpy
        from PIL import Image

        target = self.root / "tex" / file
        target.parent.mkdir(parents=True, exist_ok=True)
        array = numpy.array(pixels, dtype=numpy.uint8)
        Image.fromarray(array[..., 0] if array.shape[2] == 1 else array).save(target)

    def document(
        self,
        name: str,
        nodes: str,
        inputs: list[str],
        extra: str = "",
        material: str = "mat",
    ) -> Path:
        target = self.root / "materials" / name
        target.write_text(
            DOC.format(
                nodes=nodes, inputs="\n".join(inputs), extra=extra, name=material
            ),
            encoding="utf-8",
        )
        return target

    def convert(self, name: str, **options):
        settings = {
            "output_dir": self.root / "out",
            "texture_dir": self.root / "out" / "textures",
        }
        settings.update(options)
        return conv.convert_document(self.root / "materials" / name, **settings)

    def material(self, name: str, **options):
        return self.convert(name, **options).materials[0]

    def derived(self, material, map_key: str):
        """Pixels of the derived texture in `map_key`; None when it is a direct reference."""
        import numpy
        from PIL import Image

        for texture in material.textures:
            if conv.SUPPORTED_TEXTURE_SLOTS[texture.input_name][0] != map_key:
                continue
            path = Path(texture.filename)
            if not path.is_relative_to(self.root / "out" / "textures"):
                return None
            return numpy.asarray(Image.open(path))
        return "absent"

    def color_space(self, material, input_name: str) -> str:
        return next(
            t.color_space for t in material.textures if t.input_name == input_name
        )


# One texel per row: 64/255 = 0.25098, 128/255 = 0.50196, 191/255 = 0.74902, 64/255 in G.
RGB = [[(64, 128, 191), (10, 20, 30)], [(200, 100, 240), (255, 0, 0)]]
NORMAL = [[(200, 100, 240)]]
GRAY = [[(64, 64, 64)]]
UNIFORM = [[(64, 64, 64), (64, 64, 64)], [(64, 64, 64), (64, 64, 64)]]
ROUGH = [[(64,)], [(191,)]]  # non-uniform, so it stays a texture
RAMP = [[(64, 64, 64)], [(191, 191, 191)]]  # a gray ramp as color3


def normal_chain(file: str, scale: float | None = None, kind: str = "vector3") -> str:
    """The official pattern: extract r/g/b of a normal map, invert g, combine3, normalmap."""
    scale_input = (("scale", "float", f'value="{scale}"'),) if scale is not None else ()
    return "".join(
        [
            image("N", kind, file, "Raw"),
            extract("r", "N", kind, 0),
            extract("g", "N", kind, 1),
            extract("b", "N", kind, 2),
            node("invert", "g_inv", "float", ("in", "float", 'nodename="g"')),
            node(
                "combine3",
                "C",
                "vector3",
                ("in1", "float", 'nodename="r"'),
                ("in2", "float", 'nodename="g_inv"'),
                ("in3", "float", 'nodename="b"'),
            ),
            node(
                "normalmap",
                "M",
                "vector3",
                ("in", "vector3", 'nodename="C"'),
                *scale_input,
            ),
        ]
    )


def roughness_graph(ws: Workspace, name: str, nodes: str, target: str):
    ws.document(name, nodes, [connected("specular_roughness", "float", target)])
    return ws.derived(ws.material(name), "map_orm")


# ── operator oracles ────────────────────────────────────────────────────────


def test_image_float_reads_red(ws: Workspace) -> None:
    """`image type=float` reads the red channel; the color space transform is for color outputs."""
    ws.png("rgb.png", RGB)
    orm = roughness_graph(
        ws, "red.mtlx", image("A", "float", "rgb.png", "srgb_tx"), "A"
    )
    check(
        orm[0][0].tolist() == [255, 64, 0],
        f"float image must read r into ORM G: {orm[0][0].tolist()}",
    )
    check(
        orm[0][1][1] == 10,
        f"a float image is not the luminance (20) but r: {orm[0][1][1]}",
    )


def test_extract(ws: Workspace) -> None:
    ws.png("rgb.png", RGB)
    orm = roughness_graph(
        ws,
        "extract.mtlx",
        image("A", "color3", "rgb.png", "Raw") + extract("E", "A", "color3", 2),
        "E",
    )
    check(
        orm[0][0].tolist() == [255, 191, 0],
        f"extract index 2 is the blue channel 191: {orm[0][0].tolist()}",
    )
    orm = roughness_graph(
        ws,
        "extract0.mtlx",
        image("A", "color3", "rgb.png", "Raw") + extract("E", "A", "color3", 0),
        "E",
    )
    check(orm[0][1][1] == 10, f"index 0 is the red channel: {orm[0][1][1]}")


def test_invert(ws: Workspace) -> None:
    ws.png("rough.png", ROUGH)
    source = image("A", "float", "rough.png")
    orm = roughness_graph(
        ws,
        "invert.mtlx",
        source + node("invert", "I", "float", ("in", "float", 'nodename="A"')),
        "I",
    )
    check(
        orm[0][0][1] == q(1.0 - 64 / 255),
        f"1 - in must quantize to {q(1 - 64 / 255)}: {orm[0][0][1]}",
    )
    orm = roughness_graph(
        ws,
        "invert75.mtlx",
        source
        + node(
            "invert",
            "J",
            "float",
            ("in", "float", 'nodename="A"'),
            ("amount", "float", 'value="0.75"'),
        ),
        "J",
    )
    check(
        orm[0][0][1] == q(0.75 - 64 / 255),
        f"amount - in with amount 0.75: {orm[0][0][1]}",
    )


def test_normalmap_keeps_raw_channels(ws: Workspace) -> None:
    ws.png("n.png", NORMAL)
    ws.document(
        "normal.mtlx",
        normal_chain("n.png"),
        [connected("geometry_normal", "vector3", "M")],
    )
    material = ws.material("normal.mtlx")
    normal = ws.derived(material, "map_normal")
    check(
        normal[0][0].tolist() == [200, 155, 240],
        f"the baked normal map is (r, 1-g, b) of the source: {normal[0][0].tolist()}",
    )
    check(
        ws.color_space(material, "geometry_normal") == "data",
        "a normal map is a data map",
    )


def test_normalmap_scale(ws: Workspace) -> None:
    ws.png("n.png", NORMAL)
    ws.document(
        "normal_scale.mtlx",
        normal_chain("n.png", scale=0.5),
        [connected("geometry_normal", "vector3", "M")],
    )
    normal = ws.derived(ws.material("normal_scale.mtlx"), "map_normal")
    # MaterialX: (2*in - 1) * scale on x and y; Aether decodes 2*tex - 1: tex = in*scale + (1 - scale)/2.
    expected = [q(200 / 255 * 0.5 + 0.25), q((1 - 100 / 255) * 0.5 + 0.25), 240]
    check(
        normal[0][0].tolist() == expected,
        f"normalmap scale 0.5: expected {expected}, got {normal[0][0].tolist()}",
    )


def test_remap(ws: Workspace) -> None:
    ws.png("rough.png", ROUGH)
    nodes = image("A", "float", "rough.png") + node(
        "remap",
        "R",
        "float",
        ("in", "float", 'nodename="A"'),
        ("outlow", "float", 'value="0.25"'),
        ("outhigh", "float", 'value="0.75"'),
    )
    orm = roughness_graph(ws, "remap.mtlx", nodes, "R")
    check(
        orm[0][0][1] == q(0.25 + (64 / 255 - 0.0) * (0.75 - 0.25) / (1.0 - 0.0)),
        f"remap [0,1] -> [0.25,0.75]: expected {q(0.25 + 64 / 510)}, got {orm[0][0][1]}",
    )


def test_colorcorrect_decodes_srgb_before_math(ws: Workspace) -> None:
    ws.png("rgb.png", RGB)
    nodes = image("A", "color3", "rgb.png", "srgb_tx") + node(
        "colorcorrect",
        "CC",
        "color3",
        ("in", "color3", 'nodename="A"'),
        ("gain", "float", 'value="1.5"'),
    )
    ws.document("gain.mtlx", nodes, [connected("base_color", "color3", "CC")])
    material = ws.material("gain.mtlx")
    color = ws.derived(material, "map_base_color")
    expected = [q(srgb_encode(min(1.0, 1.5 * srgb_decode(c / 255)))) for c in RGB[0][0]]
    check(
        color[0][0].tolist() == expected,
        f"gain 1.5 on the decoded value, re-encoded: expected {expected}, got {color[0][0].tolist()}",
    )
    check(
        ws.color_space(material, "base_color") == "srgb_rec709_scene",
        "a baked base color is declared sRGB",
    )


def test_colorcorrect_parameters(ws: Workspace) -> None:
    ws.png("ramp.png", RAMP)
    x = 64 / 255
    cases = {
        "gamma": ("2", q(x**0.5)),
        "contrast": ("0.5", q((x - 0.5) * 0.5 + 0.5)),
        "exposure": ("1", q(x * 2.0)),
        "lift": ("0.5", q(x + 0.5 * (1 - x))),
        "gain": ("1.5", q(x * 1.5)),
    }
    for parameter, (value, expected) in cases.items():
        nodes = image("A", "color3", "ramp.png", "Raw") + node(
            "colorcorrect",
            "CC",
            "color3",
            ("in", "color3", 'nodename="A"'),
            (parameter, "float", f'value="{value}"'),
        )
        # geometry_opacity takes the (unchanged) luminance of the gray result: a linear data map.
        ws.document(
            f"cc_{parameter}.mtlx",
            nodes,
            [connected("geometry_opacity", "float", "CC")],
        )
        opacity = ws.derived(ws.material(f"cc_{parameter}.mtlx"), "map_opacity")
        got = int(opacity[0][0][3])
        check(
            got == expected,
            f"colorcorrect {parameter}={value} on {x:.5f}: expected {expected}, got {got}",
        )


def test_colorcorrect_saturation_and_hue(ws: Workspace) -> None:
    ws.png("rgb.png", RGB)
    r, g, b = (c / 255 for c in RGB[0][0])
    luma = 0.2126 * r + 0.7152 * g + 0.0722 * b
    nodes = image("A", "color3", "rgb.png", "Raw") + node(
        "colorcorrect",
        "CC",
        "color3",
        ("in", "color3", 'nodename="A"'),
        ("saturation", "float", 'value="0.5"'),
    )
    ws.document("cc_sat.mtlx", nodes, [connected("base_color", "color3", "CC")])
    color = ws.derived(ws.material("cc_sat.mtlx"), "map_base_color")
    expected = [q(srgb_encode(luma + 0.5 * (c - luma))) for c in (r, g, b)]
    check(
        color[0][0].tolist() == expected,
        f"saturation 0.5 blends with the Rec.709 luma {luma:.5f}: expected {expected}, got {color[0][0].tolist()}",
    )

    ws.png("red.png", [[(255, 0, 0)], [(128, 0, 0)]])
    nodes = image("A", "color3", "red.png", "Raw") + node(
        "colorcorrect",
        "CC",
        "color3",
        ("in", "color3", 'nodename="A"'),
        ("hue", "float", 'value="0.666667"'),
    )
    ws.document("cc_hue.mtlx", nodes, [connected("base_color", "color3", "CC")])
    color = ws.derived(ws.material("cc_hue.mtlx"), "map_base_color")
    # the source is linear (Raw): the value 128/255 is re-encoded to sRGB in the baked map
    check(
        [color[r][0].tolist() for r in range(2)]
        == [[0, 0, 255], [0, 0, q(srgb_encode(128 / 255))]],
        f"a 240 degree hue rotation of red is blue, value kept: {[list(color[r][0]) for r in range(2)]}",
    )


def test_color_to_float_is_luminance(ws: Workspace) -> None:
    ws.png("rgb.png", RGB)
    ws.document(
        "opacity.mtlx",
        image("A", "color3", "rgb.png", "Raw"),
        [connected("geometry_opacity", "float", "A")],
    )
    material = ws.material("opacity.mtlx")
    opacity = ws.derived(material, "map_opacity")
    r, g, b = (c / 255 for c in RGB[0][0])
    expected = q(0.2126 * r + 0.7152 * g + 0.0722 * b)
    check(
        opacity.shape == (2, 2, 4),
        f"Aether reads the alpha of map_opacity: shape {opacity.shape}",
    )
    check(
        [int(v) for v in opacity[0][0]] == [expected] * 4,
        f"color3 -> float is the Rec.709 luminance {expected} in every channel: {opacity[0][0].tolist()}",
    )
    check(
        ws.color_space(material, "geometry_opacity") == "data",
        "an opacity map is a data map",
    )


def test_orm_composition(ws: Workspace) -> None:
    ws.png("rough.png", ROUGH)
    ws.document(
        "orm.mtlx",
        image("R", "float", "rough.png"),
        [
            connected("specular_roughness", "float", "R"),
            literal("base_metalness", "float", "0.5"),
        ],
    )
    material = ws.material("orm.mtlx")
    orm = ws.derived(material, "map_orm")
    check(
        orm[0][0].tolist() == [255, 64, 128],
        f"ORM is (1, roughness, metalness): {orm[0][0].tolist()}",
    )
    check(
        material.textures[0].input_name == "orm"
        and material.textures[0].color_space == "data",
        "the ORM texture is bound through the data-space map_orm slot",
    )
    check(
        all(
            conv.SUPPORTED_TEXTURE_SLOTS[t.input_name][0]
            not in {"map_roughness", "map_metalness"}
            for t in material.textures
        ),
        "baking emits no separate map_roughness / map_metalness",
    )
    ws.document(
        "orm2.mtlx",
        image("M", "float", "rough.png"),
        [
            literal("specular_roughness", "float", "0.25"),
            connected("base_metalness", "float", "M"),
        ],
    )
    orm = ws.derived(ws.material("orm2.mtlx"), "map_orm")
    check(
        orm[0][0].tolist() == [255, 64, 64],
        f"a metalness-only texture keeps roughness 0.25: {orm[0][0].tolist()}",
    )


def test_high_precision_orm_and_scalar(ws: Workspace) -> None:
    """Non-grid source values stay 16-bit through encoding and the actual PNG bytes."""
    import struct
    import zlib

    import numpy as np

    codes = np.array([[1, 257], [32769, 65534]], dtype=np.uint16)
    roughness = (codes.astype(np.float32) / 65535.0)[..., None]
    metalness = np.zeros_like(roughness)
    orm, space, clamped = mtlx_graph.encode_orm(roughness, metalness)
    check(orm.dtype == np.uint16, f"16-bit roughness must not produce {orm.dtype}")
    check(space == "data" and clamped == 0, "the 16-bit ORM remains unclamped data")
    check(np.array_equal(orm[..., 0], np.full((2, 2), 65535)), "ORM occlusion channel is one")
    check(np.array_equal(orm[..., 1], codes), "every 16-bit roughness code value survives")

    store = mtlx_graph.TextureStore(ws.root / "out" / "textures")
    path = store.save(orm, "precision_orm")
    data = path.read_bytes()
    check(data[:8] == b"\x89PNG\r\n\x1a\n" and data[24] == 16 and data[25] == 2,
          "the derived file is an RGB16 PNG, not an RGB8 Pillow conversion")
    offset = 8
    compressed = []
    while offset < len(data):
        length = struct.unpack_from(">I", data, offset)[0]
        kind = data[offset + 4:offset + 8]
        payload = data[offset + 8:offset + 8 + length]
        if kind == b"IDAT":
            compressed.append(payload)
        offset += length + 12
    raw = zlib.decompress(b"".join(compressed))
    rows = []
    stride = 1 + 2 * 3 * 2
    for y in range(2):
        row = raw[y * stride:(y + 1) * stride]
        check(row[0] == 0, "the deterministic PNG16 writer uses filter zero")
        rows.append(np.frombuffer(row[1:], dtype=">u2").reshape(2, 3))
    decoded = np.stack(rows)
    check(np.array_equal(decoded, orm), "an independent PNG16 decode reproduces every ORM value")

    grid8 = np.array([[0.0, 64.0 / 255.0], [128.0 / 255.0, 1.0]], dtype=np.float32)[..., None]
    orm8, _, _ = mtlx_graph.encode_orm(grid8, np.zeros_like(grid8))
    check(orm8.dtype == np.uint8, "an exact 8-bit grid keeps the compact RGBA8 representation")
    scalar, _, _ = mtlx_graph.encode_scalar(roughness)
    check(scalar.dtype == np.uint16 and np.array_equal(scalar[..., 0], codes),
          "scalar maps use the same precision-preserving policy")

    hdr = np.array([[0.0, 0.5], [1.0, 2.0]], dtype=np.float32)[..., None]
    emission, emission_space, emission_clamped, emission_scale = mtlx_graph.encode_emission_luminance(hdr)
    check(emission_scale == 2.0 and emission_space == "data" and emission_clamped == 0,
          "HDR emission is normalized by its authored maximum, not clipped")
    reconstructed = emission[..., 0].astype(np.float32) / 65535.0 * emission_scale
    check(np.allclose(reconstructed, hdr[..., 0], atol=2.0 / 65535.0),
          "texture times the material scale reconstructs the specified [0, infinity) luminance")


def test_uniform_texture_folds_into_scalar(ws: Workspace) -> None:
    ws.png("uniform.png", UNIFORM)
    nodes = (
        image("R", "float", "uniform.png")
        + image("B", "color3", "uniform.png")
        + node(
            "colorcorrect",
            "CC",
            "color3",
            ("in", "color3", 'nodename="B"'),
            ("gain", "float", 'value="0.5"'),
        )
    )
    ws.document(
        "uniform.mtlx",
        nodes,
        [
            connected("specular_roughness", "float", "R"),
            connected("base_color", "color3", "CC"),
        ],
    )
    material = ws.material("uniform.mtlx")
    check(
        abs(material.specular_roughness - 64 / 255) < 1e-9,
        f"a uniform roughness texture folds: {material.specular_roughness}",
    )
    check(
        len(material.base_color) == 3
        and all(abs(v - 32 / 255) < 1e-9 for v in material.base_color),
        f"a uniform color graph folds: {material.base_color}",
    )
    check(
        material.textures == [],
        f"a material of uniform textures needs no texture: {material.textures}",
    )


def test_ramp4(ws: Workspace) -> None:
    ws.png("uniform.png", UNIFORM)
    ws.png("rgb.png", RGB)
    ramp = node(
        "ramp4",
        "R4",
        "float",
        ("texcoord", "vector2", 'nodename="st"'),
        ("valuebl", "float", 'value="0"'),
        ("valuebr", "float", 'value="1"'),
        ("valuetl", "float", 'value="0"'),
        ("valuetr", "float", 'value="1"'),
    )
    ws.document(
        "ramp4.mtlx",
        image("B", "color3", "rgb.png") + ramp,
        [
            connected("base_color", "color3", "B"),
            connected("specular_roughness", "float", "R4"),
        ],
    )
    material = ws.material("ramp4.mtlx")
    orm = ws.derived(material, "map_orm")
    expected = [q((column + 0.5) / 2) for column in range(2)]
    check(
        [int(orm[0][c][1]) for c in range(2)] == expected,
        f"ramp4 with bl=0, br=1 is a left-to-right ramp at the texel centers {expected}: "
        f"{[int(orm[0][c][1]) for c in range(2)]}",
    )
    check(
        int(orm[0][0][0]) == 255 and int(orm[0][0][2]) == 0,
        f"R stays 1 and B the metalness 0: {orm[0][0].tolist()}",
    )
    check(
        ws.derived(material, "map_base_color") is None,
        "the base color stays a direct reference",
    )


def test_heighttonormal(ws: Workspace) -> None:
    heights = [0, 51, 102, 153]  # 0, 0.2, 0.4, 0.6 along one row of four texels
    ws.png("h.png", [[(v,) for v in heights]])
    for scale in (1.0, 2.0):
        height_node = node(
            "heighttonormal",
            "H",
            "vector3",
            ("in", "float", 'nodename="B"'),
            ("scale", "float", f'value="{scale:g}"'),
        )
        chain = (
            normal_chain("h.png")
            .replace(image("N", "vector3", "h.png", "Raw"), "")
            .replace('nodename="N"', 'nodename="H"')
        )
        ws.document(
            "height.mtlx",
            image("B", "float", "h.png") + height_node + chain,
            [connected("geometry_normal", "vector3", "M")],
        )
        normal = ws.derived(ws.material("height.mtlx"), "map_normal")
        h = [v / 255 for v in heights]
        row = []
        for column in range(4):
            # central difference with periodic wrap, per texcoord unit: (h[x+1] - h[x-1]) * width / 2
            du = (h[(column + 1) % 4] - h[(column - 1) % 4]) * 4 / 2
            nx = -(scale / 16) * du
            row.append(q((nx / math.hypot(nx, 1.0) + 1.0) / 2))
        check(
            [int(v) for v in normal[0][:, 0]] == row,
            f"heighttonormal scale {scale:g}: expected r {row}, got {[int(v) for v in normal[0][:, 0]]}",
        )
        check(
            int(normal[0][1][1]) == 128,
            f"flat along v: g is 128 after the green flip, got {normal[0][1][1]}",
        )
        blue = q((1.0 / math.hypot(-(scale / 16) * ((h[2] - h[0]) * 2), 1.0) + 1.0) / 2)
        check(
            int(normal[0][1][2]) == blue,
            f"encoded z at scale {scale:g}: expected {blue}, got {normal[0][1][2]}",
        )


def test_direct_reference_and_st1(ws: Workspace) -> None:
    ws.png("rgb.png", RGB)
    nodes = (
        image("B", "color3", "rgb.png", "srgb_texture")
        + ST1
        + image("R", "float", "rgb.png", texcoord="st1")
    )
    ws.document(
        "direct.mtlx",
        nodes,
        [
            connected("base_color", "color3", "B"),
            connected("specular_roughness", "float", "R"),
        ],
    )
    material = ws.material("direct.mtlx")
    base = next(t for t in material.textures if t.input_name == "base_color")
    check(
        same_path(base.filename, ws.root / "tex" / "rgb.png"),
        f"a plain image is referenced where it is: {base.filename}",
    )
    check(
        base.color_space == "srgb_rec709_scene",
        f"its color space is mapped to Aether: {base.color_space}",
    )
    check(
        material.uv_sets == ["st", "st1"],
        f"the uv sets of every image are recorded: {material.uv_sets}",
    )


# ── policies: duplicates, unsupported, symbolic mode ────────────────────────


def test_duplicate_inputs(ws: Workspace) -> None:
    """bottle.mtlx declares specular_roughness twice, ceramicMug.mtlx base_color as literal + image."""
    ws.png("rough.png", ROUGH)
    ws.png("rgb.png", RGB)
    source = image("R", "float", "rough.png")
    ws.document(
        "dup_identical.mtlx",
        source,
        [
            connected("specular_roughness", "float", "R"),
            connected("specular_roughness", "float", "R"),
        ],
    )
    library = ws.convert("dup_identical.mtlx")
    record = library.report.duplicates[0]
    check(
        len(library.report.duplicates) == 1
        and record.identical
        and record.input == "specular_roughness"
        and record.declarations == 2
        and record.resolution == "first"
        and "identical" in record.message(),
        f"the identical duplicate is reported: {library.report.duplicates}",
    )
    check(
        len(library.materials[0].textures) == 1,
        "an identical duplicate binds the input once",
    )

    mixed = [
        literal("base_color", "color3", "0, 0.386506, 1"),
        connected("base_color", "color3", "B"),
        connected("specular_roughness", "float", "R"),
    ]
    ws.document("dup_mixed.mtlx", image("B", "color3", "rgb.png") + source, mixed)
    library = ws.convert("dup_mixed.mtlx")
    material = library.materials[0]
    check(
        material.base_color == [0.0, 0.386506, 1.0]
        and [t.input_name for t in material.textures] == ["orm"],
        f"MaterialX getInput returns the first input, so the literal wins: {material.base_color} {material.textures}",
    )
    check(
        not library.report.duplicates[0].identical
        and "different" in library.report.duplicates[0].message(),
        "a literal and a connection are different declarations",
    )

    library = ws.convert("dup_mixed.mtlx", duplicate_inputs="last")
    check(
        [t.input_name for t in library.materials[0].textures] == ["base_color", "orm"]
        and library.materials[0].base_color is None,
        "duplicate_inputs='last' binds the second declaration",
    )
    raises(
        lambda: ws.convert("dup_mixed.mtlx", duplicate_inputs="error"),
        mtlx_graph.MaterialXGraphError,
        "Duplicate child name found: base_color",
    )


def test_length_inputs_convert_cm_to_m(ws: Workspace) -> None:
    """OpenPBR/MaterialX default centimetres become Aether metres only at this boundary."""
    ws.document(
        "lengths.mtlx",
        "",
        [
            literal("transmission_depth", "float", "0.25"),
            literal("subsurface_radius", "float", "0.49686"),
            literal("thin_film_thickness", "float", "0.42"),
        ],
    )
    material = ws.material("lengths.mtlx")
    check(abs(material.transmission_depth - 0.0025) < 1.0e-9,
          f"0.25 cm must become 0.0025 m: {material.transmission_depth}")
    check(abs(material.subsurface_radius - 0.0049686) < 1.0e-9,
          f"subsurface MFP must use the same unit conversion: {material.subsurface_radius}")
    check(material.thin_film_thickness == 420.0,
          f"0.42 micrometres must become 420 nm: {material.thin_film_thickness}")

    ws.document("default_sss.mtlx", "", [literal("subsurface_weight", "float", "1")])
    default_sss = ws.material("default_sss.mtlx")
    check(default_sss.subsurface_radius == 0.01,
          f"an omitted OpenPBR radius still defaults to 1 cm: {default_sss.subsurface_radius}")

    metres_path = ws.document("metres.mtlx", "", [literal("transmission_depth", "float", "2")])
    metres_path.write_text(metres_path.read_text(encoding="utf-8").replace(
        'name="transmission_depth" type="float" value="2"',
        'name="transmission_depth" type="float" value="2" unit="meter"'), encoding="utf-8")
    raises(lambda: ws.material("metres.mtlx"), mtlx_graph.MaterialXGraphError,
           "explicit unit 'meter'", "unsupported")


def test_unsupported_node_is_reported_not_dropped(ws: Workspace) -> None:
    ws.png("gray.png", GRAY)
    nodes = image("N", "vector3", "gray.png") + node(
        "multiply",
        "broken_mix",
        "vector3",
        ("in1", "vector3", 'nodename="N"'),
        ("in2", "float", 'value="1"'),
    )
    ws.document(
        "unsupported.mtlx",
        nodes,
        [connected("geometry_normal", "vector3", "broken_mix")],
    )
    raises(
        lambda: ws.convert("unsupported.mtlx"),
        mtlx_graph.MaterialXGraphError,
        "multiply 'broken_mix'",
        "geometry_normal",
    )
    library = ws.convert("unsupported.mtlx", allow_unsupported=True)
    entry = library.report.unsupported_inputs[0]
    check(
        entry["input"] == "geometry_normal"
        and "broken_mix" in entry["reason"]
        and library.materials[0].textures == []
        and entry in library.materials[0].unsupported_inputs,
        f"--allow-unsupported keeps the default and reports the node: {entry}",
    )


def test_scalar_map_is_referenced(ws: Workspace) -> None:
    ws.png("gray.png", GRAY)
    ws.document(
        "coat.mtlx",
        image("C", "float", "gray.png"),
        [connected("coat_weight", "float", "C")],
    )
    library = ws.convert("coat.mtlx")
    material = library.materials[0]
    check(
        material.coat_weight is None
        and len(material.textures) == 1
        and material.textures[0].input_name == "coat_weight"
        and material.textures[0].color_space == "data",
        f"a connected scalar image becomes the coat-weight map: {material.textures}",
    )


def test_data_color_graph_is_baked_without_srgb(ws: Workspace) -> None:
    ws.png("rgb.png", RGB)
    ws.document(
        "scatter.mtlx",
        image("C", "color3", "rgb.png")
        + node("invert", "I", "color3", ("in", "color3", 'nodename="C"')),
        [connected("transmission_scatter", "color3", "I")],
    )
    library = ws.convert("scatter.mtlx")
    texture = library.materials[0].textures[0]
    check(
        texture.input_name == "transmission_scatter" and texture.color_space == "data",
        f"transmission scatter stays raw data through a graph: {texture}",
    )
    ws.document(
        "direct_scatter.mtlx",
        image("C", "color3", "rgb.png"),
        [connected("transmission_scatter", "color3", "C")],
    )
    direct = ws.material("direct_scatter.mtlx").textures[0]
    check(direct.input_name == "transmission_scatter" and direct.color_space == "data",
          f"a direct scattering-coefficient image must not inherit color3's sRGB fallback: {direct}")


def test_symbolic_mode_and_bake_requirement(ws: Workspace) -> None:
    ws.png("rgb.png", RGB)
    ws.png("gray.png", GRAY)
    nodes = image("B", "color3", "rgb.png", "srgb_tx") + image("R", "float", "gray.png")
    ws.document(
        "flat.mtlx",
        nodes,
        [
            connected("base_color", "color3", "B"),
            connected("specular_roughness", "float", "R"),
        ],
    )
    library = ws.convert("flat.mtlx", texture_dir=None)
    keys = [
        conv.SUPPORTED_TEXTURE_SLOTS[t.input_name][0]
        for t in library.materials[0].textures
    ]
    check(
        keys == ["map_base_color", "map_roughness"],
        f"symbolic mode keeps the legacy direct maps: {keys}",
    )
    ws.document(
        "graph.mtlx",
        nodes + node("invert", "I", "float", ("in", "float", 'nodename="R"')),
        [connected("specular_roughness", "float", "I")],
    )
    raises(
        lambda: ws.convert("graph.mtlx", texture_dir=None),
        mtlx_graph.MaterialXGraphError,
        "baking",
    )


# ── deduplication, tiles, manifest, normalization, command line ────────────


def test_content_hash_deduplication(ws: Workspace) -> None:
    ws.png("rough.png", ROUGH)
    # invert(float) and invert(extract(color3, 0)) of the same gray file evaluate to the same texels.
    nodes = (
        image("R", "float", "rough.png")
        + image("B", "color3", "rough.png")
        + node("invert", "I", "float", ("in", "float", 'nodename="R"'))
        + extract("E", "B", "color3", 0)
        + node("invert", "J", "float", ("in", "float", 'nodename="E"'))
    )
    ws.document(
        "dedup.mtlx",
        nodes,
        [connected("specular_roughness", "float", "I")],
        extra=extra_surface(
            "srf2", "twin", [connected("specular_roughness", "float", "J")]
        ),
    )
    library = ws.convert("dedup.mtlx")
    orm_files = {
        t.filename
        for m in library.materials
        for t in m.textures
        if t.input_name == "orm"
    }
    check(
        [m.name for m in library.materials] == ["mat", "twin"],
        f"both materials converted: {[m.name for m in library.materials]}",
    )
    check(
        len(orm_files) == 1 and len(library.report.store.derived) == 1,
        f"identical derived pixels share one file: {len(orm_files)} references, {len(library.report.store.derived)} files",
    )


def test_udim_tiles_and_manifest(ws: Workspace) -> None:
    for tile, level in ((1001, 64), (1002, 128), (1011, 200)):
        ws.png(f"base.{tile}.png", [[(level,) * 3, (level + 1,) * 3]] * 2)
        if tile != 1011:
            ws.png(
                f"rough.{tile}.png",
                [[(level,), (level + 1,)], [(level,), (level + 1,)]],
            )
    nodes = image("B", "color3", "base.<UDIM>.png", "srgb_tx") + image(
        "R", "float", "rough.<UDIM>.png"
    )
    document = ws.document(
        "tiled.mtlx",
        nodes,
        [
            connected("base_color", "color3", "B"),
            connected("specular_roughness", "float", "R"),
        ],
    )
    library = ws.convert(
        "tiled.mtlx", normalize_udim=True, normalized_dir=ws.root / "escaped"
    )
    names = [m.name for m in library.materials]
    check(
        names == ["mat_1001", "mat_1002"],
        f"one variant per tile every realizable image has: {names}",
    )
    check(
        len(library.report.dropped_tiles) == 1
        and "1011" in library.report.dropped_tiles[0],
        f"the tile only the base color has is reported, not picked: {library.report.dropped_tiles}",
    )
    bases = [
        t.filename
        for m in library.materials
        for t in m.textures
        if t.input_name == "base_color"
    ]
    check(
        len(bases) == 2
        and same_path(bases[0], ws.root / "tex" / "base.1001.png")
        and same_path(bases[1], ws.root / "tex" / "base.1002.png"),
        f"each variant names its own tile: {bases}",
    )
    levels = [int(ws.derived(m, "map_orm")[0][0][1]) for m in library.materials]
    check(levels == [64, 128], f"each tile bakes from its own roughness file: {levels}")
    check(
        library.report.normalized_documents[0]["escaped_tokens"] == 2,
        "both <UDIM> tokens are escaped and counted",
    )

    entry = conv.document_entry(
        library, document, ws.root / "out" / "tiled.materials.toml"
    )
    manifest = conv.build_manifest(
        [entry],
        [],
        library.report.normalized_documents,
        set(library.report.source_files),
        set(library.report.store.derived),
    )
    check(
        [m["tile"] for m in entry["materials"]] == [1001, 1002]
        and [m["aether_material"] for m in entry["materials"]] == names,
        "the manifest maps every variant to its tile",
    )
    check(
        entry["materials"][1]["textures"]["map_base_color"]
        .replace("\\", "/")
        .endswith("tex/base.1002.png")
        and entry["materials"][1]["colorspaces"]["map_base_color_colorspace"]
        == "srgb_rec709_scene",
        f"the manifest names the files and color spaces of the tile: {entry['materials'][1]}",
    )
    check(
        manifest["counts"]["tiles"] == 2
        and manifest["counts"]["variants"] == 2
        and manifest["counts"]["unique_source_textures"] == 4
        and manifest["counts"]["unique_derived_textures"] == 2,
        f"manifest counts: {manifest['counts']}",
    )


def test_udim_normalization_is_opt_in(ws: Workspace) -> None:
    ws.png("base.1001.png", [[(64, 64, 64)]])
    document = ws.document(
        "raw.mtlx",
        image("B", "color3", "base.<UDIM>.png"),
        [connected("base_color", "color3", "B")],
    )
    original = document.read_bytes()
    check(b"<UDIM>" in original, "the fixture carries the raw token")
    raises(
        lambda: ws.convert("raw.mtlx"),
        mtlx_graph.MaterialXParseError,
        "Failed to parse MaterialX XML",
    )
    check(document.read_bytes() == original, "a rejected document is never rewritten")

    library = ws.convert(
        "raw.mtlx", normalize_udim=True, normalized_dir=ws.root / "escaped"
    )
    check(document.read_bytes() == original, "normalization preserves the original")
    escaped = ws.root / "escaped" / "raw.mtlx"
    check(
        escaped.read_bytes() == original.replace(b"<UDIM>", b"&lt;UDIM&gt;"),
        "only the raw <UDIM> tokens are escaped in the copy",
    )
    record = library.report.normalized_documents[0]
    check(
        len(library.report.normalized_documents) == 1
        and record["escaped_tokens"] == 1
        and same_path(record["normalized"], escaped)
        and same_path(record["source"], document),
        f"every normalized document is reported: {library.report.normalized_documents}",
    )
    raises(
        lambda: ws.convert(
            "raw.mtlx",
            normalize_udim=True,
            normalized_dir=ws.root / "materials" / "copies",
        ),
        mtlx_graph.MaterialXUdimError,
        "outside the source tree",
    )

    (ws.root / "materials" / "broken.mtlx").write_text(
        "<materialx><open_pbr_surface name='s'", encoding="utf-8"
    )
    raises(
        lambda: ws.convert(
            "broken.mtlx", normalize_udim=True, normalized_dir=ws.root / "escaped"
        ),
        mtlx_graph.MaterialXParseError,
        "no raw <UDIM>",
    )
    raises(lambda: ws.convert("absent.mtlx"), FileNotFoundError)


def test_command_line(ws: Workspace) -> None:
    ws.png("base.1001.png", [[(64, 64, 64)]])
    ws.png("rough.png", ROUGH)
    nodes = image("B", "color3", "base.<UDIM>.png") + image("R", "float", "rough.png")
    ws.document(
        "cli_tiled.mtlx",
        nodes,
        [
            connected("base_color", "color3", "B"),
            connected("specular_roughness", "float", "R"),
        ],
    )
    ws.document(
        "cli_plain.mtlx",
        "",
        [literal("base_color", "color3", "1, 0, 0")],
        material="plain",
    )
    out = ws.root / "cli"
    code = conv.main(
        [
            str(ws.root / "materials"),
            str(out),
            "--normalize-udim",
            "--allow-unsupported",
            "--force",
            "--max-resolution",
            "64",
        ]
    )
    manifest = json.loads((out / "manifest.json").read_text(encoding="utf-8"))
    counts = manifest["counts"]
    check(code == 0, f"directory conversion exit code: {code}")
    check(
        (out / "cli_tiled.materials.toml").is_file()
        and (out / "cli_plain.materials.toml").is_file(),
        "one library per document",
    )
    check(
        counts["documents"] == 2
        and counts["converted"] == 2
        and counts["failed"] == 0
        and counts["materials"] == 2
        and counts["variants"] == 2
        and counts["tiles"] == 1
        and counts["normalized_documents"] == 1
        and counts["unique_derived_textures"] == 1,
        f"manifest counts: {counts}",
    )
    check(
        (out / "normalized" / "cli_tiled.mtlx")
        .read_text(encoding="utf-8")
        .count("&lt;UDIM&gt;")
        == 1,
        "the escaped copy is written under <output>/normalized",
    )
    text = (out / "cli_tiled.materials.toml").read_text(encoding="utf-8")
    check(
        'map_base_color = "../tex/base.1001.png"' in text
        and 'map_orm = "textures/' in text,
        f"texture paths are relative to the library: {text}",
    )
    text = (out / "cli_plain.materials.toml").read_text(encoding="utf-8")
    check(
        text
        == 'model = "openpbr"\ncolorspace = "lin_rec709_scene"\n\n[plain]\nbase_color = [1.0, 0.0, 0.0]\n',
        f"an untextured document is converted as before: {text!r}",
    )

    (ws.root / "materials" / "cli_plain.mtlx").write_text(
        "<materialx><surfacematerial name='x'/></materialx>", encoding="utf-8"
    )
    code = conv.main(
        [str(ws.root / "materials"), str(out), "--force", "--normalize-udim"]
    )
    manifest = json.loads((out / "manifest.json").read_text(encoding="utf-8"))
    check(
        code == 1
        and manifest["counts"]["failed"] == 1
        and "No OpenPBR surface node" in manifest["failures"][0]["error"],
        f"a rejected document fails the run with its reason: code {code}, {manifest['failures']}",
    )
    check(
        conv.main([str(ws.root / "materials")]) == 1
        and conv.main([str(ws.root / "nowhere.mtlx")]) == 1,
        "usage errors exit with 1",
    )


# ── negative controls ───────────────────────────────────────────────────────


def _fake_index(value: int) -> ET.Element:
    element = ET.Element("input")
    element.set("name", "index")
    element.set("type", "integer")
    element.set("value", str(value))
    return element


def negative_controls(ws: Workspace) -> list[str]:
    """Break the implementation on purpose: the oracle of each named test must then fail."""
    broken: list[str] = []
    real_invert = mtlx_graph._HANDLERS["invert"]
    real_extract = mtlx_graph._HANDLERS["extract"]
    real_decode = mtlx_graph.srgb_decode

    def extract_ignores_index(ev, node):
        copied = mtlx_graph.Node(
            node.name, node.category, node.node_type, dict(node.inputs), node.element
        )
        copied.inputs["index"] = _fake_index(0)
        return real_extract(ev, copied)

    controls = [
        (
            "invert is a no-op",
            lambda: mtlx_graph._HANDLERS.update(
                invert=lambda ev, node: ev.value(node, "in")
            ),
            lambda: mtlx_graph._HANDLERS.update(invert=real_invert),
            test_invert,
        ),
        (
            "extract reads the wrong channel",
            lambda: mtlx_graph._HANDLERS.update(extract=extract_ignores_index),
            lambda: mtlx_graph._HANDLERS.update(extract=real_extract),
            test_normalmap_keeps_raw_channels,
        ),
        (
            "an sRGB source is not decoded before the math",
            lambda: setattr(mtlx_graph, "srgb_decode", lambda array: array),
            lambda: setattr(mtlx_graph, "srgb_decode", real_decode),
            test_colorcorrect_decodes_srgb_before_math,
        ),
    ]
    for name, apply, restore, test in controls:
        before = len(FAILURES)
        apply()
        try:
            test(ws)
        except Exception as exc:
            FAILURES.append(str(exc))
        finally:
            restore()
        if len(FAILURES) == before:
            broken.append(
                f"negative control '{name}' was not caught: the oracle is blind to this defect"
            )
        del FAILURES[before:]
    return broken


def test_texture_packager_preserves_precision(ws: Workspace) -> None:
    import numpy as np
    from PIL import Image

    source = ws.root / "converted"
    (source / "materials").mkdir(parents=True)
    (source / "tex").mkdir()
    hdr = np.zeros((4, 4, 4), dtype=np.uint16)
    hdr[..., 0] = np.arange(16, dtype=np.uint16).reshape(4, 4) * 4096
    hdr[..., 1] = 65535 - hdr[..., 0]
    hdr[..., 2] = 32768
    hdr[..., 3] = 65535
    mtlx_graph.save_png16(source / "tex/hdr.png", hdr)
    Image.fromarray(np.arange(4 * 4 * 3, dtype=np.uint8).reshape(4, 4, 3)).save(
        source / "tex/color.png"
    )
    (source / "manifest.json").write_text("{}", encoding="utf-8")
    original = (
        'model = "openpbr"\n[A]\nmap_emission_luminance = "../tex/hdr.png"\n'
        'map_base_color = "../tex/color.png"\n[B]\nmap_orm = "../tex/hdr.png"\n'
    )
    (source / "materials/library.materials.toml").write_text(original, encoding="utf-8")
    output = ws.root / "small"
    payload = package_textures.package(source, output, 2)
    check(
        payload["texture_references"] == 3
        and payload["unique_textures"] == 2
        and payload["high_precision"] == 1,
        f"packager deduplicates references and counts precision: {payload}",
    )
    check(payload["problems"] == [], f"packager reports no oversize output: {payload['problems']}")
    packed_hdr = package_textures.read_png16(output / "textures/hdr_preview.png")
    check(
        packed_hdr.dtype == np.uint16
        and packed_hdr.shape == (2, 2, 4)
        and int(packed_hdr.max()) > 255,
        f"RGBA16 survives Lanczos resize without an 8-bit round trip: {packed_hdr.dtype} {packed_hdr.shape}",
    )
    check(
        Image.open(output / "textures/color_preview.png").size == (2, 2),
        "8-bit RGB output respects the same dimension cap",
    )
    converted = (output / "materials/library.materials.toml").read_text(encoding="utf-8")
    check(
        converted.count("../textures/") == 3 and "../tex/" not in converted and original != converted,
        f"all material paths are rewritten to packaged textures: {converted}",
    )
    raises(lambda: package_textures.package(source, source), ValueError, "differ")


TESTS = [
    test_image_float_reads_red,
    test_extract,
    test_invert,
    test_normalmap_keeps_raw_channels,
    test_normalmap_scale,
    test_remap,
    test_colorcorrect_decodes_srgb_before_math,
    test_colorcorrect_parameters,
    test_colorcorrect_saturation_and_hue,
    test_color_to_float_is_luminance,
    test_orm_composition,
    test_high_precision_orm_and_scalar,
    test_uniform_texture_folds_into_scalar,
    test_ramp4,
    test_heighttonormal,
    test_direct_reference_and_st1,
    test_duplicate_inputs,
    test_length_inputs_convert_cm_to_m,
    test_unsupported_node_is_reported_not_dropped,
    test_scalar_map_is_referenced,
    test_data_color_graph_is_baked_without_srgb,
    test_symbolic_mode_and_bake_requirement,
    test_content_hash_deduplication,
    test_udim_tiles_and_manifest,
    test_udim_normalization_is_opt_in,
    test_texture_packager_preserves_precision,
    test_command_line,
]


def main() -> int:
    with tempfile.TemporaryDirectory() as directory:
        for test in TESTS:
            before = len(FAILURES)
            try:
                test(Workspace(Path(directory) / test.__name__))
            except Exception as exc:
                FAILURES.append(f"{test.__name__}: {type(exc).__name__}: {exc}")
            print(f"{'PASS' if len(FAILURES) == before else 'FAIL'} {test.__name__}")
        controls = negative_controls(Workspace(Path(directory) / "controls"))
    for failure in FAILURES:
        print(f"  -> {failure}")
    for failure in controls:
        print(f"  -> {failure}")
    print(
        f"\n{CHECKS} checks in {len(TESTS)} tests and {3} negative controls; "
        f"{len(FAILURES)} failed, {len(controls)} controls not caught"
    )
    return 1 if FAILURES or controls else 0


if __name__ == "__main__":
    raise SystemExit(main())
