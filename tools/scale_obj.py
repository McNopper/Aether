#!/usr/bin/env python3
"""Scale a Wavefront OBJ file uniformly by a numerical factor.

Multiplies vertex positions (v) by the factor. Normals (vn), texture
coordinates (vt), faces (f) and all other lines are copied unchanged.

Usage:
    python scale_obj.py <input.obj> <factor> <output.obj>
"""
import sys


def fmt(value):
    """Format a scaled float compactly, avoiding binary FP noise.

    Using '%.10g' yields 10 significant digits (well beyond typical OBJ
    source precision of 6-7 digits) and suppresses artifacts like
    0.32705510000000004 that raw repr() would expose. Trailing zeros and
    exponent noise are handled by %g. Avoids '-0' for signed zero.
    """
    if value == 0:
        value = 0.0
    return "%.10g" % value


def transform_v_line(line_bytes, factor):
    """Scale the first 3 numeric tokens of a v line, preserving the rest.

    Preserves the leading keyword, any extra trailing columns (e.g. vertex
    colors r g b, which must NOT be scaled), and the exact trailing
    line-ending bytes.
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
        return line_bytes

    x = float(coords[0]) * factor
    y = float(coords[1]) * factor
    z = float(coords[2]) * factor

    new_parts = [keyword, fmt(x), fmt(y), fmt(z)]
    new_parts.extend(coords[3:])  # preserve extra columns (e.g. colors) verbatim
    out = " ".join(new_parts)
    return out.encode("ascii") + eol


def process(input_path, factor, output_path):
    with open(input_path, "rb") as fin, open(output_path, "wb") as fout:
        for line in fin:
            head = line.lstrip()
            # Only 'v ' (positions). Deliberately NOT vn/vt (they start 'vn'/'vt').
            if head.startswith(b"v ") or head.startswith(b"v\t"):
                fout.write(transform_v_line(line, factor))
            else:
                fout.write(line)


def main(argv):
    if len(argv) != 4:
        sys.stderr.write(
            "Usage: python scale_obj.py <input.obj> <factor> <output.obj>\n"
        )
        return 2
    input_path = argv[1]
    try:
        factor = float(argv[2])
    except ValueError:
        sys.stderr.write("factor must be a number (e.g. 0.1, 2.0, 10.0)\n")
        return 2
    output_path = argv[3]
    process(input_path, factor, output_path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
