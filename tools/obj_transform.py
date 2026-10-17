#!/usr/bin/env python3
"""One CLI for the per-axis Wavefront OBJ transforms (rotate/translate/scale).

The single entry point over the shared streaming module ``_obj_transform``:
exactly ONE input OBJ -> ONE output OBJ per run, ``v`` (and, for rotations,
``vn``) lines streamed and rewritten, everything else copied unchanged — the
file format is never altered.

Rotations use exact coordinate swizzling (no trig). Because the supported
angles only swap axes and flip signs, we operate directly on the ORIGINAL
string tokens (negating by toggling a leading '-') so numeric precision and
formatting of each value are preserved byte-for-byte; a 4x90° round-trip is
an exact identity.

Right-handed per-axis rotation swizzle tables (applied to the token slots):
    x  90:  (x, y, z) -> ( x, -z,  y)
    x 180:  (x, y, z) -> ( x, -y, -z)
    x 270:  (x, y, z) -> ( x,  z, -y)
    y  90:  (x, y, z) -> ( z,  y, -x)
    y 180:  (x, y, z) -> (-x,  y, -z)
    y 270:  (x, y, z) -> (-z,  y,  x)
    z  90:  (x, y, z) -> (-y,  x,  z)
    z 180:  (x, y, z) -> (-x, -y,  z)
    z 270:  (x, y, z) -> ( y, -x,  z)

Translate adds a numeric offset to one axis of every vertex position (v);
scale multiplies every vertex position by a uniform factor. Normals (vn),
texture coordinates (vt), faces (f) and all other lines are copied unchanged
by those two. When ``output`` resolves to the same file as ``input`` the
result is written through a temp file in the same directory and atomically
``os.replace``d, so in-place edits never truncate the source mid-read.

Usage:
    python tools/obj_transform.py rotate    <x|y|z> <90|180|270> <input.obj> <output.obj>
    python tools/obj_transform.py translate <x|y|z> <offset> <input.obj> <output.obj>
    python tools/obj_transform.py scale     <factor> <input.obj> <output.obj>
"""
from __future__ import annotations

import argparse
import sys

from _obj_transform import fmt, negate_token, process, transform_v_line, transform_xyz_line

# The rotation swizzles as (source-token index, flip-sign?) triples per
# (axis, angle), applied to the (x, y, z) token slots. ``+`` passes the
# original token through verbatim; ``-`` negates it via ``negate_token`` —
# matching the right-handed tables in the module docstring exactly.
_ROTATION_SWIZZLES: dict[str, dict[int, tuple[tuple[int, bool], ...]]] = {
    "x": {
        90: ((0, False), (2, True), (1, False)),
        180: ((0, False), (1, True), (2, True)),
        270: ((0, False), (2, False), (1, True)),
    },
    "y": {
        90: ((2, False), (1, False), (0, True)),
        180: ((0, True), (1, False), (2, True)),
        270: ((2, True), (1, False), (0, False)),
    },
    "z": {
        90: ((1, True), (0, False), (2, False)),
        180: ((0, True), (1, True), (2, False)),
        270: ((1, False), (0, True), (2, False)),
    },
}

# Exact validation messages of the original wrapper scripts (kept verbatim so
# error text and exit codes do not change with the CLI collapse).
_VALUE_TYPE_ERRORS = {
    "rotate": "angle must be an integer: 90, 180, or 270",
    "translate": "offset must be a number (e.g. -0.021, 2.0, -5.5)",
    "scale": "factor must be a number (e.g. 0.1, 2.0, 10.0)",
}


def make_swizzle(axis: str, angle: int):
    """Return a function mapping (x_tok, y_tok, z_tok) -> (a, b, c) tokens."""
    mapping = _ROTATION_SWIZZLES.get(axis, {}).get(angle)
    if mapping is None:
        raise ValueError("angle must be 90, 180, or 270")

    def swizzle(x: str, y: str, z: str) -> tuple[str, str, str]:
        tokens = (x, y, z)
        return tuple(negate_token(tokens[i]) if flip else tokens[i] for i, flip in mapping)

    return swizzle


def run_rotate(axis: str, angle: int, input_path: str, output_path: str) -> None:
    """Rotate every v AND vn line about one axis (positions and normals move)."""
    swizzle = make_swizzle(axis, angle)

    def fn(line: bytes) -> bytes:
        return transform_xyz_line(line, swizzle)

    process(input_path, output_path, fn, fn)


def run_translate(axis: str, offset: float, input_path: str, output_path: str) -> None:
    """Add `offset` to one axis of every vertex position (v only)."""
    slot = "xyz".index(axis)

    def modify(x: str, y: str, z: str) -> tuple[str, str, str]:
        tokens = [x, y, z]
        tokens[slot] = fmt(float(tokens[slot]) + offset)
        return (tokens[0], tokens[1], tokens[2])

    process(input_path, output_path, lambda line: transform_v_line(line, modify))


def run_scale(factor: float, input_path: str, output_path: str) -> None:
    """Multiply every vertex position by a uniform factor (v only)."""

    def modify(x: str, y: str, z: str) -> tuple[str, str, str]:
        return (fmt(float(x) * factor), fmt(float(y) * factor), fmt(float(z) * factor))

    process(input_path, output_path, lambda line: transform_v_line(line, modify))


def _build_parser() -> argparse.ArgumentParser:
    """Build the argparse command-line interface."""
    parser = argparse.ArgumentParser(
        description=(
            "Rotate/translate/scale a Wavefront OBJ: one input OBJ -> one "
            "output OBJ, vertex lines streamed and rewritten."
        )
    )
    sub = parser.add_subparsers(dest="command", required=True, metavar="command")

    rotate = sub.add_parser(
        "rotate", help="rotate by 90/180/270 degrees about one axis (v + vn lines)"
    )
    rotate.add_argument("axis", choices=("x", "y", "z"), help="rotation axis")
    rotate.add_argument("angle", help="rotation angle in degrees: 90, 180 or 270")
    rotate.add_argument("input", help="input .obj file")
    rotate.add_argument("output", help="output .obj file (may equal the input for in-place)")

    translate = sub.add_parser(
        "translate", help="add a numeric offset to one axis of every vertex (v lines)"
    )
    translate.add_argument("axis", choices=("x", "y", "z"), help="axis to offset")
    translate.add_argument("offset", help="numeric offset (e.g. -0.021, 2.0, -5.5)")
    translate.add_argument("input", help="input .obj file")
    translate.add_argument("output", help="output .obj file (may equal the input for in-place)")

    scale = sub.add_parser(
        "scale", help="multiply every vertex by a uniform factor (v lines)"
    )
    scale.add_argument("factor", help="numeric factor (e.g. 0.1, 2.0, 10.0)")
    scale.add_argument("input", help="input .obj file")
    scale.add_argument("output", help="output .obj file (may equal the input for in-place)")

    return parser


def main(argv: list[str] | None = None) -> int:
    """CLI entry point. Returns a process exit code (0 ok, 2 on a bad value)."""
    args = _build_parser().parse_args(argv)

    # Parse the numeric argument with the exact semantics of the original
    # wrapper scripts: bare message + exit 2 on an unparseable value.
    try:
        if args.command == "rotate":
            value = int(args.angle)
        elif args.command == "translate":
            value = float(args.offset)
        else:
            value = float(args.factor)
    except ValueError:
        sys.stderr.write(_VALUE_TYPE_ERRORS[args.command] + "\n")
        return 2

    try:
        if args.command == "rotate":
            run_rotate(args.axis, value, args.input, args.output)
        elif args.command == "translate":
            run_translate(args.axis, value, args.input, args.output)
        else:
            run_scale(value, args.input, args.output)
    except ValueError as exc:
        sys.stderr.write(f"Error: {exc}\n")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
