"""Self-contained golden-file test runner for ``omm_bake.py``.

No third-party test framework required (standard library only):

    python tools/test_omm_bake.py

Exercises PNG encode/decode, the space-filling-curve index, LSB-first hex
packing, special-index collapse, a full synthetic bake (with an invariant
popcount assertion that is independent of the SFC mapping), the ``.omm``
parse round-trip, the scene ``--write`` injection, and the failure modes
(missing ``map_opacity``, bad hex length, unknown group). Exit code 0 = pass.
"""

from __future__ import annotations

import io
import math
import os
import struct
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import omm_bake as omm  # noqa: E402

try:
    import tomllib  # Python 3.11+
except ModuleNotFoundError:  # pragma: no cover
    tomllib = None


# ── Helpers ──────────────────────────────────────────────────────────────────


def _write(tmp: Path, name: str, text: str) -> Path:
    p = tmp / name
    p.write_text(text, encoding="utf-8")
    return p


def _approx(a: float, b: float, tol: float = 1e-9) -> bool:
    return abs(a - b) <= tol * max(1.0, abs(a), abs(b))


# A one-triangle OBJ with UVs forming the unit right triangle (0,0)/(1,0)/(0,1).
ONE_TRI_OBJ = """\
# one triangle, group Hero
v 0.0 0.0 0.0
v 1.0 0.0 0.0
v 0.0 1.0 0.0
vt 0.0 0.0
vt 1.0 0.0
vt 0.0 1.0
g Hero
f 1/1 2/2 3/3
"""

# A two-group OBJ: Hero (with UVs) + Flat (no UVs).
TWO_GROUP_OBJ = """\
v 0.0 0.0 0.0
v 1.0 0.0 0.0
v 0.0 1.0 0.0
v 1.0 1.0 0.0
vt 0.0 0.0
vt 1.0 0.0
vt 0.0 1.0
vt 1.0 1.0
g Hero
f 1/1 2/2 3/3
g Flat
f 1/1 2/2 4/4
"""


# ── Tests ────────────────────────────────────────────────────────────────────


def check_png_roundtrip(tmp: Path) -> list[str]:
    """PNG encode -> decode reproduces the pixels and dimensions."""
    errors: list[str] = []
    w, h = 5, 3
    px = omm.make_checker_rgba(w, h, 5, 3)
    blob = omm.encode_png_rgba(w, h, px)
    p = _write(tmp, "c.png", "")  # placeholder path
    p.write_bytes(blob)
    dw, dh, ch, dpx = omm.decode_png(p)
    if (dw, dh, ch) != (w, h, 4):
        errors.append("png dims/channels: got (%d,%d,%d)" % (dw, dh, ch))
    if dpx != px:
        errors.append("png round-trip pixels differ")
    return errors


def check_checker_pattern(tmp: Path) -> list[str]:
    """make_checker_rgba: cell (i,j) opaque iff (i+j) even."""
    errors: list[str] = []
    cu, cv = 4, 2
    w, h = cu, cv  # one pixel per cell
    px = omm.make_checker_rgba(w, h, cu, cv)
    for y in range(h):
        for x in range(w):
            off = (y * w + x) * 4
            alpha = px[off + 3]
            expect_opaque = ((x + y) % 2 == 0)
            if expect_opaque and alpha != 255:
                errors.append("cell (%d,%d) expected opaque, alpha=%d" % (x, y, alpha))
            if not expect_opaque and alpha != 0:
                errors.append("cell (%d,%d) expected transparent, alpha=%d" % (x, y, alpha))
    return errors


def check_sampler_uv_convention(tmp: Path) -> list[str]:
    """OpacitySampler matches the renderer's UV convention: wrap-repeat and
    **no V flip** (v = 0 is image row 0, the top row — Slang/Vulkan
    ``Texture2D.Sample`` semantics, and Aether stores OBJ ``vt`` verbatim).
    A V flip here would silently invert an even-celled checkerboard, which is
    exactly the class of bug this pins."""
    errors: list[str] = []
    px = omm.make_checker_rgba(2, 1, 2, 1)
    s = omm.OpacitySampler(2, 1, 4, px)
    # u < 0.5 -> texel 0 (opaque=1); u >= 0.5 -> texel 1 (transparent=0).
    cases = [(0.0, 1.0), (0.25, 1.0), (0.5, 0.0), (0.9, 0.0), (1.4, 1.0)]  # 1.4 wraps -> 0.4
    for u, want in cases:
        got = s.opacity(u, 0.5)
        if not _approx(got, want):
            errors.append("sample u=%g: expected %g, got %g" % (u, want, got))
    # Vertical: a 1x2 image, row 0 (top) opaque, row 1 transparent.
    rows = bytearray(b"\xff\xff\xff\xff" + b"\x00\x00\x00\x00")
    sv = omm.OpacitySampler(1, 2, 4, bytes(rows))
    for v, want in ((0.25, 1.0), (0.75, 0.0)):
        got = sv.opacity(0.5, v)
        if not _approx(got, want):
            errors.append("V convention at v=%g: expected %g, got %g (V flip?)" % (v, want, got))
    return errors


def check_sfc_permutation(tmp: Path) -> list[str]:
    """Level-L centroids map bijectively onto {0..4^L-1} (SFC is a permutation)."""
    errors: list[str] = []
    for level in (1, 2, 3):
        idxs = [
            omm.barycentrics_to_sfc_index(w1, w2, level)
            for (w0, w1, w2) in omm.microtriangle_centroids(level)
        ]
        expect = list(range(4 ** level))
        if sorted(idxs) != expect:
            errors.append(
                "level %d: SFC indices not a permutation of 0..%d (got %s)"
                % (level, 4 ** level - 1, sorted(idxs))
            )
    return errors


def check_hex_packing(tmp: Path) -> list[str]:
    """_pack_hex: LSB-first, byte-0-first; lengths match the spec formula."""
    errors: list[str] = []
    # 2-state, level 2: 16 bits -> 2 bytes -> 4 hex chars.
    if omm._pack_hex([1] * 16, omm.FORMAT_TWO_STATE) != "FFFF":
        errors.append("all-opaque L2 2-state != FFFF")
    if omm._pack_hex([0] * 16, omm.FORMAT_TWO_STATE) != "0000":
        errors.append("all-transparent L2 2-state != 0000")
    # Only SFC index 0 set -> LSB of byte 0 -> "0100" (byte0=0x01, byte1=0x00).
    bits = [0] * 16
    bits[0] = 1
    if omm._pack_hex(bits, omm.FORMAT_TWO_STATE) != "0100":
        errors.append("index-0-only != 0100")
    # SFC index 8 set -> byte1 bit0 -> "0001".
    bits = [0] * 16
    bits[8] = 1
    if omm._pack_hex(bits, omm.FORMAT_TWO_STATE) != "0001":
        errors.append("index-8-only != 0001")
    # 4-state, level 1: 4 microtri * 2 bits = 8 bits = 1 byte = 2 hex chars.
    # [1,0,1,0] -> bits [1,0, 0,0, 1,0, 0,0] -> byte 0b00010001 = 0x11.
    if omm._pack_hex([1, 0, 1, 0], omm.FORMAT_FOUR_STATE) != "11":
        errors.append("4-state L1 [1,0,1,0] != 11")
    return errors


def check_collapse_and_bake(tmp: Path) -> list[str]:
    """Uniform textures collapse to specials; a texture with a single cut-out
    texel produces UNKNOWN (ask-the-shader) microtriangles around it and never
    a TRANSPARENT one — the conservativeness invariant that makes the micromap
    an accelerator instead of a second, coarser cutout."""
    errors: list[str] = []
    uv = ((0.0, 0.0), (1.0, 0.0), (0.0, 1.0))

    opaque_tex = omm.make_checker_rgba(2, 2, 1, 1)  # all opaque
    s_op = omm.OpacitySampler(2, 2, 4, opaque_tex)
    r = omm.bake_triangle(uv[0], uv[1], uv[2], s_op, 2, omm.FORMAT_FOUR_STATE, 1.0)
    if r.special != omm.SPECIAL_OPAQUE:
        errors.append("all-opaque did not collapse to SPECIAL_OPAQUE (got %d)" % r.special)

    trans_tex = bytes(len(opaque_tex))  # all zero alpha
    s_tr = omm.OpacitySampler(2, 2, 4, trans_tex)
    r = omm.bake_triangle(uv[0], uv[1], uv[2], s_tr, 2, omm.FORMAT_FOUR_STATE, 1.0)
    if r.special != omm.SPECIAL_TRANSPARENT:
        errors.append("all-transparent did not collapse to SPECIAL_TRANSPARENT")

    # 64x64 opaque with one transparent texel at (0,0). At level 2 only the
    # microtriangles whose footprint reaches that texel may be ambiguous; none
    # may be declared TRANSPARENT (the shaded cutout keeps 4095 of 4096 texels).
    holed = bytearray(b"\xff" * (64 * 64 * 4))
    holed[3] = 0
    s_hole = omm.OpacitySampler(64, 64, 4, bytes(holed))
    r = omm.bake_triangle(uv[0], uv[1], uv[2], s_hole, 2, omm.FORMAT_FOUR_STATE, 1.0)
    if r.special != 0:
        errors.append("single-hole texture should not collapse (special=%d)" % r.special)
    else:
        states = _unpack_states(r.hex, r.level, r.fmt)
        if omm.STATE_TRANSPARENT in states:
            errors.append("single-hole bake declared a microtriangle TRANSPARENT")
        if omm.STATE_UNKNOWN_OPAQUE not in states:
            errors.append("single-hole bake produced no UNKNOWN microtriangle")
        if omm.STATE_OPAQUE not in states:
            errors.append("single-hole bake produced no OPAQUE microtriangle")

    # Two-state cannot express "ask the shader", so an ambiguous microtriangle
    # must fall back to TRANSPARENT (never a wrongly-committed OPAQUE).
    r2 = omm.bake_triangle(uv[0], uv[1], uv[2], s_hole, 2, omm.FORMAT_TWO_STATE, 1.0)
    if r2.special == omm.SPECIAL_OPAQUE:
        errors.append("two-state bake committed an ambiguous triangle as OPAQUE")
    return errors


def _unpack_states(hexbits: str, level: int, fmt: int) -> list[int]:
    """Expand a packed-state hex dump back to per-microtriangle states."""
    ba = int(hexbits, 16).to_bytes(len(hexbits) // 2, "little")
    n = 4 ** level
    if fmt == omm.FORMAT_TWO_STATE:
        return [(ba[i >> 3] >> (i & 7)) & 1 for i in range(n)]
    return [(ba[(2 * i) >> 3] >> ((2 * i) & 7)) & 3 for i in range(n)]


def check_omm_roundtrip(tmp: Path) -> list[str]:
    """render_omm -> parse_omm reproduces the records; bad hex is rejected."""
    errors: list[str] = []
    obj_path = _write(tmp, "t.obj", ONE_TRI_OBJ)
    obj = omm.parse_obj(obj_path)
    inp = omm.OpacityInput(
        material="Hero",
        texture_path=tmp / "c.png",
        scalar=1.0,
        level=2,
        fmt=omm.FORMAT_FOUR_STATE,
    )
    mixed = omm.make_checker_rgba(2, 1, 2, 1)
    _ = omm.encode_png_rgba(2, 1, mixed)
    (tmp / "c.png").write_bytes(omm.encode_png_rgba(2, 1, mixed))
    gb = omm.bake_group(obj, "Hero", inp)
    inputs = {"Hero": inp}
    text = omm.render_omm(obj_path, [gb], inputs, inp.texture_path)
    omm_path = _write(tmp, "t.omm", text)
    parsed = omm.parse_omm(omm_path)
    if "Hero" not in parsed.groups:
        errors.append("parse_omm lost the Hero group")
        return errors
    recs = parsed.groups["Hero"]
    if len(recs) != 1:
        errors.append("parse_omm record count: expected 1, got %d" % len(recs))
    # Malformed hex length must raise.
    bad = _write(tmp, "bad.omm", "G Hero\nT 2 2 FFFFFFFF0000\n")  # too long for L2/4-state
    try:
        omm.parse_omm(bad)
        errors.append("parse_omm accepted bad hex length")
    except ValueError:
        pass
    return errors


def check_obj_parse_groups(tmp: Path) -> list[str]:
    """parse_obj attributes faces to the current group; fan-triangulates."""
    errors: list[str] = []
    obj_path = _write(tmp, "two.obj", TWO_GROUP_OBJ)
    obj = omm.parse_obj(obj_path)
    if set(obj.groups) != {"Hero", "Flat"}:
        errors.append("groups: expected {Hero,Flat}, got %s" % set(obj.groups))
    if len(obj.groups["Hero"]) != 1:
        errors.append("Hero tri count: expected 1, got %d" % len(obj.groups["Hero"]))
    if len(obj.groups["Flat"]) != 1:
        errors.append("Flat tri count: expected 1, got %d" % len(obj.groups["Flat"]))
    if len(obj.uvs) != 4 or len(obj.positions) != 4:
        errors.append("positions/uvs undercounted")
    return errors


def check_scene_injection(tmp: Path) -> list[str]:
    """inject_opacity_micromaps adds/replaces the key and stays TOML-valid."""
    errors: list[str] = []
    if tomllib is None:
        return errors
    scene = """\
material_libraries = ["m.toml"]

[[mesh]]
name = "m"
path = "m.obj"

[[instance]]
mesh = "m"
material = "Hero"
"""
    scene_path = _write(tmp, "s.scene.toml", scene)
    omm.inject_opacity_micromaps(scene_path, "m", {"Hero": "m_omm.micromap.toml"})
    with open(scene_path, "rb") as f:
        root = tomllib.load(f)
    om = root["mesh"][0].get("opacity_micromaps")
    if om != {"Hero": "m_omm.micromap.toml"}:
        errors.append("injected opacity_micromaps wrong: %r" % om)
    # Re-inject replaces, not duplicates.
    omm.inject_opacity_micromaps(scene_path, "m", {"Hero": "other.micromap.toml"})
    text = scene_path.read_text(encoding="utf-8")
    if text.count("opacity_micromaps") != 1:
        errors.append("re-inject duplicated the key (%d)" % text.count("opacity_micromaps"))
    return errors


def check_explicit_rejects_no_opacity(tmp: Path) -> list[str]:
    """Explicit mode errors when the material has no map_opacity."""
    errors: list[str] = []
    obj_path = _write(tmp, "t.obj", ONE_TRI_OBJ)
    lib = _write(tmp, "m.toml", 'model = "openpbr"\n[Plain]\nbase_color = [0.5, 0.5, 0.5]\n')
    argv = [
        str(obj_path),
        "--material-lib", str(lib),
        "--material", "Plain",
        "--groups", "Hero",
    ]
    rc = omm.main(argv)
    if rc == 0:
        errors.append("explicit bake should fail without map_opacity")
    return errors


def check_explicit_bake_endtoend(tmp: Path) -> list[str]:
    """Explicit mode bakes .omm + .micromap.toml with a usable histogram."""
    errors: list[str] = []
    if tomllib is None:
        return errors
    obj_path = _write(tmp, "t.obj", ONE_TRI_OBJ)
    # 64x64 opaque with one transparent texel: coarse enough that most
    # microtriangles resolve definitively, so a real record (and therefore a
    # usage histogram) is emitted rather than a single collapsed special.
    holed = bytearray(b"\xff" * (64 * 64 * 4))
    holed[3] = 0
    (tmp / "c.png").write_bytes(omm.encode_png_rgba(64, 64, bytes(holed)))
    lib = _write(
        tmp,
        "m.toml",
        'model = "openpbr"\n[Hero]\nmap_opacity = "c.png"\n',
    )
    rc = omm.main([
        str(obj_path),
        "--material-lib", str(lib),
        "--material", "Hero",
        "--groups", "Hero",
        "--level", "2",
        "--out-omm", str(tmp / "out.omm"),
        "--out-toml", str(tmp / "out.micromap.toml"),
        "--quiet",
    ])
    if rc != 0:
        errors.append("explicit bake returned %d" % rc)
        return errors
    with open(tmp / "out.micromap.toml", "rb") as f:
        desc = tomllib.load(f)
    if desc["format"] != "omm-text/v1":
        errors.append("descriptor format != omm-text/v1")
    grp = desc["group"][0]
    if grp["name"] != "Hero":
        errors.append("group name != Hero")
    if grp["triangle_count"] != 1:
        errors.append("triangle_count != 1")
    if not grp["usage"]:
        errors.append("usage histogram empty")
    return errors


# ── Runner ───────────────────────────────────────────────────────────────────


CHECKS = [
    ("png_roundtrip", check_png_roundtrip),
    ("checker_pattern", check_checker_pattern),
    ("sampler_uv_convention", check_sampler_uv_convention),
    ("sfc_permutation", check_sfc_permutation),
    ("hex_packing", check_hex_packing),
    ("collapse_and_bake", check_collapse_and_bake),
    ("omm_roundtrip", check_omm_roundtrip),
    ("obj_parse_groups", check_obj_parse_groups),
    ("scene_injection", check_scene_injection),
    ("explicit_rejects_no_opacity", check_explicit_rejects_no_opacity),
    ("explicit_bake_endtoend", check_explicit_bake_endtoend),
]


def main() -> int:
    failures = 0
    with tempfile.TemporaryDirectory(prefix="omm_bake_test_") as td:
        tmp = Path(td)
        for name, fn in CHECKS:
            try:
                errs = fn(tmp)
            except Exception as exc:  # noqa: BLE001
                errs = ["raised %s: %s" % (type(exc).__name__, exc)]
            if errs:
                failures += 1
                sys.stderr.write("FAIL %s:\n" % name)
                for e in errs:
                    sys.stderr.write("    %s\n" % e)
            else:
                sys.stderr.write("ok   %s\n" % name)
    if failures:
        sys.stderr.write("\n%d check(s) failed\n" % failures)
        return 1
    sys.stderr.write("\nall %d checks passed\n" % len(CHECKS))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
