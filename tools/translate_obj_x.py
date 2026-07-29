#!/usr/bin/env python3
"""Translate a Wavefront OBJ file along the X axis by a numerical offset.

Adds the offset to the X coordinate of vertex positions (v) only.
Normals (vn), texture coordinates (vt), faces (f) and all other lines are
copied unchanged. Preserves file encoding, line endings, and numeric precision.

Usage:
    python translate_obj_x.py <input.obj> <offset> <output.obj>
"""
import sys

from _obj_transform import fmt, main_for_transform, process, transform_v_line


def run(offset, input_path, output_path):
    modify = lambda x, y, z: (fmt(float(x) + offset), y, z)
    process(input_path, output_path, lambda line: transform_v_line(line, modify))


def main(argv):
    return main_for_transform(
        argv,
        script_name="translate_obj_x.py",
        usage_arg="offset",
        parse_value=float,
        value_error_msg="offset must be a number (e.g. -0.021, 2.0, -5.5)",
        run=run,
    )


if __name__ == "__main__":
    sys.exit(main(sys.argv))
