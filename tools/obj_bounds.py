#!/usr/bin/env python3
"""Compute the object-space axis-aligned bounding box (AABB) of a Wavefront OBJ.

Reads a Wavefront ``.obj`` file, streams its ``v`` (vertex-position) lines as
bytes (mirroring ``_obj_transform.process`` and ``ObjImporter::parse``), and
computes the per-axis min/max of all vertex positions. Faces (``f``), texture
coordinates (``vt``), normals (``vn``) and free-form points (``vp``) are
ignored — the bounding volume of the vertex set equals the bounding volume of
the mesh, so topology is not needed.

Emits an Aether TOML ``bounds`` block:

    # object-space bounds for shader_ball.obj (12345 vertices)
    [bounds]
    min = [-13.46, 0.21, -13.46]
    max = [13.46, 26.78, 11.55]

Optionally (``--sphere``) also computes a Ritter bounding sphere via two
streaming passes (no vertex array is held in memory, so large OBJs are fine).

Standard library only. Usage:

    python tools/obj_bounds.py assets/shader_ball.obj
    python tools/obj_bounds.py assets/shader_ball.obj --write assets/shader_ball.scene.toml
    python tools/obj_bounds.py assets/shader_ball.obj --sphere --pretty
"""

from __future__ import annotations

import argparse
import math
import os
import re
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path


# ── Number formatting ────────────────────────────────────────────────────────


def fmt(value: float) -> str:
    """Format a float compactly, suppressing binary floating-point noise.

    Duplicated from ``_obj_transform.fmt`` so this tool stays standalone (no
    import dependency on a private sibling module). ``%.10g`` yields 10
    significant digits — well beyond typical OBJ source precision of 6-7
    digits — and avoids artifacts like ``0.32705510000000004``. Signed zero is
    normalised to ``0``.
    """
    if value == 0:
        value = 0.0
    return "%.10g" % value


# ── Result dataclasses ───────────────────────────────────────────────────────


@dataclass
class Bounds:
    """An axis-aligned bounding box in object space."""

    min_x: float
    min_y: float
    min_z: float
    max_x: float
    max_y: float
    max_z: float

    def center(self) -> tuple[float, float, float]:
        """Return the box center ((min + max) / 2 per axis)."""
        return (
            (self.min_x + self.max_x) / 2.0,
            (self.min_y + self.max_y) / 2.0,
            (self.min_z + self.max_z) / 2.0,
        )

    def extents(self) -> tuple[float, float, float]:
        """Return the full per-axis extent (max - min)."""
        return (
            self.max_x - self.min_x,
            self.max_y - self.min_y,
            self.max_z - self.min_z,
        )

    def sphere_radius(self) -> float:
        """Radius of the AABB circumsphere (center to farthest corner).

        This is the simple 'corner sphere': it encloses the whole box (and
        therefore every vertex) but is not the tightest sphere. Use Ritter
        (``--sphere``) for a tighter fit.
        """
        cx, cy, cz = self.center()
        dx = self.max_x - cx
        dy = self.max_y - cy
        dz = self.max_z - cz
        return math.sqrt(dx * dx + dy * dy + dz * dz)


@dataclass
class Sphere:
    """A bounding sphere (center + radius)."""

    cx: float
    cy: float
    cz: float
    radius: float

    def center(self) -> tuple[float, float, float]:
        """Return the sphere center as a tuple."""
        return (self.cx, self.cy, self.cz)


@dataclass
class Result:
    """Outcome of analysing an OBJ: AABB, vertex count, optional sphere."""

    bounds: Bounds
    count: int
    sphere: Sphere | None = None


# ── OBJ streaming ────────────────────────────────────────────────────────────


def _warn(path: Path, lineno: int, message: str) -> None:
    """Write a single warning line to stderr."""
    sys.stderr.write("warning: %s:%d: %s\n" % (path, lineno, message))


def iter_vertices(path: Path):
    """Yield ``(x, y, z, lineno)`` for every ``v`` line in the OBJ.

    The file is streamed as **bytes** (matching ``_obj_transform.process`` and
    ``ObjImporter::parse``). Only lines whose stripped head starts with
    ``b"v "`` or ``b"v\\t"`` are treated as positions: this naturally excludes
    ``vn``, ``vt`` and ``vp`` because their second byte is a letter, not
    whitespace. The first three whitespace-separated tokens after the keyword
    are parsed as floats (extra columns such as vertex colours are ignored).
    Malformed ``v`` lines emit a warning and are skipped.
    """
    with open(path, "rb") as f:
        for lineno, line in enumerate(f, start=1):
            head = line.lstrip()
            if not (head.startswith(b"v ") or head.startswith(b"v\t")):
                continue
            parts = head.split()
            if len(parts) < 4:
                _warn(path, lineno, "ignoring 'v' line with fewer than 3 coordinates")
                continue
            try:
                x = float(parts[1])
                y = float(parts[2])
                z = float(parts[3])
            except ValueError:
                _warn(path, lineno, "ignoring 'v' line with non-numeric coordinates")
                continue
            yield x, y, z, lineno


def compute(path: Path, want_sphere: bool = False) -> Result:
    """Compute the AABB of ``path`` and, optionally, a Ritter bounding sphere.

    Raises ``ValueError`` if the OBJ contains no ``v`` lines.

    The AABB is found in a single streaming pass. When ``want_sphere`` is set,
    the same first pass also records the six per-axis extreme points; the
    farthest-apart opposite-axis pair seeds an initial sphere, and a second
    streaming pass grows that sphere to enclose every vertex (classic Ritter).
    No vertex array is buffered, so memory stays O(1).
    """
    # Pass 1: AABB (+ extreme points if a sphere is wanted).
    count = 0
    min_x = min_y = min_z = math.inf
    max_x = max_y = max_z = -math.inf
    min_x_pt = max_x_pt = min_y_pt = max_y_pt = min_z_pt = max_z_pt = None

    for x, y, z, _ in iter_vertices(path):
        count += 1
        if x < min_x:
            min_x = x
            min_x_pt = (x, y, z)
        if x > max_x:
            max_x = x
            max_x_pt = (x, y, z)
        if y < min_y:
            min_y = y
            min_y_pt = (x, y, z)
        if y > max_y:
            max_y = y
            max_y_pt = (x, y, z)
        if z < min_z:
            min_z = z
            min_z_pt = (x, y, z)
        if z > max_z:
            max_z = z
            max_z_pt = (x, y, z)

    if count == 0:
        raise ValueError("no vertex positions ('v' lines) found in %s" % path)

    bounds = Bounds(min_x, min_y, min_z, max_x, max_y, max_z)

    sphere: Sphere | None = None
    if want_sphere:
        extremes = [
            p for p in (min_x_pt, max_x_pt, min_y_pt, max_y_pt, min_z_pt, max_z_pt)
            if p is not None
        ]
        seed_a, seed_b = _farthest_pair(extremes)
        cx = (seed_a[0] + seed_b[0]) / 2.0
        cy = (seed_a[1] + seed_b[1]) / 2.0
        cz = (seed_a[2] + seed_b[2]) / 2.0
        radius = _dist(seed_a, seed_b) / 2.0

        # Pass 2: grow the sphere to enclose every vertex (Ritter).
        for x, y, z, _ in iter_vertices(path):
            dx = x - cx
            dy = y - cy
            dz = z - cz
            d = math.sqrt(dx * dx + dy * dy + dz * dz)
            if d > radius and d > 0.0:
                # Move the center halfway toward the new point and enlarge.
                new_radius = (radius + d) / 2.0
                alpha = (d - radius) / (2.0 * d)
                cx += dx * alpha
                cy += dy * alpha
                cz += dz * alpha
                radius = new_radius
        sphere = Sphere(cx, cy, cz, radius)

    return Result(bounds=bounds, count=count, sphere=sphere)


def _dist(a: tuple[float, float, float], b: tuple[float, float, float]) -> float:
    """Euclidean distance between two 3-tuples."""
    return math.sqrt(
        (a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2
    )


def _farthest_pair(points: list[tuple[float, float, float]]):
    """Return the pair of points with the greatest mutual distance.

    Used to seed the Ritter sphere from the AABB axis-extreme points (at most
    six candidates), so this O(n^2) scan is tiny.
    """
    best_a = points[0]
    best_b = points[0]
    best = -1.0
    for i in range(len(points)):
        for j in range(i + 1, len(points)):
            d = _dist(points[i], points[j])
            if d > best:
                best = d
                best_a = points[i]
                best_b = points[j]
    return best_a, best_b


# ── TOML emission ────────────────────────────────────────────────────────────


def _render_table(header: str, pairs: list[tuple[str, str]], pretty: bool) -> list[str]:
    """Render a TOML table: a ``[header]`` line then ``key = value`` lines.

    When ``pretty`` is set, the ``=`` signs are aligned by padding keys to the
    longest key width within the table.
    """
    lines = ["[%s]" % header]
    if pretty and pairs:
        width = max(len(k) for k, _ in pairs)
        for key, value in pairs:
            lines.append("%-*s = %s" % (width, key, value))
    else:
        for key, value in pairs:
            lines.append("%s = %s" % (key, value))
    return lines


def _vec3(a: float, b: float, c: float) -> str:
    """Render a 3-float TOML inline array using ``fmt``."""
    return "[%s, %s, %s]" % (fmt(a), fmt(b), fmt(c))


def render_bounds_block(
    result: Result, label: str, pretty: bool = False, with_sphere: bool = False
) -> list[str]:
    """Render the bounds TOML block as a list of lines (no trailing newline).

    ``label`` is the human-readable source name used in the comment banner.
    The block always contains a ``[bounds]`` table; when ``with_sphere`` is set
    (and a sphere was computed) a ``[sphere]`` table follows.
    """
    b = result.bounds
    lines: list[str] = [
        "# object-space bounds for %s (%d vertices)" % (label, result.count),
    ]
    lines.extend(
        _render_table(
            "bounds",
            [
                ("min", _vec3(b.min_x, b.min_y, b.min_z)),
                ("max", _vec3(b.max_x, b.max_y, b.max_z)),
            ],
            pretty,
        )
    )
    if with_sphere and result.sphere is not None:
        s = result.sphere
        lines.append("")
        lines.extend(
            _render_table(
                "sphere",
                [
                    ("center", _vec3(s.cx, s.cy, s.cz)),
                    ("radius", fmt(s.radius)),
                ],
                pretty,
            )
        )
    return lines


# ── Scene .toml in-place writer ──────────────────────────────────────────────


def _split_le(line: str) -> tuple[str, str]:
    """Split a line into (content, line_ending). Preserves ``\\r\\n``."""
    if line.endswith("\r\n"):
        return line[:-2], "\r\n"
    if line.endswith("\n"):
        return line[:-1], "\n"
    if line.endswith("\r"):
        return line[:-1], "\r"
    return line, ""


# Matches an array-of-tables header: ``[[mesh]]`` (allowing surrounding space).
_RE_ARRAY_HEADER = re.compile(r"^\s*\[\[([A-Za-z0-9_]+)\]\]\s*$")
# Matches any table header: ``[a.b.c]`` (used to find the next section).
_RE_TABLE_HEADER = re.compile(r"^\s*\[([A-Za-z0-9_]+(?:\.[A-Za-z0-9_]+)*)\]\s*$")
# Matches a ``path = "..."`` (or single-quoted) key inside a mesh entry.
_RE_PATH_VALUE = re.compile(r"""^\s*path\s*=\s*["']([^"']+)['"]\s*$""")


def _find_mesh_entry(
    lines: list[str], obj_basename: str, obj_declared: str
) -> tuple[int, int, str] | None:
    """Locate the array-of-tables entry whose ``path`` references the OBJ.

    Returns ``(start_idx, end_idx, array_key)`` where ``[start, end)`` covers
    the whole entry (header + key/values + any ``[array_key.*]`` sub-tables),
    or ``None`` if no entry references the OBJ. Matching is by basename
    (``shader_ball.obj``) or by the exact declared path string.
    """
    n = len(lines)
    i = 0
    while i < n:
        core, _ = _split_le(lines[i])
        m = _RE_ARRAY_HEADER.match(core)
        if not m:
            i += 1
            continue
        array_key = m.group(1)
        end = _entry_end(lines, i + 1, array_key, n)
        for k in range(i + 1, end):
            core_k, _ = _split_le(lines[k])
            pm = _RE_PATH_VALUE.match(core_k)
            if not pm:
                continue
            declared = pm.group(1)
            declared_base = declared.replace("\\", "/").split("/")[-1]
            if declared_base == obj_basename or declared == obj_declared:
                return i, end, array_key
        i = end
    return None


def _entry_end(lines: list[str], j: int, array_key: str, n: int) -> int:
    """Index of the first line that starts a new top-level entry.

    Sub-tables of the current entry (``[array_key.foo]``) are part of it and
    are skipped; any other ``[...]`` or ``[[...]]`` header ends the entry.
    """
    while j < n:
        core, _ = _split_le(lines[j])
        if _RE_ARRAY_HEADER.match(core):
            return j
        m = _RE_TABLE_HEADER.match(core)
        if m and m.group(1).split(".")[0] != array_key:
            return j
        j += 1
    return n


def _strip_subtable(lines: list[str], header: str) -> list[str]:
    """Remove a ``[header]`` sub-table and its key/value lines from ``lines``."""
    out: list[str] = []
    i = 0
    n = len(lines)
    while i < n:
        core, _ = _split_le(lines[i])
        if core.strip() == header:
            i += 1
            while i < n:
                core2, _ = _split_le(lines[i])
                if _RE_TABLE_HEADER.match(core2):
                    break
                i += 1
        else:
            out.append(lines[i])
            i += 1
    return out


def _atomic_write_text(path: Path, text: str) -> None:
    """Write ``text`` to ``path`` via a temp file + ``os.replace`` (atomic)."""
    directory = path.parent if str(path.parent) else Path(".")
    fd, tmp = tempfile.mkstemp(suffix=".tmp", dir=str(directory))
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="") as f:
            f.write(text)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.remove(tmp)
        except OSError:
            pass
        raise


def _last_kv_index(lines: list[str]) -> int:
    """Index of the last ``key = value`` line in ``lines`` (-1 if none).

    A key/value line contains ``=`` and is not a header, comment, or blank.
    Used to anchor sub-table insertion to the mesh entry's body rather than to
    trailing comment banners that introduce the following section.
    """
    last = -1
    for idx, line in enumerate(lines):
        core, _ = _split_le(line)
        stripped = core.strip()
        if not stripped or stripped.startswith("#") or stripped.startswith("["):
            continue
        if "=" in core:
            last = idx
    return last


def inject_into_scene(
    scene_path: Path,
    obj_path: Path,
    result: Result,
    with_sphere: bool,
    pretty: bool,
) -> str:
    """Inject ``[<array_key>.bounds]`` into the scene file's matching mesh entry.

    Finds the ``[[mesh]]`` (or other array-of-tables) entry whose ``path``
    references ``obj_path``, removes any existing ``[<array_key>.bounds]``
    sub-table, and appends a fresh one (plus ``[<array_key>.sphere]`` when
    ``with_sphere`` is set). The file is rewritten atomically.

    Returns the array key used (e.g. ``"mesh"``). Raises ``ValueError`` if no
    matching entry is found. The dotted ``[mesh.bounds]`` form is required:
    a bare ``[bounds]`` would parse as an unrelated top-level table (verified
    against ``tomllib``), not a sub-table of the array element.
    """
    text = scene_path.read_text(encoding="utf-8")
    lines = text.splitlines(keepends=True)
    obj_basename = obj_path.name
    obj_declared = obj_path.name

    found = _find_mesh_entry(lines, obj_basename, obj_declared)
    if found is None:
        raise ValueError(
            "no [[...]] entry with path = \"%s\" found in %s"
            % (obj_basename, scene_path)
        )
    start, end, array_key = found

    body = lines[start:end]
    body = _strip_subtable(body, "[%s.bounds]" % array_key)
    if with_sphere:
        body = _strip_subtable(body, "[%s.sphere]" % array_key)

    # Match the line ending already used in the file (LF or CRLF).
    _, le = _split_le(body[0]) if body else ("\n", "\n")
    if not le:
        le = "\n"

    # Insert immediately after the entry's last ``key = value`` line, so the
    # sub-table sits with the mesh entry rather than after trailing comment
    # banners that stylistically belong to the *next* section.
    insert_at = _last_kv_index(body) + 1

    bounds_pairs = [
        ("min", _vec3(result.bounds.min_x, result.bounds.min_y, result.bounds.min_z)),
        ("max", _vec3(result.bounds.max_x, result.bounds.max_y, result.bounds.max_z)),
    ]
    block: list[str] = []
    # Separate the new sub-table from the preceding key/value line.
    if insert_at > 0 and _split_le(body[insert_at - 1])[0].strip() != "":
        block.append(le)
    block.extend(
        line + le for line in _render_table("%s.bounds" % array_key, bounds_pairs, pretty)
    )
    if with_sphere and result.sphere is not None:
        s = result.sphere
        sphere_pairs = [
            ("center", _vec3(s.cx, s.cy, s.cz)),
            ("radius", fmt(s.radius)),
        ]
        block.append(le)
        block.extend(
            line + le
            for line in _render_table("%s.sphere" % array_key, sphere_pairs, pretty)
        )

    new_body = body[:insert_at] + block + body[insert_at:]
    new_lines = lines[:start] + new_body + lines[end:]
    _atomic_write_text(scene_path, "".join(new_lines))
    return array_key


# ── CLI ──────────────────────────────────────────────────────────────────────


def _build_parser() -> argparse.ArgumentParser:
    """Build the argparse command-line interface."""
    parser = argparse.ArgumentParser(
        description=(
            "Compute the object-space AABB of a Wavefront OBJ and emit an "
            "Aether TOML [bounds] block."
        )
    )
    parser.add_argument("input", help="Input .obj file")
    parser.add_argument(
        "--write",
        metavar="PATH",
        default=None,
        help=(
            "Inject the bounds into the [[mesh]] entry of this scene .toml "
            "file (whose path matches the input OBJ) instead of printing to "
            "stdout"
        ),
    )
    parser.add_argument(
        "--sphere",
        action="store_true",
        help="Also compute and emit a Ritter bounding sphere",
    )
    parser.add_argument(
        "--pretty",
        action="store_true",
        help="Align '=' signs in the emitted TOML for readability",
    )
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="Suppress the human-readable summary",
    )
    return parser


def _summary(result: Result, label: str) -> str:
    """Render the one-line human-readable summary."""
    b = result.bounds
    return (
        "%s: %d vertices, AABB min=(%s, %s, %s) max=(%s, %s, %s)"
        % (
            label,
            result.count,
            fmt(b.min_x),
            fmt(b.min_y),
            fmt(b.min_z),
            fmt(b.max_x),
            fmt(b.max_y),
            fmt(b.max_z),
        )
    )


def main(argv: list[str] | None = None) -> int:
    """CLI entry point. Returns a process exit code."""
    args = _build_parser().parse_args(argv)
    input_path = Path(args.input)
    if not input_path.exists():
        sys.stderr.write("error: input file not found: %s\n" % input_path)
        return 1

    try:
        result = compute(input_path, want_sphere=args.sphere)
    except ValueError as exc:
        sys.stderr.write("error: %s\n" % exc)
        return 1

    if not args.quiet:
        sys.stderr.write(_summary(result, input_path.name) + "\n")

    if args.write:
        scene_path = Path(args.write)
        if not scene_path.exists():
            sys.stderr.write("error: scene file not found: %s\n" % scene_path)
            return 1
        try:
            array_key = inject_into_scene(
                scene_path, input_path, result, args.sphere, args.pretty
            )
        except ValueError as exc:
            sys.stderr.write("error: %s\n" % exc)
            return 1
        if not args.quiet:
            sys.stderr.write(
                "wrote [%s.bounds] into %s\n" % (array_key, scene_path)
            )
        return 0

    block = render_bounds_block(
        result, input_path.name, pretty=args.pretty, with_sphere=args.sphere
    )
    sys.stdout.write("\n".join(block).rstrip() + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
