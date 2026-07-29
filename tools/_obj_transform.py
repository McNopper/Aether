"""Shared helpers for the OBJ-transform wrapper scripts (rotate/translate/scale).

Each wrapper supplies only its unique transform function (a per-axis swizzle
table, a per-axis offset, or a uniform scale factor) and delegates file I/O,
atomic in-place replacement, line framing, and CLI parsing to this module.
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


def fmt(value):
    """Format a float compactly, avoiding binary FP noise.

    Using '%.10g' yields 10 significant digits (well beyond typical OBJ
    source precision of 6-7 digits) and suppresses artifacts like
    0.32705510000000004 that raw repr() would expose. Trailing zeros and
    exponent noise are handled by %g. Avoids '-0' for signed zero.
    """
    if value == 0:
        value = 0.0
    return "%.10g" % value


def transform_xyz_line(line_bytes, swizzle):
    """Swizzle the first 3 numeric tokens of a v/vn line, preserving the rest.

    Preserves the leading keyword, any extra trailing columns (e.g. vertex
    colors), and the exact trailing line-ending bytes.
    """
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
        return line_bytes

    nx, ny, nz = swizzle(coords[0], coords[1], coords[2])

    new_parts = [keyword, nx, ny, nz]
    new_parts.extend(coords[3:])
    out = " ".join(new_parts)
    return out.encode("utf-8") + eol


def transform_v_line(line_bytes, modify):
    """Apply `modify` to the first 3 numeric tokens of a v line.

    `modify(x, y, z)` receives the 3 string tokens and returns 3 new string
    tokens. Preserves the leading keyword, any extra trailing columns (e.g.
    vertex colors), and the exact trailing line-ending bytes.
    """
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
        return line_bytes

    nx, ny, nz = modify(coords[0], coords[1], coords[2])

    new_parts = [keyword, nx, ny, nz]
    new_parts.extend(coords[3:])
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


def process(input_path, output_path, transform_v, transform_vn=None):
    """Stream-transform an OBJ file.

    `transform_v` is applied to every vertex-position ('v') line; `transform_vn`,
    if not None, is applied to every vertex-normal ('vn') line. All other lines
    (vt, f, comments, ...) are copied unchanged.
    """
    write_path, finalize = resolve_output(input_path, output_path)
    try:
        with open(input_path, "rb") as fin, open(write_path, "wb") as fout:
            for line in fin:
                head = line.lstrip()
                if head.startswith(b"vn ") or head.startswith(b"vn\t"):
                    fout.write(transform_vn(line) if transform_vn else line)
                elif head.startswith(b"v ") or head.startswith(b"v\t"):
                    fout.write(transform_v(line))
                else:
                    fout.write(line)
    except BaseException:
        if write_path != output_path and os.path.exists(write_path):
            os.remove(write_path)
        raise
    finalize()


def main_for_transform(argv, script_name, usage_arg, parse_value, value_error_msg, run):
    """Shared CLI: `python <script> <input.obj> <<usage_arg>> <output.obj>`.

    `parse_value(str) -> value` parses argv[2] (raises ValueError on bad input).
    `value_error_msg` is printed on parse failure. `run(value, input_path,
    output_path)` performs the transform.
    """
    if len(argv) != 4:
        sys.stderr.write(
            "Usage: python %s <input.obj> <%s> <output.obj>\n" % (script_name, usage_arg)
        )
        return 2
    input_path = argv[1]
    try:
        value = parse_value(argv[2])
    except ValueError:
        sys.stderr.write(value_error_msg + "\n")
        return 2
    output_path = argv[3]
    try:
        run(value, input_path, output_path)
    except ValueError as e:
        sys.stderr.write("Error: %s\n" % e)
        return 2
    return 0
