"""Validate converted Aether material libraries the way the Aether parser reads them.

    python tests/validate_libraries.py <directory> [--manifest PATH] [--aether-validate EXE]
                                      [--scene-out PATH]

For every ``*.materials.toml`` in ``<directory>`` it checks, with the standard
library only, what ``aether::MaterialLibrary::load`` accepts and silently
ignores otherwise:

* every key of a material is one Aether dispatches (the keyword tables of
  ``src/aether/format/MaterialKwTables.hpp`` plus the ``geometry_*`` aliases),
* every ``map_*_colorspace`` token is one Aether parses (an unknown token would
  fall back to the default with a warning),
* the colour maps share the primaries of the material's ``colorspace`` (a
  material that mixes them is skipped by Aether),
* every texture path resolves to an existing file, relative to the library.

With ``--manifest`` the textures of the manifest are checked as well. With
``--aether-validate`` a scene that references every library and binds every
material is written (``--scene-out``, default ``<directory>/_validation.scene.toml``)
and handed to ``aether-validate``; the scene parser reads the scene only, so this
proves the library references and material names assemble, not their content.
Exit code 0 = everything accepted.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tomllib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mtlx_to_toml import SUPPORTED_TEXTURE_SLOTS

# Share the converter's complete 16-slot vocabulary instead of drifting back to the old
# base/normal/ORM subset. The validator still checks the parser's separate gamut rule below.
TEXTURE_PATHS = {slot[0] for slot in SUPPORTED_TEXTURE_SLOTS.values()}
ALIASES = {
    "geometry_normal": "map_normal",
    "geometry_coat_normal": "map_coat_normal",
    "geometry_tangent": "map_tangent",
    "geometry_coat_tangent": "map_coat_tangent",
}
COLORS = {
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
BOOLS = {"geometry_thin_walled", "emission_as_light_source"}
SCALARS = {
    "base_weight",
    "base_metalness",
    "base_diffuse_roughness",
    "specular_weight",
    "specular_roughness",
    "specular_roughness_anisotropy",
    "specular_ior",
    "transmission_weight",
    "transmission_depth",
    "transmission_scatter_anisotropy",
    "transmission_dispersion_scale",
    "transmission_dispersion_abbe_number",
    "thin_film_weight",
    "thin_film_thickness",
    "thin_film_ior",
    "coat_weight",
    "coat_roughness",
    "coat_roughness_anisotropy",
    "coat_ior",
    "coat_darkening",
    "fuzz_weight",
    "fuzz_roughness",
    "emission_luminance",
    "subsurface_weight",
    "subsurface_radius",
    "subsurface_scatter_anisotropy",
    "geometry_opacity",
}
ALLOWED = (
    TEXTURE_PATHS
    | {key + "_colorspace" for key in TEXTURE_PATHS}
    | COLORS
    | BOOLS
    | SCALARS
    | set(ALIASES)
)
TEXTURE_COLORSPACES = {
    "data",
    "srgb_rec709_scene",
    "lin_rec709_scene",
    "lin_rec2020_scene",
}
COLOR_MAPS = ("map_base_color", "map_emission_color")


def _primaries(token: str) -> str:
    return "rec2020" if "2020" in token else "rec709"


def validate_library(path: Path, problems: list[str]) -> tuple[int, int, set[Path]]:
    """Return (materials, texture references, resolved texture files) of one library."""
    with path.open("rb") as handle:
        document = tomllib.load(handle)
    library_colorspace = document.get("colorspace", "lin_rec709_scene")
    if library_colorspace not in {"lin_rec709_scene", "lin_rec2020_scene"}:
        problems.append(
            f"{path.name}: unknown library colorspace {library_colorspace!r}"
        )
    if document.get("model", "openpbr") != "openpbr":
        problems.append(
            f"{path.name}: unknown model {document['model']!r} (Aether skips every material)"
        )
    materials = references = 0
    files: set[Path] = set()
    for name, table in document.items():
        if not isinstance(table, dict):
            continue
        materials += 1
        colorspaces: dict[str, str] = {}
        colorspace = table.get("colorspace", library_colorspace)
        for key, value in table.items():
            if key in {"model", "colorspace"}:
                continue
            if key not in ALLOWED:
                problems.append(f"{path.name} [{name}]: Aether ignores the key '{key}'")
                continue
            if key in ALIASES or key in TEXTURE_PATHS:
                if not isinstance(value, str):
                    problems.append(
                        f"{path.name} [{name}]: '{key}' must be a string path"
                    )
                else:
                    references += 1
                    target = (path.parent / value).resolve()
                    if not target.is_file():
                        problems.append(
                            f"{path.name} [{name}]: '{key}' does not resolve: {value}"
                        )
                    files.add(target)
            elif key.endswith("_colorspace"):
                if value not in TEXTURE_COLORSPACES:
                    problems.append(
                        f"{path.name} [{name}]: Aether cannot parse colorspace {value!r} for '{key}'"
                    )
                colorspaces[key[: -len("_colorspace")]] = value
        for map_key in COLOR_MAPS:
            token = colorspaces.get(map_key)
            if (
                map_key in table
                and token
                and _primaries(token) != _primaries(colorspace)
            ):
                problems.append(
                    f"{path.name} [{name}]: {map_key} colorspace {token} mixes primaries with the "
                    f"material colorspace {colorspace}; Aether skips the material"
                )
    return materials, references, files


def write_scene(
    libraries: list[Path], names: dict[str, list[str]], output: Path
) -> None:
    lines = [
        "# validation scene written by tests/validate_libraries.py: every library, every material"
    ]
    references = []
    for library in libraries:
        try:
            reference = library.relative_to(output.parent).as_posix()
        except ValueError:
            reference = library.as_posix()
        references.append(f'"{reference}"')
    lines.append("material_libraries = [" + ", ".join(references) + "]")
    lines += [
        "",
        "[[mesh]]",
        'name = "box"',
        'type = "box"',
        "half_extents = [0.5, 0.5, 0.5]",
    ]
    for index, (library, materials) in enumerate(names.items()):
        for material in materials:
            lines += [
                "",
                "[[instance]]",
                'mesh = "box"',
                f'material = "{material}"',
                f"translate = [{index % 12}, {index // 12}, 0.0]",
            ]
    output.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("directory", help="Directory with *.materials.toml libraries")
    parser.add_argument(
        "--manifest", help="Conversion manifest whose textures are checked too"
    )
    parser.add_argument(
        "--aether-validate",
        help="Path of aether-validate.exe; writes and validates a scene",
    )
    parser.add_argument(
        "--scene-out",
        help="Where the validation scene goes (default: <directory>/_validation.scene.toml)",
    )
    args = parser.parse_args(argv)

    directory = Path(args.directory)
    libraries = sorted(directory.glob("*.materials.toml"))
    if not libraries:
        print(f"ERROR: no *.materials.toml in {directory}")
        return 1
    problems: list[str] = []
    names: dict[str, list[str]] = {}
    files: set[Path] = set()
    materials = references = 0
    for library in libraries:
        count, refs, resolved = validate_library(library, problems)
        materials += count
        references += refs
        files |= resolved
        with library.open("rb") as handle:
            names[library.name] = [
                key
                for key, value in tomllib.load(handle).items()
                if isinstance(value, dict)
            ]

    if args.manifest:
        import json

        manifest = json.loads(Path(args.manifest).read_text(encoding="utf-8"))
        base = directory
        for entry in manifest.get("documents", []):
            for material in entry["materials"]:
                for map_key, value in material["textures"].items():
                    target = (base / entry["output"]).parent / value
                    if not target.resolve().is_file():
                        problems.append(
                            f"manifest {entry['source']} [{material['aether_material']}]: "
                            f"{map_key} does not resolve: {value}"
                        )
                    else:
                        files.add(target.resolve())

    for problem in problems:
        print(f"PROBLEM {problem}")
    print(
        f"{len(libraries)} libraries, {materials} materials, {references} texture references, "
        f"{len(files)} unique texture files; {len(problems)} problem(s)"
    )

    if args.aether_validate:
        scene = (
            Path(args.scene_out)
            if args.scene_out
            else directory / "_validation.scene.toml"
        )
        write_scene(libraries, names, scene)
        result = subprocess.run(
            [args.aether_validate, str(scene)],
            capture_output=True,
            text=True,
            check=False,
        )
        print(
            f"aether-validate exit {result.returncode}: {result.stdout.strip()} {result.stderr.strip()}"
        )
        if result.returncode != 0:
            problems.append(f"aether-validate rejected {scene}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
