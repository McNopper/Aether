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
"""

from __future__ import annotations

import argparse
import json
import os
import xml.etree.ElementTree as ET
from collections.abc import Iterable
from dataclasses import dataclass, field
from pathlib import Path


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
}

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


class MaterialXError(RuntimeError):
    """Base error for the converter."""


class OpenPBRValidationError(MaterialXError):
    """The document does not use the OpenPBR Surface model."""


class MaterialXParseError(MaterialXError):
    """The .mtlx file could not be parsed as XML."""


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


@dataclass
class MaterialLibrary:
    model: str = "openpbr"
    colorspace: str = "lin_rec709_scene"
    materials: list[OpenPBRMaterial] = field(default_factory=list)


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


def _relativize_texture_path(texture_path: str, output_dir: Path, source_file: Path) -> str:
    if not texture_path or any(ch in texture_path for ch in "<>*?"):
        return texture_path
    raw = Path(texture_path)
    if not raw.is_absolute():
        raw = (source_file.parent / raw)
    try:
        return os.path.relpath(raw, output_dir).replace("\\", "/")
    except ValueError:
        return str(raw).replace("\\", "/")


def _write_material_library(library: MaterialLibrary, output_path: Path, source_file: Path) -> None:
    lines: list[str] = [
        f"model = {_quote(library.model)}",
        f"colorspace = {_quote(library.colorspace)}",
        "",
    ]
    for material in library.materials:
        lines.append(f"[{material.name}]")
        for field_name in material.__dataclass_fields__:
            if field_name in {"name", "textures"}:
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
            rel = _relativize_texture_path(texture.filename, output_path.parent, source_file)
            lines.append(f"{map_name} = {_quote(rel)}")
            lines.append(f"{map_name}_colorspace = {_quote(texture.color_space or default_cs)}")
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


def _index_nodes(root: ET.Element) -> dict[str, ET.Element]:
    """Map node instance name -> element for all direct children with a name."""
    nodes: dict[str, ET.Element] = {}
    for child in root:
        name = child.get("name")
        if name:
            nodes[name] = child
    return nodes


def _resolve_image_filename(image_node: ET.Element) -> str | None:
    for inp in image_node:
        if _strip_ns(inp.tag) != "input":
            continue
        if inp.get("name") == "file" and inp.get("type") == "filename":
            return inp.get("value")
    return None


def _texture_from_input(inp: ET.Element, nodes: dict[str, ET.Element], slot_default_cs: str) -> str | None:
    """If an input connects to an <image> node, return its file path."""
    ref = inp.get("nodename") or inp.get("nodegraph")
    if not ref:
        return None
    target = nodes.get(ref)
    if target is None or _strip_ns(target.tag) != "image":
        return None
    return _resolve_image_filename(target)


def _image_colorspace(inp: ET.Element, nodes: dict[str, ET.Element]) -> str | None:
    ref = inp.get("nodename")
    if not ref:
        return None
    target = nodes.get(ref)
    if target is None:
        return None
    cs = target.get("colorspace")
    if cs:
        return cs
    for child in target:
        if _strip_ns(child.tag) == "input" and child.get("name") == "file":
            return child.get("colorspace")
    return None


def _extract_material(node: ET.Element, name: str, nodes: dict[str, ET.Element]) -> OpenPBRMaterial:
    material = OpenPBRMaterial(name=name)
    fields = material.__dataclass_fields__
    for inp in node:
        if _strip_ns(inp.tag) != "input":
            continue
        input_name = inp.get("name")
        if not input_name:
            continue

        # Texture connection (image node) takes precedence over inline values.
        slot = SUPPORTED_TEXTURE_SLOTS.get(input_name)
        if slot is not None:
            filename = _texture_from_input(inp, nodes, slot[1])
            if filename:
                cs = _map_colorspace(_image_colorspace(inp, nodes), slot[1])
                material.textures.append(
                    TextureReference(filename=filename, input_name=input_name, color_space=cs)
                )
                continue

        value = inp.get("value")
        if value is None or input_name not in fields:
            continue
        try:
            if input_name in COLOR3_INPUTS:
                comps = _parse_floats(value)
                if len(comps) >= 3:
                    setattr(material, input_name, comps[:3])
            elif input_name in BOOL_INPUTS:
                setattr(material, input_name, _parse_bool(value))
            else:
                setattr(material, input_name, _parse_scalar(value))
        except ValueError:
            continue
    return material


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


def _all_node_categories(root: ET.Element) -> list[str]:
    return [_strip_ns(child.tag) for child in root]


def _validate_openpbr(root: ET.Element) -> None:
    categories = _all_node_categories(root)
    if any(cat in OPENPBR_SURFACE_NODES for cat in categories):
        return
    for cat in categories:
        if cat in NON_OPENPBR_SURFACES:
            raise OpenPBRValidationError(
                f"MaterialX file uses a non-OpenPBR surface model: {cat}"
            )
    raise OpenPBRValidationError("No OpenPBR surface node found in document")


def convert_document(source_file: Path) -> MaterialLibrary:
    try:
        tree = ET.parse(str(source_file))
    except ET.ParseError as exc:
        raise MaterialXParseError(f"Failed to parse MaterialX XML: {exc}") from exc
    root = tree.getroot()

    _validate_openpbr(root)

    nodes = _index_nodes(root)
    shader_to_material = _material_name_map(root)
    colorspace = _document_colorspace(root)

    materials: list[OpenPBRMaterial] = []
    for child in root:
        if _strip_ns(child.tag) not in OPENPBR_SURFACE_NODES:
            continue
        node_name = child.get("name") or "material"
        material_name = shader_to_material.get(node_name, node_name)
        materials.append(_extract_material(child, material_name, nodes))

    return MaterialLibrary(model="openpbr", colorspace=colorspace, materials=materials)


# ── CLI ──────────────────────────────────────────────────────────────────────


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Convert an OpenPBR MaterialX (.mtlx) file to an Aether .materials.toml file"
    )
    parser.add_argument("input", help="Input .mtlx file")
    parser.add_argument("output", nargs="?", help="Output .materials.toml file or directory")
    parser.add_argument(
        "--colorspace",
        choices=["lin_rec709_scene", "lin_rec2020_scene"],
        default=None,
        help="Override the material-library color space",
    )
    parser.add_argument("--force", action="store_true", help="Overwrite an existing output file")
    return parser


def _output_path(input_path: Path, output_arg: str | None) -> Path:
    if output_arg is None:
        return input_path.with_suffix(".materials.toml")
    out = Path(output_arg)
    if out.is_dir() or str(output_arg).endswith(("\\", "/")):
        return out / input_path.with_suffix(".materials.toml").name
    return out


def main(argv: list[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    try:
        input_path = Path(args.input)
        if not input_path.exists():
            print(f"ERROR: input file not found: {input_path}")
            return 1

        output_path = _output_path(input_path, args.output)
        output_path.parent.mkdir(parents=True, exist_ok=True)
        if output_path.exists() and not args.force:
            print(f"ERROR: output file already exists (use --force): {output_path}")
            return 1

        library = convert_document(input_path)
        if args.colorspace:
            library.colorspace = args.colorspace

        _write_material_library(library, output_path, input_path)
        print(
            f"Converted {input_path.name} -> {output_path.name} "
            f"({len(library.materials)} material(s), colorspace={library.colorspace})"
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
