#!/usr/bin/env python3
"""Scale a Wavefront OBJ file uniformly by a numerical factor.

Multiplies vertex positions (v) by the factor. Normals (vn), texture
coordinates (vt), faces (f) and all other lines are copied unchanged.

Usage:
    python scale_obj.py <input.obj> <factor> <output.obj>
"""
import sys

from _obj_transform import fmt, main_for_transform, process, transform_v_line


def run(factor, input_path, output_path):
    modify = lambda x, y, z: (fmt(float(x) * factor), fmt(float(y) * factor), fmt(float(z) * factor))
    process(input_path, output_path, lambda line: transform_v_line(line, modify))


def main(argv):
    return main_for_transform(
        argv,
        script_name="scale_obj.py",
        usage_arg="factor",
        parse_value=float,
        value_error_msg="factor must be a number (e.g. 0.1, 2.0, 10.0)",
        run=run,
    )


if __name__ == "__main__":
    sys.exit(main(sys.argv))
