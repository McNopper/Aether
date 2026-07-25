#!/usr/bin/env python3
"""Rotate a Wavefront OBJ file around the X axis by 90, 180, or 270 degrees.

Uses exact coordinate swizzling (no trig). Because these angles only swap
axes and flip signs, we operate directly on the ORIGINAL string tokens
(negating by toggling a leading '-') so numeric precision/formatting of each
value is preserved byte-for-byte.

Right-handed X-axis rotation swizzle table (applied to the token slots):
    90:  (x, y, z) -> (x, -z,  y)
    180: (x, y, z) -> (x, -y, -z)
    270: (x, y, z) -> (x,  z, -y)

Usage:
    python rotate_obj_x.py <input.obj> <90|180|270> <output.obj>
"""
import os
import sys
import tempfile


def negate_token(tok):
    """Return the numeric string token with its sign flipped.

    Preserves the exact digit text; only the leading sign changes.
    Avoids producing '-0' style artifacts by leaving a zero-valued token
    unsigned (a plain '0', '0.0', etc.).
    """
    # Determine if the value is zero to avoid signed-zero artifacts.
    try:
        is_zero = float(tok) == 0.0
    except ValueError:
        is_zero = False

    if tok.startswith("-"):
        return tok[1:]
    if tok.startswith("+"):
        body = tok[1:]
        return body if is_zero else "-" + body
    return tok if is_zero else "-" + tok


def make_swizzle(angle):
    """Return a function mapping (x_tok, y_tok, z_tok) -> (a, b, c) tokens."""
    if angle == 90:
        return lambda x, y, z: (x, negate_token(z), y)
    if angle == 180:
        return lambda x, y, z: (x, negate_token(y), negate_token(z))
    if angle == 270:
        return lambda x, y, z: (x, z, negate_token(y))
    raise ValueError("angle must be 90, 180, or 270")


def transform_xyz_line(line_bytes, swizzle):
    """Swizzle the first 3 numeric tokens of a v/vn line, preserving the rest.

    Preserves the leading keyword, any extra trailing columns (e.g. vertex
    colors), and the exact trailing line-ending bytes.
    """
    # Separate trailing EOL bytes so we can restore them verbatim.
    stripped = line_bytes
    eol = b""
    while stripped.endswith(b"\n") or stripped.endswith(b"\r"):
        eol = stripped[-1:] + eol
        stripped = stripped[:-1]

    text = stripped.decode("utf-8")
    parts = text.split()
    keyword = parts[0]
    coords = parts[1:]

    if len(coords) < 3:
        # Malformed; pass through unchanged.
        return line_bytes

    nx, ny, nz = swizzle(coords[0], coords[1], coords[2])

    new_parts = [keyword, nx, ny, nz]
    new_parts.extend(coords[3:])  # preserve extra columns verbatim (e.g. trailing "# ..." comments)
    out = " ".join(new_parts)
    return out.encode("utf-8") + eol


def resolve_output(input_path, output_path):
    """Return (write_path, finalize). In-place (output == input) writes to a temp
    file in the same directory and atomically replaces it on finalize(); otherwise
    writes the output directly. Prevents truncating the input mid-read during an
    in-place overwrite (output_path opened 'wb' would zero the file before the
    read loop finishes)."""
    try:
        same = os.path.samefile(input_path, output_path)
    except OSError:
        same = os.path.abspath(input_path) == os.path.abspath(output_path)
    if not same:
        return output_path, lambda: None
    directory = os.path.dirname(os.path.abspath(output_path)) or "."
    fd, tmp = tempfile.mkstemp(suffix=".tmp", dir=directory)
    os.close(fd)

    def finalize():
        os.replace(tmp, output_path)

    return tmp, finalize


def process(input_path, angle, output_path):
    swizzle = make_swizzle(angle)
    write_path, finalize = resolve_output(input_path, output_path)
    try:
        with open(input_path, "rb") as fin, open(write_path, "wb") as fout:
            for line in fin:
                head = line.lstrip()
                if head.startswith(b"vn ") or head.startswith(b"vn\t"):
                    fout.write(transform_xyz_line(line, swizzle))
                elif head.startswith(b"v ") or head.startswith(b"v\t"):
                    fout.write(transform_xyz_line(line, swizzle))
                else:
                    fout.write(line)
    except BaseException:
        if write_path != output_path and os.path.exists(write_path):
            os.remove(write_path)
        raise
    finalize()


def main(argv):
    if len(argv) != 4:
        sys.stderr.write(
            "Usage: python rotate_obj_x.py <input.obj> <90|180|270> <output.obj>\n"
        )
        return 2
    input_path = argv[1]
    try:
        angle = int(argv[2])
    except ValueError:
        sys.stderr.write("angle must be an integer: 90, 180, or 270\n")
        return 2
    output_path = argv[3]
    try:
        process(input_path, angle, output_path)
    except ValueError as e:
        sys.stderr.write("Error: %s\n" % e)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
