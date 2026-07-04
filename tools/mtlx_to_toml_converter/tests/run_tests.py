"""Self-contained test runner for the MaterialX -> Aether TOML converter.

No third-party test framework required:

    python tests/run_tests.py

For every OpenPBR fixture in tests/fixtures it regenerates the .materials.toml
and compares it byte-for-byte against the committed golden file in
tests/expected. The non-OpenPBR fixture must be rejected. Exit code 0 = pass.
"""

from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT))

from mtlx_to_toml import (  # noqa: E402
    OpenPBRValidationError,
    convert_document,
    _write_material_library,
)

FIXTURES = HERE / "fixtures"
EXPECTED = HERE / "expected"


def _check_openpbr_fixture(mtlx: Path) -> list[str]:
    errors: list[str] = []
    golden = EXPECTED / f"{mtlx.stem}.materials.toml"
    if not golden.exists():
        return [f"{mtlx.name}: missing golden file {golden.name}"]

    # Regenerate into the expected/ dir (as .actual) so relative texture paths
    # match the golden files, then compare.
    actual = EXPECTED / f"{mtlx.stem}.materials.toml.actual"
    library = convert_document(mtlx)
    _write_material_library(library, actual, mtlx)
    try:
        if actual.read_text(encoding="utf-8") != golden.read_text(encoding="utf-8"):
            errors.append(f"{mtlx.name}: output differs from {golden.name}")
    finally:
        actual.unlink(missing_ok=True)
    return errors


def _check_rejected(mtlx: Path) -> list[str]:
    try:
        convert_document(mtlx)
    except OpenPBRValidationError:
        return []
    return [f"{mtlx.name}: expected OpenPBRValidationError, but conversion succeeded"]


def main() -> int:
    failures: list[str] = []
    passed = 0
    for mtlx in sorted(FIXTURES.glob("*.mtlx")):
        if mtlx.name.startswith("not_openpbr"):
            errs = _check_rejected(mtlx)
        else:
            errs = _check_openpbr_fixture(mtlx)
        if errs:
            failures.extend(errs)
        else:
            passed += 1
            print(f"PASS {mtlx.name}")

    for err in failures:
        print(f"FAIL {err}")
    print(f"\n{passed} passed, {len(failures)} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
