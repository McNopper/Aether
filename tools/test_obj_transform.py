"""Self-contained golden-file test runner for ``obj_transform.py``.

No third-party test framework required (standard library only):

    python tools/test_obj_transform.py

Exercises every subcommand of the unified OBJ-transform CLI: the exact
token swizzles of all nine rotate (axis x angle) cases on v AND vn lines,
translate/scale on v only, byte-exact pass-through of every other line kind
(incl. extra vertex-color columns and short v lines), CRLF preservation,
4x90° round-trip identity, safe in-place edits, and the exact validation
messages/exit codes inherited from the original wrapper scripts.
Exit code 0 = all pass.
"""

from __future__ import annotations

import io
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import obj_transform

# Fixture with distinct per-axis token texts so every swizzle slot is
# observable, plus vn/vt/f/comment lines that must survive verbatim.
FIXTURE_LINES = [
    "# test fixture",
    "v 1.5 -2.25 3.75",
    "vn 0.5 0.25 -0.75",
    "vt 0.1 0.9",
    "v 0.0 0.0 0.0",
    "f 1/1 2/1 3/1",
]
FIXTURE = "\n".join(FIXTURE_LINES) + "\n"

# Hand-derived golden outputs for all nine rotate (axis, angle) cases.
# v tokens are (1.5, -2.25, 3.75); vn tokens are (0.5, 0.25, -0.75).
ROTATE_GOLDEN = {
    ("x", 90): ("v 1.5 -3.75 -2.25", "vn 0.5 0.75 0.25"),
    ("x", 180): ("v 1.5 2.25 -3.75", "vn 0.5 -0.25 0.75"),
    ("x", 270): ("v 1.5 3.75 2.25", "vn 0.5 -0.75 -0.25"),
    ("y", 90): ("v 3.75 -2.25 -1.5", "vn -0.75 0.25 -0.5"),
    ("y", 180): ("v -1.5 -2.25 -3.75", "vn -0.5 0.25 0.75"),
    ("y", 270): ("v -3.75 -2.25 1.5", "vn 0.75 0.25 0.5"),
    ("z", 90): ("v 2.25 1.5 3.75", "vn -0.25 0.5 -0.75"),
    ("z", 180): ("v -1.5 2.25 3.75", "vn -0.5 -0.25 -0.75"),
    ("z", 270): ("v -2.25 -1.5 3.75", "vn 0.25 -0.5 -0.75"),
}


def _write(tmp: Path, name: str, text: str) -> Path:
    p = tmp / name
    p.write_bytes(text.encode("utf-8"))
    return p


def _run(argv: list[str]) -> tuple[int, str]:
    """Call obj_transform.main(argv) with captured stderr; return (rc, stderr)."""
    err = io.StringIO()
    old_err = sys.stderr
    try:
        sys.stderr = err
        rc = obj_transform.main(argv)
    finally:
        sys.stderr = old_err
    return rc, err.getvalue()


def _lines_of(path: Path) -> list[str]:
    return path.read_bytes().decode("utf-8").splitlines()


def check_rotate_golden(tmp: Path) -> list[str]:
    """(a) All nine rotate cases match the hand-derived swizzle tables on v and vn."""
    errors: list[str] = []
    for (axis, angle), (want_v, want_vn) in ROTATE_GOLDEN.items():
        src = _write(tmp, f"rot_{axis}_{angle}.obj", FIXTURE)
        dst = tmp / f"rot_{axis}_{angle}.out.obj"
        rc, err = _run(["rotate", axis, str(angle), str(src), str(dst)])
        if rc != 0:
            errors.append(f"rotate {axis} {angle}: exit {rc} (stderr={err!r})")
            continue
        lines = _lines_of(dst)
        want = [
            "# test fixture",
            want_v,
            want_vn,
            "vt 0.1 0.9",
            "v 0.0 0.0 0.0",
            "f 1/1 2/1 3/1",
        ]
        if lines != want:
            errors.append(f"rotate {axis} {angle}: got {lines!r}, want {want!r}")
    return errors


def check_translate_golden(tmp: Path) -> list[str]:
    """(b) translate moves exactly one v axis (fmt-rounded); vn/vt untouched."""
    errors: list[str] = []
    cases = [
        ("x", "-0.21", ["# test fixture", "v 1.29 -2.25 3.75", "vn 0.5 0.25 -0.75",
                        "vt 0.1 0.9", "v -0.21 0.0 0.0", "f 1/1 2/1 3/1"]),
        ("y", "2.0", ["# test fixture", "v 1.5 -0.25 3.75", "vn 0.5 0.25 -0.75",
                      "vt 0.1 0.9", "v 0.0 2 0.0", "f 1/1 2/1 3/1"]),
        ("z", "-5.5", ["# test fixture", "v 1.5 -2.25 -1.75", "vn 0.5 0.25 -0.75",
                       "vt 0.1 0.9", "v 0.0 0.0 -5.5", "f 1/1 2/1 3/1"]),
    ]
    for axis, offset, want in cases:
        src = _write(tmp, f"tra_{axis}.obj", FIXTURE)
        dst = tmp / f"tra_{axis}.out.obj"
        rc, err = _run(["translate", axis, offset, str(src), str(dst)])
        if rc != 0:
            errors.append(f"translate {axis} {offset}: exit {rc} (stderr={err!r})")
            continue
        lines = _lines_of(dst)
        if lines != want:
            errors.append(f"translate {axis} {offset}: got {lines!r}, want {want!r}")
    return errors


def check_scale_golden(tmp: Path) -> list[str]:
    """(c) scale multiplies v positions only (%.10g, no FP noise); vn untouched."""
    errors: list[str] = []
    cases = [
        ("0.1", "v 0.15 -0.225 0.375"),
        ("2.0", "v 3 -4.5 7.5"),
        ("10.0", "v 15 -22.5 37.5"),
    ]
    for factor, want_v in cases:
        src = _write(tmp, f"scl_{factor}.obj", FIXTURE)
        dst = tmp / f"scl_{factor}.out.obj"
        rc, err = _run(["scale", factor, str(src), str(dst)])
        if rc != 0:
            errors.append(f"scale {factor}: exit {rc} (stderr={err!r})")
            continue
        lines = _lines_of(dst)
        want = [
            "# test fixture",
            want_v,
            "vn 0.5 0.25 -0.75",
            "vt 0.1 0.9",
            "v 0 0 0",
            "f 1/1 2/1 3/1",
        ]
        if lines != want:
            errors.append(f"scale {factor}: got {lines!r}, want {want!r}")
    return errors


def check_rotate_roundtrip_identity(tmp: Path) -> list[str]:
    """(d) 4x90° and 2x180° are byte-exact identities (token-level negation)."""
    errors: list[str] = []
    for axis in ("x", "y", "z"):
        cur = _write(tmp, f"rt_{axis}.obj", FIXTURE)
        for _ in range(4):
            dst = tmp / f"rt_{axis}.out.obj"
            rc, err = _run(["rotate", axis, "90", str(cur), str(dst)])
            if rc != 0:
                errors.append(f"roundtrip rotate {axis} 90: exit {rc} (stderr={err!r})")
                return errors
            cur = dst
        if cur.read_bytes() != FIXTURE.encode("utf-8"):
            errors.append(f"4x90° about {axis} is not a byte-exact identity")
        cur = _write(tmp, f"rt180_{axis}.obj", FIXTURE)
        for _ in range(2):
            dst = tmp / f"rt180_{axis}.out.obj"
            rc, err = _run(["rotate", axis, "180", str(cur), str(dst)])
            if rc != 0:
                errors.append(f"roundtrip rotate {axis} 180: exit {rc} (stderr={err!r})")
                return errors
            cur = dst
        if cur.read_bytes() != FIXTURE.encode("utf-8"):
            errors.append(f"2x180° about {axis} is not a byte-exact identity")
    return errors


def check_crlf_preserved(tmp: Path) -> list[str]:
    """(e) CRLF line endings survive rotate/translate byte-for-byte."""
    errors: list[str] = []
    crlf = "\r\n".join(FIXTURE_LINES) + "\r\n"
    cases = [
        (["rotate", "y", "90"],
         ["# test fixture", "v 3.75 -2.25 -1.5", "vn -0.75 0.25 -0.5",
          "vt 0.1 0.9", "v 0.0 0.0 0.0", "f 1/1 2/1 3/1"]),
        (["translate", "x", "0.5"],
         ["# test fixture", "v 2 -2.25 3.75", "vn 0.5 0.25 -0.75",
          "vt 0.1 0.9", "v 0.5 0.0 0.0", "f 1/1 2/1 3/1"]),
    ]
    for argv_tail, want_lines in cases:
        src = _write(tmp, f"crlf_{argv_tail[0]}.obj", crlf)
        dst = tmp / f"crlf_{argv_tail[0]}.out.obj"
        rc, err = _run([*argv_tail, str(src), str(dst)])
        if rc != 0:
            errors.append(f"crlf {argv_tail[0]}: exit {rc} (stderr={err!r})")
            continue
        want = "\r\n".join(want_lines) + "\r\n"
        got = dst.read_bytes().decode("utf-8")
        if got != want:
            errors.append(f"crlf {argv_tail[0]}: got {got!r}, want {want!r}")
    return errors


def check_extra_columns_and_short_lines(tmp: Path) -> list[str]:
    """(f) Extra v columns (vertex colors) preserved; short v lines pass through."""
    errors: list[str] = []
    text = (
        "# extras\n"
        "v 1.5 -2.25 3.75 0.25 0.5 0.75\n"
        "v 1.5 -2.25\n"
        "f 1 2\n"
    )
    src = _write(tmp, "extras.obj", text)
    dst = tmp / "extras.out.obj"
    rc, err = _run(["rotate", "x", "90", str(src), str(dst)])
    if rc != 0:
        errors.append(f"extras rotate: exit {rc} (stderr={err!r})")
        return errors
    want = (
        "# extras\n"
        "v 1.5 -3.75 -2.25 0.25 0.5 0.75\n"
        "v 1.5 -2.25\n"
        "f 1 2\n"
    )
    got = dst.read_bytes().decode("utf-8")
    if got != want:
        errors.append(f"extras: got {got!r}, want {want!r}")
    return errors


def check_in_place_edit(tmp: Path) -> list[str]:
    """(g) output == input replaces the file atomically and leaves no temp files."""
    errors: list[str] = []
    src = _write(tmp, "inplace.obj", FIXTURE)
    rc, err = _run(["translate", "y", "-0.21", str(src), str(src)])
    if rc != 0:
        errors.append(f"in-place: exit {rc} (stderr={err!r})")
        return errors
    lines = _lines_of(src)
    if lines[1] != "v 1.5 -2.46 3.75":
        errors.append(f"in-place content: got {lines[1]!r}")
    leftovers = sorted(p.name for p in tmp.iterdir() if p.is_file() and p.suffix == ".tmp")
    if leftovers:
        errors.append(f"in-place left temp files: {leftovers}")
    return errors


def check_validation_messages(tmp: Path) -> list[str]:
    """(h) Bad values reproduce the original wrapper scripts' exact messages."""
    errors: list[str] = []
    src = _write(tmp, "val.obj", FIXTURE)
    dst = tmp / "val.out.obj"
    cases = [
        (["rotate", "x", "abc"], "angle must be an integer: 90, 180, or 270\n"),
        (["rotate", "x", "90.5"], "angle must be an integer: 90, 180, or 270\n"),
        (["rotate", "x", "45"], "Error: angle must be 90, 180, or 270\n"),
        (["translate", "y", "abc"], "offset must be a number (e.g. -0.021, 2.0, -5.5)\n"),
        (["scale", "abc"], "factor must be a number (e.g. 0.1, 2.0, 10.0)\n"),
    ]
    for argv, want_err in cases:
        rc, err = _run([*argv, str(src), str(dst)])
        if rc != 2:
            errors.append(f"{argv[0]} bad value: expected exit 2, got {rc}")
        if err != want_err:
            errors.append(f"{argv[0]} bad value: stderr {err!r}, want {want_err!r}")
    # argparse-level usage errors (missing args / bad axis) exit 2 via SystemExit.
    for argv in ([], ["rotate"], ["rotate", "w", "90", str(src), str(dst)], ["scale"]):
        old_err = sys.stderr
        try:
            sys.stderr = io.StringIO()
            obj_transform.main(argv)
            errors.append(f"usage {argv!r}: expected SystemExit(2)")
        except SystemExit as exc:
            if exc.code != 2:
                errors.append(f"usage {argv!r}: expected exit code 2, got {exc.code}")
        finally:
            sys.stderr = old_err
    return errors


CHECKS = [
    ("rotate golden swizzles (v+vn)", check_rotate_golden),
    ("translate golden offsets", check_translate_golden),
    ("scale golden factors", check_scale_golden),
    ("rotate round-trip identity", check_rotate_roundtrip_identity),
    ("CRLF preserved", check_crlf_preserved),
    ("extra columns / short lines", check_extra_columns_and_short_lines),
    ("in-place edit", check_in_place_edit),
    ("validation messages", check_validation_messages),
]


def main() -> int:
    """Run every check in a throwaway temp dir; 0 = all pass."""
    failures: list[str] = []
    passed = 0
    with tempfile.TemporaryDirectory(prefix="obj_transform_test_") as td:
        tmp = Path(td)
        for name, fn in CHECKS:
            errs = fn(tmp)
            if errs:
                for e in errs:
                    failures.append(f"{name}: {e}")
            else:
                passed += 1
                print(f"PASS {name}")
    for err in failures:
        print(f"FAIL {err}")
    print("\n%d passed, %d failed" % (passed, len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
