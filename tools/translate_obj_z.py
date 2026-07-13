#!/usr/bin/env python3
"""Translate a Wavefront OBJ file along the Z axis by a numerical offset.

Adds the offset to the Z coordinate of vertex positions (v) only.
Normals (vn), texture coordinates (vt), faces (f) and all other lines are
copied unchanged. Preserves file encoding, line endings, and numeric precision.

Usage:
    python translate_obj_z.py <input.obj> <offset> <output.obj>
"""
import sys


def fmt(value):
    """Format a translated float compactly, avoiding binary FP noise.

    Using '%.10g' yields 10 significant digits (well beyond typical OBJ
    source precision of 6-7 digits) and suppresses artifacts like
    0.32705510000000004 that raw repr() would expose. Trailing zeros and
    exponent noise are handled by %g. Avoids '-0' for signed zero.
    """
    if value == 0:
        value = 0.0
    return "%.10g" % value


def transform_v_line(line_bytes, offset):
    """Translate the Z coordinate of a v line, preserving the rest.

    Adds the offset to the Z (third numeric token) only.
    Preserves the leading keyword, X and Y coordinates, any extra trailing
    columns (e.g. vertex colors r g b), and the exact trailing line-ending bytes.
    """
    stripped = line_bytes
    eol = b""
    while stripped.endswith(b"\n") or stripped.endswith(b"\r"):
        eol = stripped[-1:] + eol
        stripped = stripped[:-1]

    text = stripped.decode("ascii")
    parts = text.split()
    keyword = parts[0]
    coords = parts[1:]

    if len(coords) < 3:
        # Malformed; pass through unchanged.
        return line_bytes

    x = coords[0]  # keep X unchanged (as string token)
    y = coords[1]  # keep Y unchanged (as string token)
    z = float(coords[2]) + offset  # translate Z only

    new_parts = [keyword, x, y, fmt(z)]
    new_parts.extend(coords[3:])  # preserve extra columns (e.g. colors) verbatim
    out = " ".join(new_parts)
    return out.encode("ascii") + eol


def process(input_path, offset, output_path):
    with open(input_path, "rb") as fin, open(output_path, "wb") as fout:
        for line in fin:
            head = line.lstrip()
            # Only 'v ' (positions). Deliberately NOT vn/vt (they start 'vn'/'vt').
            if head.startswith(b"v ") or head.startswith(b"v\t"):
                fout.write(transform_v_line(line, offset))
            else:
                fout.write(line)


def main(argv):
    if len(argv) != 4:
        sys.stderr.write(
            "Usage: python translate_obj_z.py <input.obj> <offset> <output.obj>\n"
        )
        return 2
    input_path = argv[1]
    try:
        offset = float(argv[2])
    except ValueError:
        sys.stderr.write("offset must be a number (e.g. -0.021, 2.0, -5.5)\n")
        return 2
    output_path = argv[3]
    try:
        process(input_path, offset, output_path)
    except Exception as e:
        sys.stderr.write("Error: %s\n" % e)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
