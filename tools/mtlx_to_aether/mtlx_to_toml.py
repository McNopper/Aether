"""Convert OpenPBR MaterialX (.mtlx) libraries to Aether .materials.toml.

MaterialX documents are plain XML, so this converter parses them directly with
the Python standard library (xml.etree.ElementTree) and has **no dependency on
the MaterialX Python bindings**. Only OpenPBR Surface materials are accepted;
other surface models (Standard Surface, glTF PBR, ...) are rejected.

Output matches the Aether material-library contract (see
src/aether/format/MaterialLibrary.cpp and src/aether/types/MaterialDesc.hpp):
  * top-level `model = "openpbr"` and `colorspace = "lin_rec709_scene" | ...`
  * one TOML table per material, keyed by name
  * scalar/colour parameters use exact OpenPBR Surface 1.1.1 input names
  * texture bindings use Aether `map_*` keys (+ `map_*_colorspace`)

Two modes:

* **symbolic** (no ``texture_dir``): a surface input fed by a plain ``image`` node becomes a
  direct texture reference, exactly as before. Anything else that would need a derived texture is
  an error. Needs the standard library only.
* **baking** (``texture_dir`` given; the default of the command line): every connected input is
  resolved through the node graph (``mtlx_graph``) and baked into an equivalent Aether texture:
  base color, ORM (occlusion 1, roughness, metalness), normals, emission, opacity, coat normals
  and tangents. Derived files are deduplicated by content hash. A UDIM material becomes one
  variant per tile, named ``<material>_<tile>``.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import xml.etree.ElementTree as ET
from collections.abc import Iterable
from dataclasses import dataclass, field
from pathlib import Path

try:  # imported as the `mtlx_to_aether` package ...
    from . import mtlx_graph
except ImportError:  # ... or as a script / top-level module next to mtlx_graph.py
    import mtlx_graph

MaterialXError = mtlx_graph.MaterialXError
MaterialXParseError = mtlx_graph.MaterialXParseError
MaterialXGraphError = mtlx_graph.MaterialXGraphError
MaterialXUdimError = mtlx_graph.MaterialXUdimError
UdimFiles = mtlx_graph.UdimPattern

# OpenPBR surface node categories (element tag of a node instance).
OPENPBR_SURFACE_NODES = ("open_pbr_surface", "openpbr_surface")

# Non-OpenPBR surface node categories that must be rejected.
NON_OPENPBR_SURFACES = {
    "standard_surface",
    "gltf_pbr",
    "disney_principled",
    "usd_preview_surface",
    "UsdPreviewSurface",
}

# MaterialX OpenPBR input name -> (Aether map_* key, default color space).
SUPPORTED_TEXTURE_SLOTS = {
    "base_color": ("map_base_color", "srgb_rec709_scene"),
    "specular_roughness": ("map_roughness", "data"),
    "roughness": ("map_roughness", "data"),
    "base_metalness": ("map_metalness", "data"),
    "metalness": ("map_metalness", "data"),
    "geometry_normal": ("map_normal", "data"),
    "normal": ("map_normal", "data"),
    "geometry_coat_normal": ("map_coat_normal", "data"),
    "coat_normal": ("map_coat_normal", "data"),
    "geometry_tangent": ("map_tangent", "data"),
    "tangent": ("map_tangent", "data"),
    "geometry_coat_tangent": ("map_coat_tangent", "data"),
    "coat_tangent": ("map_coat_tangent", "data"),
    "emission_color": ("map_emission_color", "srgb_rec709_scene"),
    "orm": ("map_orm", "data"),
    "occlusion_roughness_metalness": ("map_orm", "data"),
    "geometry_opacity": ("map_opacity", "data"),
    "coat_weight": ("map_coat_weight", "data"),
    "transmission_weight": ("map_transmission_weight", "data"),
    "subsurface_color": ("map_subsurface_color", "srgb_rec709_scene"),
    "subsurface_weight": ("map_subsurface_weight", "data"),
    "transmission_color": ("map_transmission_color", "srgb_rec709_scene"),
    "transmission_scatter": ("map_transmission_scatter", "data"),
    "emission_luminance": ("map_emission_luminance", "data"),
    "fuzz_weight": ("map_fuzz_weight", "data"),
}

# Connected OpenPBR inputs -> (Aether map key, how the converter realizes it).
#   color    plain image is referenced, anything else is baked (sRGB-encoded)
#   normal   plain image is referenced, a `normalmap` chain is baked
#   tangent  plain image only
#   orm      plain image only (an explicit `orm` input)
#   roughness / metalness  one baked ORM texture (G, B; R = 1)
#   opacity  baked into the alpha of `map_opacity`
AETHER_SLOTS = {
    "base_color": ("map_base_color", "color"),
    "emission_color": ("map_emission_color", "color"),
    "geometry_normal": ("map_normal", "normal"),
    "normal": ("map_normal", "normal"),
    "geometry_coat_normal": ("map_coat_normal", "normal"),
    "coat_normal": ("map_coat_normal", "normal"),
    "geometry_tangent": ("map_tangent", "tangent"),
    "tangent": ("map_tangent", "tangent"),
    "geometry_coat_tangent": ("map_coat_tangent", "tangent"),
    "coat_tangent": ("map_coat_tangent", "tangent"),
    "orm": ("map_orm", "orm"),
    "occlusion_roughness_metalness": ("map_orm", "orm"),
    "specular_roughness": ("", "roughness"),
    "roughness": ("", "roughness"),
    "base_metalness": ("", "metalness"),
    "metalness": ("", "metalness"),
    "geometry_opacity": ("map_opacity", "opacity"),
    "coat_weight": ("map_coat_weight", "scalar"),
    "transmission_weight": ("map_transmission_weight", "scalar"),
    "subsurface_color": ("map_subsurface_color", "color"),
    "subsurface_weight": ("map_subsurface_weight", "scalar"),
    "transmission_color": ("map_transmission_color", "color"),
    "transmission_scatter": ("map_transmission_scatter", "data_color"),
    "emission_luminance": ("map_emission_luminance", "scalar"),
    "fuzz_weight": ("map_fuzz_weight", "scalar"),
}

# Inputs whose texture is referenced as is (no baking possible or needed).
DIRECT_KINDS = ("color", "normal", "tangent", "orm", "scalar", "data_color")

# Scalar / colour / boolean OpenPBR parameters this converter understands.
COLOR3_INPUTS = {
    "base_color",
    "specular_color",
    "transmission_color",
    "transmission_scatter",
    "coat_color",
    "fuzz_color",
    "emission_color",
    "subsurface_color",
    "subsurface_radius_scale",
}
BOOL_INPUTS = {"geometry_thin_walled", "emission_as_light_source"}

# OpenPBR Surface 1.1 defaults of the two ORM channels, used when only one of them is textured.
OPENPBR_DEFAULTS = {"specular_roughness": 0.3, "base_metalness": 0.0}

# MaterialX/OpenPBR length parameters default to centimetres; Aether's renderer-facing material
# values are metres. Native Aether libraries are not converted, only values entering through here.
CENTIMETRE_LENGTH_INPUTS = {"transmission_depth", "subsurface_radius"}
DEFAULT_LENGTH_UNITS = {"cm", "centimeter", "centimeters", "um", "micrometer", "micrometers"}


def _parameter_value(input_name: str, value: float) -> float:
    if input_name in CENTIMETRE_LENGTH_INPUTS:
        return value * 0.01
    if input_name == "thin_film_thickness":
        return value * 1000.0  # OpenPBR micrometres -> Aether nanometres
    return value


def _validate_default_units(root) -> None:
    """Reject explicit overrides rather than silently applying the wrong default-unit scale."""
    for element in root.iter():
        for inp in element:
            if mtlx_graph._strip_ns(inp.tag) != "input":
                continue
            unit = (inp.get("unit") or "").strip().lower()
            if unit and unit not in DEFAULT_LENGTH_UNITS:
                raise MaterialXGraphError(
                    f"explicit unit '{inp.get('unit')}' on input '{inp.get('name')}' is unsupported; "
                    "convert to OpenPBR's default centimetres/micrometres before conversion"
                )


MANIFEST_GENERATOR = "mtlx_to_aether/2"


class OpenPBRValidationError(MaterialXError):
    """The document does not use the OpenPBR Surface model."""


@dataclass
class TextureReference:
    filename: str
    input_name: str
    color_space: str


@dataclass
class OpenPBRMaterial:
    name: str
    base_color: list[float] | None = None
    base_weight: float | None = None
    base_metalness: float | None = None
    base_diffuse_roughness: float | None = None
    specular_color: list[float] | None = None
    specular_weight: float | None = None
    specular_roughness: float | None = None
    specular_roughness_anisotropy: float | None = None
    specular_ior: float | None = None
    transmission_weight: float | None = None
    transmission_color: list[float] | None = None
    transmission_depth: float | None = None
    transmission_scatter: list[float] | None = None
    transmission_scatter_anisotropy: float | None = None
    transmission_dispersion_scale: float | None = None
    transmission_dispersion_abbe_number: float | None = None
    coat_weight: float | None = None
    coat_color: list[float] | None = None
    coat_roughness: float | None = None
    coat_roughness_anisotropy: float | None = None
    coat_ior: float | None = None
    coat_darkening: float | None = None
    fuzz_weight: float | None = None
    fuzz_color: list[float] | None = None
    fuzz_roughness: float | None = None
    thin_film_weight: float | None = None
    thin_film_thickness: float | None = None
    thin_film_ior: float | None = None
    emission_color: list[float] | None = None
    emission_luminance: float | None = None
    emission_as_light_source: bool | None = None
    subsurface_weight: float | None = None
    subsurface_color: list[float] | None = None
    subsurface_radius: float | None = None
    subsurface_radius_scale: list[float] | None = None
    subsurface_scatter_anisotropy: float | None = None
    geometry_opacity: float | None = None
    geometry_thin_walled: bool | None = None
    textures: list[TextureReference] = field(default_factory=list)
    # Not written to TOML: bookkeeping for tiled variants and the manifest.
    tile: int | None = None
    source_name: str | None = None

    def __post_init__(self) -> None:
        self.uv_sets: list[str] = []
        self.unsupported_inputs: list[dict] = []
        self.notes: list[str] = []


# Fields of OpenPBRMaterial that are TOML parameters.
PARAMETER_FIELDS = frozenset(OpenPBRMaterial.__dataclass_fields__) - {
    "name",
    "textures",
    "tile",
    "source_name",
}


@dataclass
class ConversionReport:
    """Everything the conversion found besides the materials themselves."""

    normalized_documents: list[dict] = field(default_factory=list)
    duplicates: list[mtlx_graph.DuplicateInput] = field(default_factory=list)
    unsupported_inputs: list[dict] = field(default_factory=list)
    approximations: list[str] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)
    source_files: set[Path] = field(default_factory=set)
    tiles: dict[str, list[int | None]] = field(default_factory=dict)
    dropped_tiles: list[str] = field(default_factory=list)
    store: mtlx_graph.TextureStore | None = None


@dataclass
class MaterialLibrary:
    model: str = "openpbr"
    colorspace: str = "lin_rec709_scene"
    materials: list[OpenPBRMaterial] = field(default_factory=list)
    report: ConversionReport | None = None


# ── TOML emission ───────────────────────────────────────────────────────────


def _quote(value: str) -> str:
    return json.dumps(value)


def _format_value(value) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        return repr(float(value))
    if isinstance(value, str):
        return _quote(value)
    if isinstance(value, Path):
        return _quote(str(value))
    if isinstance(value, Iterable):
        return "[" + ", ".join(_format_value(item) for item in value) + "]"
    raise TypeError(f"Unsupported TOML value: {type(value)!r}")


def _relativize_texture_path(
    texture_path: str, output_dir: Path, source_file: Path
) -> str:
    if not texture_path or any(ch in texture_path for ch in "<>*?"):
        return texture_path
    raw = Path(texture_path)
    if not raw.is_absolute():
        raw = source_file.parent / raw
    try:
        return os.path.relpath(raw, output_dir).replace("\\", "/")
    except ValueError:
        return str(raw).replace("\\", "/")


def _write_material_library(
    library: MaterialLibrary, output_path: Path, source_file: Path
) -> None:
    lines: list[str] = [
        f"model = {_quote(library.model)}",
        f"colorspace = {_quote(library.colorspace)}",
        "",
    ]
    for material in library.materials:
        lines.append(f"[{material.name}]")
        for field_name in material.__dataclass_fields__:
            if field_name not in PARAMETER_FIELDS:
                continue
            value = getattr(material, field_name)
            if value is None:
                continue
            lines.append(f"{field_name} = {_format_value(value)}")
        for texture in material.textures:
            slot = SUPPORTED_TEXTURE_SLOTS.get(texture.input_name)
            if slot is None:
                continue
            map_name, default_cs = slot
            rel = _relativize_texture_path(
                texture.filename, output_path.parent, source_file
            )
            lines.append(f"{map_name} = {_quote(rel)}")
            lines.append(
                f"{map_name}_colorspace = {_quote(texture.color_space or default_cs)}"
            )
        lines.append("")
    output_path.write_text("\n".join(lines).rstrip() + "\n", encoding="utf-8")


# ── MaterialX XML parsing (no MaterialX library) ─────────────────────────────


def _strip_ns(tag: str) -> str:
    """Strip an XML namespace prefix (``{uri}tag`` -> ``tag``)."""
    return tag.rsplit("}", 1)[-1]


def _parse_floats(text: str) -> list[float]:
    parts = [p for p in text.replace(",", " ").split() if p]
    return [float(p) for p in parts]


def _parse_scalar(value: str) -> float:
    return float(value.strip())


def _parse_bool(value: str) -> bool:
    return value.strip().lower() in {"true", "1", "yes"}


def _map_colorspace(token: str | None, fallback: str) -> str:
    if not token:
        return fallback
    t = token.strip().lower()
    if "2020" in t:
        return "lin_rec2020_scene"
    if "srgb" in t:
        return "srgb_rec709_scene"
    if "lin_rec709" in t or "709" in t:
        return "lin_rec709_scene"
    if t in {"raw", "none", "data"}:
        return "data"
    return fallback


def _document_colorspace(root: ET.Element) -> str:
    """Map the document `colorspace` attribute to an Aether scene color space."""
    token = root.get("colorspace")
    if not token:
        return "lin_rec709_scene"
    t = token.strip().lower()
    if "2020" in t:
        return "lin_rec2020_scene"
    # sRGB primaries are Rec.709; Aether records linear Rec.709 for scene values.
    return "lin_rec709_scene"


def _material_name_map(root: ET.Element) -> dict[str, str]:
    """Map surfaceshader node name -> owning surfacematerial name (if any)."""
    mapping: dict[str, str] = {}
    for child in root:
        if _strip_ns(child.tag) != "surfacematerial":
            continue
        mat_name = child.get("name")
        if not mat_name:
            continue
        for inp in child:
            if _strip_ns(inp.tag) == "input" and inp.get("name") == "surfaceshader":
                shader = inp.get("nodename")
                if shader:
                    mapping[shader] = mat_name
    return mapping


def _validate_openpbr(root: ET.Element) -> None:
    categories = [_strip_ns(child.tag) for child in root]
    if any(cat in OPENPBR_SURFACE_NODES for cat in categories):
        return
    for cat in categories:
        if cat in NON_OPENPBR_SURFACES:
            raise OpenPBRValidationError(
                f"MaterialX file uses a non-OpenPBR surface model: {cat}"
            )
    raise OpenPBRValidationError("No OpenPBR surface node found in document")


# ── graph resolution and baking ─────────────────────────────────────────────


def _reachable_images(
    doc: mtlx_graph.Document, surface: mtlx_graph.Node, input_name: str
):
    """The image nodes an input depends on, following ``nodename`` connections."""
    seen: set[str] = set()
    images: list[mtlx_graph.Node] = []
    stack = [mtlx_graph.connection_of(surface.inputs.get(input_name))]
    while stack:
        ref = stack.pop()
        if not ref or ref in seen:
            continue
        seen.add(ref)
        node = doc.nodes.get(ref)
        if node is None:
            continue  # reported when the input is evaluated
        if node.category == "image":
            images.append(node)
        stack.extend(mtlx_graph.connection_of(inp) for inp in node.inputs.values())
    return images


def _material_tiles(
    doc: mtlx_graph.Document, surface: mtlx_graph.Node, source_dir: Path
):
    """Yield ``(tile, tile files, dropped tiles)``: one entry per UDIM tile the material has.

    A variant exists only where *every* image of every input Aether can realize has the tile;
    tiles that only some images have are reported, never picked silently. Images of inputs
    without an Aether map take no part in that decision but are resolved for each tile as well.
    """
    patterns: dict[str, dict[int, Path]] = {}
    realizable: list[str] = []
    for input_name in surface.inputs:
        if input_name in AETHER_SLOTS:
            realizable.append(input_name)
        for image in _reachable_images(doc, surface, input_name):
            file_input = image.inputs.get("file")
            file_value = (
                (file_input.get("value") or "") if file_input is not None else ""
            )
            if file_value in patterns:
                continue
            pattern = mtlx_graph.split_udim(file_value)
            if pattern is None:
                continue
            # The pattern is relative to the document; only its file name is the tile pattern.
            directory = (source_dir / pattern.prefix).parent
            tiles = UdimFiles(os.path.basename(pattern.prefix), pattern.suffix).files(
                directory
            )
            if not tiles:
                raise MaterialXUdimError(
                    f"{image.path()}: no file matches the UDIM pattern '{file_value}' in {directory}"
                )
            patterns[file_value] = tiles
    if not patterns:
        yield None, {}, []
        return
    required = [
        patterns[file_value]
        for input_name in realizable
        for image in _reachable_images(doc, surface, input_name)
        if (file_value := (image.inputs.get("file").get("value") or "")) in patterns
    ]
    common = set.intersection(
        *(set(tiles) for tiles in (required or list(patterns.values())))
    )
    dropped = [
        f"'{file_value}' has {sorted(set(tiles) - common)} but the material has not"
        for file_value, tiles in patterns.items()
        if set(tiles) - common
    ]
    if not common:
        raise MaterialXUdimError(
            f"{surface.path()}: its UDIM images share no tile: "
            + ", ".join(
                f"'{name}': {sorted(tiles)}" for name, tiles in patterns.items()
            )
        )
    for tile in sorted(common):
        available = {
            name: tiles[tile] for name, tiles in patterns.items() if tile in tiles
        }
        yield tile, available, dropped


def _direct_reference(
    direct: mtlx_graph.DirectImage, input_name: str
) -> TextureReference:
    """A reference to an untouched source file (absolute, so a UDIM tile is the concrete file)."""
    color_space = "data" if AETHER_SLOTS.get(input_name, ("", ""))[1] == "data_color" else direct.aether_colorspace
    return TextureReference(
        filename=str(direct.path),
        input_name=input_name,
        color_space=color_space,
    )


def _producer(signal: mtlx_graph.Signal) -> str:
    if signal.node is not None:
        return signal.node.path()
    if signal.direct is not None:
        return "an image"
    return "a value"


def _report_unsupported(
    report: ConversionReport,
    material: OpenPBRMaterial,
    input_name: str,
    reason: str,
    allow_unsupported: bool,
) -> None:
    entry = {
        "material": material.source_name,
        "tile": material.tile,
        "input": input_name,
        "reason": reason,
    }
    if not allow_unsupported:
        raise MaterialXGraphError(
            f"material '{material.source_name}': input '{input_name}': {reason} "
            "(pass --allow-unsupported to keep the input's default and report it)"
        )
    report.unsupported_inputs.append(entry)
    material.unsupported_inputs.append(entry)


def _fold_constant(
    material: OpenPBRMaterial, input_name: str, signal: mtlx_graph.Signal
) -> None:
    """Store a graph that resolved to a constant in the scalar/color parameter instead of a map."""
    channels = signal.channels or (0.0,)
    if input_name in BOOL_INPUTS or input_name not in PARAMETER_FIELDS:
        return
    if input_name in COLOR3_INPUTS:
        setattr(material, input_name, [float(round(v, 9)) for v in channels[:3]])
    else:
        setattr(material, input_name, float(round(_parameter_value(input_name, channels[0]), 9)))
    material.notes.append(
        f"'{input_name}' is driven by a uniform value and folds to {channels}"
    )


def _label(material: OpenPBRMaterial, suffix: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", f"{material.name}_{suffix}")


def _baked_reference(
    store: mtlx_graph.TextureStore,
    pixels,
    material: OpenPBRMaterial,
    slot: str,
    input_name: str,
    color_space: str,
) -> TextureReference:
    path = store.save(pixels, _label(material, slot))
    return TextureReference(
        filename=str(path), input_name=input_name, color_space=color_space
    )


def _orm_signal(material: OpenPBRMaterial, kind: str, part):
    """The roughness/metalness signal of an ORM channel: a texture, or the scalar parameter."""
    if part is None:
        value = getattr(
            material, "specular_roughness" if kind == "roughness" else "base_metalness"
        )
        default = OPENPBR_DEFAULTS[
            "specular_roughness" if kind == "roughness" else "base_metalness"
        ]
        return mtlx_graph.constant(default if value is None else value), True
    _input_name, signal = part
    if signal.array is not None:
        uniform = mtlx_graph.uniform_value(signal.array)
        if uniform is not None:
            return mtlx_graph.constant(uniform[0]), True
    return signal, False


def _bind_surface(
    evaluator: mtlx_graph.Evaluator,
    surface: mtlx_graph.Node,
    material: OpenPBRMaterial,
    *,
    store: mtlx_graph.TextureStore | None,
    allow_unsupported: bool,
    report: ConversionReport,
) -> None:
    """Resolve every input of one OpenPBR surface node into parameters and texture references."""
    connected: dict[str, mtlx_graph.Signal] = {}
    order: list[str] = []
    for input_name, inp in surface.inputs.items():
        if mtlx_graph.connection_of(inp):
            order.append(input_name)
            try:
                connected[input_name] = evaluator.evaluate_connection(inp)
            except mtlx_graph.MaterialXError as exc:
                _report_unsupported(
                    report, material, input_name, str(exc), allow_unsupported
                )
            continue

        # A literal input; one that Aether has no parameter for is reported, never dropped silently.
        value = inp.get("value")
        if value is None:
            continue
        if input_name not in PARAMETER_FIELDS:
            report.unsupported_inputs.append(
                {
                    "material": material.source_name,
                    "tile": material.tile,
                    "input": input_name,
                    "reason": f"Aether has no parameter '{input_name}' (literal {value!r} is not written)",
                }
            )
            continue
        try:
            if input_name in COLOR3_INPUTS:
                comps = _parse_floats(value)
                if len(comps) >= 3:
                    setattr(material, input_name, comps[:3])
            elif input_name in BOOL_INPUTS:
                setattr(material, input_name, _parse_bool(value))
            else:
                setattr(material, input_name, _parameter_value(input_name, _parse_scalar(value)))
        except ValueError:
            continue

    entries: list[object] = []  # TextureReference or the "orm" marker, in input order
    orm: dict[str, tuple[str, mtlx_graph.Signal]] = {}
    for input_name in order:
        signal = connected.get(input_name)
        if signal is None:
            continue
        slot = AETHER_SLOTS.get(input_name)
        if slot is None:
            _report_unsupported(
                report,
                material,
                input_name,
                f"Aether has no texture map for the OpenPBR input (driven by {_producer(signal)})",
                allow_unsupported,
            )
            continue
        _map_key, kind = slot
        if signal.is_constant:
            _fold_constant(material, input_name, signal)
            continue
        direct = signal.direct
        if direct is not None and kind in DIRECT_KINDS:
            if input_name == "emission_luminance":
                # The shader multiplies a mapped luminance by this scale. Direct images need no
                # normalization, but legacy scalar zero means the historical scale of one.
                material.emission_luminance = 1.0
            material.textures.append(_direct_reference(direct, input_name))
            continue
        if kind in ("roughness", "metalness"):
            if direct is not None and store is None:
                # Legacy symbolic output: separate maps, which Aether records but Harmonia does not
                # upload. Baking mode produces one ORM texture instead.
                material.textures.append(_direct_reference(direct, input_name))
                continue
            if kind not in orm:
                entries.append("orm")
            orm[kind] = (input_name, signal)
            continue
        if store is None or signal.array is None:
            raise MaterialXGraphError(
                f"material '{material.source_name}': input '{input_name}' needs a derived texture "
                "(pass texture_dir to bake it)"
            )
        array = signal.array
        uniform = mtlx_graph.uniform_value(array)
        if kind == "color":
            if uniform is not None:
                _fold_constant(
                    material, input_name, mtlx_graph.constant(*uniform, is_color=True)
                )
                continue
            pixels, color_space, clamped = mtlx_graph.encode_color(array)
            if clamped:
                material.notes.append(
                    f"'{input_name}': {clamped} texel(s) outside [0, 1] were clamped"
                )
            entries.append(
                _baked_reference(
                    store, pixels, material, "color", input_name, color_space
                )
            )
        elif kind == "normal":
            if signal.node is None or signal.node.category != "normalmap":
                _report_unsupported(
                    report,
                    material,
                    input_name,
                    f"Aether normal maps are baked from a 'normalmap' node, not from {_producer(signal)}",
                    allow_unsupported,
                )
                continue
            pixels, color_space, _ = mtlx_graph.encode_normal(array)
            entries.append(
                _baked_reference(
                    store, pixels, material, "normal", input_name, color_space
                )
            )
        elif kind == "opacity":
            if uniform is not None:
                _fold_constant(material, input_name, mtlx_graph.constant(uniform[0]))
                continue
            pixels, color_space, _ = mtlx_graph.encode_opacity(
                mtlx_graph.convert(signal, 1).array
            )
            entries.append(
                _baked_reference(
                    store, pixels, material, "opacity", input_name, color_space
                )
            )
        elif kind == "scalar":
            if uniform is not None:
                _fold_constant(material, input_name, mtlx_graph.constant(uniform[0]))
                continue
            scalar = mtlx_graph.convert(signal, 1).array
            if input_name == "emission_luminance":
                pixels, color_space, clamped, scale = mtlx_graph.encode_emission_luminance(scalar)
                material.emission_luminance = scale
                if scale > 1.0:
                    material.notes.append(
                        f"'{input_name}': normalized by the authored HDR scale {scale:.9g}; no highlight clipping"
                    )
            else:
                pixels, color_space, clamped = mtlx_graph.encode_scalar(scalar)
            if clamped:
                material.notes.append(
                    f"'{input_name}': {clamped} texel(s) outside [0, 1] were clamped"
                )
            entries.append(
                _baked_reference(
                    store, pixels, material, "scalar", input_name, color_space
                )
            )
        elif kind == "data_color":
            if uniform is not None:
                _fold_constant(
                    material, input_name, mtlx_graph.constant(*uniform, is_color=True)
                )
                continue
            pixels, color_space, clamped = mtlx_graph.encode_data_color(array)
            if clamped:
                material.notes.append(
                    f"'{input_name}': {clamped} texel(s) outside [0, 1] were clamped"
                )
            entries.append(
                _baked_reference(
                    store, pixels, material, "data_color", input_name, color_space
                )
            )
        else:  # tangent / orm through a graph
            _report_unsupported(
                report,
                material,
                input_name,
                f"Aether has no baked form for this map (driven by {_producer(signal)}); "
                "only a plain image can be referenced",
                allow_unsupported,
            )

    if orm:
        roughness, rough_is_const = _orm_signal(
            material, "roughness", orm.get("roughness")
        )
        metalness, metal_is_const = _orm_signal(
            material, "metalness", orm.get("metalness")
        )
        if rough_is_const and metal_is_const:
            for kind, sig in (("roughness", roughness), ("metalness", metalness)):
                if kind in orm:
                    _fold_constant(material, orm[kind][0], sig)
        else:
            shape = next(
                s.array.shape[:2] for s in (roughness, metalness) if s.array is not None
            )
            pixels, color_space, _ = mtlx_graph.encode_orm(
                evaluator.broadcast(roughness, shape),
                evaluator.broadcast(metalness, shape),
            )
            # The ORM texture takes the place its first channel was declared in.
            entries[entries.index("orm")] = _baked_reference(
                store, pixels, material, "orm", "orm", color_space
            )
    if (material.subsurface_weight or 0.0) > 0.0 and material.subsurface_radius is None:
        material.subsurface_radius = 0.01  # OpenPBR default 1 cm, in Aether metres
    if (material.thin_film_weight or 0.0) > 0.0 and material.thin_film_thickness is None:
        material.thin_film_thickness = 500.0  # OpenPBR default 0.5 micrometres, in nanometres
    material.textures.extend(
        ref for ref in entries if isinstance(ref, TextureReference)
    )


# ── conversion ──────────────────────────────────────────────────────────────


def convert_document(
    source_file: Path,
    *,
    output_dir: Path | None = None,
    texture_dir: Path | None = None,
    normalize_udim: bool = False,
    normalized_dir: Path | None = None,
    allow_unsupported: bool = False,
    duplicate_inputs: str = "first",
    max_resolution: int | None = None,
) -> MaterialLibrary:
    """Convert one OpenPBR MaterialX document into a material library.

    Without ``texture_dir`` the conversion is *symbolic*: an input fed by a plain ``image`` node
    becomes a direct texture reference (the original behavior) and anything that would need a
    derived texture is an error. With ``texture_dir`` every such input is baked into a PNG under
    that directory (deduplicated by content hash) and roughness/metalness maps become one ORM
    texture. ``output_dir`` is where the TOML will be written; texture paths are made relative
    to it. A UDIM material yields one variant per tile named ``<material>_<tile>``.
    """
    source_file = Path(source_file)
    output_dir = Path(output_dir) if output_dir is not None else source_file.parent
    root, normalized = mtlx_graph.load_xml(
        source_file, normalize_udim=normalize_udim, normalized_dir=normalized_dir
    )

    _validate_openpbr(root)
    _validate_default_units(root)

    doc = mtlx_graph.Document(root, duplicate_policy=duplicate_inputs)
    shader_to_material = _material_name_map(root)
    colorspace = _document_colorspace(root)
    report = ConversionReport(duplicates=list(doc.duplicates))
    if normalized is not None:
        report.normalized_documents.append(
            {
                "source": str(normalized.source),
                "normalized": str(normalized.normalized),
                "escaped_tokens": normalized.escaped_tokens,
            }
        )
    store = (
        mtlx_graph.TextureStore(texture_dir, max_resolution=max_resolution)
        if texture_dir
        else None
    )
    report.store = store

    materials: list[OpenPBRMaterial] = []
    seen: set[str] = set()
    for child in root:
        if _strip_ns(child.tag) not in OPENPBR_SURFACE_NODES:
            continue
        node_name = child.get("name") or "material"
        surface = doc.nodes.get(node_name) or doc.make_node(child)
        base_name = shader_to_material.get(node_name, node_name)
        for tile, tile_files, dropped in _material_tiles(
            doc, surface, source_file.parent
        ):
            evaluator = mtlx_graph.Evaluator(
                doc, source_dir=source_file.parent, store=store, tile_files=tile_files
            )
            material = OpenPBRMaterial(
                name=base_name if tile is None else f"{base_name}_{tile}"
            )
            material.tile = tile
            material.source_name = base_name
            _bind_surface(
                evaluator,
                surface,
                material,
                store=store,
                allow_unsupported=allow_unsupported,
                report=report,
            )
            if material.name in seen:
                raise MaterialXGraphError(f"two materials are named '{material.name}'")
            seen.add(material.name)
            material.uv_sets = sorted(evaluator.uv_sets)
            materials.append(material)
            report.tiles.setdefault(base_name, []).append(tile)
            report.source_files.update(evaluator.source_files)
            for approximation in evaluator.approximations:
                if approximation not in report.approximations:
                    report.approximations.append(approximation)
            for note in dropped:
                if f"{base_name}: {note}" not in report.dropped_tiles:
                    report.dropped_tiles.append(f"{base_name}: {note}")
            if any(uv != "st" for uv in material.uv_sets):
                report.notes.append(
                    f"{base_name}: texture coordinates {material.uv_sets} are used; Aether samples "
                    "every map with the mesh's single texture-coordinate set"
                )

    return MaterialLibrary(
        model="openpbr", colorspace=colorspace, materials=materials, report=report
    )


def _material_entry(
    material: OpenPBRMaterial, document: Path, output_dir: Path
) -> dict:
    textures: dict[str, str] = {}
    colorspaces: dict[str, str] = {}
    for texture in material.textures:
        slot = SUPPORTED_TEXTURE_SLOTS.get(texture.input_name)
        if slot is None:
            continue
        map_name, default_cs = slot
        textures[map_name] = _relativize_texture_path(
            texture.filename, output_dir, document
        )
        colorspaces[f"{map_name}_colorspace"] = texture.color_space or default_cs
    return {
        "material": material.source_name,
        "tile": material.tile,
        "aether_material": material.name,
        "uv_sets": material.uv_sets,
        "textures": textures,
        "colorspaces": colorspaces,
        "unsupported_inputs": material.unsupported_inputs,
        "notes": material.notes,
    }


def document_entry(library: MaterialLibrary, document: Path, output_path: Path) -> dict:
    """The manifest record of one converted document."""
    report = library.report or ConversionReport()
    return {
        "source": document.name,
        "output": output_path.name,
        "materials": [
            _material_entry(m, document, output_path.parent) for m in library.materials
        ],
        "unsupported_inputs": report.unsupported_inputs,
        "duplicates": [d.message() for d in report.duplicates],
        "approximations": list(report.approximations),
        "dropped_tiles": list(report.dropped_tiles),
        "notes": list(report.notes),
    }


def build_manifest(
    entries: list[dict],
    failures: list[dict],
    normalized: list[dict],
    source_files: set[Path],
    derived: set[str],
) -> dict:
    materials = [m for entry in entries for m in entry["materials"]]
    return {
        "generator": MANIFEST_GENERATOR,
        "tile_naming": "aether_material = '<material>_<tile>'; a material without a tile keeps its name",
        "counts": {
            "documents": len(entries) + len(failures),
            "converted": len(entries),
            "failed": len(failures),
            "materials": len(
                {(e["source"], m["material"]) for e in entries for m in e["materials"]}
            ),
            "variants": len(materials),
            "tiles": sum(1 for m in materials if m["tile"] is not None),
            "unique_source_textures": len(source_files),
            "unique_derived_textures": len(derived),
            "unsupported_inputs": sum(len(e["unsupported_inputs"]) for e in entries),
            "normalized_documents": len(normalized),
        },
        "normalized_documents": normalized,
        "documents": entries,
        "failures": failures,
    }


# ── CLI ──────────────────────────────────────────────────────────────────────


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Convert an OpenPBR MaterialX (.mtlx) file, or a directory of them, to Aether "
        ".materials.toml material libraries",
    )
    parser.add_argument("input", help="Input .mtlx file, or a directory of .mtlx files")
    parser.add_argument(
        "output",
        nargs="?",
        help="Output .materials.toml file or directory (a directory input needs one)",
    )
    parser.add_argument(
        "--colorspace",
        choices=["lin_rec709_scene", "lin_rec2020_scene"],
        default=None,
        help="Override the material-library color space",
    )
    parser.add_argument(
        "--force", action="store_true", help="Overwrite an existing output file"
    )
    parser.add_argument(
        "--no-bake",
        action="store_true",
        help="Symbolic conversion without derived textures: plain images are referenced and any "
        "node graph that needs baking is an error",
    )
    parser.add_argument(
        "--texture-dir",
        help="Directory for derived textures (default: <output dir>/textures)",
    )
    parser.add_argument(
        "--max-resolution",
        type=int,
        default=None,
        help="Shrink every texture to at most this many pixels on its longer side (box filter)",
    )
    parser.add_argument(
        "--normalize-udim",
        action="store_true",
        help="Opt in: escape raw <UDIM> tokens in a copy of each affected document, written to "
        "--normalized-dir outside the source tree, and report every normalized document",
    )
    parser.add_argument(
        "--normalized-dir",
        help="Where the escaped copies go (default: <output dir>/normalized)",
    )
    parser.add_argument(
        "--allow-unsupported",
        action="store_true",
        help="Keep the default of an input Aether cannot realize (no texture map, unsupported node) "
        "and report it instead of failing",
    )
    parser.add_argument(
        "--duplicate-inputs",
        choices=mtlx_graph.DUPLICATE_POLICIES,
        default="first",
        help="Resolution of an input declared twice on one node (default: first, as MaterialX "
        "resolves getInput; 'error' is its validator)",
    )
    parser.add_argument(
        "--manifest",
        help="Write a JSON manifest (a directory input defaults to <output>/manifest.json)",
    )
    return parser


def _output_path(input_path: Path, output_arg: str | None) -> Path:
    if output_arg is None:
        return input_path.with_suffix(".materials.toml")
    out = Path(output_arg)
    if out.is_dir() or str(output_arg).endswith(("\\", "/")):
        return out / input_path.with_suffix(".materials.toml").name
    return out


def _print_report(library: MaterialLibrary) -> None:
    report = library.report
    if report is None:
        return
    for record in report.normalized_documents:
        print(
            f"Normalized {Path(record['source']).name}: {record['escaped_tokens']} raw <UDIM> "
            f"token(s) escaped -> {record['normalized']}"
        )
    for duplicate in report.duplicates:
        print(f"WARNING: {duplicate.message()}")
    for entry in report.unsupported_inputs:
        print(
            f"WARNING: unsupported input '{entry['input']}' on material '{entry['material']}'"
            f" (tile {entry['tile']}): {entry['reason']}"
        )
    for note in [*report.dropped_tiles, *report.notes, *report.approximations]:
        print(f"NOTE: {note}")


def _guard_outside(path: Path, source: Path, what: str) -> None:
    if path.resolve().is_relative_to(source.resolve()):
        raise MaterialXError(
            f"{what} must be written outside the source tree: {path} is inside {source}"
        )


def _convert_directory(
    args: argparse.Namespace, source_dir: Path, output_dir: Path
) -> int:
    documents = sorted(
        p for p in source_dir.iterdir() if p.is_file() and p.suffix.lower() == ".mtlx"
    )
    if not documents:
        print(f"ERROR: no .mtlx documents in {source_dir}")
        return 1
    texture_dir = (
        None
        if args.no_bake
        else Path(args.texture_dir)
        if args.texture_dir
        else output_dir / "textures"
    )
    normalized_dir = (
        (
            Path(args.normalized_dir)
            if args.normalized_dir
            else output_dir / "normalized"
        )
        if args.normalize_udim
        else None
    )
    manifest_path = (
        Path(args.manifest) if args.manifest else output_dir / "manifest.json"
    )
    if texture_dir is not None:
        _guard_outside(texture_dir, source_dir, "derived textures")
    if normalized_dir is not None:
        _guard_outside(normalized_dir, source_dir, "normalized copies")

    entries: list[dict] = []
    failures: list[dict] = []
    normalized: list[dict] = []
    source_files: set[Path] = set()
    derived: set[str] = set()
    output_dir.mkdir(parents=True, exist_ok=True)
    for document in documents:
        output_path = output_dir / document.with_suffix(".materials.toml").name
        try:
            if output_path.exists() and not args.force:
                raise MaterialXError(
                    f"output file already exists (use --force): {output_path}"
                )
            library = convert_document(
                document,
                output_dir=output_dir,
                texture_dir=texture_dir,
                normalize_udim=args.normalize_udim,
                normalized_dir=normalized_dir,
                allow_unsupported=args.allow_unsupported,
                duplicate_inputs=args.duplicate_inputs,
                max_resolution=args.max_resolution,
            )
            if args.colorspace:
                library.colorspace = args.colorspace
            _write_material_library(library, output_path, document)
        except MaterialXError as exc:
            failures.append({"source": document.name, "error": str(exc)})
            print(f"FAILED {document.name}: {exc}")
            continue
        report = library.report or ConversionReport()
        normalized.extend(report.normalized_documents)
        source_files.update(report.source_files)
        if report.store is not None:
            derived.update(report.store.derived)
        entries.append(document_entry(library, document, output_path))
        print(
            f"Converted {document.name} -> {output_path.name} "
            f"({len(library.materials)} material variant(s), colorspace={library.colorspace})"
        )
        _print_report(library)

    manifest = build_manifest(entries, failures, normalized, source_files, derived)
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(
        json.dumps(manifest, indent=2, default=str) + "\n", encoding="utf-8"
    )
    counts = manifest["counts"]
    print(
        f"{counts['converted']} of {counts['documents']} document(s) converted, "
        f"{counts['failed']} failed; {counts['materials']} materials, {counts['variants']} variants, "
        f"{counts['tiles']} tiled; {counts['unique_source_textures']} unique source texture(s), "
        f"{counts['unique_derived_textures']} unique derived texture(s); "
        f"{counts['unsupported_inputs']} unsupported input(s); "
        f"{counts['normalized_documents']} normalized document(s); manifest: {manifest_path}"
    )
    return 1 if failures else 0


def main(argv: list[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    try:
        input_path = Path(args.input)
        if not input_path.exists():
            print(f"ERROR: input file not found: {input_path}")
            return 1
        if input_path.is_dir():
            if not args.output:
                print("ERROR: a directory input needs an output directory")
                return 1
            return _convert_directory(args, input_path, Path(args.output))

        output_path = _output_path(input_path, args.output)
        output_path.parent.mkdir(parents=True, exist_ok=True)
        if output_path.exists() and not args.force:
            print(f"ERROR: output file already exists (use --force): {output_path}")
            return 1

        texture_dir = (
            None
            if args.no_bake
            else Path(args.texture_dir)
            if args.texture_dir
            else output_path.parent / "textures"
        )
        normalized_dir = (
            (
                Path(args.normalized_dir)
                if args.normalized_dir
                else output_path.parent / "normalized"
            )
            if args.normalize_udim
            else None
        )
        library = convert_document(
            input_path,
            output_dir=output_path.parent,
            texture_dir=texture_dir,
            normalize_udim=args.normalize_udim,
            normalized_dir=normalized_dir,
            allow_unsupported=args.allow_unsupported,
            duplicate_inputs=args.duplicate_inputs,
            max_resolution=args.max_resolution,
        )
        if args.colorspace:
            library.colorspace = args.colorspace

        _write_material_library(library, output_path, input_path)
        _print_report(library)
        report = library.report or ConversionReport()
        print(
            f"Converted {input_path.name} -> {output_path.name} "
            f"({len(library.materials)} material(s), colorspace={library.colorspace}); "
            f"{len(report.source_files)} source texture(s), "
            f"{len(report.store.derived) if report.store else 0} derived texture(s)"
        )
        if args.manifest:
            manifest = build_manifest(
                [document_entry(library, input_path, output_path)],
                [],
                report.normalized_documents,
                set(report.source_files),
                set(report.store.derived) if report.store else set(),
            )
            Path(args.manifest).parent.mkdir(parents=True, exist_ok=True)
            Path(args.manifest).write_text(
                json.dumps(manifest, indent=2, default=str) + "\n", encoding="utf-8"
            )
        return 0
    except OpenPBRValidationError as exc:
        print(f"ERROR: {exc}")
        print("This converter only supports OpenPBR Surface materials.")
        return 1
    except MaterialXError as exc:
        print(f"ERROR: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
