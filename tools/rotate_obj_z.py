#!/usr/bin/env python3
"""Rotate a Wavefront OBJ file around the Z axis by 90, 180, or 270 degrees.

Uses exact coordinate swizzling (no trig). Because these angles only swap
axes and flip signs, we operate directly on the ORIGINAL string tokens
(negating by toggling a leading '-') so numeric precision/formatting of each
value is preserved byte-for-byte.

Right-handed Z-axis rotation swizzle table (applied to the token slots):
    90:  (x, y, z) -> (-y,  x, z)
    180: (x, y, z) -> (-x, -y, z)
    270: (x, y, z) -> ( y, -x, z)

Usage:
    python rotate_obj_z.py <input.obj> <90|180|270> <output.obj>
"""
import sys

from _obj_transform import (
    main_for_transform,
    negate_token,
    process,
    transform_xyz_line,
)


def make_swizzle(angle):
    """Return a function mapping (x_tok, y_tok, z_tok) -> (a, b, c) tokens."""
    if angle == 90:
        return lambda x, y, z: (negate_token(y), x, z)
    if angle == 180:
        return lambda x, y, z: (negate_token(x), negate_token(y), z)
    if angle == 270:
        return lambda x, y, z: (y, negate_token(x), z)
    raise ValueError("angle must be 90, 180, or 270")


def run(angle, input_path, output_path):
    swizzle = make_swizzle(angle)
    fn = lambda line: transform_xyz_line(line, swizzle)
    process(input_path, output_path, fn, fn)


def main(argv):
    return main_for_transform(
        argv,
        script_name="rotate_obj_z.py",
        usage_arg="90|180|270",
        parse_value=int,
        value_error_msg="angle must be an integer: 90, 180, or 270",
        run=run,
    )


if __name__ == "__main__":
    sys.exit(main(sys.argv))
