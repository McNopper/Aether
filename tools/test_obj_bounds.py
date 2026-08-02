"""Self-contained golden-file test runner for ``obj_bounds.py``.

No third-party test framework required (standard library only):

    python tools/test_obj_bounds.py

Synthetic OBJ fixtures with known bounds are written into a temporary
directory; the AABB computation, TOML emission, exclusion of ``vn``/``vt``/``f``
lines, empty-OBJ rejection, Ritter sphere enclosure, and the scene ``--write``
injection are all exercised. Exit code 0 = all pass.
"""

from __future__ import annotations

import io
import math
import os
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import obj_bounds  # noqa: E402

try:
    import tomllib  # Python 3.11+
except ModuleNotFoundError:  # pragma: no cover - fallback for very old Python
    tomllib = None


# Fixture: 4 positions whose AABB is unambiguous. Includes vn/vt/vp/f lines
# that MUST be ignored (a stray 'vn' or 'vt' value must not leak into the AABB).
FIXTURE_OBJ = """\
# test cube-like OBJ
v -1.0 -2.0 -3.0
v  1.0  2.0  3.0
v  0.0  0.0  0.0
v -0.5  1.5 -2.5
vn 100.0 100.0 100.0
vt 100.0 100.0
vp 50.0 50.0 50.0
f 1 2 3
f 1 3 4
"""

EXPECTED_MIN = (-1.0, -2.0, -3.0)
EXPECTED_MAX = (1.0, 2.0, 3.0)
EXPECTED_COUNT = 4

# Points whose distance from the AABB center tests the Ritter enclosure.
FIXTURE_POINTS = [
    (-1.0, -2.0, -3.0),
    (1.0, 2.0, 3.0),
    (0.0, 0.0, 0.0),
    (-0.5, 1.5, -2.5),
]

EMPTY_OBJ = """\
# no vertices here
# just comments and a face reference to nothing
vn 0.0 1.0 0.0
"""


def _approx_equal(a: float, b: float, tol: float = 1e-9) -> bool:
    return abs(a - b) <= tol * max(1.0, abs(a), abs(b))


def _write(tmp: Path, name: str, text: str) -> Path:
    p = tmp / name
    p.write_text(text, encoding="utf-8")
    return p


def check_basic_aabb(tmp: Path) -> list[str]:
    """(a) AABB computation matches the expected min/max and count."""
    errors: list[str] = []
    obj = _write(tmp, "basic.obj", FIXTURE_OBJ)
    result = obj_bounds.compute(obj)
    if result.count != EXPECTED_COUNT:
        errors.append(
            "count: expected %d, got %d" % (EXPECTED_COUNT, result.count)
        )
    got_min = (result.bounds.min_x, result.bounds.min_y, result.bounds.min_z)
    got_max = (result.bounds.max_x, result.bounds.max_y, result.bounds.max_z)
    if not all(_approx_equal(a, b) for a, b in zip(got_min, EXPECTED_MIN)):
        errors.append("min: expected %s, got %s" % (EXPECTED_MIN, got_min))
    if not all(_approx_equal(a, b) for a, b in zip(got_max, EXPECTED_MAX)):
        errors.append("max: expected %s, got %s" % (EXPECTED_MAX, got_max))
    return errors


def check_toml_block(tmp: Path) -> list[str]:
    """(b) Emitted TOML block parses and contains the right numbers."""
    errors: list[str] = []
    if tomllib is None:
        return ["tomllib unavailable on this Python; cannot verify TOML parsing"]
    obj = _write(tmp, "toml.obj", FIXTURE_OBJ)
    out = io.StringIO()
    err = io.StringIO()
    old_out, old_err = sys.stdout, sys.stderr
    try:
        sys.stdout, sys.stderr = out, err
        rc = obj_bounds.main([str(obj)])
    finally:
        sys.stdout, sys.stderr = old_out, old_err
    if rc != 0:
        errors.append("exit code: expected 0, got %d (stderr=%r)" % (rc, err.getvalue()))
        return errors
    # Stdout must be parseable TOML with a top-level [bounds] table.
    try:
        doc = tomllib.loads(out.getvalue())
    except Exception as exc:  # noqa: BLE001 - surface any parse failure
        errors.append("stdout did not parse as TOML: %s" % exc)
        return errors
    bounds = doc.get("bounds")
    if not isinstance(bounds, dict):
        errors.append("no [bounds] table in output")
        return errors
    got_min = list(bounds.get("min", []))
    got_max = list(bounds.get("max", []))
    if not all(_approx_equal(a, b) for a, b in zip(got_min, EXPECTED_MIN)):
        errors.append("bounds.min: expected %s, got %s" % (EXPECTED_MIN, got_min))
    if not all(_approx_equal(a, b) for a, b in zip(got_max, EXPECTED_MAX)):
        errors.append("bounds.max: expected %s, got %s" % (EXPECTED_MAX, got_max))
    return errors


def check_ignores_non_v(tmp: Path) -> list[str]:
    """(c) vn/vt/vp/f lines never affect the AABB.

    The fixture's stray ``vn 100 100 100`` / ``vt 100 100`` / ``vp 50 50 50``
    would explode the bounds to +/-100 if mistakenly treated as positions, so
    asserting the exact expected min/max here is a strict regression guard.
    """
    errors: list[str] = []
    obj = _write(tmp, "ignore.obj", FIXTURE_OBJ)
    result = obj_bounds.compute(obj)
    if result.count != EXPECTED_COUNT:
        errors.append("count: vn/vt/vp leaked in (expected %d, got %d)" % (EXPECTED_COUNT, result.count))
    got_min = (result.bounds.min_x, result.bounds.min_y, result.bounds.min_z)
    got_max = (result.bounds.max_x, result.bounds.max_y, result.bounds.max_z)
    if not all(_approx_equal(a, b) for a, b in zip(got_min, EXPECTED_MIN)):
        errors.append("min leaked: expected %s, got %s" % (EXPECTED_MIN, got_min))
    if not all(_approx_equal(a, b) for a, b in zip(got_max, EXPECTED_MAX)):
        errors.append("max leaked: expected %s, got %s" % (EXPECTED_MAX, got_max))
    if result.bounds.max_x >= 50.0 or result.bounds.min_x <= -50.0:
        errors.append("vn/vt/vp clearly contaminated bounds: %s" % result.bounds)
    return errors


def check_empty_obj(tmp: Path) -> list[str]:
    """(d) An OBJ with no 'v' lines is rejected with exit code 1."""
    errors: list[str] = []
    obj = _write(tmp, "empty.obj", EMPTY_OBJ)
    try:
        obj_bounds.compute(obj)
        errors.append("compute() should have raised ValueError for an empty OBJ")
    except ValueError:
        pass
    out = io.StringIO()
    err = io.StringIO()
    old_out, old_err = sys.stdout, sys.stderr
    try:
        sys.stdout, sys.stderr = out, err
        rc = obj_bounds.main([str(obj)])
    finally:
        sys.stdout, sys.stderr = old_out, old_err
    if rc != 1:
        errors.append("exit code: expected 1 for empty OBJ, got %d" % rc)
    return errors


def check_ritter_encloses(tmp: Path) -> list[str]:
    """(e) --sphere yields a sphere that encloses every vertex."""
    errors: list[str] = []
    obj = _write(tmp, "sphere.obj", FIXTURE_OBJ)
    result = obj_bounds.compute(obj, want_sphere=True)
    if result.sphere is None:
        errors.append("--sphere produced no sphere")
        return errors
    s = result.sphere
    cx, cy, cz = s.center()
    for px, py, pz in FIXTURE_POINTS:
        d = math.sqrt((px - cx) ** 2 + (py - cy) ** 2 + (pz - cz) ** 2)
        if d > s.radius + 1e-9:
            errors.append(
                "point %s outside sphere (d=%s > radius=%s)"
                % ((px, py, pz), d, s.radius)
            )
    # The circumsphere radius is an upper bound: Ritter must be no larger.
    circ = result.bounds.sphere_radius()
    if s.radius > circ + 1e-9:
        errors.append(
            "Ritter radius %s exceeds circumsphere %s" % (s.radius, circ)
        )
    return errors


def check_scene_write(tmp: Path) -> list[str]:
    """Bonus: --write injects a parseable [mesh.bounds] sub-table, and replaces."""
    errors: list[str] = []
    if tomllib is None:
        return ["tomllib unavailable on this Python; cannot verify --write"]
    obj = _write(tmp, "thing.obj", FIXTURE_OBJ)
    scene_text = (
        'material_libraries = ["thing.materials.toml"]\n\n'
        "[[mesh]]\n"
        'name = "other"\n'
        'path = "other.obj"\n\n'
        "[[mesh]]\n"
        'name = "thing"\n'
        'path = "thing.obj"\n\n'
        "[[instance]]\n"
        'mesh = "thing"\n'
    )
    scene = _write(tmp, "thing.scene.toml", scene_text)

    old_out, old_err = sys.stdout, sys.stderr
    try:
        sys.stdout = sys.stderr = io.StringIO()
        rc = obj_bounds.main([str(obj), "--write", str(scene)])
    finally:
        sys.stdout, sys.stderr = old_out, old_err
    if rc != 0:
        return ["--write exit code: expected 0, got %d" % rc]

    doc = tomllib.loads(scene.read_text(encoding="utf-8"))
    meshes = doc.get("mesh", [])
    thing = next((m for m in meshes if m.get("name") == "thing"), None)
    if thing is None:
        return ["--write: 'thing' mesh entry not found after injection"]
    bounds = thing.get("bounds")
    if not isinstance(bounds, dict):
        return ["--write: no [mesh.bounds] sub-table on the 'thing' entry"]
    got_min = list(bounds.get("min", []))
    if not all(_approx_equal(a, b) for a, b in zip(got_min, EXPECTED_MIN)):
        errors.append("--write bounds.min: expected %s, got %s" % (EXPECTED_MIN, got_min))
    # 'other' mesh must be untouched (no bounds key).
    other = next((m for m in meshes if m.get("name") == "other"), None)
    if other is not None and "bounds" in other:
        errors.append("--write accidentally added bounds to the 'other' entry")

    # Second pass must REPLACE, not duplicate.
    old_out, old_err = sys.stdout, sys.stderr
    try:
        sys.stdout = sys.stderr = io.StringIO()
        rc = obj_bounds.main([str(obj), "--write", str(scene)])
    finally:
        sys.stdout, sys.stderr = old_out, old_err
    if rc != 0:
        errors.append("--write (replace) exit code: expected 0, got %d" % rc)
        return errors
    text2 = scene.read_text(encoding="utf-8")
    if text2.count("[mesh.bounds]") != 1:
        errors.append(
            "--write did not replace: found %d [mesh.bounds] blocks"
            % text2.count("[mesh.bounds]")
        )
    return errors


CHECKS = [
    ("basic AABB", check_basic_aabb),
    ("TOML block parses", check_toml_block),
    ("ignores vn/vt/vp/f", check_ignores_non_v),
    ("empty OBJ rejected", check_empty_obj),
    ("Ritter sphere encloses", check_ritter_encloses),
    ("scene --write injection", check_scene_write),
]


def main() -> int:
    """Run every check in a throwaway temp dir; 0 = all pass."""
    failures: list[str] = []
    passed = 0
    with tempfile.TemporaryDirectory(prefix="obj_bounds_test_") as td:
        tmp = Path(td)
        for name, fn in CHECKS:
            errs = fn(tmp)
            if errs:
                for e in errs:
                    failures.append("%s: %s" % (name, e))
            else:
                passed += 1
                print("PASS %s" % name)
    for err in failures:
        print("FAIL %s" % err)
    print("\n%d passed, %d failed" % (passed, len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
